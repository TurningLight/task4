/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL_RESV
#include <string>
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ob_resolver.h"
#include "sql/resolver/ob_resolver_utils.h"
#include "sql/resolver/expr/ob_raw_expr_util.h"

using namespace oceanbase;
using namespace sql;
using namespace common;
const static int64_t EXPLAIN_FORMAT = 0;
const static int64_t EXPLAIN_DISPLAY_OPTION = 1;
const static int64_t EXPLAIN_INTO_TABLE = 2;
const static int64_t EXPLAIN_STMT_ID = 3;
const static int64_t EXPLAIN_CHILD_STMT = 4;

// Result shape of the DuckDB style `DESCRIBE <select>`.
const static int64_t FS_DESC_COLUMN_CNT = 6;
const static char *FS_DESC_HEADERS[FS_DESC_COLUMN_CNT] = {
    "column_name", "column_type", "null", "key", "default", "extra"};

// `describe <stmt>` is parsed exactly like `explain <stmt>`: the same T_EXPLAIN node with
// no trace of the spelling left in the tree, so it is recovered from the statement text.
// The format/display/into/statement_id children must all be absent, which guarantees that
// a plain `explain ...` or one of its formatted variants can never take this path, and
// `describe <table>` keeps its own T_SHOW_COLUMNS node (MySQL SHOW COLUMNS).
static bool is_file_table_describe(const ParseNode &parse_tree, ObSQLSessionInfo *session_info)
{
  bool is_describe = false;
  if (NULL != session_info
      && NULL == parse_tree.children_[EXPLAIN_FORMAT]
      && NULL == parse_tree.children_[EXPLAIN_DISPLAY_OPTION]
      && NULL == parse_tree.children_[EXPLAIN_INTO_TABLE]
      && NULL == parse_tree.children_[EXPLAIN_STMT_ID]) {
    const ObString sql = session_info->get_current_query_string();
    const char *ptr = sql.ptr();
    const int64_t len = sql.length();
    int64_t pos = 0;
    int64_t keyword_len = 0;
    while (pos < len && (' ' == ptr[pos] || '\t' == ptr[pos] || '\r' == ptr[pos]
                         || '\n' == ptr[pos] || '\f' == ptr[pos])) {
      ++pos;
    }
    if (len - pos >= 8 && 0 == STRNCASECMP(ptr + pos, "describe", 8)) {
      keyword_len = 8;
    } else if (len - pos >= 4 && 0 == STRNCASECMP(ptr + pos, "desc", 4)) {
      keyword_len = 4;
    }
    if (keyword_len > 0) {
      const int64_t next = pos + keyword_len;
      // the whole keyword, not the prefix of a longer identifier
      if (next >= len || ' ' == ptr[next] || '\t' == ptr[next] || '\r' == ptr[next]
          || '\n' == ptr[next] || '\f' == ptr[next] || '(' == ptr[next]) {
        is_describe = true;
      }
    }
  }
  return is_describe;
}

// Append FS_DESC_COLUMN_CNT constant string cells as one row of the metadata values table.
static int fs_desc_add_row(ObIAllocator &allocator,
                           ObRawExprFactory &expr_factory,
                           ObSQLSessionInfo *session_info,
                           const char *const *cells,
                           ObIArray<ObRawExpr *> &values_vector,
                           ObIArray<ObRawExprResType> &value_types)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < FS_DESC_COLUMN_CNT; ++i) {
    ObString cell;
    ObConstRawExpr *c_expr = NULL;
    ObRawExprResType res_type;
    if (OB_ISNULL(cells[i])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null describe cell", K(ret), K(i));
    } else if (OB_FAIL(ob_write_string(allocator, ObString::make_string(cells[i]), cell))) {
      LOG_WARN("failed to write describe cell", K(ret));
    } else if (OB_FAIL(ObRawExprUtils::build_const_string_expr(expr_factory, ObVarcharType, cell,
                                                               CS_TYPE_UTF8MB4_GENERAL_CI,
                                                               c_expr))) {
      LOG_WARN("failed to build const string expr", K(ret));
    } else if (OB_ISNULL(c_expr)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null const expr", K(ret));
    } else if (OB_FAIL(c_expr->formalize(session_info))) {
      LOG_WARN("failed to formalize const expr", K(ret));
    } else if (FALSE_IT(res_type = c_expr->get_result_type())) {
    } else if (OB_FAIL(values_vector.push_back(c_expr))) {
      LOG_WARN("failed to push back describe value", K(ret));
    } else if (OB_FAIL(value_types.push_back(res_type))) {
      LOG_WARN("failed to push back describe value type", K(ret));
    }
  }
  return ret;
}

static ObJtColBaseInfo *fs_find_file_table_column(ObJsonTableDef &table_def, uint64_t column_id)
{
  ObJtColBaseInfo *found = NULL;
  for (int64_t i = 0; NULL == found && i < table_def.all_cols_.count(); ++i) {
    ObJtColBaseInfo *col = table_def.all_cols_.at(i);
    if (NULL != col
        && (static_cast<uint64_t>(col->output_column_idx_) == column_id
            || static_cast<uint64_t>(col->id_) == column_id)) {
      found = col;
    }
  }
  return found;
}

// DuckDB style DESCRIBE over a file table. `describe select ...` is parsed as EXPLAIN, and
// the DuckDB column metadata (column_name/column_type/null/key/default/extra) is produced
// here, at resolve time, as a small values-table select: the inferred schema is already
// known, so no planner or spec plumbing is needed and the ordinary execution path applies.
// `handled` stays false whenever the inner statement is not a plain projection over a
// single file table, in which case the caller keeps the MySQL EXPLAIN behaviour.
int ObExplainResolver::resolve_file_table_describe(ObStmt &child_stmt, bool &handled)
{
  int ret = OB_SUCCESS;
  handled = false;
  ObSelectStmt *child = NULL;
  TableItem *file_item = NULL;
  ObJsonTableDef *table_def = NULL;
  if (stmt::T_SELECT != child_stmt.get_stmt_type()) {
    // only `describe select ...` uses the DuckDB result shape
  } else if (OB_ISNULL(child = static_cast<ObSelectStmt *>(&child_stmt))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("null select stmt", K(ret));
  } else if (child->is_set_stmt() || 1 != child->get_table_size()) {
    // union/intersect/except or a join: keep the EXPLAIN behaviour
  } else if (OB_ISNULL(file_item = child->get_table_item(static_cast<int64_t>(0)))
             || !file_item->is_file_table()
             || OB_ISNULL(table_def = file_item->json_table_def_)) {
    // not a file table (read_csv/read_json/read_parquet/glob): keep the EXPLAIN behaviour
  } else {
    ObSEArray<ObJtColBaseInfo *, 8> cols;
    bool valid = !child->get_select_items().empty();
    for (int64_t i = 0; OB_SUCC(ret) && valid && i < child->get_select_item_size(); ++i) {
      ObRawExpr *expr = child->get_select_item(i).expr_;
      ObJtColBaseInfo *col = NULL;
      if (OB_ISNULL(expr) || !expr->is_column_ref_expr()) {
        // an expression/aggregate has no inferred file type
        valid = false;
      } else {
        ObColumnRefRawExpr *col_expr = static_cast<ObColumnRefRawExpr *>(expr);
        if (col_expr->get_table_id() != file_item->table_id_) {
          valid = false;
        } else if (OB_ISNULL(col = fs_find_file_table_column(*table_def,
                                                             col_expr->get_column_id()))
                   || col->fs_type_name_.empty()) {
          valid = false;
        } else if (OB_FAIL(cols.push_back(col))) {
          LOG_WARN("failed to push back describe column", K(ret));
        }
      }
    }
    if (OB_SUCC(ret) && valid) {
      if (OB_FAIL(build_file_table_describe(*child, cols))) {
        LOG_WARN("failed to build file table describe", K(ret));
      } else {
        handled = true;
      }
    }
  }
  return ret;
}

int ObExplainResolver::build_file_table_describe(ObSelectStmt &child,
                                                 ObIArray<ObJtColBaseInfo *> &cols)
{
  int ret = OB_SUCCESS;
  ObSelectStmt *meta_stmt = NULL;
  ObValuesTableDef *table_def = NULL;
  void *def_buf = NULL;
  ObSEArray<ObRawExprResType, 8> cur_types;
  ObSEArray<int64_t, 8> max_len;
  if (OB_ISNULL(allocator_) || OB_ISNULL(params_.expr_factory_) || OB_ISNULL(params_.query_ctx_)
      || OB_ISNULL(session_info_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("explain resolver is not inited", K(ret));
  } else if (OB_ISNULL(meta_stmt = create_stmt<ObSelectStmt>())) {
    ret = OB_SQL_RESOLVER_NO_MEMORY;
    LOG_WARN("failed to create stmt for describe", K(ret));
  } else if (OB_ISNULL(def_buf = allocator_->alloc(sizeof(ObValuesTableDef)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    LOG_WARN("failed to allocate values table def", K(ret));
  } else if (FALSE_IT(table_def = new (def_buf) ObValuesTableDef())) {
  } else if (FALSE_IT(table_def->column_cnt_ = FS_DESC_COLUMN_CNT)) {
  } else if (FALSE_IT(table_def->is_const_ = true)) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < cols.count(); ++i) {
    const char *cells[FS_DESC_COLUMN_CNT];
    ObJtColBaseInfo *col = cols.at(i);
    std::string name;
    std::string type;
    cur_types.reuse();
    if (OB_ISNULL(col)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("null describe column", K(ret), K(i));
    } else {
      const SelectItem &select_item = child.get_select_item(i);
      if (!select_item.alias_name_.empty()) {
        name.assign(select_item.alias_name_.ptr(), select_item.alias_name_.length());
      } else {
        name.assign(col->col_name_.ptr(), col->col_name_.length());
      }
      type.assign(col->fs_type_name_.ptr(), col->fs_type_name_.length());
      cells[0] = name.c_str();
      cells[1] = type.c_str();
      cells[2] = col->fs_nullable_ ? "YES" : "NO";
      cells[3] = "NULL";
      cells[4] = "NULL";
      cells[5] = "NULL";
      if (OB_FAIL(fs_desc_add_row(*allocator_, *params_.expr_factory_, session_info_,
                                  cells, table_def->access_exprs_, cur_types))) {
        LOG_WARN("failed to build describe row", K(ret));
      } else {
        // every cell is a plain varchar, so the widest cell of a column is its column type
        for (int64_t c = 0; OB_SUCC(ret) && c < FS_DESC_COLUMN_CNT; ++c) {
          const int64_t len = static_cast<int64_t>(strlen(cells[c]));
          if (table_def->column_types_.count() <= c) {
            if (OB_FAIL(table_def->column_types_.push_back(cur_types.at(c)))) {
              LOG_WARN("failed to push back describe column type", K(ret));
            } else if (OB_FAIL(max_len.push_back(len))) {
              LOG_WARN("failed to push back describe cell length", K(ret));
            }
          } else if (len > max_len.at(c)) {
            max_len.at(c) = len;
            table_def->column_types_.at(c) = cur_types.at(c);
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(try_add_cast_to_values(table_def->column_types_, table_def->access_exprs_))) {
      LOG_WARN("failed to add cast to describe values", K(ret));
    } else if (OB_FAIL(compute_values_table_row_count(*table_def))) {
      LOG_WARN("failed to compute describe values row count", K(ret));
    } else if (OB_FAIL(estimate_values_table_stats(*table_def))) {
      LOG_WARN("failed to estimate describe values stats", K(ret));
    } else if (OB_FAIL(ObResolverUtils::create_values_table_query(session_info_, allocator_,
                                                                 params_.expr_factory_,
                                                                 params_.query_ctx_,
                                                                 meta_stmt, table_def))) {
      LOG_WARN("failed to create describe values query", K(ret));
    } else {
      // the result column names of the metadata select
      for (int64_t i = 0; OB_SUCC(ret) && i < FS_DESC_COLUMN_CNT; ++i) {
        SelectItem &select_item = meta_stmt->get_select_item(i);
        select_item.alias_name_ = ObString::make_string(FS_DESC_HEADERS[i]);
        select_item.expr_name_ = select_item.alias_name_;
        select_item.is_real_alias_ = true;
      }
    }
  }
  return ret;
}

int ObExplainResolver::resolve(const ParseNode &parse_tree)
{
  int ret = OB_SUCCESS;
  ObExplainStmt *explain_stmt = NULL;
  ObStmt *child_stmt = NULL;
  ParseNode *child_node = NULL;
  bool handled = false;
  if (!(T_EXPLAIN == parse_tree.type_  && 5 == parse_tree.num_child_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid EXPLAIN syntax",
             "type", parse_tree.type_,
             "# of child", parse_tree.num_child_);
  } else if (OB_ISNULL(child_node = parse_tree.children_[EXPLAIN_CHILD_STMT])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("empty parse tree for stmt");
  } else if (!(T_SELECT == child_node->type_
               || T_DELETE == child_node->type_
               || T_INSERT == child_node->type_
               || T_UPDATE == child_node->type_
               || T_REPLACE == child_node->type_)) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("invalid stmt type for EXPLAIN", "type", child_node->type_);
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "stmt type for EXPLAIN");
  } else {
    ObResolver resolver(params_);
    if (OB_FAIL(resolver.resolve(ObResolver::IS_NOT_PREPARED_STMT,
                                 *child_node,
                                 child_stmt))) {
      LOG_WARN("failed to resolve the inner stmt of EXPLAIN", K(ret));
    } else if (OB_ISNULL(child_stmt)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to generate stmt for child stmt");
    } else if (is_file_table_describe(parse_tree, session_info_)
               && OB_FAIL(resolve_file_table_describe(*child_stmt, handled))) {
      LOG_WARN("failed to resolve DESCRIBE over a file table", K(ret));
    }
  }
  if (OB_SUCC(ret) && !handled) {
    ObExplainDisplayOpt opt;
    opt.with_tree_line_ = true;
    if (OB_ISNULL(explain_stmt = create_stmt<ObExplainStmt>())) {
      ret = OB_SQL_RESOLVER_NO_MEMORY;
      LOG_WARN("failed to create explain stmt");
    } else {
      explain_stmt->set_explain_query_stmt(static_cast<ObDMLStmt *>(child_stmt));
      ParseNode *format = parse_tree.children_[EXPLAIN_FORMAT];
      if (NULL != format) {
        switch (format->type_) {
        case T_OUTLINE:
        {
          explain_stmt->set_explain_format(EXPLAIN_OUTLINE);
          opt.with_tree_line_ = true;
        } break;
        case T_EXTENDED:
        {
          explain_stmt->set_explain_format(EXPLAIN_EXTENDED);
          opt.with_tree_line_ = true;
        } break;
        case T_PARTITIONS:
        {
          explain_stmt->set_explain_format(EXPLAIN_PARTITIONS);
          opt.with_tree_line_ = true;
        } break;
        case T_TRADITIONAL:
        {
          explain_stmt->set_explain_format(EXPLAIN_TRADITIONAL);
          opt.with_tree_line_ = true;
        } break;
        case T_FORMAT_JSON:
        {
          explain_stmt->set_explain_format(EXPLAIN_FORMAT_JSON);
          opt.with_tree_line_ = false;
        } break;
        case T_BASIC: {
          explain_stmt->set_explain_format(EXPLAIN_BASIC);
          opt.with_tree_line_ = true;
          break;
        }
        case T_EXTENDED_NOADDR: {
          explain_stmt->set_explain_format(EXPLAIN_EXTENDED_NOADDR);
          opt.with_tree_line_ = true;
          break;
        }
        case T_PLANREGRESS: {
          explain_stmt->set_explain_format(EXPLAIN_PLANREGRESS);
          opt.with_tree_line_ = true;
          break;
        }
        default:
          ret = OB_ERROR;
          LOG_WARN("unknown explain format", "format", format->type_);
          break;
        }
      }
    }
    ParseNode *opt_node = parse_tree.children_[EXPLAIN_DISPLAY_OPTION];
    if (OB_SUCC(ret) && NULL != opt_node) {
      switch (opt_node->type_) {
      case T_PRETTY_COLOR:
        opt.with_color_ = true;
        // pass
      case T_PRETTY:
        opt.with_tree_line_ = true;
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
      }
    }
    if (OB_SUCC(ret)) {
      explain_stmt->set_display_opt(opt);
    }
    ParseNode *into_table_node = parse_tree.children_[EXPLAIN_INTO_TABLE];
    if (OB_SUCC(ret) && NULL != into_table_node) {
      ObString into_table(into_table_node->str_len_, into_table_node->str_value_);
      explain_stmt->set_into_table(into_table);
    }
    ParseNode *statement_id_node = parse_tree.children_[EXPLAIN_STMT_ID];
    if (OB_SUCC(ret) && NULL != statement_id_node) {
      ObString statement_id(statement_id_node->str_len_, statement_id_node->str_value_);
      explain_stmt->set_statement_id(statement_id);
    }
  }

  return ret;
}

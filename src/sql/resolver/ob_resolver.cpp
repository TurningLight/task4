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
#include "sql/resolver/ob_resolver.h"
#include "lib/utility/ob_smart_call.h"
#include "lib/stat/ob_diagnostic_info_guard.h"
#include "sql/resolver/cmd/ob_alter_system_resolver.h"
#include "sql/resolver/dml/ob_insert_resolver.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_delete_resolver.h"
#include "sql/resolver/tcl/ob_start_trans_resolver.h"
#include "sql/resolver/tcl/ob_end_trans_resolver.h"
#include "sql/resolver/prepare/ob_prepare_resolver.h"
#include "sql/resolver/prepare/ob_execute_resolver.h"
#include "sql/resolver/prepare/ob_deallocate_resolver.h"
#include "sql/resolver/ddl/ob_create_table_resolver.h"
#include "sql/resolver/ddl/ob_rename_table_resolver.h"
#include "sql/resolver/ddl/ob_truncate_table_resolver.h"
#include "sql/resolver/ddl/ob_fork_table_resolver.h"
#include "sql/resolver/ddl/ob_fork_database_resolver.h"
#include "sql/resolver/ddl/ob_create_table_like_resolver.h"
#include "sql/resolver/ddl/ob_alter_table_resolver.h"
#include "sql/resolver/ddl/ob_drop_table_resolver.h"
#include "sql/resolver/ddl/ob_create_index_resolver.h"
#include "sql/resolver/ddl/ob_drop_index_resolver.h"
#include "sql/resolver/ddl/ob_create_database_resolver.h"
#include "sql/resolver/ddl/ob_alter_database_resolver.h"
#include "sql/resolver/ddl/ob_use_database_resolver.h"
#include "sql/resolver/ddl/ob_drop_database_resolver.h"
#include "sql/resolver/ddl/ob_create_view_resolver.h"
#include "sql/resolver/ddl/ob_explain_resolver.h"
#include "sql/resolver/ddl/ob_create_outline_resolver.h"
#include "sql/resolver/ddl/ob_alter_outline_resolver.h"
#include "sql/resolver/ddl/ob_drop_outline_resolver.h"
#include "sql/resolver/ddl/ob_drop_routine_resolver.h"
#include "sql/resolver/ddl/ob_trigger_resolver.h"
#include "sql/resolver/ddl/ob_optimize_resolver.h"
#include "ddl/ob_drop_routine_resolver.h"
#include "ddl/ob_alter_routine_resolver.h"
#include "sql/resolver/ddl/ob_create_package_resolver.h"
#include "sql/resolver/ddl/ob_drop_package_resolver.h"
#include "sql/resolver/ddl/ob_recyclebin_restore_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_analyze_stmt_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_set_comment_resolver.h"
#include "sql/resolver/ddl/ob_lock_table_resolver.h"
#include "sql/resolver/dml/ob_insert_resolver.h"
#include "sql/resolver/dml/ob_update_resolver.h"
#include "sql/resolver/dml/ob_delete_resolver.h"
#include "sql/resolver/dcl/ob_create_user_resolver.h"
#include "sql/resolver/dcl/ob_drop_user_resolver.h"
#include "sql/resolver/dcl/ob_rename_user_resolver.h"
#include "sql/resolver/dcl/ob_set_password_resolver.h"
#include "sql/resolver/dcl/ob_lock_user_resolver.h"
#include "sql/resolver/dcl/ob_grant_resolver.h"
#include "sql/resolver/dcl/ob_revoke_resolver.h"
#include "sql/resolver/dcl/ob_create_role_resolver.h"
#include "sql/resolver/dcl/ob_drop_role_resolver.h"
#include "sql/resolver/dcl/ob_alter_user_role_resolver.h"
#include "sql/resolver/tcl/ob_start_trans_resolver.h"
#include "sql/resolver/tcl/ob_end_trans_resolver.h"
#include "tcl/ob_savepoint_resolver.h"
#include "sql/resolver/cmd/ob_variable_set_resolver.h"
#include "sql/resolver/cmd/ob_show_resolver.h"
#include "sql/resolver/cmd/ob_diff_table_resolver.h"
#include "sql/resolver/cmd/ob_merge_table_resolver.h"
#include "sql/resolver/cmd/ob_kill_resolver.h"
#include "sql/resolver/cmd/ob_set_names_resolver.h"
#include "sql/resolver/cmd/ob_set_transaction_resolver.h"
#include "sql/resolver/cmd/ob_empty_query_resolver.h"
#include "sql/resolver/cmd/ob_anonymous_block_resolver.h"
#include "sql/resolver/cmd/ob_call_procedure_resolver.h"
#include "sql/resolver/cmd/ob_load_data_resolver.h"
#include "sql/resolver/prepare/ob_execute_resolver.h"
#include "sql/resolver/prepare/ob_deallocate_resolver.h"
#include "sql/resolver/ddl/ob_purge_resolver.h"
#include "sql/resolver/ddl/ob_set_comment_resolver.h"
#include "sql/resolver/expr/ob_raw_expr_wrap_enum_set.h"
#include "sql/resolver/cmd/ob_get_diagnostics_resolver.h"
#include "sql/resolver/dcl/ob_alter_role_resolver.h"
#include "sql/pl/ob_pl_package.h"
#include "lib/string/ob_sql_string.h"
#include "sql/parser/ob_parser.h"

namespace oceanbase
{
using namespace common;
using namespace share;
namespace sql
{
ObResolver::ObResolver(ObResolverParams &params)
    : params_(params)
{

}

ObResolver::~ObResolver()
{
}

template <typename ResolverType>
int ObResolver::stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(ResolverType, stmt_resolver, params) {
    if (OB_FAIL(SMART_CALL(stmt_resolver.resolve(parse_tree)))) {
    }
    stmt = stmt_resolver.get_basic_stmt();
  }
  return ret;
}

template <typename SelectResolverType>
int ObResolver::select_stmt_resolver_func(ObResolverParams &params, const ParseNode &parse_tree, ObStmt *&stmt)
{
  int ret = OB_SUCCESS;
  HEAP_VAR(SelectResolverType, stmt_resolver, params) {
    stmt_resolver.set_calc_found_rows(true);
    stmt_resolver.set_has_top_limit(true);
    if (OB_FAIL(SMART_CALL(stmt_resolver.resolve(parse_tree)))) {
    }
    stmt = stmt_resolver.get_basic_stmt();
  }
  return ret;
}

// DESCRIBE <select over file tables> and COPY (<select>) TO '<file>' have no
// dedicated executor support, so both are rewritten here into plain SELECT
// statements that reuse the existing JSON_TABLE and INTO OUTFILE machinery.

static bool file_sql_tree_has_file_table(const ParseNode *node)
{
  bool found = false;
  if (OB_NOT_NULL(node)) {
    if (T_FILE_TABLE == node->type_) {
      found = true;
    } else if (node->num_child_ > 0) {
      for (int32_t i = 0; !found && i < node->num_child_; ++i) {
        found = file_sql_tree_has_file_table(node->children_[i]);
      }
    }
  }
  return found;
}

static int file_sql_append_sql_string(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  const char *ptr = value.ptr();
  if (OB_FAIL(sql.append("'"))) {
  } else {
    int64_t begin = 0;
    for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
      if ('\'' == ptr[i] || '\\' == ptr[i]) {
        if (OB_FAIL(sql.append(ptr + begin, i - begin + 1))) {
        } else if (OB_FAIL(sql.append(ptr + i, 1))) {
        } else {
          begin = i + 1;
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(sql.append(ptr + begin, value.length() - begin))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
    }
  }
  return ret;
}

static int file_sql_get_name(const ParseNode *node, ObString &name)
{
  int ret = OB_SUCCESS;
  name.reset();
  if (OB_ISNULL(node)) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else if (T_RELATION_FACTOR == node->type_ && 2 == node->num_child_
             && OB_NOT_NULL(node->children_[1])) {
    node = node->children_[1];
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(node->str_value_) || node->str_len_ <= 0) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else {
    name.assign_ptr(node->str_value_, node->str_len_);
  }
  return ret;
}

// The option list production is left recursive, so a list of options is
// nested as link(link(option0, option1), option2).
static int file_sql_collect_nodes(const ParseNode *node,
                                  ObIArray<const ParseNode *> &nodes)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_LINK_NODE == node->type_ && 2 == node->num_child_) {
    if (OB_FAIL(file_sql_collect_nodes(node->children_[0], nodes))) {
    } else {
      ret = file_sql_collect_nodes(node->children_[1], nodes);
    }
  } else {
    ret = nodes.push_back(node);
  }
  return ret;
}

static const char *file_sql_describe_type_name(const ObObjType type)
{
  const char *name = "VARCHAR";
  switch (type) {
    case ObTinyIntType:
    case ObUTinyIntType:
      name = "BOOLEAN";
      break;
    case ObSmallIntType:
    case ObMediumIntType:
    case ObIntType:
    case ObInt32Type:
    case ObUInt64Type:
      name = "BIGINT";
      break;
    case ObFloatType:
    case ObDoubleType:
      name = "DOUBLE";
      break;
    case ObDateType:
      name = "DATE";
      break;
    case ObDateTimeType:
      name = "DATETIME";
      break;
    default:
      name = "VARCHAR";
      break;
  }
  return name;
}

static int file_sql_parse_statement(ObIAllocator &alloc,
                                    ObSQLSessionInfo *session_info,
                                    const ObString &sql_text,
                                    ParseNode *&new_tree)
{
  int ret = OB_SUCCESS;
  ParseResult parse_result;
  new_tree = NULL;
  if (OB_ISNULL(session_info)) {
    ret = OB_NOT_INIT;
  } else {
    ObParser parser(alloc, session_info->get_sql_mode());
    if (OB_FAIL(parser.parse(sql_text, parse_result))) {
      LOG_WARN("failed to parse rewritten file sql", K(ret), K(sql_text));
    } else if (OB_ISNULL(parse_result.result_tree_)
               || 1 != parse_result.result_tree_->num_child_
               || OB_ISNULL(parse_result.result_tree_->children_[0])) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unexpected rewritten file sql parse tree", K(ret), K(sql_text));
    } else {
      new_tree = parse_result.result_tree_->children_[0];
    }
  }
  return ret;
}

int ObResolver::resolve_file_query(const ParseNode *select_node, ObSelectStmt *&select_stmt)
{
  int ret = OB_SUCCESS;
  ObStmt *stmt = NULL;
  select_stmt = NULL;
  if (OB_ISNULL(select_node)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    params_.resolver_scope_stmt_type_ = T_SELECT;
    if (OB_FAIL(select_stmt_resolver_func<ObSelectResolver>(params_, *select_node, stmt))) {
    } else if (OB_ISNULL(stmt) || !stmt->is_select_stmt()) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("failed to resolve file query", K(ret));
    } else {
      select_stmt = static_cast<ObSelectStmt *>(stmt);
    }
  }
  return ret;
}

int ObResolver::rewrite_describe_file_query(const ParseNode &parse_tree, ParseNode *&new_tree)
{
  int ret = OB_SUCCESS;
  new_tree = NULL;
  const ParseNode *select_node = NULL;
  ObSelectStmt *select_stmt = NULL;
  ObSqlString sql;
  if (T_EXPLAIN != parse_tree.type_ || 5 != parse_tree.num_child_
      || OB_ISNULL(select_node = parse_tree.children_[4])) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid describe node", K(ret));
  } else if (OB_FAIL(resolve_file_query(select_node, select_stmt))) {
  } else if (OB_FAIL(sql.append("select column_name, column_type, `null`, `key`, "
                                "`default`, `extra` from ("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < select_stmt->get_select_item_size(); ++i) {
    const SelectItem &item = select_stmt->get_select_item(i);
    const ObJtColBaseInfo *col_info = NULL;
    const char *type_name = "VARCHAR";
    ObString column_name;
    bool nullable = true;
    if (OB_ISNULL(item.expr_)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      if (item.expr_->is_column_ref_expr()) {
        ObColumnRefRawExpr *col_expr = static_cast<ObColumnRefRawExpr *>(item.expr_);
        TableItem *table_item = select_stmt->get_table_item_by_id(col_expr->get_table_id());
        if (OB_NOT_NULL(table_item) && table_item->is_json_table()
            && OB_NOT_NULL(table_item->get_json_table_def())) {
          const ObJsonTableDef *jt_def = table_item->get_json_table_def();
          for (int64_t j = 0; NULL == col_info && j < jt_def->all_cols_.count(); ++j) {
            const ObJtColBaseInfo *info = jt_def->all_cols_.at(j);
            if (OB_NOT_NULL(info) && info->output_column_idx_ == col_expr->get_column_id()) {
              col_info = info;
            }
          }
        }
      }
      if (OB_NOT_NULL(col_info)) {
        type_name = file_sql_describe_type_name(col_info->data_type_.get_obj_type());
        column_name = col_info->col_name_;
        nullable = col_info->is_nullable_;
      } else {
        type_name = file_sql_describe_type_name(item.expr_->get_result_type().get_type());
      }
      if (column_name.empty()) {
        column_name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      }
      if (!item.alias_name_.empty()) {
        column_name = item.alias_name_;
      }
    }
    if (OB_FAIL(ret)) {
    } else if (i > 0 && OB_FAIL(sql.append(" union all "))) {
    } else if (OB_FAIL(sql.append_fmt("select %ld as __ob_fs_ord, ", i))) {
    } else if (OB_FAIL(file_sql_append_sql_string(sql, column_name))) {
    } else if (OB_FAIL(sql.append(" as column_name, "))) {
    } else if (OB_FAIL(file_sql_append_sql_string(sql, ObString(type_name)))) {
    } else if (OB_FAIL(sql.append(" as column_type, "))) {
    } else if (OB_FAIL(sql.append(nullable ? "'YES'" : "'NO'"))) {
    } else if (OB_FAIL(sql.append(" as `null`, null as `key`, null as `default`, "
                                  "null as `extra`"))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (0 == select_stmt->get_select_item_size()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("describe file query has no column", K(ret));
  } else if (OB_FAIL(sql.append(") __ob_fs_desc order by __ob_fs_ord"))) {
  } else if (OB_FAIL(file_sql_parse_statement(*params_.allocator_, params_.session_info_,
                                              sql.string(), new_tree))) {
  }
  return ret;
}

int ObResolver::rewrite_copy_query(const ParseNode &parse_tree, ParseNode *&new_tree)
{
  int ret = OB_SUCCESS;
  new_tree = NULL;
  const ParseNode *select_node = NULL;
  const ParseNode *path_node = NULL;
  const ParseNode *option_node = NULL;
  ObSelectStmt *select_stmt = NULL;
  ObSqlString sql;
  ObString select_text;
  ObString path;
  ObString field_delimiter(",");
  bool with_header = false;
  bool enabled = false;
  if (T_COPY_QUERY != parse_tree.type_ || 3 != parse_tree.num_child_) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("invalid copy node", K(ret), K(parse_tree.type_), K(parse_tree.num_child_));
  } else {
    select_node = parse_tree.children_[0];
    path_node = parse_tree.children_[1];
    option_node = parse_tree.children_[2];
    select_text.assign_ptr(parse_tree.str_value_, parse_tree.str_len_);
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(select_node) || OB_ISNULL(path_node)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (T_VARCHAR != path_node->type_ || OB_ISNULL(path_node->str_value_)) {
    ret = OB_ERR_PARSER_SYNTAX;
    LOG_WARN("copy target must be a string literal", K(ret));
  } else if (OB_ISNULL(params_.session_info_)) {
    ret = OB_NOT_INIT;
  } else {
    path.assign_ptr(path_node->str_value_, path_node->str_len_);
    if (OB_FAIL(params_.session_info_->get_enable_file_sql(enabled))) {
    } else if (!enabled) {
      ret = OB_NOT_SUPPORTED;
      LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy query, enable_file_sql is off");
    }
  }
  ObSEArray<const ParseNode *, 4> options;
  if (OB_SUCC(ret) && OB_NOT_NULL(option_node)
      && OB_FAIL(file_sql_collect_nodes(option_node, options))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < options.count(); ++i) {
    const ParseNode *option = options.at(i);
    if (OB_ISNULL(option)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const ParseNode *name_node = option;
      const ParseNode *value_node = NULL;
      ObString option_name;
      if (T_OP_EQ == option->type_ && 2 == option->num_child_) {
        name_node = option->children_[0];
        value_node = option->children_[1];
      }
      if (OB_FAIL(file_sql_get_name(name_node, option_name))) {
      } else if (0 == option_name.case_compare("header")) {
        if (OB_ISNULL(value_node)) {
          with_header = true;
        } else if (T_BOOL == value_node->type_) {
          with_header = (0 != value_node->value_);
        } else {
          ret = OB_ERR_PARSER_SYNTAX;
          LOG_WARN("copy header option requires a boolean value", K(ret));
        }
      } else if (0 == option_name.case_compare("format")) {
        ObString format;
        if (OB_FAIL(file_sql_get_name(value_node, format))) {
        } else if (0 != format.case_compare("csv")) {
          ret = OB_NOT_SUPPORTED;
          LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy format other than CSV");
        }
      } else if (0 == option_name.case_compare("nullstr")) {
        // INTO OUTFILE writes NULL values in its own canonical form
      } else if (0 == option_name.case_compare("delim")
                 || 0 == option_name.case_compare("delimiter")) {
        ObString delimiter;
        if (OB_FAIL(file_sql_get_name(value_node, delimiter))) {
        } else {
          field_delimiter = delimiter;
        }
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "copy option");
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(resolve_file_query(select_node, select_stmt))) {
  } else if (0 == select_stmt->get_select_item_size()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("copy query has no column", K(ret));
  } else if (with_header) {
    const int64_t column_count = select_stmt->get_select_item_size();
    if (OB_FAIL(sql.append("select "))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
      if (i > 0 && OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(sql.append_fmt("c%ld", i + 1))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append(" from (select 0 as __ob_fs_head"))) {
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
      const SelectItem &item = select_stmt->get_select_item(i);
      ObString column_name = item.alias_name_.empty() ? item.expr_name_ : item.alias_name_;
      if (OB_FAIL(sql.append(", "))) {
      } else if (OB_FAIL(file_sql_append_sql_string(sql, column_name))) {
      } else if (OB_FAIL(sql.append_fmt(" as c%ld", i + 1))) {
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(sql.append(" union all select 1 as __ob_fs_head, * from ("))) {
    } else if (OB_FAIL(sql.append(select_text))) {
    } else if (OB_FAIL(sql.append(") __ob_fs) __ob_fs_copy order by __ob_fs_head"))) {
    }
  } else if (OB_FAIL(sql.append(select_text))) {
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append(" into outfile "))) {
  } else if (OB_FAIL(file_sql_append_sql_string(sql, path))) {
  } else if (OB_FAIL(sql.append(" format (type = 'csv', field_delimiter = "))) {
  } else if (OB_FAIL(file_sql_append_sql_string(sql, field_delimiter))) {
  } else if (OB_FAIL(sql.append(")"))) {
  } else if (OB_FAIL(file_sql_parse_statement(*params_.allocator_, params_.session_info_,
                                              sql.string(), new_tree))) {
  }
  return ret;
}

int ObResolver::resolve(IsPrepared if_prepared, const ParseNode &parse_tree, ObStmt *&stmt)
{
#define REGISTER_STMT_RESOLVER(name)                                   \
  do {                                                                 \
    ret = stmt_resolver_func<Ob##name##Resolver>(params_, *real_parse_tree, stmt); \
  } while (0)

#define REGISTER_SELECT_STMT_RESOLVER(name)                                   \
  do {                                                                        \
    ret = select_stmt_resolver_func<Ob##name##Resolver>(params_, *real_parse_tree, stmt); \
  } while (0)
  ACTIVE_SESSION_FLAG_SETTER_GUARD(in_resolve);
  int ret = OB_SUCCESS;
  const ParseNode *real_parse_tree = NULL;
  int64_t questionmark_count = 0;
  UNUSED(if_prepared);
  if (OB_ISNULL(params_.allocator_)
      || OB_ISNULL(params_.schema_checker_)
      || OB_ISNULL(params_.session_info_)
      || OB_ISNULL(params_.expr_factory_)
      || OB_ISNULL(params_.query_ctx_)) {
    ret = OB_NOT_INIT;
  } else {
    real_parse_tree = &parse_tree;
  }

  if (OB_SUCC(ret) && T_COPY_QUERY == real_parse_tree->type_) {
    ParseNode *new_tree = NULL;
    if (OB_FAIL(rewrite_copy_query(*real_parse_tree, new_tree))) {
    } else if (OB_ISNULL(new_tree)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      real_parse_tree = new_tree;
    }
  } else if (OB_SUCC(ret) && T_EXPLAIN == real_parse_tree->type_
             && 5 == real_parse_tree->num_child_
             && file_sql_tree_has_file_table(real_parse_tree->children_[4])) {
    ParseNode *new_tree = NULL;
    if (OB_FAIL(rewrite_describe_file_query(*real_parse_tree, new_tree))) {
    } else if (OB_ISNULL(new_tree)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      real_parse_tree = new_tree;
    }
  }

  if (OB_SUCC(ret)) {
    params_.resolver_scope_stmt_type_ = real_parse_tree->type_;
    params_.session_info_->set_stmt_type(
      ObResolverUtils::get_stmt_type_by_item_type(real_parse_tree->type_));
    ObExecContext *exec_ctx = params_.session_info_->get_cur_exec_ctx();
    if (exec_ctx != nullptr && exec_ctx->get_sql_ctx() != nullptr) {
      //save the current stmt type on my context,
      //some other modules may need to get the current SQL stmt type through its context
      ObSqlCtx *sql_ctx = exec_ctx->get_sql_ctx();
      sql_ctx->stmt_type_ = ObResolverUtils::get_stmt_type_by_item_type(real_parse_tree->type_);
    }

    switch (real_parse_tree->type_) {
      case T_CREATE_TABLE: {
        REGISTER_STMT_RESOLVER(CreateTable);
        break;
      }
      case T_ALTER_TABLE: {
        REGISTER_STMT_RESOLVER(AlterTable);
        break;
      }
      case T_CREATE_INDEX: {
        REGISTER_STMT_RESOLVER(CreateIndex);
        break;
      }
      case T_CREATE_VIEW: {
        REGISTER_STMT_RESOLVER(CreateView);
        break;
      }
      case T_ALTER_VIEW: {
#if 0
        ObAlterViewResolver stmt_resolver(params_);
        if (OB_FAIL(stmt_resolver.resolve(*node))) {
        }

        stmt = stmt_resolver.get_basic_stmt();
#endif
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("not support to alter view");
	      LOG_USER_ERROR(OB_NOT_SUPPORTED, "alter view");
        break;
      }
      case T_DROP_VIEW:
      case T_DROP_TABLE: {
        REGISTER_STMT_RESOLVER(DropTable);
        break;
      }
      case T_DROP_INDEX: {
        REGISTER_STMT_RESOLVER(DropIndex);
        break;
      }
      case T_CREATE_TABLE_LIKE: {
        REGISTER_STMT_RESOLVER(CreateTableLike);
        break;
      }
      case T_FORK_TABLE: {
        REGISTER_STMT_RESOLVER(ForkTable);
        break;
      }
      case T_FORK_DATABASE: {
        REGISTER_STMT_RESOLVER(ForkDatabase);
        break;
      }
      case T_DIFF_TABLE: {
        REGISTER_STMT_RESOLVER(DiffTable);
        break;
      }
      case T_MERGE_TABLE: {
        REGISTER_STMT_RESOLVER(MergeTable);
        break;
      }
      case T_SELECT: {
        REGISTER_SELECT_STMT_RESOLVER(Select);
        break;
      }
      case T_INSERT: {
        REGISTER_STMT_RESOLVER(Insert);
        break;
      }
      case T_UPDATE: {
        REGISTER_STMT_RESOLVER(Update);
        break;
      }
      case T_DELETE: {
        REGISTER_STMT_RESOLVER(Delete);
        break;
      }
      case T_BEGIN: {
        REGISTER_STMT_RESOLVER(StartTrans);
        break;
      }
      case T_ROLLBACK:
        //fall through
      case T_COMMIT: {
        REGISTER_STMT_RESOLVER(EndTrans);
        break;
      }
      case T_FREEZE: {
        REGISTER_STMT_RESOLVER(Freeze);
        break;
      }
      case T_FLUSH_CACHE: {
        REGISTER_STMT_RESOLVER(FlushCache);
        break;
      }
      case T_FLUSH_KVCACHE: {
        REGISTER_STMT_RESOLVER(FlushKVCache);
        break;
      }
      case T_FLUSH_ILOGCACHE: {
        REGISTER_STMT_RESOLVER(FlushIlogCache);
        break;
      }
      case T_FLUSH_DAG_WARNINGS: {
        REGISTER_STMT_RESOLVER(FlushDagWarnings);
        break;
      }
      case T_MERGE_CONTROL: {
        REGISTER_STMT_RESOLVER(AdminMerge);
        break;
      }
      case T_CANCEL_TASK: {
        REGISTER_STMT_RESOLVER(CancelTask);
        break;
      }
      case T_REFRESH_MEMORY_STAT: {
        REGISTER_STMT_RESOLVER(RefreshMemStat);
        break;
      }
      case T_REFRESH_IO_CALIBRATION: {
        REGISTER_STMT_RESOLVER(RefreshIOCalibration);
        break;
      }
      case T_SWITCHOVER_TO_STANDBY:
      case T_SWITCHOVER_TO_PRIMARY:
      case T_ACTIVATE_STANDBY: {
        REGISTER_STMT_RESOLVER(SwitchRole);
        break;
      }
      case T_ALTER_SYSTEM_SET_PARAMETER: {
        REGISTER_STMT_RESOLVER(SetConfig);
        break;
      }
      case T_ALTER_SYSTEM_SETTP: {
        REGISTER_STMT_RESOLVER(SetTP);
        break;
      }
      case T_CLEAR_MERGE_ERROR: {
        REGISTER_STMT_RESOLVER(ClearMergeError);
        break;
      }
      case T_CREATE_DATABASE: {
        REGISTER_STMT_RESOLVER(CreateDatabase);
        break;
      }
      case T_USE_DATABASE: {
        REGISTER_STMT_RESOLVER(UseDatabase);
        break;
      }
      case T_ALTER_DATABASE: {
        REGISTER_STMT_RESOLVER(AlterDatabase);
        break;
      }
      case T_DROP_DATABASE: {
        REGISTER_STMT_RESOLVER(DropDatabase);
        break;
      }
      case T_RENAME_TABLE: {
        REGISTER_STMT_RESOLVER(RenameTable);
        break;
      }
      case T_TRUNCATE_TABLE: {
        REGISTER_STMT_RESOLVER(TruncateTable);
        break;
      }
      case T_RECYCLEBIN_RESTORE_TABLE: {
        REGISTER_STMT_RESOLVER(RecyclebinRestoreTable);
        break;
      }
      case T_RECYCLEBIN_RESTORE_DATABASE: {
        REGISTER_STMT_RESOLVER(RecyclebinRestoreDatabase);
        break;
      }
      case T_PURGE_TABLE: {
        REGISTER_STMT_RESOLVER(PurgeTable);
        break;
      }
      case T_PURGE_INDEX: {
        REGISTER_STMT_RESOLVER(PurgeIndex);
        break;
      }
      case T_PURGE_DATABASE: {
        REGISTER_STMT_RESOLVER(PurgeDatabase);
        break;
      }
      case T_PURGE_RECYCLEBIN: {
        REGISTER_STMT_RESOLVER(PurgeRecycleBin);
        break;
      }
      case T_OPTIMIZE_TABLE: {
        REGISTER_STMT_RESOLVER(OptimizeTable);
        break;
      }
      case T_PREPARE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Prepare);
        }
        break;
      }
      case T_EXECUTE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Execute);
        }
        break;
      }
      case T_DEALLOCATE: {
        if (params_.is_prepare_protocol_) {
          ret = OB_ERR_PARSE_SQL;
        } else {
          REGISTER_STMT_RESOLVER(Deallocate);
        }
        break;
      }
      case T_VARIABLE_SET: {
        REGISTER_STMT_RESOLVER(VariableSet);
        break;
      }
      case T_TRANSACTION: {
        REGISTER_STMT_RESOLVER(SetTransaction);
        break;
      }
      case T_SHOW_TABLES:
      case T_SHOW_DATABASES:
      case T_SHOW_VARIABLES:
      case T_SHOW_COLUMNS:
      case T_SHOW_SCHEMA:
      case T_SHOW_CREATE_DATABASE:
      case T_SHOW_CREATE_TABLE:
      case T_SHOW_CREATE_VIEW:
      case T_SHOW_CREATE_PROCEDURE:
      case T_SHOW_CREATE_FUNCTION:
      case T_SHOW_TABLE_STATUS:
      case T_SHOW_PARAMETERS:
      case T_SHOW_INDEXES:
      case T_SHOW_PROCESSLIST:
      case T_SHOW_WARNINGS:
      case T_SHOW_ERRORS:
      case T_SHOW_TRACE:
      case T_SHOW_ENGINES:
      case T_SHOW_PROFILE:
      case T_SHOW_ENGINE:
      case T_SHOW_OPEN_TABLES:
      case T_SHOW_PRIVILEGES:
      case T_SHOW_CHARSET:
      case T_SHOW_COLLATION:
      case T_SHOW_GRANTS:
      case T_SHOW_RECYCLEBIN:
      case T_SHOW_PROCEDURE_STATUS:
      case T_SHOW_FUNCTION_STATUS:
      case T_SHOW_TRIGGERS:
      case T_SHOW_STATUS:
      case T_SHOW_CREATE_TRIGGER:
      case T_SHOW_CHECK_TABLE:
      case T_SHOW_CREATE_USER: {
        REGISTER_STMT_RESOLVER(Show);
        break;
      }
      case T_CREATE_USER: {
        REGISTER_STMT_RESOLVER(CreateUser);
        break;
      }
      case T_DROP_USER: {
        REGISTER_STMT_RESOLVER(DropUser);
        break;
      }
      case T_RENAME_USER: {
        REGISTER_STMT_RESOLVER(RenameUser);
        break;
      }
      case T_SET_PASSWORD: {
        REGISTER_STMT_RESOLVER(SetPassword);
        break;
      }
      case T_LOCK_USER: {
        REGISTER_STMT_RESOLVER(LockUser);
        break;
      }
      case T_ALTER_USER_DEFAULT_ROLE:
      case T_SET_ROLE: {
        REGISTER_STMT_RESOLVER(AlterUserRole);
        break;
      }

      case T_GRANT_ROLE:
      case T_SYSTEM_GRANT:
      case T_GRANT: {
        REGISTER_STMT_RESOLVER(Grant);
        break;
      }
      case T_SYSTEM_REVOKE:
      case T_REVOKE:
      case T_REVOKE_ALL: {
        REGISTER_STMT_RESOLVER(Revoke);
        break;
      }
      case T_EXPLAIN: {
        REGISTER_STMT_RESOLVER(Explain);
        break;
      }
      case T_ALTER_SYSTEM_SET: {
        REGISTER_STMT_RESOLVER(AlterSystemSet);
        break;
      }
      case T_ALTER_SYSTEM_KILL: {
        REGISTER_STMT_RESOLVER(Kill);
        break;
      }
      case T_ALTER_SESSION_SET: {
        REGISTER_STMT_RESOLVER(AlterSessionSet);
        break;
      }
      case T_SET_NAMES:
        // fall through
      case T_SET_CHARSET: {
        REGISTER_STMT_RESOLVER(SetNames);
        break;
      }
      case T_KILL: {
        REGISTER_STMT_RESOLVER(Kill);
        break;
      }
      case T_EMPTY_QUERY: {
        REGISTER_STMT_RESOLVER(EmptyQuery);
        break;
      }
      case T_FLUSH_PRIVILEGES: {
        REGISTER_STMT_RESOLVER(EmptyQuery);
        break;
      }
      case T_LOCK_TABLE: {
        REGISTER_STMT_RESOLVER(LockTable);
        break;
      }
      case T_CREATE_OUTLINE: {
        REGISTER_STMT_RESOLVER(CreateOutline);
        break;
      }
      case T_ALTER_OUTLINE: {
        REGISTER_STMT_RESOLVER(AlterOutline);
        break;
      }
      case T_DROP_OUTLINE: {
        REGISTER_STMT_RESOLVER(DropOutline);
        break;
      }
      case T_SP_CREATE: {
        REGISTER_STMT_RESOLVER(CreateProcedure);
        break;
      }
      case T_SP_ALTER: {
        REGISTER_STMT_RESOLVER(AlterProcedure);
        break;
      }
      case T_SP_CALL_STMT: {
        REGISTER_STMT_RESOLVER(CallProcedure);
        break;
      }
      case T_SF_CREATE: {
        REGISTER_STMT_RESOLVER(CreateFunction);
        break;
      }
      case T_SF_ALTER: {
        REGISTER_STMT_RESOLVER(AlterFunction);
        break;
      }
      case T_SP_DROP: {
        REGISTER_STMT_RESOLVER(DropProcedure);
        break;
      }
      case T_SF_DROP: {
        REGISTER_STMT_RESOLVER(DropFunction);
        break;
      }
      case T_SP_ANONYMOUS_BLOCK: {
        REGISTER_STMT_RESOLVER(AnonymousBlock);
        break;
      }
      case T_PACKAGE_CREATE: {
        REGISTER_STMT_RESOLVER(CreatePackage);
        break;
      }
      case T_PACKAGE_CREATE_BODY: {
        REGISTER_STMT_RESOLVER(CreatePackageBody);
        break;
      }
      case T_PACKAGE_DROP: {
        REGISTER_STMT_RESOLVER(DropPackage);
        break;
      }
      case T_MYSQL_UPDATE_HISTOGRAM:
      case T_MYSQL_DROP_HISTOGRAM:
      case T_MYSQL_ANALYZE: {
        REGISTER_STMT_RESOLVER(AnalyzeStmt);
        break;
      }
      case T_LOAD_DATA_URL:
      case T_LOAD_DATA: {
        REGISTER_STMT_RESOLVER(LoadData);
        break;
      }
      case T_SET_TABLE_COMMENT:
      case T_SET_COLUMN_COMMENT: {
        REGISTER_STMT_RESOLVER(SetComment);
        break;
      }
      case T_CREATE_ROLE: {
        REGISTER_STMT_RESOLVER(CreateRole);
        break;
      }
      case T_DROP_ROLE: {
        REGISTER_STMT_RESOLVER(DropRole);
        break;
      }
      case T_ALTER_ROLE: {
        REGISTER_STMT_RESOLVER(AlterRole);
        break;
      }
      case T_CREATE_SAVEPOINT:
      case T_ROLLBACK_SAVEPOINT:
      case T_RELEASE_SAVEPOINT: {
        REGISTER_STMT_RESOLVER(SavePoint);
        break;
      }
      case T_TG_CREATE: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_TG_DROP: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_TG_ALTER: {
        REGISTER_STMT_RESOLVER(Trigger);
        break;
      }
      case T_DIAGNOSTICS: {
        REGISTER_STMT_RESOLVER(GetDiagnostics);
        break;
      }
      case T_ALTER_SYSTEM_RESET_PARAMETER: {
        REGISTER_STMT_RESOLVER(ResetConfig);
        break;
      }
      case T_ALTER_SYSTEM_RESET: {
        REGISTER_STMT_RESOLVER(AlterSystemReset);
        break;
      }
      default: {
        ret = OB_NOT_SUPPORTED;
        const char *type_name = get_type_name(real_parse_tree->type_);
        LOG_USER_ERROR(OB_NOT_SUPPORTED, "statement type");
        break;
      }
    }  // end switch
    if (OB_SUCC(ret) && stmt->is_dml_write_stmt()) {
      // todo yanli:check leader-follower database
    }
    
    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !params_.session_info_->is_varparams_sql_prepare()) {
      ObDMLStmt *dml_stmt = static_cast<ObDMLStmt*>(stmt);
      ObRawExprWrapEnumSet enum_set_wrapper(*params_.expr_factory_, params_.session_info_);
      if (OB_FAIL(enum_set_wrapper.wrap_enum_set(*dml_stmt))) {
      }
    }

    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !stmt->is_explain_stmt()) {
      if (OB_FAIL(params_.query_ctx_->query_hint_.init_query_hint(params_.allocator_,
                                                                  params_.session_info_,
                                                                  static_cast<ObDMLStmt*>(stmt)))) {
      } else if (OB_FAIL(params_.query_ctx_->query_hint_.check_and_set_params_from_hint(params_,
                                                         *static_cast<ObDMLStmt*>(stmt)))) {
      }
    }

    if (OB_SUCC(ret) && stmt->is_dml_stmt() && !stmt->is_explain_stmt()) {
      bool is_contain_inner_table = false;
      bool is_contain_select_for_update = false;
      ObDMLStmt *dml_stmt = static_cast<ObDMLStmt*>(stmt);
      if (OB_FAIL(dml_stmt->check_if_contain_inner_table(is_contain_inner_table))) {
      } else if (OB_FAIL(dml_stmt->check_if_contain_select_for_update(
                  is_contain_select_for_update))) {
      } else {
        params_.query_ctx_->is_contain_inner_table_ = is_contain_inner_table;
        params_.query_ctx_->is_contain_select_for_update_ = is_contain_select_for_update;
        params_.query_ctx_->has_dml_write_stmt_ = dml_stmt->is_dml_write_stmt();
      }

    }
    if (OB_SUCC(ret)) {
      stmt::StmtType stmt_type = stmt->get_stmt_type();
      if (ObStmt::is_ddl_stmt(stmt_type, stmt->has_global_variable()) || ObStmt::is_dcl_stmt(stmt_type)) {
        ObDDLStmt *ddl_stmt = static_cast<ObDDLStmt*>(stmt);
        obcall::ObDDLArg &ddl_arg = ddl_stmt->get_ddl_arg();

        if (OB_ISNULL(params_.query_ctx_)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          ddl_arg.ddl_stmt_str_ = params_.query_ctx_->get_sql_stmt();
        }
      }
    }
  }  // end if
  return ret;
}

}  // namespace sql
}  // namespace oceanbase

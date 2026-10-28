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

#define USING_LOG_PREFIX SQL_ENG

#include "ob_table_modify_op.h"
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <vector>
#include "lib/string/ob_sql_string.h"
#include "share/ob_server_struct.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/ob_autoincrement_service.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/schema/ob_table_schema.h"
#include "share/schema/ob_constraint.h"
#include "query/session/ob_inner_sql_connection_access.h"
#include "sql/engine/ob_physical_plan.h"
#include "sql/engine/dml/ob_dml_service.h"

namespace oceanbase
{
using namespace common;
using namespace common::sqlclient;
using namespace storage;
using namespace share;
using namespace share::schema;
using namespace observer;
namespace sql
{
int ForeignKeyHandle::do_handle(ObTableModifyOp &op,
                                const ObDMLBaseCtDef &dml_ctdef,
                                ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  if (op.need_foreign_key_checks()) {
    const ObExprPtrIArray &old_row = dml_ctdef.old_row_;
    const ObExprPtrIArray &new_row = dml_ctdef.new_row_;
    const bool save_in_ignore_cascading = op.get_exec_ctx().get_das_ctx().in_ignore_cascading_;
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = true;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = dml_ctdef.das_base_ctdef_.is_ignore_;
    if (OB_FAIL(op.check_stack())) {
    }
    for (int i = 0; OB_SUCC(ret) && i < dml_ctdef.fk_args_.count(); i++) {
      const ObForeignKeyArg &fk_arg = dml_ctdef.fk_args_.at(i);
      ObForeignKeyChecker *fk_checker = dml_rtdef.fk_checker_array_.at(i);
      if (OB_SUCC(ret) && !new_row.empty()) {
        if (ACTION_CHECK_EXIST == fk_arg.ref_action_) {
          // insert or update.
          bool is_foreign_key_cascade = ObSQLUtils::is_fk_nested_sql(&op.get_exec_ctx());
          bool in_ignore_cascading = false;
          ObExecContext* parent_ctx = op.get_exec_ctx().get_parent_ctx();
          if (OB_NOT_NULL(parent_ctx)) {
            in_ignore_cascading = parent_ctx->get_das_ctx().in_ignore_cascading_;
          }

          if (is_foreign_key_cascade && !in_ignore_cascading) {
            // nested update can not check parent row.
          } else if (OB_FAIL(check_exist(op, fk_arg, new_row, fk_checker, false, fk_arg.use_das_scan_))) {
          }
        }
      }
      if (OB_SUCC(ret) && !old_row.empty()) {
        if (ACTION_RESTRICT == fk_arg.ref_action_ || ACTION_NO_ACTION == fk_arg.ref_action_) {
          // update or delete.
          bool has_changed = false;
          if (OB_FAIL(value_changed(op, fk_arg.columns_, old_row, new_row, has_changed))) {
          } else if (!has_changed) {
            // nothing.
          } else if (OB_FAIL(check_exist(op, fk_arg, old_row, fk_checker, true, fk_arg.use_das_scan_))) {
          }
        } else if (ACTION_CASCADE == fk_arg.ref_action_) {
          // update or delete.
          bool is_self_ref = false;
          if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), old_row, fk_arg, is_self_ref))) {
          } else if (new_row.empty() && is_self_ref && op.is_fk_nested_session()) {
            // delete self refercnced row should not cascade delete.
          } else if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(cascade(op, fk_arg, old_row, new_row))) {
          } else if (!new_row.empty() && is_self_ref) {
            // we got here only when:
            //  1. handling update operator and
            //  2. foreign key constraint is self reference
            // need to change %new_row and %old_row
            //   (see 
            // xxx_row_res_info helps to restore row. restore row before get_next_row()
            //op.fk_self_ref_row_res_infos_.reset();
            for (int64_t i = 0; OB_SUCC(ret) && i < fk_arg.columns_.count(); i++) {
              const int32_t val_idx = fk_arg.columns_.at(i).idx_;
              const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
              if (OB_SUCC(ret)) {
                const ObDatum &updated_value = new_row.at(val_idx)->
                                                    locate_expr_datum(op.get_eval_ctx());
                ObDatum &new_row_name_col = new_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());
                ObDatum &old_row_name_col = old_row.at(name_idx)->
                                                  locate_expr_datum(op.get_eval_ctx());

                OX(new_row_name_col = updated_value);
                OX(old_row_name_col = updated_value);
              }
            }
          }
        } else if (ACTION_SET_NULL == fk_arg.ref_action_) {
          if (OB_FAIL(check_exist_inner_sql(op, fk_arg, old_row, true, true))) {
          } else if (OB_FAIL(set_null(op, fk_arg, old_row))) {
          }
        }
      } // if (old_row.is_valid())
    } // for
    //fk cascading end
    op.get_exec_ctx().get_das_ctx().is_fk_cascading_ = false;
    op.get_exec_ctx().get_das_ctx().in_ignore_cascading_ = save_in_ignore_cascading;
  } else {
  }
  return ret;
}

int ForeignKeyHandle::value_changed(ObTableModifyOp &op,
                                    const ObIArray<ObForeignKeyColumn> &columns,
                                    const ObExprPtrIArray &old_row,
                                    const ObExprPtrIArray &new_row,
                                    bool &has_changed)
{
  int ret = OB_SUCCESS;
  has_changed = false;
  if (!old_row.empty() && !new_row.empty()) {
    ObDatum *old_row_col = NULL;
    ObDatum *new_row_col = NULL;
    for (int64_t i = 0; OB_SUCC(ret) && !has_changed && i < columns.count(); ++i) {
      int64_t col_idx = columns.at(i).idx_;
      CK(col_idx < old_row.count());
      CK(col_idx < new_row.count());
      CK(OB_NOT_NULL(old_row.at(col_idx)));
      CK(OB_NOT_NULL(new_row.at(col_idx)));
      OZ(old_row.at(col_idx)->eval(op.get_eval_ctx(), old_row_col));
      OZ(new_row.at(col_idx)->eval(op.get_eval_ctx(), new_row_col));
      OX(has_changed = (false == ObDatum::binary_equal(*old_row_col, *new_row_col)));
    }
  } else {
    has_changed = true;
  }
  return ret;
}

int ForeignKeyHandle::check_exist(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker, bool expect_zero, bool use_das_scan)
{
  int ret = OB_SUCCESS;
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  // insertup and merge_into currently have a core problem when checking foreign keys through das_scan_task.
  // Because both insertup and merge_into have both INSERT and UPDATE semantics,
  // the data may come from the new_row of INSERT or the new_row of UPDATE during the operation.
  // Earlier versions did not consider this problem, so there is a possibility of core
  bool is_insertup = false;
  ObPhyOperatorType op_type = modify_op.get_spec().get_type();
  if (op_type == PHY_INSERT_ON_DUP) {
    is_insertup = true;
  }
  if (use_das_scan && !is_insertup) {
    ret = check_exist_scan_task(modify_op, fk_arg, row, fk_checker);
  } else {
    if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, true))) {
    } else if (OB_FAIL(check_exist_inner_sql(modify_op, fk_arg, row, expect_zero, false))) {
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_scan_task(ObTableModifyOp &modify_op, const ObForeignKeyArg &fk_arg,
                         const ObExprPtrIArray &row, ObForeignKeyChecker *fk_checker)
{
  int ret = OB_SUCCESS;
  bool has_result = false;
  if (OB_ISNULL(fk_checker)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!fk_arg.use_das_scan_) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(fk_checker->do_fk_check_single_row(fk_arg.columns_, row, has_result))) {
  } else {
    if (!has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    } else {
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

int ForeignKeyHandle::check_exist_inner_sql(ObTableModifyOp &op,
                                            const ObForeignKeyArg &fk_arg,
                                            const ObExprPtrIArray &row,
                                            bool expect_zero,
                                            bool iter_uncommitted_row)
{
  int ret = OB_SUCCESS;
  const char *select_fmt = "select /*+ no_parallel */ 1 from `%.*s`.`%.*s` where %.*s for update";
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip check exist if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, select_fmt,
                                     database_name.length(), database_name.ptr(),
                                     table_name.length(), table_name.ptr(),
                                     static_cast<int>(where_pos), where_buf))) {
  } else {
    stmt_buf[stmt_pos++] = 0;
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (iter_uncommitted_row) {
      op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = true;
    }
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
      } else {
        // must call end_nested_session() if begin_nested_session() success.
        bool is_zero = false;
        if (OB_FAIL(op.execute_read(stmt_buf, res))) {
        } else {
          // must call res.get_result()->close() if execute_read() success.
          if (OB_ISNULL(res.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(res.get_result()->next())) {
            if (OB_ITER_END == ret) {
              is_zero = true;
              ret = OB_SUCCESS;
            } else {
            }
          }
          if (OB_SUCC(ret)) {
            if (is_zero != expect_zero) {
              // If it is a self-referencing foreign key new row references new row
              bool is_self_ref = false;
              // The FOR UPDATE check query does not use aggregate functions, so when is_zero is false
              // judge whether only one row is affected by whether the second record can be obtained.
              bool is_affect_only_one = false;
              if (!is_zero && OB_FAIL(res.get_result()->next())) {
                if (OB_ITER_END == ret) {
                  is_affect_only_one = true;
                  ret = OB_SUCCESS;
                } else {
                }
              }
              /**
               *  expect_zero is false when fk_arg.ref_action_ is equal to
               *  ACTION_CHECK_EXIST, if the is_zero != expect_zero condition is
               *  true, then is_zero is true, need to exclude the case of self reference.
               *  other cases return OB_ERR_NO_REFERENCED_ROW.
               *
               *  expect_zero is true when fk_arg.ref_action_ is equal to
               *  ACTION_RESTRICT or ACTION_NO_ACTION, if is_zero != expect_zero condition
               *  is true, then is_zero is false, need to exclude the case of self reference and
               *  only affect one row. other cases return OB_ERR_ROW_IS_REFERENCED.
               */
              if (!iter_uncommitted_row) {
                if (OB_FAIL(is_self_ref_row(op.get_eval_ctx(), row, fk_arg, is_self_ref))) {
                } else if (is_zero && !is_self_ref) {
                  ret = OB_ERR_NO_REFERENCED_ROW;
                } else if (!is_zero) {
                  ret = OB_ERR_ROW_IS_REFERENCED;
                }
              }
            }
          }
          int close_ret = res.get_result()->close();
          if (OB_SUCCESS != close_ret) {
            LOG_WARN("failed to close", K(close_ret));
            if (OB_SUCCESS == ret) {
              ret = close_ret;
            }
          }
        }
        int end_ret = op.end_nested_session();
        if (OB_SUCCESS != end_ret) {
          LOG_WARN("failed to end nested session", K(end_ret));
          if (OB_SUCCESS == ret) {
            ret = end_ret;
          }
        }
      }
    }
  }
  op.get_exec_ctx().get_das_ctx().iter_uncommitted_row_ = false;

  return ret;
}

int ForeignKeyHandle::cascade(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row,
                              const ObExprPtrIArray &new_row)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (OB_FAIL(op.get_exec_ctx().check_status())) {
  } else if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    if (!new_row.empty()) {
      // update.
      const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
      char *set_buf = nullptr;
      int64_t set_len = 0;
      int64_t set_pos = 0;
      if (OB_FAIL(gen_set(op.get_eval_ctx(), set_buf, set_len, set_pos,
                          fk_arg.columns_, new_row, alloc, op.get_obj_print_params()))) {
      } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                         database_name.length(), database_name.ptr(),
                                         table_name.length(), table_name.ptr(),
                                         static_cast<int>(set_pos), set_buf,
                                         static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    } else {
      // delete.
      const char *delete_fmt = "delete from `%.*s`.`%.*s` where %.*s";
      if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, delete_fmt,
                                  database_name.length(), database_name.ptr(),
                                  table_name.length(), table_name.ptr(),
                                  static_cast<int>(where_pos), where_buf))) {
      } else {
        stmt_buf[stmt_pos++] = 0;
      }
    }
  }
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }
  return ret;
}

int ForeignKeyHandle::set_null(ObTableModifyOp &op,
                              const ObForeignKeyArg &fk_arg,
                              const ObExprPtrIArray &old_row)
{
  int ret = OB_SUCCESS;
  
  ObArenaAllocator alloc(ObModIds::OB_MODULE_PAGE_ALLOCATOR,
                          OB_MALLOC_NORMAL_BLOCK_SIZE);
  char *stmt_buf = nullptr;
  int64_t stmt_len = 0;
  char *where_buf = nullptr;
  int64_t where_len = 0;
  int64_t stmt_pos = 0;
  int64_t where_pos = 0;
  const ObString &database_name = fk_arg.database_name_;
  const ObString &table_name = fk_arg.table_name_;
  if (old_row.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(gen_where(op.get_eval_ctx(), where_buf, where_len, where_pos,
                               fk_arg.columns_, old_row, alloc, op.get_obj_print_params()))) {
    if (OB_LIKELY(OB_ERR_NULL_VALUE == ret)) {
      // skip cascade if any column in foreign key is NULL.
      ret = OB_SUCCESS;
      stmt_pos = 0;
    } else {
    }
  } else {
    const char *update_fmt = "update `%.*s`.`%.*s` set %.*s where %.*s";
    char *set_buf = nullptr;
    int64_t set_len = 0;
    int64_t set_pos = 0;
    if (OB_FAIL(gen_column_null_value(op.get_eval_ctx(), set_buf, set_len, set_pos, fk_arg.columns_, alloc, op.get_obj_print_params()))) {
    } else if (OB_FAIL(databuff_printf(stmt_buf, stmt_len, stmt_pos, alloc, update_fmt,
                                      database_name.length(), database_name.ptr(),
                                        table_name.length(), table_name.ptr(),
                                        static_cast<int>(set_pos), set_buf,
                                        static_cast<int>(where_pos), where_buf))) {
    } else {
      stmt_buf[stmt_pos++] = 0;
    }
  }
  
  if (OB_SUCC(ret) && stmt_pos > 0) {
    if (OB_FAIL(op.begin_nested_session(fk_arg.is_self_ref_))) {
    } else {
      if (OB_FAIL(op.execute_write(stmt_buf))) {
      }
      int end_ret = op.end_nested_session();
      if (OB_SUCCESS != end_ret) {
        LOG_WARN("failed to end nested session", K(end_ret));
        if (OB_SUCCESS == ret) {
          ret = end_ret;
        }
      }
    }
  }

  return ret;
}

int ForeignKeyHandle::gen_set(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                              const ObIArray<ObForeignKeyColumn> &columns,
                              const ObExprPtrIArray &row,
                              ObIAllocator &alloc,
                              const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, ", ", alloc, print_params, false);
}

int ForeignKeyHandle::gen_where(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                const ObIArray<ObForeignKeyColumn> &columns,
                                const ObExprPtrIArray &row,
                                ObIAllocator &alloc,
                                const ObObjPrintParams &print_params)
{
  return gen_column_value(eval_ctx, buf, len, pos, columns, row, " and ", alloc, print_params, true);
}

int ForeignKeyHandle::gen_column_value(ObEvalCtx &eval_ctx, char *&buf, int64_t &len, int64_t &pos,
                                       const ObIArray<ObForeignKeyColumn> &columns,
                                       const ObExprPtrIArray &row,
                                       const char *delimiter,
                                       ObIAllocator &alloc,
                                       const ObObjPrintParams &print_params,
                                       bool forbid_null)
{
  const char *column_fmt = "`%.*s` = ";
  int ret = OB_SUCCESS;
  ObDatum *col_datum = NULL;
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    OZ(row.at(columns.at(i).idx_)->eval(eval_ctx, col_datum));
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      const ObObjMeta &col_obj_meta = row.at(columns.at(i).idx_)->obj_meta_;
      ObObj col_obj;
      if (forbid_null && col_datum->is_null()) {
        ret = OB_ERR_NULL_VALUE;
        // NO LOG.
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_datum->to_obj(col_obj, col_obj_meta))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int  ForeignKeyHandle::gen_column_null_value(ObEvalCtx &ctx, char *&buf, int64_t &len, int64_t &pos,
                              const common::ObIArray<ObForeignKeyColumn> &columns,
                              common::ObIAllocator &alloc,
                              const common::ObObjPrintParams &print_params)
{
  int ret = OB_SUCCESS;
  const char *column_fmt = "`%.*s` = ";
  const char *delimiter = ", ";
  if (OB_ISNULL(delimiter) || OB_ISNULL(print_params.tz_info_)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (columns.count() <= 0) {
    ret= OB_ERR_UNEXPECTED;
    LOG_ERROR("columns count of fk is zero or less than zero", K(ret), K(columns.count()), K(columns));
  }
  ObObj col_obj;
  col_obj.set_null();
  for (int64_t i = 0; OB_SUCC(ret) && i < columns.count(); i++) {
    if (OB_SUCC(ret)) {
      const ObString &col_name = columns.at(i).name_;
      if (OB_FAIL(databuff_printf(buf, len, pos, alloc, column_fmt,
                                        col_name.length(), col_name.ptr()))) {
      } else if (OB_FAIL(col_obj.print_sql_literal(buf, len, pos, alloc, print_params))) {
      } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s", delimiter))) {
      }
    }
  }
  if (OB_SUCC(ret)) {
    pos -= STRLEN(delimiter);
    buf[pos++] = 0;
  }
  return ret;
}

int ForeignKeyHandle::is_self_ref_row(ObEvalCtx &eval_ctx,
                                      const ObExprPtrIArray &row,
                                      const ObForeignKeyArg &fk_arg,
                                      bool &is_self_ref)
{
  int ret = OB_SUCCESS;
  const common::ObDatumAccessContext *access_ctx = nullptr;
  is_self_ref = fk_arg.is_self_ref_;
  ObDatum *name_col = NULL;
  ObDatum *val_col = NULL;
  if (OB_FAIL(eval_ctx.get_datum_access_ctx(access_ctx))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && is_self_ref && i < fk_arg.columns_.count(); i++) {
    const int32_t name_idx = fk_arg.columns_.at(i).name_idx_;
    const int32_t val_idx = fk_arg.columns_.at(i).idx_;
    ObExprCmpFuncType cmp_func = row.at(name_idx)->basic_funcs_->null_first_cmp_;
    // TODO qubin.qb: uncomment below block revert the defensive check
    //OZ((cmp_func = (row.at(name_idx)->basic_funcs_->null_first_cmp_))
    //   != row.at(val_idx)->basic_funcs_->null_first_cmp_);
    OZ(row.at(name_idx)->eval(eval_ctx, name_col));
    OZ(row.at(val_idx)->eval(eval_ctx, val_col));
    int cmp_ret = 0;
    if (OB_FAIL(cmp_func(*name_col, *val_col, cmp_ret, access_ctx))) {
    } else {
      is_self_ref = (0 == cmp_ret);
    }
  }
  return ret;
}

// make MY_SPEC macro available.
OB_INLINE static const ObTableModifySpec &get_my_spec(const ObTableModifyOp &op)
{
  return op.get_spec();
}

ObTableModifySpec::ObTableModifySpec(common::ObIAllocator &alloc,
                                     const ObPhyOperatorType type)
  : ObOpSpec(alloc, type),
    expr_frame_info_(NULL),
    ab_stmt_id_(nullptr),
    flags_(0),
    das_dop_(0)
{
}

OB_DEF_SERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_SER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ENCODE(flags_);
  OB_UNIS_ENCODE(ab_stmt_id_);
  OB_UNIS_ENCODE(das_dop_);
  return ret;
}

OB_DEF_DESERIALIZE(ObTableModifySpec)
{
  int ret = OB_SUCCESS;
  BASE_DESER((ObTableModifySpec, ObOpSpec));
  OB_UNIS_DECODE(flags_);
  OB_UNIS_DECODE(ab_stmt_id_);
  OB_UNIS_DECODE(das_dop_);
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObTableModifySpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObTableModifySpec, ObOpSpec));
  OB_UNIS_ADD_LEN(flags_);
  OB_UNIS_ADD_LEN(ab_stmt_id_);
  OB_UNIS_ADD_LEN(das_dop_);
  return len;
}

OB_SERIALIZE_MEMBER(ObTableModifyOpInput);

ObTableModifyOp::ObTableModifyOp(ObExecContext &ctx,
                                 const ObOpSpec &spec,
                                 ObOpInput *input)
  : ObOperator(ctx, spec, input),
    sql_proxy_(NULL),
    inner_conn_guard_(),
    inner_conn_(NULL),
    saved_conn_(),
    need_foreign_key_check_(false),
    need_close_conn_(false),
    iter_end_(false),
    dml_rtctx_(eval_ctx_, ctx, *this),
    execute_single_row_(false),
    dml_modify_rows_(ctx.get_allocator()),
    last_store_row_(),
    saved_session_(NULL)
{
  obj_print_params_ = CREATE_OBJ_PRINT_PARAM(ctx_.get_my_session());
  obj_print_params_.need_cast_expr_ = true;
  // bugfix:
  // in NO_BACKSLASH_ESCAPES, obj_print_sql<ObVarcharType> won't escape.
  // We use skip_escape_ to indicate this case. It will finally be passed to ObHexEscapeSqlStr.
  GET_SQL_MODE_BIT(IS_NO_BACKSLASH_ESCAPES, ctx_.get_my_session()->get_sql_mode(), obj_print_params_.skip_escape_);
}

int ObTableModifyOp::inner_open()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *my_session = GET_MY_SESSION(ctx_);
  if (OB_FAIL(init_foreign_key_operation())) {
  } else if (MY_SPEC.plan_->has_nested_sql() && OB_FAIL(open_inner_conn())) {
  } else if (OB_FAIL(calc_single_table_loc())) {
  } else if (OB_FAIL(check_need_exec_single_row())) {
  } else {
    init_das_dml_ctx();
  }
  return ret;
}

void ObTableModifyOp::clear_dml_evaluated_flag(ObExpr *clear_expr)
{
  if (clear_expr->is_batch_result()) {
    clear_expr->get_evaluated_flags(eval_ctx_).unset(eval_ctx_.get_batch_idx());
  } else {
    clear_expr->get_eval_info(eval_ctx_).clear_evaluated_flag();
  }
  if (0 < clear_expr->parent_cnt_) {
    clear_dml_evaluated_flag(clear_expr->parent_cnt_, clear_expr->parents_);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag(int64_t parent_cnt, ObExpr **parent_exprs)
{
  for (int64_t i = 0; i < parent_cnt; ++i) {
    clear_dml_evaluated_flag(parent_exprs[i]);
  }
}

void ObTableModifyOp::clear_dml_evaluated_flag()
{
  // Clear the parent expr because the assignment expr may be changed
  for (int64_t i = 0; i < trigger_clear_exprs_.count(); ++i) {
    ObExpr *expr = trigger_clear_exprs_.at(i);
    clear_dml_evaluated_flag(expr);
  }
}

ObDasParallelType ObTableModifyOp::check_das_parallel_type()
{
  ObDasParallelType type = DAS_SERIALIZATION;
  // Local DAS modification runs serially.
  return type;
}

OB_INLINE int ObTableModifyOp::init_das_dml_ctx()
{
  ObSQLSessionInfo *session = GET_MY_SESSION(ctx_);
  const char *label = nullptr;
  switch(MY_SPEC.type_) {
  case PHY_INSERT:
    label = "InsDASCtx";
    break;
  case PHY_UPDATE:
    label = "UpdDASCtx";
    break;
  case PHY_DELETE:
    label = "DelDASCtx";
    break;
  case PHY_LOCK:
    label = "LockDASCtx";
    break;
  default:
    label = "DmlDASCtx";
    break;
  }
  ObMemAttr memattr(label, ObCtxIds::EXECUTE_CTX_ID);
  dml_rtctx_.das_ref_.set_expr_frame_info(nullptr != MY_SPEC.expr_frame_info_
                                        ? MY_SPEC.expr_frame_info_
                                        : &MY_SPEC.plan_->get_expr_frame_info());
  dml_rtctx_.das_ref_.set_mem_attr(memattr);
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_parallel_type(check_das_parallel_type());
  dml_rtctx_.das_ref_.get_das_parallel_ctx().set_das_dop(ctx_.get_das_ctx().get_real_das_dop());
  if (check_das_parallel_type() != DAS_SERIALIZATION) {
    LOG_TRACE("this sql use das parallel submit", K(check_das_parallel_type()));
  }
  return OB_SUCCESS;
}

int ObTableModifyOp::replace_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->replace_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::prepare_implict_cursor(int64_t affected_rows,
                                            int64_t found_rows,
                                            int64_t matched_rows,
                                            int64_t duplicated_rows,
                                            ObImplicitCursorInfo &implicit_cursor)
{
  int ret = OB_SUCCESS;
  ObDatum *stmt_id_datum = nullptr;
  if (OB_FAIL(MY_SPEC.ab_stmt_id_->eval(eval_ctx_, stmt_id_datum))) {
  } else {
    ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
    int64_t stmt_id = stmt_id_datum->get_int();
    implicit_cursor.stmt_id_ = stmt_id;
    implicit_cursor.found_rows_ += found_rows;
    implicit_cursor.matched_rows_ += matched_rows;
    implicit_cursor.affected_rows_ += affected_rows;
    implicit_cursor.duplicated_rows_ += duplicated_rows;
    implicit_cursor.last_insert_id_ = plan_ctx->get_autoinc_col_value();
  }
  return ret;
}

int ObTableModifyOp::merge_implict_cursor(int64_t affected_rows,
                                          int64_t found_rows,
                                          int64_t matched_rows,
                                          int64_t duplicated_rows)
{
  int ret = OB_SUCCESS;
  bool is_ins_val_opt = ctx_.get_sql_ctx()->is_do_insert_batch_opt();
  ObPhysicalPlanCtx *plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (MY_SPEC.ab_stmt_id_ != nullptr && !is_ins_val_opt) {
    ObDatum *stmt_id_datum = nullptr;
    ObImplicitCursorInfo implicit_cursor;
    if (OB_FAIL(prepare_implict_cursor(affected_rows,
                                       found_rows,
                                       matched_rows,
                                       duplicated_rows,
                                       implicit_cursor))) {
    } else if (OB_FAIL(plan_ctx->merge_implicit_cursor_info(implicit_cursor))) {
    }
  }
  return ret;
}

int ObTableModifyOp::inner_switch_iterator()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_switch_iterator())) {
  }

  return ret;
}

int ObTableModifyOp::inner_close()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *plan_ctx = ctx_.get_physical_plan_ctx();
  // release cache_handle for auto-increment
  share::ObAutoincrementService &auto_service = share::ObAutoincrementService::get_instance();
  ObIArray<share::AutoincParam> &autoinc_params = plan_ctx->get_autoinc_params();
  for (int64_t i = 0; i < autoinc_params.count(); ++i) {
    if (NULL != autoinc_params.at(i).cache_handle_) {
      auto_service.release_handle(autoinc_params.at(i).cache_handle_);
    }
  }
  close_inner_conn();
  if (dml_rtctx_.das_ref_.has_task()) {
    if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else {
      dml_rtctx_.das_ref_.reset();
    }
  }
  dml_modify_rows_.clear();
  return ret;
}

OB_INLINE int ObTableModifyOp::init_foreign_key_operation()
{
  int ret = OB_SUCCESS;
  ObPhysicalPlanCtx *phy_plan_ctx = GET_PHY_PLAN_CTX(ctx_);
  if (OB_ISNULL(phy_plan_ctx)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!phy_plan_ctx->need_foreign_key_checks()) {
    // set fk check even if ret != OB_SUCCESS. see ObTableModifyOp::inner_close()
    need_foreign_key_check_ = false;
  } else {
    need_foreign_key_check_ = MY_SPEC.need_foreign_key_check_;
  }
  return ret;
}

int ObTableModifyOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    iter_end_ = false;
    if (dml_rtctx_.das_ref_.has_task()) {
      if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
      } else {
        dml_rtctx_.reuse();
      }
    }
  }
  if (OB_SUCC(ret)) {
    dml_modify_rows_.clear();
    if (OB_FAIL(calc_single_table_loc())) {
    }
  }
  return ret;
}

int ObTableModifyOp::check_need_exec_single_row() {
  int ret = OB_SUCCESS;
  if (MY_SPEC.is_returning_ && (need_foreign_key_checks() || MY_SPEC.need_trigger_fire_)) {
    execute_single_row_ = true;
  }
  return ret;
}

int ObTableModifyOp::get_gi_task()
{
  int ret = OB_SUCCESS;
  ObGranuleTaskInfo gi_task_info;
  GIPrepareTaskMap *gi_prepare_map = nullptr;
  ObDASCtx &das_ctx = ctx_.get_das_ctx();
  ObTableID table_loc_id = OB_INVALID_ID;
  ObTableID ref_table_id = OB_INVALID_ID;
  ObTabletID tablet_id;
  if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
  } else if (OB_FAIL(ctx_.get_gi_task_map(gi_prepare_map))) {
  } else if (OB_FAIL(gi_prepare_map->get_refactored(MY_SPEC.id_, gi_task_info))) {
    if (ret != OB_HASH_NOT_EXIST) {
    } else {
      LOG_DEBUG("no prepared task info, set table modify to end",
        K(MY_SPEC.get_id()), K(this), K(lbt()));
      // Current DML operator cannot obtain task from GI, indicating current DML operator iter end
      iter_end_ = true;
      ret = OB_SUCCESS;
    }
  } else if (OB_ISNULL(get_input()->table_loc_) &&
      OB_ISNULL(get_input()->table_loc_ = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    get_input()->tablet_loc_ = gi_task_info.tablet_loc_;
    LOG_DEBUG("DML operator consume a task", K(ret),
              K(gi_task_info), KPC(get_input()), K(MY_SPEC.id_), K(get_input()));
  }
  return ret;
}

int ObTableModifyOp::calc_single_table_loc()
{
  int ret = OB_SUCCESS;
  if (!MY_SPEC.use_dist_das_) {
    ObTableID table_loc_id = OB_INVALID_ID;
    ObTableID ref_table_id = OB_INVALID_ID;
    ObDASCtx &das_ctx = ctx_.get_das_ctx();
    ObDASTableLoc *table_loc = nullptr;
    if (OB_UNLIKELY(MY_SPEC.gi_above_)) {
      if (OB_FAIL(get_gi_task())) {
      }
    } else if (OB_FAIL(MY_SPEC.get_single_table_loc_id(table_loc_id, ref_table_id))) {
    } else if (OB_ISNULL(table_loc = das_ctx.get_table_loc_by_id(table_loc_id, ref_table_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      get_input()->table_loc_ = table_loc;
      get_input()->tablet_loc_ = table_loc->get_first_tablet_loc();
    }
  }
  return ret;
}

int ObTableModifyOp::open_inner_conn()
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session = NULL;
  if (OB_ISNULL(sql_proxy_ = ctx_.get_sql_proxy())) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(session = ctx_.get_my_session())) {
    ret = OB_NOT_INIT;
  } else if (NULL != session->get_inner_conn()) {
    // do nothing.
  } else if (OB_FAIL(
                 query::ObInnerSQLConnectionAccess::
                     create_connection_with_external_session(
                         session, inner_conn_guard_))) {
  } else if (OB_ISNULL(inner_conn_guard_.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    /**
     * session is the only data struct which can pass through multi layer nested sql,
     * so we put inner conn in session to share it within multi layer nested sql.
     */
    session->set_inner_conn(inner_conn_guard_.get_ptr());
    need_close_conn_ = true;
  }
  if (OB_SUCC(ret)) {
    inner_conn_ = as_inner_sql_connection(session->get_inner_conn());
    if (OB_ISNULL(inner_conn_)) {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int ObTableModifyOp::close_inner_conn()
{
  /**
   * we can call it even if open_inner_conn() failed, because only the one who call
   * open_inner_conn() succeed first will do close job by "if (need_close_conn_)".
   */
  int ret = OB_SUCCESS;
  if (need_close_conn_) {
    ObSQLSessionInfo *session = ctx_.get_my_session();
    if (OB_ISNULL(sql_proxy_) || OB_ISNULL(session)) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      OX(session->set_inner_conn(NULL));
      OX(inner_conn_guard_.reset());
    }
    need_close_conn_ = false;
  }
  sql_proxy_ = NULL;
  inner_conn_guard_.reset();
  inner_conn_ = NULL;
  return ret;
}

int ObTableModifyOp::begin_nested_session(bool skip_cur_stmt_tables)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->begin_nested_session(get_saved_session(), saved_conn_,
                                                       skip_cur_stmt_tables))) {
  }
  return ret;
}

int ObTableModifyOp::end_nested_session()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(inner_conn_->end_nested_session(get_saved_session(), saved_conn_))) {
  }
  return ret;
}
int ObTableModifyOp::execute_write(const char *sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  if (OB_ISNULL(inner_conn_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_write(sql, affected_rows))) {
  }
  return ret;
}

int ObTableModifyOp::execute_read(const char *sql, ObMySQLProxy::MySQLResult &res)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(inner_conn_) || OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(sql)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(inner_conn_->execute_read(sql, res))) {
  }
 return ret;
}

int ObTableModifyOp::check_stack()
{
  int ret = OB_SUCCESS;
  const int max_stack_deep = 16;
  bool is_stack_overflow = false;
  ObSQLSessionInfo *session = ctx_.get_my_session();
  if (OB_ISNULL(session)) {
    ret = OB_ERR_NULL_VALUE;
  } else if (session->get_nested_count() > max_stack_deep) {
    ret = OB_SIZE_OVERFLOW;
  } else if (OB_FAIL(check_stack_overflow(is_stack_overflow))) {
  } else if (is_stack_overflow) {
    ret = OB_SIZE_OVERFLOW;
  }
  return ret;
}
OperatorOpenOrder ObTableModifyOp::get_operator_open_order() const
{
  OperatorOpenOrder open_order = OPEN_CHILDREN_FIRST;
  if (spec_.plan_->is_use_px()) {
    // If PX is enabled in DML, there are two scenarios:
    // 1. PDML: in pdml, pdml-op completely does not need its corresponding input to provide runtime parameters, so it will directly return
    // open_order = OPEN_CHILDREN_FIRST
    // 2. DML+PX: In this case, the parameters of the dml run are completely fed by the GI operator above it, for example, such a plan:
    //   PX COORD
    //    PX TRANSMIT
    //      GI (FULL PARTITION WISE)
    //       DELETE
    //        TSC
    //  Such a plan requires GI to drive the deletion, GI rescan delete each time a new Task is iterated,
    //  Pass the information of the corresponding Task to the delete operator.
    open_order = OPEN_CHILDREN_FIRST;
  }
  return open_order;
}

int ObTableModifyOp::submit_all_dml_task()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.das_ref_.has_task()) {
    if (dml_rtctx_.need_pick_del_task_first() &&
                OB_FAIL(dml_rtctx_.das_ref_.pick_del_task_to_first())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.execute_all_task())) {
    } else if (OB_FAIL(dml_rtctx_.das_ref_.close_all_task())) {
    } else if (OB_FAIL(ObDMLService::handle_after_row_processing(this, &get_dml_modify_row_list()))) {
    } else {
      dml_modify_rows_.clear();
      dml_rtctx_.reuse();
    }
  }
  return ret;
}

//The data to be written by DML will be buffered in the DAS Write Buffer
//When the buffer data exceeds 6M,
//needs to be written to the storage to release the memory.
int ObTableModifyOp::discharge_das_write_buffer()
{
  int ret = OB_SUCCESS;
  if (dml_rtctx_.need_submit_all_tasks()) {
    LOG_INFO("DASWriteBuffer full, now to write storage",
             "buffer memory", dml_rtctx_.das_ref_.get_das_alloc().used(),
             K(dml_rtctx_.get_das_task_memory_size()), K(dml_rtctx_.get_das_parallel_task_size()));
    ret = submit_all_dml_task();
  } else if (execute_single_row_) {
    if (REACH_COUNT_INTERVAL(100)) { // print log per 100 times.
    }
    ret = submit_all_dml_task();
  }
  return ret;
}

int ObTableModifyOp::get_next_row_from_child()
{
  int ret = OB_SUCCESS;
  clear_evaluated_flag();
  if (OB_FAIL(child_->get_next_row())) {
    if (OB_ITER_END != ret) {
    }
  } else {
    LOG_DEBUG("child output row", "row", ROWEXPR2STR(eval_ctx_, child_->get_spec().output_));
  }
  return ret;
}

int ObTableModifyOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (iter_end_) {
    ret = OB_ITER_END;
  } else {
    int64_t row_count = 0;
    while (OB_SUCC(ret)) {
      if (OB_FAIL(try_check_status())) {
      } else if (OB_FAIL(get_next_row_from_child())) {
        if (OB_ITER_END != ret) {
        } else {
          iter_end_ = true;
          ret = OB_SUCCESS;
          break;
        }
      } else if (OB_FAIL(write_row_to_das_buffer())) {
      } else if (OB_FAIL(discharge_das_write_buffer())) {
      } else if (MY_SPEC.is_returning_) {
        break;
      }
      row_count ++;
    }

    if (OB_FAIL(ret)) {
      record_err_for_load_data(ret, row_count);
    }

    if (OB_SUCC(ret) && iter_end_ && dml_rtctx_.das_ref_.has_task()) {
      //DML operator reach iter end,
      //now submit the remaining rows in the DAS Write Buffer to the storage
      if (OB_FAIL(submit_all_dml_task())) {
      }
    }
    //to post process the DML info after writing all data to the storage or returning one row
    ret = write_rows_post_proc(ret);
    if (OB_SUCC(ret) && iter_end_) {
      ret = OB_ITER_END;
    }
  }
  return ret;
}

int ObTableModifyOp::perform_batch_fk_check()
{
  DEBUG_SYNC(BEFORE_FOREIGN_KEY_CONSTRAINT_CHECK);
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < fk_checkers_.count(); ++i) {
    bool all_has_result = false;
    ObForeignKeyChecker *fk_checker = fk_checkers_.at(i);
    if (OB_ISNULL(fk_checker)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(fk_checker->do_fk_check_batch(all_has_result))) {
    } else if (!all_has_result) {
      ret = OB_ERR_NO_REFERENCED_ROW;
    }
  }
  return ret;
}

// ---------------------------------------------------------------------------
// AI column task maintenance.  Task rows live in __all_ai_column_task and are
// written through an inner SQL connection bound to the current session, i.e.
// inside the user transaction.
// ---------------------------------------------------------------------------
namespace
{
std::atomic<uint64_t> g_ai_column_task_seq(0);

bool is_identifier_char(const char c)
{
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
      || (c >= 'A' && c <= 'Z') || '_' == c || '$' == c;
}
}  // namespace

int AiColumnHandle::collect_pk_column_ids(const share::schema::ObTableSchema &table_schema,
                                           share::schema::ObSchemaGetterGuard *schema_guard,
                                           common::ObIArray<uint64_t> &column_ids)
{
  int ret = OB_SUCCESS;
  column_ids.reuse();
  if (OB_NOT_NULL(schema_guard)
      && OB_SUCC(table_schema.get_logic_pk_column_ids(schema_guard, column_ids))
      && !column_ids.empty()) {
    // user-defined primary key columns
  } else {
    // Fall back to the storage rowkey (tables without a primary key).
    column_ids.reuse();
    const ObRowkeyInfo &rowkey_info = table_schema.get_rowkey_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
      const ObRowkeyColumn *rowkey_column = rowkey_info.get_column(i);
      if (OB_ISNULL(rowkey_column)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(column_ids.push_back(rowkey_column->column_id_))) {
      }
    }
  }
  return ret;
}

int AiColumnHandle::find_column_index(const common::ObIArray<uint64_t> &column_ids,
                                      const uint64_t column_id,
                                      int64_t &idx)
{
  int ret = OB_SUCCESS;
  idx = OB_INVALID_INDEX;
  for (int64_t i = 0; OB_SUCC(ret) && i < column_ids.count(); ++i) {
    if (column_ids.at(i) == column_id) {
      idx = i;
      break;
    }
  }
  if (OB_SUCC(ret) && OB_INVALID_INDEX == idx) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column not found in dml row", K(ret), K(column_id));
  }
  return ret;
}

int AiColumnHandle::collect_ai_columns(
    const share::schema::ObTableSchema &table_schema,
    common::ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns)
{
  int ret = OB_SUCCESS;
  ai_columns.reuse();
  for (ObTableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
    const ObColumnSchemaV2 *column = *it;
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (column->is_ai_column() && OB_FAIL(ai_columns.push_back(column))) {
    }
  }
  return ret;
}

namespace
{
// AI column metadata is persisted in __all_ai_column; the table schema itself
// only carries the AI flag.  Cache the (table, schema_version) metadata so a
// statement only pays for one lookup.
struct AiColumnMetaEntry
{
  uint64_t table_id_;
  int64_t schema_version_;
  uint64_t column_id_;
  uint64_t state_column_id_;
  char *expr_;
  int32_t expr_len_;
};
std::mutex g_ai_meta_mutex;
std::vector<AiColumnMetaEntry> g_ai_meta_cache;

int fetch_ai_table_meta(const uint64_t table_id, const int64_t schema_version)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  if (OB_FAIL(sql.assign_fmt(
        "select column_id, state_column_id, normalized_expr"
        " from oceanbase.__all_ai_column where table_id = %ld",
        static_cast<int64_t>(table_id)))) {
  } else {
    SMART_VAR(common::ObMySQLProxy::MySQLResult, result) {
      if (OB_FAIL(GCTX.sql_proxy_->read(result, sql.ptr()))) {
      } else if (OB_ISNULL(result.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        sqlclient::ObMySQLResult *row = result.get_result();
        while (OB_SUCC(ret) && OB_SUCC(row->next())) {
          int64_t column_id = 0;
          int64_t state_column_id = 0;
          ObString expr;
          if (OB_FAIL(row->get_int("column_id", column_id))) {
          } else if (OB_FAIL(row->get_int("state_column_id", state_column_id))) {
          } else if (OB_FAIL(row->get_varchar("normalized_expr", expr))) {
            if (OB_ERR_NULL_VALUE == ret) {
              ret = OB_SUCCESS;
              expr.reset();
            }
          }
          if (OB_SUCC(ret)) {
            char *buf = nullptr;
            if (expr.length() > 0) {
              buf = static_cast<char *>(std::malloc(expr.length() + 1));
              if (OB_ISNULL(buf)) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                MEMCPY(buf, expr.ptr(), expr.length());
                buf[expr.length()] = '\0';
              }
            }
            if (OB_SUCC(ret)) {
              AiColumnMetaEntry entry;
              entry.table_id_ = table_id;
              entry.schema_version_ = schema_version;
              entry.column_id_ = static_cast<uint64_t>(column_id);
              entry.state_column_id_ = static_cast<uint64_t>(state_column_id);
              entry.expr_ = buf;
              entry.expr_len_ = static_cast<int32_t>(expr.length());
              std::lock_guard<std::mutex> guard(g_ai_meta_mutex);
              bool exists = false;
              for (size_t i = 0; !exists && i < g_ai_meta_cache.size(); ++i) {
                const AiColumnMetaEntry &e = g_ai_meta_cache.at(i);
                if (e.table_id_ == entry.table_id_
                    && e.schema_version_ == entry.schema_version_
                    && e.column_id_ == entry.column_id_) {
                  exists = true;
                }
              }
              if (!exists) {
                g_ai_meta_cache.push_back(entry);
              } else {
                std::free(buf);
              }
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
        int close_ret = row->close();
        if (OB_SUCCESS != close_ret && OB_SUCC(ret)) {
          ret = close_ret;
        }
      }
    }
  }
  return ret;
}

int load_ai_column_meta(const share::schema::ObTableSchema &table_schema,
                        const common::ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns,
                        common::ObIArray<AiColumnMetaEntry> &metas)
{
  int ret = OB_SUCCESS;
  const uint64_t table_id = table_schema.get_table_id();
  const int64_t schema_version = table_schema.get_schema_version();
  bool need_fetch = false;
  metas.reuse();
  {
    std::lock_guard<std::mutex> guard(g_ai_meta_mutex);
    for (int64_t i = 0; i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *column = ai_columns.at(i);
      bool found = false;
      for (size_t j = 0; !found && j < g_ai_meta_cache.size(); ++j) {
        const AiColumnMetaEntry &e = g_ai_meta_cache.at(j);
        if (e.table_id_ == table_id && e.schema_version_ == schema_version
            && e.column_id_ == column->get_column_id()) {
          found = true;
        }
      }
      if (!found) {
        need_fetch = true;
      }
    }
  }
  if (need_fetch && OB_FAIL(fetch_ai_table_meta(table_id, schema_version))) {
  } else {
    std::lock_guard<std::mutex> guard(g_ai_meta_mutex);
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *column = ai_columns.at(i);
      // Prefer the live schema (fresh DDL), then the persisted metadata.
      AiColumnMetaEntry meta;
      MEMSET(&meta, 0, sizeof(meta));
      meta.table_id_ = table_id;
      meta.schema_version_ = schema_version;
      meta.column_id_ = column->get_column_id();
      meta.state_column_id_ = column->get_ai_state_column_id();
      meta.expr_ = const_cast<char *>(column->get_ai_normalized_expr().ptr());
      meta.expr_len_ = column->get_ai_normalized_expr().length();
      if (meta.state_column_id_ == OB_INVALID_ID || meta.expr_len_ <= 0) {
        bool found = false;
        for (size_t j = 0; !found && j < g_ai_meta_cache.size(); ++j) {
          const AiColumnMetaEntry &e = g_ai_meta_cache.at(j);
          if (e.table_id_ == table_id && e.schema_version_ == schema_version
              && e.column_id_ == column->get_column_id()) {
            meta.state_column_id_ = e.state_column_id_;
            meta.expr_ = e.expr_;
            meta.expr_len_ = e.expr_len_;
            found = true;
          }
        }
        if (OB_UNLIKELY(!found)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("ai column metadata not found", K(ret), K(table_id), K(column->get_column_id()));
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(metas.push_back(meta))) {
      }
    }
  }
  return ret;
}
}  // namespace

// Extract column references from the stored AI expression.  String literals are
// skipped so that prompt text never contributes a dependency.
int AiColumnHandle::collect_depend_columns(
    const share::schema::ObTableSchema &table_schema,
    const common::ObString &expr,
    common::ObIArray<uint64_t> &depend_column_ids)
{
  int ret = OB_SUCCESS;
  depend_column_ids.reuse();
  if (expr.empty()) {
    ret = OB_INVALID_ARGUMENT;
  }
  const int64_t len = expr.length();
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < len) {
    const char c = expr[pos];
    if ('\'' == c || '"' == c) {
      const char quote = c;
      ++pos;
      bool closed = false;
      while (!closed && pos < len) {
        if (expr[pos] == quote) {
          if (pos + 1 < len && expr[pos + 1] == quote) {
            pos += 2;
          } else {
            ++pos;
            closed = true;
          }
        } else if ('\\' == expr[pos] && pos + 1 < len) {
          pos += 2;
        } else {
          ++pos;
        }
      }
    } else if ('`' == c) {
      const int64_t start = ++pos;
      while (pos < len && '`' != expr[pos]) {
        ++pos;
      }
      if (pos > start) {
        ObString ident(static_cast<int32_t>(pos - start), expr.ptr() + start);
        for (ObTableSchema::const_column_iterator it = table_schema.column_begin();
             OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
          const ObColumnSchemaV2 *column = *it;
          if (OB_ISNULL(column)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (!column->is_ai_column() && !column->is_hidden()
                     && ObCharset::case_insensitive_equal(ident, column->get_column_name_str())
                     && !has_exist_in_array(depend_column_ids, column->get_column_id())
                     && OB_FAIL(depend_column_ids.push_back(column->get_column_id()))) {
          }
        }
      }
      if (pos < len) {
        ++pos;
      }
    } else if (is_identifier_char(c) && !(c >= '0' && c <= '9')) {
      const int64_t start = pos;
      while (pos < len && is_identifier_char(expr[pos])) {
        ++pos;
      }
      ObString ident(static_cast<int32_t>(pos - start), expr.ptr() + start);
      for (ObTableSchema::const_column_iterator it = table_schema.column_begin();
           OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
        const ObColumnSchemaV2 *column = *it;
        if (OB_ISNULL(column)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (!column->is_ai_column() && !column->is_hidden()
                   && ObCharset::case_insensitive_equal(ident, column->get_column_name_str())
                   && !has_exist_in_array(depend_column_ids, column->get_column_id())
                   && OB_FAIL(depend_column_ids.push_back(column->get_column_id()))) {
        }
      }
    } else {
      ++pos;
    }
  }
  return ret;
}

bool AiColumnHandle::is_column_updated(const ObDASUpdCtDef &upd_ctdef,
                                       const uint64_t column_id)
{
  return has_exist_in_array(upd_ctdef.updated_column_ids_, column_id);
}

bool AiColumnHandle::is_rowkey_updated(const share::schema::ObTableSchema &table_schema,
                                       share::schema::ObSchemaGetterGuard *schema_guard,
                                       const ObDASUpdCtDef &upd_ctdef)
{
  int ret = OB_SUCCESS;
  bool updated = false;
  ObSEArray<uint64_t, 4> pk_column_ids;
  if (OB_FAIL(collect_pk_column_ids(table_schema, schema_guard, pk_column_ids))) {
    LOG_WARN("collect primary key columns failed", K(ret));
  } else {
    for (int64_t i = 0; !updated && i < pk_column_ids.count(); ++i) {
      if (is_column_updated(upd_ctdef, pk_column_ids.at(i))) {
        updated = true;
      }
    }
  }
  return updated;
}

int AiColumnHandle::set_column_datum_null(const ObExprPtrIArray &row,
                                          const int64_t idx,
                                          ObEvalCtx &eval_ctx)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;
  if (OB_UNLIKELY(idx < 0 || idx >= row.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(row.at(idx)->eval(eval_ctx, datum))) {
  } else if (OB_ISNULL(datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    datum->set_null();
  }
  return ret;
}

int AiColumnHandle::set_column_datum_string(const ObExprPtrIArray &row,
                                            const int64_t idx,
                                            ObEvalCtx &eval_ctx,
                                            common::ObIAllocator &alloc,
                                            const common::ObString &value)
{
  int ret = OB_SUCCESS;
  ObDatum *datum = nullptr;
  char *buf = nullptr;
  if (OB_UNLIKELY(idx < 0 || idx >= row.count())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(row.at(idx)->eval(eval_ctx, datum))) {
  } else if (OB_ISNULL(datum)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(value.length())))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, value.ptr(), value.length());
    datum->set_string(ObString(value.length(), buf));
  }
  return ret;
}

int AiColumnHandle::gen_rowkey_predicate(ObTableModifyOp &op,
                                         const share::schema::ObTableSchema &table_schema,
                                         const common::ObIArray<uint64_t> &column_ids,
                                         const ObExprPtrIArray &row,
                                         common::ObIAllocator &alloc,
                                         common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  int64_t len = 0;
  int64_t pos = 0;
  ObSEArray<uint64_t, 4> pk_column_ids;
  ObEvalCtx &eval_ctx = op.get_eval_ctx();
  ObSchemaGetterGuard *schema_guard = nullptr;
  if (OB_NOT_NULL(op.get_exec_ctx().get_sql_ctx())) {
    schema_guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_;
  }
  if (OB_FAIL(collect_pk_column_ids(table_schema, schema_guard, pk_column_ids))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < pk_column_ids.count(); ++i) {
    const ObColumnSchemaV2 *column = nullptr;
    int64_t idx = OB_INVALID_INDEX;
    ObDatum *datum = nullptr;
    ObObj obj;
    if (OB_ISNULL(column = table_schema.get_column_schema(pk_column_ids.at(i)))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(find_column_index(column_ids, pk_column_ids.at(i), idx))) {
    } else if (OB_FAIL(row.at(idx)->eval(eval_ctx, datum)) || OB_ISNULL(datum)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(datum->to_obj(obj, row.at(idx)->obj_meta_))) {
    } else if (OB_FAIL(databuff_printf(buf, len, pos, alloc, "%s`%.*s` = ",
               i > 0 ? " and " : "",
               static_cast<int32_t>(column->get_column_name_str().length()),
               column->get_column_name_str().ptr()))) {
    } else if (OB_FAIL(obj.print_sql_literal(buf, len, pos, alloc,
                       op.get_obj_print_params()))) {
    }
  }
  if (OB_SUCC(ret)) {
    predicate.assign_ptr(buf, static_cast<int32_t>(pos));
  }
  return ret;
}

int AiColumnHandle::gen_request_id(common::ObIAllocator &alloc,
                                   common::ObString &request_id)
{
  int ret = OB_SUCCESS;
  const int64_t now = ObTimeUtility::current_time();
  const uint64_t seq = g_ai_column_task_seq.fetch_add(1);
  char buf[128];
  const int64_t len = snprintf(buf, sizeof(buf), "%ld_%lu", now, seq);
  if (OB_UNLIKELY(len <= 0 || len >= static_cast<int64_t>(sizeof(buf)))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(ob_write_string(alloc, ObString(static_cast<int32_t>(len), buf),
                                     request_id))) {
  }
  return ret;
}

int AiColumnHandle::insert_tasks(ObTableModifyOp &op,
                                 const share::schema::ObTableSchema &table_schema,
                                 const common::ObIArray<const share::schema::ObColumnSchemaV2 *> &ai_columns,
                                 const common::ObString &request_id,
                                 const common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColTask");
  UNUSED(table_schema);
  ObSEArray<common::ObString, 4> sqls;
  for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
    const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
    share::ObDMLSqlSplicer splicer;
    common::ObSqlString sql;
    char *sql_buf = nullptr;
    if (OB_ISNULL(ai_column)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      const int64_t task_id = ObTimeUtility::current_time()
                  + static_cast<int64_t>(g_ai_column_task_seq.fetch_add(1) % 1000);
      if (OB_FAIL(splicer.add_column("task_id", task_id))) {
      } else if (OB_FAIL(splicer.add_column("table_id",
                     static_cast<int64_t>(ai_column->get_table_id())))) {
      } else if (OB_FAIL(splicer.add_column("request_id", request_id))) {
      } else if (OB_FAIL(splicer.add_column("ai_column_id",
                     static_cast<int64_t>(ai_column->get_column_id())))) {
      } else if (OB_FAIL(splicer.add_column("rowkey_predicate", predicate))) {
      } else if (OB_FAIL(splicer.add_function_call("eligible_ts", "now(6)"))) {
      } else if (OB_FAIL(splicer.add_column("state", ObString::make_string("0")))) {
      } else if (OB_FAIL(splicer.add_column("retry_count", static_cast<int64_t>(0)))) {
      } else if (OB_FAIL(splicer.splice_insert_sql("oceanbase.__all_ai_column_task", sql))) {
      } else if (OB_ISNULL(sql_buf = static_cast<char *>(allocator.alloc(sql.length() + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(sql_buf, sql.ptr(), sql.length());
        sql_buf[sql.length()] = '\0';
        if (OB_FAIL(sqls.push_back(ObString(static_cast<int32_t>(sql.length()), sql_buf)))) {
        }
      }
    }
  }
  if (OB_SUCC(ret) && !sqls.empty()) {
    if (OB_FAIL(exec_nested_sql(op, sqls, true))) {
    }
  }
  return ret;
}

int AiColumnHandle::delete_tasks(ObTableModifyOp &op,
                                 const uint64_t table_id,
                                 const common::ObString &predicate)
{
  int ret = OB_SUCCESS;
  share::ObDMLSqlSplicer splicer;
  common::ObSqlString sql;
  ObSEArray<common::ObString, 1> sqls;
  if (OB_FAIL(splicer.add_pk_column("table_id", static_cast<int64_t>(table_id)))) {
  } else if (OB_FAIL(splicer.add_pk_column("rowkey_predicate", predicate))) {
  } else if (OB_FAIL(splicer.splice_delete_sql("oceanbase.__all_ai_column_task", sql))) {
  } else if (OB_FAIL(sqls.push_back(ObString(static_cast<int32_t>(sql.length()), sql.ptr())))) {
  } else if (OB_FAIL(exec_nested_sql(op, sqls, true))) {
  }
  return ret;
}

int AiColumnHandle::exec_nested_sql(ObTableModifyOp &op,
                                    const common::ObIArray<common::ObString> &sqls,
                                    const bool skip_cur_stmt_tables)
{
  int ret = OB_SUCCESS;
  // Mark the outer statement as a cascading nested SQL so that the transaction
  // layer treats the inner statement as nested (same as foreign key cascades).
  ObExecContext &exec_ctx = op.get_exec_ctx();
  const bool saved_fk_cascading = exec_ctx.get_das_ctx().is_fk_cascading_;
  exec_ctx.get_das_ctx().is_fk_cascading_ = true;
  if (OB_FAIL(op.open_inner_conn())) {
  } else if (OB_FAIL(op.begin_nested_session(skip_cur_stmt_tables))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < sqls.count(); ++i) {
      if (OB_FAIL(op.execute_write(sqls.at(i).ptr()))) {
        LOG_WARN("execute ai column inner sql failed", K(ret), K(sqls.at(i)));
      }
    }
    const int end_ret = op.end_nested_session();
    if (OB_SUCCESS != end_ret && OB_SUCC(ret)) {
      ret = end_ret;
    }
  }
  exec_ctx.get_das_ctx().is_fk_cascading_ = saved_fk_cascading;
  return ret;
}

int AiColumnHandle::execute_pending_invalidations(ObTableModifyOp &op)
{
  int ret = OB_SUCCESS;
  ObIArray<ObString> &sqls = op.get_ai_invalidate_sqls();
  if (!sqls.empty()) {
    if (OB_FAIL(exec_nested_sql(op, sqls, true))) {
    }
    op.clear_ai_invalidate_sqls();
  }
  return ret;
}

int AiColumnHandle::handle_insert(ObTableModifyOp &op,
                                  const share::schema::ObTableSchema &table_schema,
                                  const ObInsCtDef &ins_ctdef)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObSEArray<AiColumnMetaEntry, 4> metas;
  ObArenaAllocator allocator("AiColIns");
  ObString request_id;
  ObString predicate;
  if (OB_FAIL(collect_ai_columns(table_schema, ai_columns))) {
  } else if (ai_columns.empty()) {
  } else if (OB_FAIL(load_ai_column_meta(table_schema, ai_columns, metas))) {
  } else if (OB_FAIL(gen_request_id(allocator, request_id))) {
  } else if (OB_FAIL(gen_rowkey_predicate(op, table_schema, ins_ctdef.column_ids_,
                                          ins_ctdef.new_row_, allocator, predicate))) {
  } else {
    // The AI result is always regenerated asynchronously; the inserted value
    // must be NULL until the worker publishes a result.
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      int64_t idx = OB_INVALID_INDEX;
      if (OB_FAIL(find_column_index(ins_ctdef.column_ids_,
                                    ai_columns.at(i)->get_column_id(), idx))) {
      } else if (OB_FAIL(set_column_datum_null(ins_ctdef.new_row_, idx, op.get_eval_ctx()))) {
      } else {
        const uint64_t state_cid = metas.at(i).state_column_id_;
        int64_t state_idx = OB_INVALID_INDEX;
        if (OB_INVALID_ID != state_cid
            && has_exist_in_array(ins_ctdef.column_ids_, state_cid, &state_idx)
            && OB_FAIL(set_column_datum_string(ins_ctdef.new_row_, state_idx,
                       op.get_eval_ctx(), op.get_exec_ctx().get_allocator(),
                       ObString::make_string("PENDING")))) {
        }
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(insert_tasks(op, table_schema, ai_columns,
                                             request_id, predicate))) {
    }
  }
  return ret;
}

int AiColumnHandle::handle_update(ObTableModifyOp &op,
                                  const share::schema::ObTableSchema &table_schema,
                                  const ObUpdCtDef &upd_ctdef)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObSEArray<const ObColumnSchemaV2 *, 4> refresh_columns;
  ObSEArray<AiColumnMetaEntry, 4> metas;
  ObSEArray<AiColumnMetaEntry, 4> refresh_metas;
  ObArenaAllocator allocator("AiColUpd");
  ObString request_id;
  ObString old_predicate;
  ObString new_predicate;
  const ObDASUpdCtDef &dupd_ctdef = upd_ctdef.dupd_ctdef_;
  ObSchemaGetterGuard *schema_guard = nullptr;
  if (OB_NOT_NULL(op.get_exec_ctx().get_sql_ctx())) {
    schema_guard = op.get_exec_ctx().get_sql_ctx()->schema_guard_;
  }
  const bool rowkey_changed = is_rowkey_updated(table_schema, schema_guard, dupd_ctdef);
  const bool is_nested = ObSQLUtils::is_nested_sql(&op.get_exec_ctx());
  ObString database_name;
  const ObDatabaseSchema *database_schema = nullptr;
  if (OB_NOT_NULL(schema_guard)
      && OB_FAIL(schema_guard->get_database_schema(table_schema.get_database_id(),
                                                   database_schema))) {
  } else if (OB_NOT_NULL(database_schema)) {
    database_name = database_schema->get_database_name_str();
  }
  if (OB_FAIL(collect_ai_columns(table_schema, ai_columns))) {
  } else if (ai_columns.empty()) {
  } else if (OB_FAIL(load_ai_column_meta(table_schema, ai_columns, metas))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < ai_columns.count(); ++i) {
      const ObColumnSchemaV2 *ai_column = ai_columns.at(i);
      bool need_refresh = rowkey_changed;
      ObString ai_expr(metas.at(i).expr_len_, metas.at(i).expr_);
      if (!need_refresh && !ai_expr.empty()) {
        ObSEArray<uint64_t, 4> depend_column_ids;
        if (OB_FAIL(collect_depend_columns(table_schema, ai_expr, depend_column_ids))) {
        } else {
          for (int64_t j = 0; !need_refresh && j < depend_column_ids.count(); ++j) {
            if (is_column_updated(dupd_ctdef, depend_column_ids.at(j))) {
              need_refresh = true;
            }
          }
        }
      }
      // An explicit assignment of NULL to an AI column is an invalidation
      // request (used by ALTER TABLE ... RETRY AI COLUMN).  Nested SQL
      // generated by this handler itself must not re-trigger the refresh.
      if (OB_SUCC(ret) && !need_refresh && !is_nested
          && is_column_updated(dupd_ctdef, ai_column->get_column_id())) {
        int64_t idx = OB_INVALID_INDEX;
        ObDatum *datum = nullptr;
        if (has_exist_in_array(upd_ctdef.column_ids_,
                               ai_column->get_column_id(), &idx)
            && OB_SUCC(upd_ctdef.new_row_.at(idx)->eval(op.get_eval_ctx(), datum))
            && OB_NOT_NULL(datum) && datum->is_null()) {
          need_refresh = true;
        }
      }
      if (OB_SUCC(ret) && need_refresh
          && (OB_FAIL(refresh_columns.push_back(ai_column))
              || OB_FAIL(refresh_metas.push_back(metas.at(i))))) {
      }
    }
  }
  if (OB_SUCC(ret) && !refresh_columns.empty()) {
    if (OB_FAIL(gen_request_id(allocator, request_id))) {
    } else if (OB_FAIL(gen_rowkey_predicate(op, table_schema, upd_ctdef.column_ids_,
                                            upd_ctdef.old_row_, allocator, old_predicate))) {
    } else if (OB_FAIL(gen_rowkey_predicate(op, table_schema, upd_ctdef.column_ids_,
                                            upd_ctdef.new_row_, allocator, new_predicate))) {
    } else if (rowkey_changed) {
      // The new row is written through the insert path, so the NULL result is
      // written with it.  The hidden state column is updated after the
      // statement; changing it in the row image would also change the old row
      // used by the split delete.
      for (int64_t i = 0; OB_SUCC(ret) && i < refresh_columns.count(); ++i) {
        int64_t idx = OB_INVALID_INDEX;
        if (OB_FAIL(find_column_index(upd_ctdef.column_ids_,
                                      refresh_columns.at(i)->get_column_id(), idx))) {
        } else if (OB_FAIL(set_column_datum_null(upd_ctdef.new_row_, idx, op.get_eval_ctx()))) {
        }
      }
      if (OB_SUCC(ret)) {
        ObSqlString state_sql;
        if (OB_FAIL(state_sql.assign_fmt("update `%.*s`.`%.*s` set ",
               static_cast<int32_t>(database_name.length()), database_name.ptr(),
               static_cast<int32_t>(table_schema.get_table_name_str().length()),
               table_schema.get_table_name_str().ptr()))) {
        }
        for (int64_t i = 0; OB_SUCC(ret) && i < refresh_columns.count(); ++i) {
          const ObColumnSchemaV2 *state_column = nullptr;
          if (OB_INVALID_ID == refresh_metas.at(i).state_column_id_) {
          } else if (OB_ISNULL(state_column = table_schema.get_column_schema(
                          refresh_metas.at(i).state_column_id_))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(state_sql.append_fmt("%s`%.*s` = 'PENDING'",
                     i > 0 ? ", " : "",
                     static_cast<int32_t>(state_column->get_column_name_str().length()),
                     state_column->get_column_name_str().ptr()))) {
          }
        }
        if (OB_SUCC(ret) && state_sql.length() > 0
            && OB_FAIL(state_sql.append_fmt(" where %.*s",
               static_cast<int32_t>(new_predicate.length()), new_predicate.ptr()))) {
        } else if (OB_SUCC(ret) && state_sql.length() > 0) {
          char *sql_buf = static_cast<char *>(
              op.get_exec_ctx().get_allocator().alloc(state_sql.length() + 1));
          if (OB_ISNULL(sql_buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(sql_buf, state_sql.ptr(), state_sql.length());
            sql_buf[state_sql.length()] = '\0';
            if (OB_FAIL(op.add_ai_invalidate_sql(ObString(
                    static_cast<int32_t>(state_sql.length()), sql_buf)))) {
            }
          }
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(delete_tasks(op, table_schema.get_table_id(), old_predicate))) {
      } else if (OB_SUCC(ret)
          && OB_FAIL(insert_tasks(op, table_schema, refresh_columns,
                                  request_id, new_predicate))) {
      }
    } else {
      // Invalidate the old results and mark them PENDING in the same
      // transaction; the outer statement does not touch these columns.
      ObSqlString invalidate_sql;
      if (OB_FAIL(invalidate_sql.assign_fmt("update `%.*s`.`%.*s` set ",
             static_cast<int32_t>(database_name.length()), database_name.ptr(),
             static_cast<int32_t>(table_schema.get_table_name_str().length()),
             table_schema.get_table_name_str().ptr()))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < refresh_columns.count(); ++i) {
        const ObColumnSchemaV2 *state_column = nullptr;
        if (i > 0 && OB_FAIL(invalidate_sql.append(", "))) {
        } else if (OB_FAIL(invalidate_sql.append_fmt("`%.*s` = null",
                   static_cast<int32_t>(refresh_columns.at(i)->get_column_name_str().length()),
                   refresh_columns.at(i)->get_column_name_str().ptr()))) {
        } else if (OB_INVALID_ID != refresh_metas.at(i).state_column_id_) {
          if (OB_ISNULL(state_column = table_schema.get_column_schema(
                          refresh_metas.at(i).state_column_id_))) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(invalidate_sql.append_fmt(", `%.*s` = 'PENDING'",
                     static_cast<int32_t>(state_column->get_column_name_str().length()),
                     state_column->get_column_name_str().ptr()))) {
          }
        }
      }
      if (OB_SUCC(ret) && OB_FAIL(invalidate_sql.append_fmt(" where %.*s",
             static_cast<int32_t>(new_predicate.length()), new_predicate.ptr()))) {
      } else if (OB_SUCC(ret)) {
        char *sql_buf = static_cast<char *>(
            op.get_exec_ctx().get_allocator().alloc(invalidate_sql.length() + 1));
        if (OB_ISNULL(sql_buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(sql_buf, invalidate_sql.ptr(), invalidate_sql.length());
          sql_buf[invalidate_sql.length()] = '\0';
          if (OB_FAIL(op.add_ai_invalidate_sql(ObString(
                  static_cast<int32_t>(invalidate_sql.length()), sql_buf)))) {
          }
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(delete_tasks(op, table_schema.get_table_id(), old_predicate))) {
      } else if (OB_SUCC(ret)
          && OB_FAIL(insert_tasks(op, table_schema, refresh_columns,
                                  request_id, new_predicate))) {
      }
    }
  }
  return ret;
}

int AiColumnHandle::handle_delete(ObTableModifyOp &op,
                                  const share::schema::ObTableSchema &table_schema,
                                  const ObDelCtDef &del_ctdef)
{
  int ret = OB_SUCCESS;
  ObSEArray<const ObColumnSchemaV2 *, 4> ai_columns;
  ObArenaAllocator allocator("AiColDel");
  ObString predicate;
  if (OB_FAIL(collect_ai_columns(table_schema, ai_columns))) {
  } else if (ai_columns.empty()) {
  } else if (OB_FAIL(gen_rowkey_predicate(op, table_schema, del_ctdef.column_ids_,
                                          del_ctdef.old_row_, allocator, predicate))) {
  } else if (OB_FAIL(delete_tasks(op, table_schema.get_table_id(), predicate))) {
  }
  return ret;
}

int AiColumnHandle::do_handle(ObTableModifyOp &op,
                              const ObDMLBaseCtDef &dml_ctdef,
                              ObDMLBaseRtDef &dml_rtdef)
{
  int ret = OB_SUCCESS;
  UNUSED(dml_rtdef);
  const ObDASDMLBaseCtDef &das_ctdef = dml_ctdef.das_base_ctdef_;
  const uint64_t table_id = das_ctdef.index_tid_;
  ObSqlCtx *sql_ctx = op.get_exec_ctx().get_sql_ctx();
  share::schema::ObSchemaGetterGuard *schema_guard = nullptr;
  const share::schema::ObTableSchema *table_schema = nullptr;
  if (OB_ISNULL(sql_ctx)
      || OB_ISNULL(schema_guard = sql_ctx->schema_guard_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(schema_guard->get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    bool has_ai_column = false;
    for (ObTableSchema::const_column_iterator it = table_schema->column_begin();
         !has_ai_column && it != table_schema->column_end(); ++it) {
      if (OB_NOT_NULL(*it) && (*it)->is_ai_column()) {
        has_ai_column = true;
      }
    }
    if (OB_SUCC(ret) && has_ai_column) {
      switch (dml_ctdef.dml_type_) {
        case DAS_OP_TABLE_INSERT: {
          if (OB_FAIL(handle_insert(op, *table_schema,
                                    static_cast<const ObInsCtDef &>(dml_ctdef)))) {
          }
          break;
        }
        case DAS_OP_TABLE_UPDATE: {
          if (OB_FAIL(handle_update(op, *table_schema,
                                    static_cast<const ObUpdCtDef &>(dml_ctdef)))) {
          }
          break;
        }
        case DAS_OP_TABLE_DELETE: {
          if (OB_FAIL(handle_delete(op, *table_schema,
                                    static_cast<const ObDelCtDef &>(dml_ctdef)))) {
          }
          break;
        }
        default:
          break;
      }
    }
  }
  return ret;
}
}  // namespace sql
}  // namespace oceanbase

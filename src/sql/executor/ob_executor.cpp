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

#define USING_LOG_PREFIX SQL_EXE

#include "ob_executor.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/ob_sql_utils.h"
#include "share/ob_server_struct.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"
using namespace oceanbase::common;
using namespace oceanbase::sql;

int ObExecutor::init(ObPhysicalPlan *plan)
{
  int ret = OB_SUCCESS;
  if (true == inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_ISNULL(plan)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    phy_plan_ = plan;
    inited_ = true;
  }
  return ret;
}

void ObExecutor::reset()
{
  inited_ = false;
  phy_plan_ = NULL;
  execution_id_ = OB_INVALID_ID;
}


int ObExecutor::check_ai_column_strict_read(ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session_info = ctx.get_my_session();
  const ObIArray<uint64_t> &table_ids = phy_plan_->get_ai_column_ref_table_ids();
  const ObIArray<uint64_t> &column_ids = phy_plan_->get_ai_column_ref_column_ids();
  bool strict_read = false;
  if (OB_ISNULL(session_info)) {
    ret = OB_NOT_INIT;
  } else if (table_ids.empty()
             || ObSQLUtils::is_nested_sql(&ctx)) {
    // nothing to check
  } else if (OB_FAIL(session_info->get_sys_variable(
               share::SYS_VAR_AI_COLUMN_STRICT_READ, strict_read))) {
  } else if (!strict_read) {
    // strict read disabled
  } else {
    ObSchemaGetterGuard *schema_guard = nullptr;
    if (OB_ISNULL(ctx.get_sql_ctx())
        || OB_ISNULL(schema_guard = ctx.get_sql_ctx()->schema_guard_)) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < table_ids.count(); ++i) {
      const ObTableSchema *table_schema = nullptr;
      const ObDatabaseSchema *database_schema = nullptr;
      const ObColumnSchemaV2 *column_schema = nullptr;
      ObSqlString sql;
      if (OB_FAIL(schema_guard->get_table_schema(table_ids.at(i), table_schema))) {
      } else if (OB_ISNULL(table_schema)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(schema_guard->get_database_schema(table_schema->get_database_id(),
                                                           database_schema))) {
      } else if (OB_ISNULL(database_schema)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_ISNULL(column_schema = table_schema->get_column_schema(column_ids.at(i)))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(sql.assign_fmt("select /*+ no_parallel */ 1 from `%.*s`.`%.*s` where `%.*s` is null limit 1",
                     static_cast<int32_t>(database_schema->get_database_name_str().length()),
                     database_schema->get_database_name_str().ptr(),
                     static_cast<int32_t>(table_schema->get_table_name_str().length()),
                     table_schema->get_table_name_str().ptr(),
                     static_cast<int32_t>(column_schema->get_column_name_str().length()),
                     column_schema->get_column_name_str().ptr()))) {
      } else {
        SMART_VAR(ObMySQLProxy::MySQLResult, result) {
          bool not_ready = false;
          if (OB_FAIL(GCTX.sql_proxy_->read(result, sql.ptr()))) {
          } else if (OB_ISNULL(result.get_result())) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            if (OB_FAIL(result.get_result()->next())) {
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
              }
            } else {
              not_ready = true;
            }
            int close_ret = result.get_result()->close();
            if (OB_SUCCESS != close_ret && OB_SUCC(ret)) {
              ret = close_ret;
            }
          }
          if (OB_SUCC(ret) && not_ready) {
            FORWARD_USER_ERROR_MSG(OB_AI_COLUMN_STRICT_READ,
                "AI column has unfinished tasks for table_id=%lu, column_id=%lu",
                table_ids.at(i), column_ids.at(i));
            ret = OB_AI_COLUMN_STRICT_READ;
          }
        }
      }
    }
  }
  return ret;
}

int ObExecutor::execute_plan(ObExecContext &ctx)
{
  NG_TRACE(exec_plan_begin);
  int ret = OB_SUCCESS;
  ObSqlExecutorCtx &task_exec_ctx = ctx.get_sql_exec_ctx();
  ObExecuteResult &exec_result = task_exec_ctx.get_execute_result();
  ObSQLSessionInfo *session_info = ctx.get_my_session();
  ObPhysicalPlanCtx *plan_ctx = ctx.get_physical_plan_ctx();
  int64_t batched_stmt_cnt = ctx.get_sql_ctx()->multi_stmt_item_.get_batched_stmt_cnt();
  ctx.set_use_temp_expr_ctx_cache(true);
  // If the batch execution is rewritten by insert multi values, there is no need to repack multiple times
  if (ctx.get_sql_ctx()->is_do_insert_batch_opt()) {
    batched_stmt_cnt = 0;
  }
  if (OB_UNLIKELY(!inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(session_info)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(phy_plan_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(check_ai_column_strict_read(ctx))) {
  } else if (OB_FAIL(session_info->set_cur_phy_plan(phy_plan_))) {
  } else if (OB_FAIL(phy_plan_->get_expr_frame_info()
                                 .pre_alloc_exec_memory(ctx))) {
  } else if (batched_stmt_cnt > 0
      && OB_FAIL(plan_ctx->create_implicit_cursor_infos(batched_stmt_cnt))) {
  } else {
    ObPhyPlanType execute_type = phy_plan_->get_plan_type();
    // Some parallel plans do not require the PX scheduler.
    if (execute_type != OB_PHY_PLAN_LOCAL && phy_plan_->is_require_local_execution()) {
      execute_type = OB_PHY_PLAN_LOCAL;
      LOG_TRACE("change the plan execution type",
          "fact", execute_type, K(phy_plan_->get_plan_type()));
    }

    switch (execute_type) {
      case OB_PHY_PLAN_LOCAL: {
        ObOperator *op = NULL;
        if (OB_FAIL(phy_plan_->get_root_op_spec()->create_operator(ctx, op))) {
        } else if (OB_ISNULL(op)) {
          ret = OB_ERR_UNEXPECTED;
        } else {
          exec_result.set_static_engine_root(op);
        }
        break;
      }
      case OB_PHY_PLAN_DISTRIBUTED:
        // PX special path
        // PX mode, scheduling work is handled by the ObPxCoord operator
        ret = execute_static_cg_px_plan(ctx);
        break;
      default:
        ret = OB_ERR_UNEXPECTED;
        break;
    }
  }
  NG_TRACE(exec_plan_end);
  return ret;
}

int ObExecutor::execute_static_cg_px_plan(ObExecContext &ctx)
{
  int ret = OB_SUCCESS;
  ObOperator *op = NULL;
  if (OB_FAIL(phy_plan_->get_root_op_spec()->create_op_input(ctx))) {
  } else if (OB_FAIL(phy_plan_->get_root_op_spec()->create_operator(ctx, op))) {
  } else if (OB_ISNULL(op)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ctx.get_sql_executor_ctx()
        ->get_execute_result()
        .set_static_engine_root(op);
  }
  return ret;
}

int ObExecutor::close(ObExecContext &ctx)
{
  // close function should be designed to be callable at any time, therefore regardless of the value of inited_
  int ret = OB_SUCCESS;
  ObSQLSessionInfo *session_info = ctx.get_my_session();
  if (OB_LIKELY(NULL != session_info)) {
    // Reset cur_phy_plan_ in session to NULL
    session_info->reset_cur_phy_plan_to_null();
  }
  return ret;
}

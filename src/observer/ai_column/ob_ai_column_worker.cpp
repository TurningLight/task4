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

#define USING_LOG_PREFIX SHARE

#include "observer/ai_column/ob_ai_column_worker.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/thread/ob_thread_name.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_string.h"
#include "lib/utility/utility.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "common/mysqlclient/ob_isql_connection.h"
#include "common/mysqlclient/ob_mysql_result.h"
#include "share/rc/ob_server_runtime.h"
#include "share/ob_server_struct.h"
#include "share/ob_server_status.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace observer
{

namespace
{

const int64_t IDLE_SLEEP_US = 20 * 1000;
const int64_t BUSY_SLEEP_US = 1000;
const int32_t SQL_GROUP_ID = 0;

bool same_group(const share::ObAiColumnTaskRow &left, const share::ObAiColumnTaskRow &right)
{
  return left.table_id_ == right.table_id_
      && 0 == left.request_id_.compare(right.request_id_)
      && 0 == left.rowkey_predicate_.compare(right.rowkey_predicate_);
}

int get_string_copy(ObIAllocator &alloc,
                    sqlclient::ObMySQLResult &result,
                    const int64_t column_idx,
                    ObString &value)
{
  int ret = OB_SUCCESS;
  ObString tmp;
  if (OB_FAIL(result.get_varchar(column_idx, tmp))) {
  } else if (OB_FAIL(ob_write_string(alloc, tmp, value))) {
  }
  return ret;
}

// Column order of ObAiColumnTaskHelper::build_load_tasks_sql.
int read_task_row(ObIAllocator &alloc,
                  sqlclient::ObMySQLResult &result,
                  share::ObAiColumnTaskRow &row)
{
  int ret = OB_SUCCESS;
  int64_t table_id = 0;
  int64_t column_id = 0;
  if (OB_FAIL(result.get_int(static_cast<int64_t>(0), table_id))) {
  } else if (OB_FAIL(result.get_int(static_cast<int64_t>(1), column_id))) {
  } else if (OB_FAIL(get_string_copy(alloc, result, 2, row.rowkey_predicate_))) {
  } else if (OB_FAIL(get_string_copy(alloc, result, 3, row.request_id_))) {
  } else if (OB_FAIL(result.get_int(static_cast<int64_t>(4), row.state_))) {
  } else if (OB_FAIL(result.get_int(static_cast<int64_t>(5), row.retry_count_))) {
  } else if (OB_FAIL(get_string_copy(alloc, result, 6, row.database_name_))) {
  } else if (OB_FAIL(get_string_copy(alloc, result, 7, row.table_name_))) {
  } else if (OB_FAIL(get_string_copy(alloc, result, 8, row.column_name_))) {
  } else if (OB_FAIL(result.get_int(static_cast<int64_t>(9), row.max_retries_))) {
  } else if (OB_FAIL(get_string_copy(alloc, result, 10, row.normalized_expr_))) {
  } else {
    row.table_id_ = static_cast<uint64_t>(table_id);
    row.column_id_ = static_cast<uint64_t>(column_id);
  }
  return ret;
}

} // namespace

ObAiColumnWorker::ObAiColumnWorker()
  : lib::Threads(1),
    is_inited_(false),
    sql_proxy_(nullptr)
{
}

ObAiColumnWorker::~ObAiColumnWorker()
{
  destroy();
}

int ObAiColumnWorker::init(common::ObISQLClient &sql_proxy)
{
  int ret = OB_SUCCESS;
  if (is_inited_) {
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(lib::Threads::init())) {
    LOG_WARN("failed to init ai column worker threads", K(ret));
  } else {
    sql_proxy_ = &sql_proxy;
    is_inited_ = true;
  }
  return ret;
}

int ObAiColumnWorker::start()
{
  int ret = OB_SUCCESS;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(lib::Threads::start())) {
    LOG_WARN("failed to start ai column worker", K(ret));
  }
  return ret;
}

void ObAiColumnWorker::stop()
{
  if (is_inited_) {
    lib::Threads::stop();
  }
}

void ObAiColumnWorker::wait()
{
  if (is_inited_) {
    lib::Threads::wait();
  }
}

void ObAiColumnWorker::destroy()
{
  if (is_inited_) {
    lib::Threads::stop();
    lib::Threads::wait();
    lib::Threads::destroy();
    sql_proxy_ = nullptr;
    is_inited_ = false;
  }
}

void ObAiColumnWorker::run1()
{
  int ret = OB_SUCCESS;
  lib::set_thread_name("AiColumnWkr");
  while (!has_set_stop()) {
    bool has_work = false;
    if (share::SS_SERVING != GCTX.status_) {
      // The inner tables exist only once the server is serving, and queries against a
      // half-built server would only fill the log.
      ob_usleep(IDLE_SLEEP_US);
    } else if (OB_FAIL(do_one_batch(has_work))) {
      if (REACH_TIME_INTERVAL(10 * 1000 * 1000)) {
        LOG_WARN("failed to process ai column tasks", K(ret));
      }
      ob_usleep(IDLE_SLEEP_US);
    } else {
      ob_usleep(has_work ? BUSY_SLEEP_US : IDLE_SLEEP_US);
    }
  }
}

int ObAiColumnWorker::do_one_batch(bool &has_work)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColumnWkr");
  ObSEArray<share::ObAiColumnTaskRow, 16> rows;
  bool full_batch = false;
  int64_t group_start = 0;
  has_work = false;
  if (OB_FAIL(load_tasks(allocator, rows, full_batch))) {
    // the caller reports the failure once per interval
  }
  while (OB_SUCC(ret) && group_start < rows.count()) {
    int64_t group_end = group_start + 1;
    while (group_end < rows.count() && same_group(rows.at(group_start), rows.at(group_end))) {
      ++group_end;
    }
    // The LIMIT may cut the last group in half. Leaving it to the next batch keeps the columns
    // of that row together, and is only safe when some group was already drained, otherwise a
    // single group larger than the batch would stall.
    if (full_batch && group_end == rows.count() && group_start > 0) {
      break;
    }
    ObSEArray<share::ObAiColumnTaskRow, 8> group;
    for (int64_t i = group_start; OB_SUCC(ret) && i < group_end; ++i) {
      if (OB_FAIL(group.push_back(rows.at(i)))) {
        LOG_WARN("failed to record ai column task group", K(ret));
      }
    }
    if (OB_SUCC(ret)) {
      has_work = true;
      if (OB_FAIL(process_group(allocator, group))) {
        LOG_WARN("failed to process ai column task group", K(ret), K(group.count()));
      }
    }
    group_start = group_end;
  }
  return ret;
}

int ObAiColumnWorker::load_tasks(ObIAllocator &alloc,
                                 ObIArray<share::ObAiColumnTaskRow> &rows,
                                 bool &full_batch)
{
  int ret = OB_SUCCESS;
  ObString sql;
  sqlclient::ObISQLConnectionGuard guard;
  full_batch = false;
  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(share::ObAiColumnTaskHelper::build_load_tasks_sql(alloc, sql))) {
  } else if (OB_FAIL(sql_proxy_->acquire_connection(guard, SQL_GROUP_ID))) {
    LOG_WARN("failed to acquire connection for ai column tasks", K(ret));
  } else if (OB_ISNULL(guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObISQLClient::ReadResult result;
    if (OB_FAIL(guard.get_ptr()->execute_read(sql, result, false))) {
      LOG_WARN("failed to load ai column tasks", K(ret), K(sql));
    } else if (OB_ISNULL(result.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("ai column task result is null", K(ret));
    } else {
      sqlclient::ObMySQLResult &mysql_result = *result.get_result();
      while (OB_SUCC(ret)) {
        const int tmp_ret = mysql_result.next();
        if (OB_ITER_END == tmp_ret) {
          break;
        } else if (OB_SUCCESS != tmp_ret) {
          ret = tmp_ret;
          LOG_WARN("failed to read ai column task row", K(ret));
        } else {
          share::ObAiColumnTaskRow row;
          if (OB_FAIL(read_task_row(alloc, mysql_result, row))) {
            LOG_WARN("failed to parse ai column task row", K(ret));
          } else if (OB_FAIL(rows.push_back(row))) {
            LOG_WARN("failed to record ai column task", K(ret));
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    full_batch = (rows.count() >= share::ObAiColumnTaskHelper::LOAD_TASK_BATCH_SIZE);
  }
  return ret;
}

int ObAiColumnWorker::process_group(ObIAllocator &alloc,
                                    const ObIArray<share::ObAiColumnTaskRow> &group)
{
  int ret = OB_SUCCESS;
  ObString update_sql;
  ObString delete_sql;
  sqlclient::ObISQLConnectionGuard guard;
  bool committed = false;
  if (OB_ISNULL(sql_proxy_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(share::ObAiColumnTaskHelper::build_worker_update_sql(alloc, group, update_sql))) {
  } else if (OB_FAIL(share::ObAiColumnTaskHelper::build_group_delete_sql(alloc, group, delete_sql))) {
  } else if (OB_FAIL(sql_proxy_->acquire_connection(guard, SQL_GROUP_ID))) {
    LOG_WARN("failed to acquire connection for ai column update", K(ret));
  } else if (OB_ISNULL(guard.get_ptr())) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    sqlclient::ObISQLConnection *conn = guard.get_ptr();
    int64_t affected_rows = 0;
    if (OB_FAIL(conn->start_transaction())) {
      LOG_WARN("failed to start ai column transaction", K(ret));
    } else if (OB_FAIL(conn->execute_write(update_sql, affected_rows, false))) {
    } else if (OB_FAIL(conn->execute_write(delete_sql, affected_rows, false))) {
    } else if (OB_FAIL(conn->commit())) {
      LOG_WARN("failed to commit ai column transaction", K(ret));
    } else {
      committed = true;
    }
    if (OB_FAIL(ret)) {
      const int tmp_ret = conn->rollback();
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to rollback ai column transaction", K(tmp_ret));
      }
    }
  }
  if (OB_FAIL(ret) && !committed) {
    const int saved_ret = ret;
    ret = OB_SUCCESS;
    LOG_WARN("failed to recompute ai columns, will retry", K(saved_ret), K(group.count()));
    if (OB_FAIL(fail_group(alloc, group))) {
      LOG_WARN("failed to record ai column task failure", K(ret), K(saved_ret));
    }
  }
  return ret;
}

int ObAiColumnWorker::fail_group(ObIAllocator &alloc,
                                 const ObIArray<share::ObAiColumnTaskRow> &group)
{
  int ret = OB_SUCCESS;
  ObSEArray<share::ObAiColumnTaskRow, 8> retry_rows;
  ObSEArray<share::ObAiColumnTaskRow, 8> give_up_rows;
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const share::ObAiColumnTaskRow &row = group.at(i);
    if (row.retry_count_ < row.max_retries_) {
      if (OB_FAIL(retry_rows.push_back(row))) {
      }
    } else if (OB_FAIL(give_up_rows.push_back(row))) {
    }
  }
  if (OB_SUCC(ret) && (!retry_rows.empty() || !give_up_rows.empty())) {
    ObString retry_sql;
    ObString drop_sql;
    sqlclient::ObISQLConnectionGuard guard;
    if (!retry_rows.empty()
        && OB_FAIL(share::ObAiColumnTaskHelper::build_group_retry_sql(alloc, retry_rows, retry_sql))) {
    } else if (!give_up_rows.empty()
               && OB_FAIL(share::ObAiColumnTaskHelper::build_group_delete_sql(alloc,
                                                                             give_up_rows,
                                                                             drop_sql))) {
    } else if (OB_FAIL(sql_proxy_->acquire_connection(guard, SQL_GROUP_ID))) {
      LOG_WARN("failed to acquire connection for ai column retry", K(ret));
    } else if (OB_ISNULL(guard.get_ptr())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int64_t affected_rows = 0;
      if (!retry_sql.empty()) {
        if (OB_FAIL(guard.get_ptr()->execute_write(retry_sql, affected_rows, false))) {
          LOG_WARN("failed to requeue ai column tasks", K(ret));
        }
      }
      if (OB_SUCC(ret) && !drop_sql.empty()) {
        if (OB_FAIL(guard.get_ptr()->execute_write(drop_sql, affected_rows, false))) {
          LOG_WARN("failed to drop ai column tasks", K(ret));
        } else {
          LOG_WARN("ai column task exhausted its retries and was dropped",
                   K(affected_rows), K(group.count()));
        }
      }
    }
  }
  return ret;
}

} // namespace observer
} // namespace oceanbase

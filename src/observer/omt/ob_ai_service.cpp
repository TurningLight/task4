/**
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

#define USING_LOG_PREFIX SERVER_OMT
#include <unistd.h>
#include <string.h>
#include "ob_ai_service.h"
#include "observer/ai_service/ob_ai_service_executor.h"
#include "sql/privilege_check/ob_ai_model_priv_util.h"
#include "sql/session/ob_sql_session_info.h"
#include "sql/engine/ob_exec_context.h"
#include "sql/ob_ai_column_worker_env.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_mysql_transaction.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"

using namespace oceanbase::share;
using namespace oceanbase::common;
namespace oceanbase
{
namespace omt
{

namespace
{

// Queue of the AI COLUMN feature. The DML hook of ObSql::stmt_query inserts one
// row per (statement, AI column, changed row) inside the caller's transaction.
const char *const AI_TASK_TABLE_NAME = "oceanbase.__all_ai_column_task";
const char *const AI_REQUEST_COLUMN_PREFIX = "__ai_request_";

// state values of __all_ai_column_task
const int64_t AI_TASK_STATE_PENDING = 0;
const int64_t AI_TASK_STATE_RETRYING = 3;

const int64_t AI_WORKER_BATCH_SIZE = 32;
const int64_t AI_WORKER_POLL_INTERVAL_US = 50 * 1000;
const int64_t AI_WORKER_RETRY_DELAY_SEC = 1;
// the AI call result is materialized as a plain varchar so that the inner SQL
// result never hands back a lob locator instead of the text
const int64_t AI_WORKER_TEXT_LENGTH = 16384;

struct AiColumnTaskRow
{
  AiColumnTaskRow()
    : table_id_(OB_INVALID_ID), request_id_(0), column_id_(OB_INVALID_ID),
      retry_count_(0), max_retries_(0)
  {}
  uint64_t table_id_;
  int64_t request_id_;
  uint64_t column_id_;
  int64_t retry_count_;
  int64_t max_retries_;
  common::ObString predicate_;
  common::ObString column_name_;
  common::ObString ai_expr_;
};

int deep_copy_string(common::ObIAllocator &alloc,
                     const common::ObString &src,
                     common::ObString &dst)
{
  int ret = OB_SUCCESS;
  dst.reset();
  if (!src.empty()) {
    char *buf = static_cast<char *>(alloc.alloc(src.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("no memory to copy ai column task text", KR(ret), "len", src.length());
    } else {
      MEMCPY(buf, src.ptr(), src.length());
      dst.assign_ptr(buf, src.length());
    }
  }
  return ret;
}

// Append one SQL string literal, MySQL style. Both the quote and the backslash
// are doubled, so any model reply can be written back verbatim.
int append_escaped(common::ObSqlString &sql, const common::ObString &text)
{
  int ret = OB_SUCCESS;
  char pair[2] = { '\0', '\0' };
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
    pair[0] = text.ptr()[i];
    if ('\'' == pair[0] || '\\' == pair[0]) {
      pair[1] = pair[0];
      if (OB_FAIL(sql.append(pair, 2))) {
      }
    } else if (OB_FAIL(sql.append(pair, 1))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}

template <typename SQL_CLIENT>
int read_int(SQL_CLIENT &client, const common::ObSqlString &sql, int64_t &value)
{
  int ret = OB_SUCCESS;
  value = 0;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = nullptr;
    if (OB_FAIL(client.read(res, sql.ptr()))) {
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
    } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), value))) {
    }
  }
  return ret;
}

// Returns the single scalar of a one column, one row query. has_row is false
// when the query simply produced no row, which the caller reads as "the row is
// gone" rather than as a failure.
template <typename SQL_CLIENT>
int read_text(SQL_CLIENT &client,
              const common::ObSqlString &sql,
              common::ObIAllocator &alloc,
              common::ObString &value,
              bool &has_row)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  has_row = false;
  value.reset();
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    sqlclient::ObMySQLResult *result = nullptr;
    if (OB_FAIL(client.read(res, sql.ptr()))) {
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_SUCCESS != (tmp_ret = result->next())) {
      if (OB_ITER_END == tmp_ret) {
        has_row = false;
      } else {
        ret = tmp_ret;
      }
    } else {
      common::ObString text;
      has_row = true;
      if (OB_FAIL(result->get_varchar(static_cast<int64_t>(0), text))) {
      } else if (OB_FAIL(deep_copy_string(alloc, text, value))) {
      }
    }
  }
  return ret;
}

template <typename SQL_CLIENT>
int write_sql(SQL_CLIENT &client, const common::ObSqlString &sql, int64_t &affected_rows)
{
  int ret = OB_SUCCESS;
  affected_rows = 0;
  if (OB_FAIL(client.write(sql.ptr(), affected_rows))) {
  }
  return ret;
}

int fetch_pending_tasks(common::ObIAllocator &alloc,
                        common::ObIArray<AiColumnTaskRow> &rows)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append_fmt(
               "select table_id, request_id, column_id, retry_count, max_retries, "
               "cast(rowkey_predicate as char(%ld)), cast(column_name as char(512)), "
               "cast(ai_expr as char(8192)) from %s "
               "where state in (%ld, %ld) and eligible_ts <= now(6) "
               "order by table_id, request_id, column_id limit %ld",
               AI_WORKER_TEXT_LENGTH, AI_TASK_TABLE_NAME,
               AI_TASK_STATE_PENDING, AI_TASK_STATE_RETRYING,
               AI_WORKER_BATCH_SIZE))) {
    LOG_WARN("failed to build the ai column task query", KR(ret));
  } else {
    SMART_VAR(ObMySQLProxy::MySQLResult, res) {
      sqlclient::ObMySQLResult *result = nullptr;
      if (OB_FAIL(GCTX.sql_proxy_->read(res, sql.ptr()))) {
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        while (OB_SUCC(ret)) {
          int tmp_ret = result->next();
          if (OB_ITER_END == tmp_ret) {
            break;
          } else if (OB_SUCCESS != tmp_ret) {
            ret = tmp_ret;
          } else {
            AiColumnTaskRow row;
            int64_t table_id = 0;
            int64_t request_id = 0;
            int64_t column_id = 0;
            int64_t retry_count = 0;
            int64_t max_retries = 0;
            common::ObString predicate;
            common::ObString column_name;
            common::ObString ai_expr;
            if (OB_FAIL(result->get_int(static_cast<int64_t>(0), table_id))) {
            } else if (OB_FAIL(result->get_int(1, request_id))) {
            } else if (OB_FAIL(result->get_int(2, column_id))) {
            } else if (OB_FAIL(result->get_int(3, retry_count))) {
            } else if (OB_FAIL(result->get_int(4, max_retries))) {
            } else if (OB_FAIL(result->get_varchar(5, predicate))) {
            } else if (OB_FAIL(result->get_varchar(6, column_name))) {
            } else if (OB_FAIL(result->get_varchar(7, ai_expr))) {
            } else if (FALSE_IT(row.table_id_ = static_cast<uint64_t>(table_id))) {
            } else if (FALSE_IT(row.request_id_ = request_id)) {
            } else if (FALSE_IT(row.column_id_ = static_cast<uint64_t>(column_id))) {
            } else if (FALSE_IT(row.retry_count_ = retry_count)) {
            } else if (FALSE_IT(row.max_retries_ = max_retries)) {
            } else if (OB_FAIL(deep_copy_string(alloc, predicate, row.predicate_))) {
            } else if (OB_FAIL(deep_copy_string(alloc, column_name, row.column_name_))) {
            } else if (OB_FAIL(deep_copy_string(alloc, ai_expr, row.ai_expr_))) {
            } else if (OB_FAIL(rows.push_back(row))) {
            }
          }
        }
      }
    }
  }
  return ret;
}

// The type hint sentence of an AI column lives in the comment of its hidden
// request column, i.e. in the normalized expression rendered at DDL time:
//   Return exactly one valid JSON value.\nAI_COMPLETE('model', concat(...))
// The first line (when there is one) is the system message of the request.
int get_system_hint(const share::schema::ObTableSchema &table_schema,
                    const common::ObString &column_name,
                    common::ObString &hint)
{
  int ret = OB_SUCCESS;
  common::ObSqlString name_buf;
  const share::schema::ObColumnSchemaV2 *request_column = nullptr;
  hint.reset();
  if (OB_FAIL(name_buf.append(AI_REQUEST_COLUMN_PREFIX))) {
  } else if (OB_FAIL(name_buf.append(column_name))) {
  } else if (OB_ISNULL(request_column = table_schema.get_column_schema(name_buf.string()))) {
    // the table is not an AI column table any more
  } else {
    const common::ObString &comment = request_column->get_comment_str();
    const char *newline = comment.empty()
        ? nullptr
        : static_cast<const char *>(memchr(comment.ptr(), '\n', comment.length()));
    if (nullptr != newline) {
      hint.assign_ptr(comment.ptr(), static_cast<int32_t>(newline - comment.ptr()));
    }
  }
  return ret;
}

int mark_task_retry(const AiColumnTaskRow &row)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append_fmt(
               "update %s set state = %ld, retry_count = %ld, "
               "eligible_ts = now(6) + interval %ld second "
               "where table_id = %lu and request_id = %ld and column_id = %lu",
               AI_TASK_TABLE_NAME, AI_TASK_STATE_RETRYING, row.retry_count_ + 1,
               AI_WORKER_RETRY_DELAY_SEC, static_cast<unsigned long>(row.table_id_),
               row.request_id_, static_cast<unsigned long>(row.column_id_)))) {
  } else {
    int64_t affected_rows = 0;
    if (OB_FAIL(write_sql(*GCTX.sql_proxy_, sql, affected_rows))) {
      LOG_WARN("failed to put an ai column task back to retrying", KR(ret),
               "column_id", row.column_id_);
    }
  }
  return ret;
}

int drop_task(const AiColumnTaskRow &row)
{
  int ret = OB_SUCCESS;
  common::ObSqlString sql;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(sql.append_fmt(
               "delete from %s where table_id = %lu and request_id = %ld and column_id = %lu",
               AI_TASK_TABLE_NAME, static_cast<unsigned long>(row.table_id_),
               row.request_id_, static_cast<unsigned long>(row.column_id_)))) {
  } else {
    int64_t affected_rows = 0;
    if (OB_FAIL(write_sql(*GCTX.sql_proxy_, sql, affected_rows))) {
      LOG_WARN("failed to drop an ai column task", KR(ret), "column_id", row.column_id_);
    }
  }
  return ret;
}

int publish_row_group(common::ObIAllocator &alloc,
                      const uint64_t table_id,
                      const int64_t request_id,
                      const common::ObString &db_name,
                      const common::ObString &table_name,
                      const common::ObString &predicate,
                      const common::ObIArray<AiColumnTaskRow> &rows,
                      const common::ObIArray<int64_t> &ok_pos,
                      const common::ObIArray<common::ObString> &ok_values)
{
  int ret = OB_SUCCESS;
  int tmp_ret = OB_SUCCESS;
  common::ObMySQLTransaction trans;
  bool superseded = false;
  int64_t affected_rows = 0;
  UNUSED(alloc);
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(trans.start(GCTX.sql_proxy_))) {
    LOG_WARN("failed to start the ai column publish transaction", KR(ret));
  } else {
    {
      // Stale guard: the DML hook deletes the pending tasks of a row whenever a
      // newer statement touches it. If any of our task rows is already gone the
      // value we just computed describes a row version that no longer exists,
      // so the result must be thrown away instead of overwriting the newer one.
      common::ObSqlString sql;
      int64_t alive = 0;
      if (OB_FAIL(sql.append_fmt(
                   "select count(*) from %s where table_id = %lu and request_id = %ld "
                   "and rowkey_predicate = ",
                   AI_TASK_TABLE_NAME, static_cast<unsigned long>(table_id), request_id))) {
      } else if (OB_FAIL(append_escaped(sql, predicate))) {
      } else if (OB_FAIL(read_int(trans, sql, alive))) {
      } else if (alive < ok_pos.count()) {
        superseded = true;
        LOG_INFO("ai column result superseded by a newer statement", K(table_id),
                 K(request_id), K(alive), "computed", ok_pos.count());
      }
    }
    if (OB_SUCC(ret) && !superseded) {
      common::ObSqlString update;
      if (OB_FAIL(update.append_fmt("update `%.*s`.`%.*s` set ",
                                    db_name.length(), db_name.ptr(),
                                    table_name.length(), table_name.ptr()))) {
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < ok_pos.count(); ++i) {
        const AiColumnTaskRow &row = rows.at(ok_pos.at(i));
        if (OB_FAIL(update.append_fmt("%s`%.*s` = ", i > 0 ? ", " : "",
                                      row.column_name_.length(),
                                      row.column_name_.ptr()))) {
        } else if (OB_FAIL(append_escaped(update, ok_values.at(i)))) {
        }
      }
      if (OB_SUCC(ret)
          && OB_FAIL(update.append_fmt(" where %.*s", predicate.length(), predicate.ptr()))) {
      } else if (OB_SUCC(ret) && OB_FAIL(write_sql(trans, update, affected_rows))) {
        LOG_WARN("failed to write the ai column results back", KR(ret));
      }
    }
    if (OB_SUCC(ret)) {
      // The task rows are retired in the very same transaction as the publish,
      // so a group is always either fully published and dequeued, or untouched.
      common::ObSqlString del;
      if (OB_FAIL(del.append_fmt(
                   "delete from %s where table_id = %lu and request_id = %ld "
                   "and rowkey_predicate = ",
                   AI_TASK_TABLE_NAME, static_cast<unsigned long>(table_id), request_id))) {
      } else if (OB_FAIL(append_escaped(del, predicate))) {
      } else if (OB_FAIL(write_sql(trans, del, affected_rows))) {
        LOG_WARN("failed to retire the ai column tasks", KR(ret));
      }
    }
  }
  if (trans.is_started()) {
    tmp_ret = trans.end(OB_SUCC(ret));
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to end the ai column publish transaction", KR(ret), KR(tmp_ret));
      if (OB_SUCC(ret)) {
        ret = tmp_ret;
      }
    }
  }
  return ret;
}

int process_row_group(const share::schema::ObTableSchema &table_schema,
                      const common::ObString &db_name,
                      const common::ObString &table_name,
                      const uint64_t table_id,
                      const int64_t request_id,
                      const common::ObIArray<AiColumnTaskRow> &rows,
                      const int64_t begin,
                      const int64_t end)
{
  int ret = OB_SUCCESS;
  common::ObArenaAllocator allocator("AiColWorker");
  common::ObSEArray<int64_t, 4> ok_pos;
  common::ObSEArray<common::ObString, 4> ok_values;
  common::ObSEArray<int64_t, 4> failed_pos;
  common::ObSEArray<int64_t, 4> obsolete_pos;
  for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
    const AiColumnTaskRow &row = rows.at(i);
    common::ObString hint;
    common::ObSqlString sql;
    if (OB_FAIL(get_system_hint(table_schema, row.column_name_, hint))) {
      LOG_WARN("failed to read the ai column type hint", KR(ret), K(row.column_name_));
    } else if (OB_FAIL(sql.append("select cast("))) {
    } else if (OB_FAIL(sql.append(row.ai_expr_))) {
    } else if (OB_FAIL(sql.append_fmt(" as char(%ld)) from `%.*s`.`%.*s` where %.*s",
                                      AI_WORKER_TEXT_LENGTH,
                                      db_name.length(), db_name.ptr(),
                                      table_name.length(), table_name.ptr(),
                                      row.predicate_.length(), row.predicate_.ptr()))) {
    } else {
      common::ObString value;
      bool has_row = false;
      int tmp_ret = OB_SUCCESS;
      sql::ObAiColumnWorkerEnv::set_system_prompt(hint);
      tmp_ret = read_text(*GCTX.sql_proxy_, sql, allocator, value, has_row);
      sql::ObAiColumnWorkerEnv::reset_system_prompt();
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("ai column model request failed", KR(tmp_ret), K(row.column_name_),
                 "retry_count", row.retry_count_, "max_retries", row.max_retries_);
        if (OB_FAIL(failed_pos.push_back(i))) {
        }
      } else if (!has_row) {
        // the row was deleted while the task was queued
        if (OB_FAIL(obsolete_pos.push_back(i))) {
        }
      } else if (OB_FAIL(ok_pos.push_back(i))) {
      } else if (OB_FAIL(ok_values.push_back(value))) {
      }
    }
  }
  if (OB_SUCC(ret) && !ok_pos.empty()) {
    int tmp_ret = publish_row_group(allocator, table_id, request_id, db_name, table_name,
                                    rows.at(begin).predicate_, rows, ok_pos, ok_values);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to publish the ai column results", KR(tmp_ret), K(table_id),
               K(request_id), "column_count", ok_pos.count());
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < failed_pos.count(); ++i) {
    const AiColumnTaskRow &row = rows.at(failed_pos.at(i));
    int tmp_ret = OB_SUCCESS;
    if (row.retry_count_ >= row.max_retries_) {
      // the retry budget of this task is exhausted: keep the column NULL and
      // make sure the task itself does not stay in the queue
      tmp_ret = drop_task(row);
      LOG_INFO("ai column task failed permanently", K(row.column_name_), K(row.retry_count_),
               K(row.max_retries_));
    } else {
      tmp_ret = mark_task_retry(row);
    }
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to update an ai column task state", KR(tmp_ret));
    }
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < obsolete_pos.count(); ++i) {
    const AiColumnTaskRow &row = rows.at(obsolete_pos.at(i));
    int tmp_ret = drop_task(row);
    if (OB_SUCCESS != tmp_ret) {
      LOG_WARN("failed to drop an obsolete ai column task", KR(tmp_ret));
    }
  }
  return ret;
}

int process_request_group(common::ObIAllocator &alloc,
                          const common::ObIArray<AiColumnTaskRow> &rows,
                          const int64_t begin,
                          const int64_t end)
{
  int ret = OB_SUCCESS;
  const uint64_t table_id = rows.at(begin).table_id_;
  const int64_t request_id = rows.at(begin).request_id_;
  share::schema::ObSchemaGetterGuard guard;
  const share::schema::ObTableSchema *table_schema = nullptr;
  const share::schema::ObDatabaseSchema *db_schema = nullptr;
  UNUSED(alloc);
  if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(guard))) {
  } else if (OB_FAIL(guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    // the table was dropped while the task was queued
    LOG_INFO("ai column task for a dropped table is discarded", K(table_id), K(request_id));
    for (int64_t i = begin; OB_SUCC(ret) && i < end; ++i) {
      int tmp_ret = drop_task(rows.at(i));
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to drop an ai column task of a dropped table", KR(tmp_ret));
      }
    }
  } else if (OB_FAIL(guard.get_database_schema(table_schema->get_database_id(), db_schema))) {
  } else if (OB_ISNULL(db_schema)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const common::ObString &db_name = db_schema->get_database_name_str();
    const common::ObString &table_name = table_schema->get_table_name_str();
    for (int64_t i = begin; OB_SUCC(ret) && i < end;) {
      int64_t j = i + 1;
      while (j < end && 0 == rows.at(j).predicate_.compare(rows.at(i).predicate_)) {
        ++j;
      }
      int tmp_ret = process_row_group(*table_schema, db_name, table_name, table_id,
                                      request_id, rows, i, j);
      if (OB_SUCCESS != tmp_ret) {
        LOG_WARN("failed to process an ai column row group", KR(tmp_ret), K(table_id),
                 K(request_id));
      }
      i = j;
    }
  }
  return ret;
}

} // anonymous namespace

ObAiColumnWorker::ObAiColumnWorker()
  : lib::Threads(1)
{
}

void ObAiColumnWorker::run1()
{
  // Every statement this thread runs is part of the AI COLUMN machinery and
  // must not be re-interpreted as user DML by the AI COLUMN hook.
  sql::ObAiColumnWorkerGuard worker_guard;
  LOG_INFO("ai column worker starts");
  while (!has_set_stop()) {
    poll_once();
    for (int64_t slept = 0; slept < AI_WORKER_POLL_INTERVAL_US && !has_set_stop();
         slept += 10 * 1000) {
      ::usleep(10 * 1000);
    }
  }
  LOG_INFO("ai column worker exits");
}

void ObAiColumnWorker::poll_once()
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.sql_proxy_) || OB_ISNULL(GCTX.schema_service_)) {
    // the observer is still booting
  } else {
    common::ObArenaAllocator allocator("AiColWorker");
    common::ObSEArray<AiColumnTaskRow, 8> rows;
    if (OB_FAIL(fetch_pending_tasks(allocator, rows))) {
      LOG_WARN("ai column worker failed to read the task queue", KR(ret));
    } else {
      int64_t i = 0;
      while (OB_SUCC(ret) && i < rows.count()) {
        int64_t j = i + 1;
        while (j < rows.count() && rows.at(j).table_id_ == rows.at(i).table_id_
               && rows.at(j).request_id_ == rows.at(i).request_id_) {
          ++j;
        }
        int tmp_ret = process_request_group(allocator, rows, i, j);
        if (OB_SUCCESS != tmp_ret) {
          LOG_WARN("ai column worker failed to process a request group", KR(tmp_ret),
                   "table_id", rows.at(i).table_id_, "request_id", rows.at(i).request_id_);
        }
        i = j;
      }
    }
  }
}

ObAiService::ObAiService()
  : is_inited_(false),
    ai_column_worker_()
{
}

ObAiService::~ObAiService()
{
  destroy();
}

int ObAiService::server_module_init(ObAiService* &ai_service)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(ai_service)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ai_service->init())) {
  }
  return ret;
}

int ObAiService::init()
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
  } else {
    is_inited_ = true;
  }
  return ret;
}

int ObAiService::start()
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("ai service is not inited", KR(ret));
  } else if (OB_FAIL(ai_column_worker_.set_thread_count(1))) {
    LOG_WARN("failed to set the ai column worker thread count", KR(ret));
  } else if (OB_FAIL(ai_column_worker_.start())) {
    LOG_WARN("failed to start the ai column worker", KR(ret));
  } else {
    LOG_INFO("ai column worker started");
  }
  if (OB_FAIL(ret)) {
    // the AI COLUMN worker only serves an optional feature of the runtime
    LOG_ERROR("ai column worker unavailable", KR(ret));
    ret = OB_SUCCESS;
  }
  return ret;
}

void ObAiService::stop()
{
  ai_column_worker_.stop();
}

void ObAiService::wait()
{
  ai_column_worker_.wait();
}

void ObAiService::destroy()
{
  if (is_inited_) {
    ai_column_worker_.stop();
    ai_column_worker_.wait();
    ai_column_worker_.destroy();
    is_inited_ = false;
  }
}

int ObAiService::get_ai_service_guard(ObAiServiceGuard &ai_service_guard)
{
  // TODO: implement after cache support
  int ret = OB_SUCCESS;
  UNUSED(ai_service_guard);
  return ret;
}

ObAiServiceGuard::ObAiServiceGuard()
  : local_allocator_(ObMemAttr("AiServiceGuard", ObCtxIds::DEFAULT_CTX_ID))
{
}

ObAiServiceGuard::~ObAiServiceGuard()
{
}

int ObAiServiceGuard::check_access_privilege()
{
  int ret = OB_SUCCESS;
  bool has_priv = false;

  sql::ObSQLSessionInfo *session = nullptr;
  if (OB_ISNULL(session = THIS_WORKER.get_session())) {
    // system background task thread do not have session, skip check
  } else {
    ObArenaAllocator tmp_allocator;
    share::schema::ObSchemaGetterGuard *schema_guard = session->get_cur_exec_ctx()->get_sql_ctx()->schema_guard_;
    if (OB_ISNULL(schema_guard)) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      sql::ObAIServiceEndpointPrivUtil priv_util(*schema_guard);
      share::schema::ObSessionPrivInfo session_priv;
      uint64_t user_id = session->get_priv_user_id();
      if (user_id == OB_INVALID_ID) {
        user_id = OB_SYS_USER_ID;
      }
      if (OB_FAIL(schema_guard->get_session_priv_info(
                                                    user_id,
                                                    session->get_database_name(),
                                                    session_priv))) {
      } else if (OB_FAIL(priv_util.check_access_ai_model_priv(tmp_allocator, session_priv, has_priv))) {
      } else if (!has_priv) {
        ret = OB_ERR_NO_PRIVILEGE;
        LOG_WARN("no privilege for ai model access", K(ret));
        LOG_USER_ERROR(OB_ERR_NO_PRIVILEGE, "access ai model");
      }
    }
  }

  return ret;
}

int ObAiServiceGuard::get_ai_endpoint(const common::ObString &name, const share::ObAiModelEndpointInfo *&endpoint_info)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (OB_FAIL(check_access_privilege())) {
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint(local_allocator_, name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

int ObAiServiceGuard::get_ai_endpoint_by_ai_model_name(const common::ObString &ai_model_name, const share::ObAiModelEndpointInfo *&endpoint_info, bool need_check)
{
  int ret = OB_SUCCESS;
  ObAiModelEndpointInfo *tmp_endpoint_info = nullptr;

  if (need_check && OB_FAIL(check_access_privilege())) {
    LOG_WARN("failed to check access privilege", K(ret));
  } else if (OB_ISNULL(tmp_endpoint_info = OB_NEWx(ObAiModelEndpointInfo, &local_allocator_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_FAIL(ObAiServiceExecutor::read_ai_endpoint_by_ai_model_name(local_allocator_, ai_model_name, *tmp_endpoint_info))) {
  } else {
    endpoint_info = tmp_endpoint_info;
  }
  return ret;
}

} // namespace omt
} // namespace oceanbase

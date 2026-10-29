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

#include "share/ai_column/ob_ai_column_task.h"
#include "share/ai_column/ob_ai_column_utils.h"
#include "share/ob_errno.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/schema/ob_table_schema.h"
#include "lib/ob_define.h"
#include "lib/string/ob_sql_string.h"
#include "lib/time/ob_time_utility.h"
#include "lib/atomic/ob_atomic.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{

namespace
{

// Single quoted literal. The predicates and request ids only contain identifier characters,
// but quoting is still done properly so that a wider predicate can never break the statement.
int append_string_literal(ObSqlString &sql, const ObString &value)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("'"))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < value.length(); ++i) {
    const char c = value[i];
    if ('\'' == c) {
      if (OB_FAIL(sql.append("''"))) {
      }
    } else if (OB_FAIL(sql.append(&c, 1))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append("'"))) {
  }
  return ret;
}

int append_qualified_name(ObSqlString &sql, const ObString &database_name, const ObString &table_name)
{
  int ret = OB_SUCCESS;
  if (!database_name.empty()) {
    if (OB_FAIL(sql.append("`"))) {
    } else if (OB_FAIL(sql.append(database_name))) {
    } else if (OB_FAIL(sql.append("`."))) {
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(sql.append("`"))) {
    } else if (OB_FAIL(sql.append(table_name))) {
    } else if (OB_FAIL(sql.append("`"))) {
    }
  }
  return ret;
}

int append_column_list(ObSqlString &sql, const ObIArray<ObAiColumnTaskRow> &rows)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(sql.append("("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
    if (i > 0 && OB_FAIL(sql.append(", "))) {
    } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("%ld", static_cast<int64_t>(rows.at(i).column_id_)))) {
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append(")"))) {
  }
  return ret;
}

// `where table_id = .. and column_id in (..) and rowkey_predicate = '..' and request_id = '..'`,
// i.e. the exact task rows of one group.
int append_group_predicate(ObSqlString &sql, const ObIArray<ObAiColumnTaskRow> &rows)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(rows.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append_fmt(" where table_id = %ld and column_id in ",
                                    static_cast<int64_t>(rows.at(0).table_id_)))) {
  } else if (OB_FAIL(append_column_list(sql, rows))) {
  } else if (OB_FAIL(sql.append(" and rowkey_predicate = "))) {
  } else if (OB_FAIL(append_string_literal(sql, rows.at(0).rowkey_predicate_))) {
  } else if (OB_FAIL(sql.append(" and request_id = "))) {
  } else if (OB_FAIL(append_string_literal(sql, rows.at(0).request_id_))) {
  }
  return ret;
}

} // namespace

int ObAiColumnTaskHelper::build_rowkey_predicate_expr(ObIAllocator &alloc,
                                                      const schema::ObTableSchema &table_schema,
                                                      ObString &predicate_expr)
{
  int ret = OB_SUCCESS;
  common::ObSEArray<uint64_t, 4> rowkey_ids;
  ObSqlString sql;
  if (OB_FAIL(table_schema.get_rowkey_column_ids(rowkey_ids))) {
    LOG_WARN("failed to get rowkey column ids", K(ret), K(table_schema.get_table_id()));
  } else if (rowkey_ids.empty()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column table has no rowkey", K(ret), K(table_schema.get_table_id()));
  } else if (OB_FAIL(sql.append("concat("))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_ids.count(); ++i) {
    const schema::ObColumnSchemaV2 *column = table_schema.get_column_schema(rowkey_ids.at(i));
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("rowkey column schema is null", K(ret), K(rowkey_ids.at(i)));
    } else {
      const ObString name = column->get_column_name_str();
      if (i > 0 && OB_FAIL(sql.append(", ' and "))) {
      } else if (OB_SUCC(ret) && OB_FAIL(sql.append_fmt("'`%.*s` = ', cast(`%.*s` as char)",
                                                        name.length(), name.ptr(),
                                                        name.length(), name.ptr()))) {
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(sql.append(")"))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc,
                                              ObString(static_cast<int32_t>(sql.length()), sql.ptr()),
                                              predicate_expr))) {
  }
  return ret;
}

int ObAiColumnTaskHelper::generate_request_id(ObIAllocator &alloc, ObString &request_id)
{
  int ret = OB_SUCCESS;
  static int64_t request_seq = 0;
  ObSqlString id;
  const int64_t seq = ATOMIC_AAF(&request_seq, 1);
  if (OB_FAIL(id.append_fmt("aic-%ld-%ld", ObTimeUtility::current_time(), seq))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(id.length()), id.ptr()),
                                     request_id))) {
  }
  return ret;
}

int ObAiColumnTaskHelper::build_enqueue_sql(ObIAllocator &alloc,
                                            const uint64_t table_id,
                                            const uint64_t column_id,
                                            const ObString &database_name,
                                            const ObString &table_name,
                                            const ObString &column_name,
                                            const ObString &predicate_expr,
                                            const ObString &request_id,
                                            ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  if (OB_FAIL(buffer.append_fmt("replace into oceanbase.%s"
                                "(table_id, column_id, rowkey_predicate, request_id, state,"
                                " retry_count, eligible_ts) select %ld, %ld, ",
                                OB_ALL_AI_COLUMN_TASK_TNAME,
                                static_cast<int64_t>(table_id),
                                static_cast<int64_t>(column_id)))) {
  } else if (OB_FAIL(buffer.append(predicate_expr))) {
  } else if (OB_FAIL(buffer.append(", "))) {
  } else if (OB_FAIL(append_string_literal(buffer, request_id))) {
  } else if (OB_FAIL(buffer.append_fmt(", %ld, 0, now(6) from ",
                                       static_cast<int64_t>(OB_AI_COLUMN_TASK_PENDING)))) {
  } else if (OB_FAIL(append_qualified_name(buffer, database_name, table_name))) {
  } else if (OB_FAIL(buffer.append(" where `"))) {
  } else if (OB_FAIL(buffer.append(column_name))) {
  } else if (OB_FAIL(buffer.append("` is null"))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(buffer.length()), buffer.ptr()),
                                     sql))) {
  }
  return ret;
}

int ObAiColumnTaskHelper::build_cleanup_sql(ObIAllocator &alloc,
                                            const uint64_t table_id,
                                            const uint64_t column_id,
                                            const ObString &database_name,
                                            const ObString &table_name,
                                            const ObString &column_name,
                                            const ObString &predicate_expr,
                                            ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  if (OB_FAIL(buffer.append_fmt("delete from oceanbase.%s where table_id = %ld and column_id = %ld"
                                " and rowkey_predicate not in (select ",
                                OB_ALL_AI_COLUMN_TASK_TNAME,
                                static_cast<int64_t>(table_id),
                                static_cast<int64_t>(column_id)))) {
  } else if (OB_FAIL(buffer.append(predicate_expr))) {
  } else if (OB_FAIL(buffer.append(" from "))) {
  } else if (OB_FAIL(append_qualified_name(buffer, database_name, table_name))) {
  } else if (OB_FAIL(buffer.append(" where `"))) {
  } else if (OB_FAIL(buffer.append(column_name))) {
  } else if (OB_FAIL(buffer.append("` is null)"))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(buffer.length()), buffer.ptr()),
                                     sql))) {
  }
  return ret;
}

int ObAiColumnTaskHelper::build_load_tasks_sql(ObIAllocator &alloc, ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  if (OB_FAIL(buffer.append_fmt("select task.table_id, task.column_id, task.rowkey_predicate,"
                                " task.request_id, task.state, task.retry_count,"
                                " meta.database_name, meta.table_name, meta.column_name,"
                                " meta.max_retries, meta.normalized_expr"
                                " from oceanbase.%s task join oceanbase.%s meta"
                                " on meta.table_id = task.table_id"
                                " and meta.column_id = task.column_id"
                                " where task.state in (%ld, %ld) and task.eligible_ts <= now(6)"
                                " order by task.table_id, task.request_id, task.rowkey_predicate"
                                " limit %ld",
                                OB_ALL_AI_COLUMN_TASK_TNAME,
                                OB_ALL_AI_COLUMN_TNAME,
                                static_cast<int64_t>(OB_AI_COLUMN_TASK_PENDING),
                                static_cast<int64_t>(OB_AI_COLUMN_TASK_RETRY),
                                LOAD_TASK_BATCH_SIZE))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(buffer.length()), buffer.ptr()),
                                     sql))) {
  }
  return ret;
}

int ObAiColumnTaskHelper::build_worker_update_sql(ObIAllocator &alloc,
                                                  const ObIArray<ObAiColumnTaskRow> &group,
                                                  ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  if (OB_UNLIKELY(group.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buffer.append("update "))) {
  } else if (OB_FAIL(append_qualified_name(buffer,
                                           group.at(0).database_name_,
                                           group.at(0).table_name_))) {
  } else if (OB_FAIL(buffer.append(" set "))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < group.count(); ++i) {
    const ObAiColumnTaskRow &row = group.at(i);
    if (i > 0 && OB_FAIL(buffer.append(", "))) {
    } else if (OB_SUCC(ret) && OB_FAIL(buffer.append("`"))) {
    } else if (OB_SUCC(ret) && OB_FAIL(buffer.append(row.column_name_))) {
    } else if (OB_SUCC(ret) && OB_FAIL(buffer.append("` = "))) {
    } else if (OB_SUCC(ret) && OB_FAIL(buffer.append(row.normalized_expr_))) {
    }
  }
  // The row predicate keeps the recomputation on the row the task was created for, the EXISTS
  // guard drops the write when a newer request replaced the task in the meantime.
  if (OB_SUCC(ret) && OB_FAIL(buffer.append(" where ("))) {
  } else if (OB_SUCC(ret) && OB_FAIL(buffer.append(group.at(0).rowkey_predicate_))) {
  } else if (OB_SUCC(ret) && OB_FAIL(buffer.append(") and exists (select 1 from oceanbase."))) {
  } else if (OB_SUCC(ret) && OB_FAIL(buffer.append(OB_ALL_AI_COLUMN_TASK_TNAME))) {
  } else if (OB_SUCC(ret) && OB_FAIL(append_group_predicate(buffer, group))) {
  } else if (OB_SUCC(ret) && OB_FAIL(buffer.append(")"))) {
  } else if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc,
                                                     ObString(static_cast<int32_t>(buffer.length()),
                                                              buffer.ptr()),
                                                     sql))) {
  }
  return ret;
}

int ObAiColumnTaskHelper::build_group_delete_sql(ObIAllocator &alloc,
                                                 const ObIArray<ObAiColumnTaskRow> &rows,
                                                 ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  if (OB_UNLIKELY(rows.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buffer.append_fmt("delete from oceanbase.%s",
                                       OB_ALL_AI_COLUMN_TASK_TNAME))) {
  } else if (OB_FAIL(append_group_predicate(buffer, rows))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(buffer.length()), buffer.ptr()),
                                     sql))) {
  }
  return ret;
}

int ObAiColumnTaskHelper::build_group_retry_sql(ObIAllocator &alloc,
                                                const ObIArray<ObAiColumnTaskRow> &rows,
                                                ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  if (OB_UNLIKELY(rows.empty())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buffer.append_fmt("update oceanbase.%s set state = %ld,"
                                       " retry_count = retry_count + 1,"
                                       " eligible_ts = now(6) + interval 1 second",
                                       OB_ALL_AI_COLUMN_TASK_TNAME,
                                       static_cast<int64_t>(OB_AI_COLUMN_TASK_RETRY)))) {
  } else if (OB_FAIL(append_group_predicate(buffer, rows))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(buffer.length()), buffer.ptr()),
                                     sql))) {
  }
  return ret;
}

} // namespace share
} // namespace oceanbase

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

#define USING_LOG_PREFIX SHARE_SCHEMA

#include "share/schema/ob_ai_column_sql_service.h"
#include "share/schema/ob_table_schema.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "oblib/common/mysqlclient/ob_isql_client.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

int ObAiColumnSqlService::batch_add_ai_columns_for_create_table(
    common::ObIArray<ObTableSchema> &tables,
    common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  if (tables.empty()) {
    // do nothing
  } else {
    ObDMLSqlSplicer dml;
    int64_t column_count = 0;
    for (int64_t i = 0; i < tables.count() && OB_SUCC(ret); ++i) {
      const ObTableSchema &table = tables.at(i);
      if (table.is_index_table() || table.is_aux_lob_table() || table.is_view_table()) {
        continue;
      }
      for (ObTableSchema::const_column_iterator iter = table.column_begin();
           OB_SUCC(ret) && iter != table.column_end(); ++iter) {
        const ObColumnSchemaV2 &column = **iter;
        if (!column.is_ai_column()) {
          continue;
        }
        if (OB_FAIL(dml.add_pk_column("table_id", table.get_table_id()))) {
        } else if (OB_FAIL(dml.add_pk_column("column_id", column.get_column_id()))) {
        } else if (OB_FAIL(dml.add_column("column_name",
                   ObHexEscapeSqlStr(column.get_column_name_str())))) {
        } else if (OB_FAIL(dml.add_column("max_retries", column.get_ai_max_retries()))) {
        } else if (OB_FAIL(dml.add_column("state_column_id", column.get_ai_state_column_id()))) {
        } else if (OB_FAIL(dml.add_column("request_column_id", column.get_ai_request_column_id()))) {
        } else if (OB_FAIL(dml.add_column("result_spec_column_id",
                                          column.get_ai_result_spec_column_id()))) {
        } else if (OB_FAIL(dml.add_column("normalized_expr",
                   ObHexEscapeSqlStr(column.get_ai_normalized_expr())))) {
        } else if (OB_FAIL(dml.add_column("sync_mode",
                   ObHexEscapeSqlStr(column.get_ai_sync_mode())))) {
        } else if (OB_FAIL(dml.finish_row())) {
        } else {
          ++column_count;
        }
      }
    }
    if (OB_SUCC(ret) && column_count > 0) {
      ObDMLExecHelper exec(sql_client);
      int64_t affected_rows = 0;
      if (OB_FAIL(exec.exec_batch_insert(OB_ALL_AI_COLUMN_TNAME, dml, affected_rows,
                                         "INSERT"))) {
        LOG_WARN("insert __all_ai_column failed", K(ret));
      } else if (affected_rows != column_count) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("unexpected affected rows", K(ret), K(affected_rows), K(column_count));
      }
    }
  }
  return ret;
}


int ObAiColumnSqlService::add_ai_column(const uint64_t table_id,
                                        const ObColumnSchemaV2 &column,
                                        common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer dml;
  ObDMLExecHelper exec(sql_client);
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(OB_INVALID_ID == table_id) || !column.is_ai_column()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(dml.add_pk_column("table_id", table_id))) {
  } else if (OB_FAIL(dml.add_pk_column("column_id", column.get_column_id()))) {
  } else if (OB_FAIL(dml.add_column("column_name",
             ObHexEscapeSqlStr(column.get_column_name_str())))) {
  } else if (OB_FAIL(dml.add_column("max_retries", column.get_ai_max_retries()))) {
  } else if (OB_FAIL(dml.add_column("state_column_id", column.get_ai_state_column_id()))) {
  } else if (OB_FAIL(dml.add_column("request_column_id", column.get_ai_request_column_id()))) {
  } else if (OB_FAIL(dml.add_column("result_spec_column_id",
                                    column.get_ai_result_spec_column_id()))) {
  } else if (OB_FAIL(dml.add_column("normalized_expr",
             ObHexEscapeSqlStr(column.get_ai_normalized_expr())))) {
  } else if (OB_FAIL(dml.add_column("sync_mode",
             ObHexEscapeSqlStr(column.get_ai_sync_mode())))) {
  } else if (OB_FAIL(dml.finish_row())) {
  } else if (OB_FAIL(exec.exec_insert(OB_ALL_AI_COLUMN_TNAME, dml, affected_rows))) {
    LOG_WARN("insert __all_ai_column failed", K(ret), K(table_id));
  } else if (affected_rows != 1) {
    ret = OB_ERR_UNEXPECTED;
  }
  return ret;
}

int ObAiColumnSqlService::delete_ai_columns(const uint64_t table_id,
                                            common::ObISQLClient &sql_client)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(OB_INVALID_ID == table_id)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.assign_fmt("DELETE FROM %s WHERE table_id = %lu",
                                    OB_ALL_AI_COLUMN_TNAME, table_id))) {
    LOG_WARN("assign sql failed", K(ret));
  } else if (OB_FAIL(sql_client.write(sql.ptr(), affected_rows))) {
    LOG_WARN("delete from __all_ai_column failed", K(ret), K(table_id));
  }
  return ret;
}

}  // namespace schema
}  // namespace share
}  // namespace oceanbase

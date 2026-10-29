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

#include "share/ai_column/ob_ai_column_meta.h"
#include "share/ob_errno.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/inner_table/ob_inner_table_schema_constants.h"
#include "share/schema/ob_multi_version_schema_service.h"
#include "share/schema/ob_schema_getter_guard.h"
#include "share/schema/ob_table_schema.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "lib/allocator/page_arena.h"
#include "lib/string/ob_sql_string.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{

void ObAiColumnMeta::reset()
{
  table_id_ = OB_INVALID_ID;
  column_id_ = OB_INVALID_ID;
  ai_type_ = OB_AI_COLUMN_TYPE_INVALID;
  max_retries_ = 0;
  sync_mode_ = 1;
  dimension_ = 0;
  state_column_id_ = OB_INVALID_ID;
  request_column_id_ = OB_INVALID_ID;
  result_spec_column_id_ = OB_INVALID_ID;
  database_name_.reset();
  table_name_.reset();
  column_name_.reset();
  model_name_.reset();
  normalized_expr_.reset();
}

int ObAiColumnMetaHelper::sync_table_ai_columns(ObISQLClient &sql_proxy,
                                                const uint64_t table_id,
                                                int64_t &ai_column_count)
{
  int ret = OB_SUCCESS;
  ai_column_count = 0;
  ObArenaAllocator allocator("AiColumnMeta");
  ObSchemaGetterGuard schema_guard;
  const schema::ObTableSchema *table_schema = NULL;
  const ObDatabaseSchema *database_schema = NULL;
  ObSEArray<ObAiColumnMeta, 4> metas;
  if (OB_INVALID_ID == table_id) {
    // e.g. create table if not exists hit an existing table, nothing to reconcile
  } else if (OB_FAIL(ObMultiVersionSchemaService::get_instance()
                       .get_runtime_schema_guard_with_version_in_inner_table(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_table_schema(table_id, table_schema))) {
  } else if (OB_ISNULL(table_schema)) {
    LOG_INFO("table is gone, skip ai column metadata", K(ret), K(table_id));
  } else if (OB_FAIL(schema_guard.get_database_schema(table_schema->get_database_id(),
                                                      database_schema))) {
  } else if (OB_ISNULL(database_schema)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("database schema is null", K(ret), K(table_id));
  } else if (OB_FAIL(get_table_ai_columns(allocator,
                                          *table_schema,
                                          database_schema->get_database_name_str(),
                                          metas,
                                          ai_column_count))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < metas.count(); ++i) {
      int64_t affected_rows = 0;
      ObString sql;
      if (OB_FAIL(build_upsert_sql(allocator, metas.at(i), sql))) {
      } else if (OB_FAIL(sql_proxy.write(sql.ptr(), affected_rows))) {
        LOG_WARN("failed to write ai column metadata", K(ret), K(metas.at(i)));
      }
    }
  }
  if (OB_SUCC(ret)) {
    LOG_INFO("ai column metadata synced", K(table_id), K(ai_column_count));
  }
  return ret;
}

int ObAiColumnMetaHelper::build_upsert_sql(ObIAllocator &alloc,
                                           const ObAiColumnMeta &meta,
                                           ObString &sql)
{
  int ret = OB_SUCCESS;
  ObDMLSqlSplicer splicer;
  ObSqlString buffer;
  if (OB_FAIL(splicer.add_pk_column("table_id", static_cast<int64_t>(meta.table_id_)))) {
  } else if (OB_FAIL(splicer.add_pk_column("column_id", static_cast<int64_t>(meta.column_id_)))) {
  } else if (OB_FAIL(splicer.add_column("database_name", ObHexEscapeSqlStr(meta.database_name_)))) {
  } else if (OB_FAIL(splicer.add_column("table_name", ObHexEscapeSqlStr(meta.table_name_)))) {
  } else if (OB_FAIL(splicer.add_column("column_name", ObHexEscapeSqlStr(meta.column_name_)))) {
  } else if (OB_FAIL(splicer.add_column("ai_type", meta.ai_type_))) {
  } else if (OB_FAIL(splicer.add_column("model_name", ObHexEscapeSqlStr(meta.model_name_)))) {
  } else if (OB_FAIL(splicer.add_column("sync_mode", meta.sync_mode_))) {
  } else if (OB_FAIL(splicer.add_column("max_retries", meta.max_retries_))) {
  } else if (OB_FAIL(splicer.add_column("state_column_id", static_cast<int64_t>(meta.state_column_id_)))) {
  } else if (OB_FAIL(splicer.add_column("request_column_id", static_cast<int64_t>(meta.request_column_id_)))) {
  } else if (OB_FAIL(splicer.add_column("result_spec_column_id", static_cast<int64_t>(meta.result_spec_column_id_)))) {
  } else if (OB_FAIL(splicer.add_column("dimension", meta.dimension_))) {
  } else if (OB_FAIL(splicer.add_column("normalized_expr", ObHexEscapeSqlStr(meta.normalized_expr_)))) {
  } else if (OB_FAIL(splicer.splice_insert_update_sql(OB_ALL_AI_COLUMN_TNAME, buffer))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(buffer.length()), buffer.ptr()),
                                     sql,
                                     true /* c_style */))) {
  }
  return ret;
}

int ObAiColumnMetaHelper::build_delete_sql(ObIAllocator &alloc,
                                           const uint64_t table_id,
                                           ObString &sql)
{
  int ret = OB_SUCCESS;
  ObSqlString buffer;
  if (OB_FAIL(buffer.append_fmt("delete from %s where table_id = %ld",
                                OB_ALL_AI_COLUMN_TNAME,
                                static_cast<int64_t>(table_id)))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(buffer.length()), buffer.ptr()),
                                     sql,
                                     true /* c_style */))) {
  }
  return ret;
}

int ObAiColumnMetaHelper::get_table_ai_columns(ObIAllocator &alloc,
                                               const schema::ObTableSchema &table_schema,
                                               const ObString &database_name,
                                               ObIArray<ObAiColumnMeta> &metas,
                                               int64_t &ai_column_count)
{
  int ret = OB_SUCCESS;
  ai_column_count = 0;
  for (schema::ObTableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
    const schema::ObColumnSchemaV2 *column = *it;
    bool is_ai_column = false;
    ObAiColumnSpec spec;
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null", K(ret), K(table_schema.get_table_id()));
    } else if (OB_FAIL(ObAiColumnUtils::parse_spec(column->get_comment_str(),
                                                   alloc,
                                                   spec,
                                                   is_ai_column))) {
      LOG_WARN("failed to parse ai column comment, ignored", K(ret), KPC(column));
      ret = OB_SUCCESS;
    } else if (!is_ai_column) {
      // ordinary column
    } else {
      ObAiColumnMeta meta;
      if (OB_FAIL(build_meta_(alloc, table_schema, *column, spec, database_name, meta))) {
      } else if (OB_FAIL(metas.push_back(meta))) {
      } else {
        ++ai_column_count;
      }
    }
  }
  return ret;
}

int ObAiColumnMetaHelper::build_meta_(ObIAllocator &alloc,
                                      const schema::ObTableSchema &table_schema,
                                      const schema::ObColumnSchemaV2 &column,
                                      const ObAiColumnSpec &spec,
                                      const ObString &database_name,
                                      ObAiColumnMeta &meta)
{
  int ret = OB_SUCCESS;
  ObString type_name;
  meta.reset();
  meta.table_id_ = table_schema.get_table_id();
  meta.column_id_ = column.get_column_id();
  meta.ai_type_ = spec.ai_type_;
  meta.max_retries_ = spec.max_retries_;
  meta.sync_mode_ = spec.sync_mode_;
  meta.dimension_ = spec.dimension_;
  meta.database_name_ = database_name;
  meta.table_name_ = table_schema.get_table_name_str();
  meta.column_name_ = column.get_column_name_str();
  meta.model_name_ = spec.model_name_;
  if (OB_FAIL(ObAiColumnUtils::get_column_type_name(column, type_name))) {
  } else if (OB_FAIL(ObAiColumnUtils::build_normalized_expr(alloc,
                                                            spec,
                                                            type_name,
                                                            meta.normalized_expr_))) {
  } else if (OB_FAIL(get_hidden_column_id_(table_schema, alloc, meta.column_name_,
                                           OB_AI_COLUMN_HIDDEN_STATE, meta.state_column_id_))) {
  } else if (OB_FAIL(get_hidden_column_id_(table_schema, alloc, meta.column_name_,
                                           OB_AI_COLUMN_HIDDEN_REQUEST, meta.request_column_id_))) {
  } else if (OB_FAIL(get_hidden_column_id_(table_schema, alloc, meta.column_name_,
                                           OB_AI_COLUMN_HIDDEN_SPEC, meta.result_spec_column_id_))) {
  }
  return ret;
}

int ObAiColumnMetaHelper::get_hidden_column_id_(const schema::ObTableSchema &table_schema,
                                                ObIAllocator &alloc,
                                                const ObString &column_name,
                                                const int64_t kind,
                                                uint64_t &column_id)
{
  int ret = OB_SUCCESS;
  ObString hidden_name;
  const schema::ObColumnSchemaV2 *hidden_column = NULL;
  column_id = OB_INVALID_ID;
  if (OB_FAIL(ObAiColumnUtils::build_hidden_column_name(alloc, column_name, kind, hidden_name))) {
  } else if (OB_ISNULL(hidden_column = table_schema.get_column_schema(hidden_name))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("ai column maintenance column is missing", K(ret), K(table_schema.get_table_id()),
             K(column_name), K(kind));
  } else {
    column_id = hidden_column->get_column_id();
  }
  return ret;
}

} // namespace share
} // namespace oceanbase

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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SQL_SERVICE_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SQL_SERVICE_H_

#include "lib/container/ob_iarray.h"
#include "lib/utility/ob_macro_utils.h"

namespace oceanbase
{
namespace common
{
class ObISQLClient;
}
namespace share
{
namespace schema
{
class ObTableSchema;
class ObColumnSchemaV2;

// Writes AI column metadata (__all_ai_column) during table DDL.
class ObAiColumnSqlService
{
public:
  ObAiColumnSqlService() {}
  virtual ~ObAiColumnSqlService() {}

  int batch_add_ai_columns_for_create_table(common::ObIArray<ObTableSchema> &tables,
                                            common::ObISQLClient &sql_client);
  int add_ai_column(const uint64_t table_id,
                    const ObColumnSchemaV2 &column,
                    common::ObISQLClient &sql_client);
  int delete_ai_columns(const uint64_t table_id,
                        common::ObISQLClient &sql_client);
private:
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnSqlService);
};

}  // namespace schema
}  // namespace share
}  // namespace oceanbase

#endif  // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SQL_SERVICE_H_

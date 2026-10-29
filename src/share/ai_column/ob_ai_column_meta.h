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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_META_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_META_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "share/ai_column/ob_ai_column_utils.h"

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
}

// Row of __all_ai_column. The catalog is what the worker, the strict read check and the
// RETRY statement consult, so it has to be rebuilt from the committed schema rather than
// from the resolver output: only the DDL service knows the final column ids.
struct ObAiColumnMeta
{
public:
  ObAiColumnMeta() { reset(); }
  ~ObAiColumnMeta() {}
  void reset();
  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(ai_type), K_(max_retries),
               K_(sync_mode), K_(dimension), K_(state_column_id), K_(request_column_id),
               K_(result_spec_column_id), K_(normalized_expr));

public:
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t ai_type_;
  int64_t max_retries_;
  int64_t sync_mode_;
  int64_t dimension_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  common::ObString database_name_;
  common::ObString table_name_;
  common::ObString column_name_;
  common::ObString model_name_;
  common::ObString normalized_expr_;
};

class ObAiColumnMetaHelper
{
public:
  // Writes one __all_ai_column row per AI column the table currently has. The schema is read
  // back through the schema service, so the caller only needs the committed table id.
  static int sync_table_ai_columns(common::ObISQLClient &sql_proxy,
                                   const uint64_t table_id,
                                   int64_t &ai_column_count);
  static int build_upsert_sql(common::ObIAllocator &alloc,
                              const ObAiColumnMeta &meta,
                              common::ObString &sql);
  static int build_delete_sql(common::ObIAllocator &alloc,
                              const uint64_t table_id,
                              common::ObString &sql);
  static int get_table_ai_columns(common::ObIAllocator &alloc,
                                  const schema::ObTableSchema &table_schema,
                                  const common::ObString &database_name,
                                  common::ObIArray<ObAiColumnMeta> &metas,
                                  int64_t &ai_column_count);

private:
  static int build_meta_(common::ObIAllocator &alloc,
                         const schema::ObTableSchema &table_schema,
                         const schema::ObColumnSchemaV2 &column,
                         const ObAiColumnSpec &spec,
                         const common::ObString &database_name,
                         ObAiColumnMeta &meta);
  static int get_hidden_column_id_(const schema::ObTableSchema &table_schema,
                                   common::ObIAllocator &alloc,
                                   const common::ObString &column_name,
                                   const int64_t kind,
                                   uint64_t &column_id);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_META_H_

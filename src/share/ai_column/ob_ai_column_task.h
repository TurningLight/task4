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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_TASK_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_TASK_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace share
{

namespace schema
{
class ObTableSchema;
}
// One row of __all_ai_column_task joined with the catalog row of its AI column.
struct ObAiColumnTaskRow
{
public:
  ObAiColumnTaskRow()
    : table_id_(OB_INVALID_ID), column_id_(OB_INVALID_ID), state_(0), retry_count_(0),
      max_retries_(0)
  {}
  TO_STRING_KV(K_(table_id), K_(column_id), K_(rowkey_predicate), K_(request_id), K_(state),
               K_(retry_count), K_(max_retries), K_(database_name), K_(table_name),
               K_(column_name));
public:
  uint64_t table_id_;
  uint64_t column_id_;
  int64_t state_;
  int64_t retry_count_;
  int64_t max_retries_;
  common::ObString rowkey_predicate_;
  common::ObString request_id_;
  common::ObString database_name_;
  common::ObString table_name_;
  common::ObString column_name_;
  common::ObString normalized_expr_;
};

// Work queue shared by the DML maintenance path and the background worker. A task is one
// (row, AI column) pair: `rowkey_predicate` is both the identity of the row and the predicate
// that finds it again, `request_id` identifies the statement that asked for the recomputation.
// A newer request replaces the task row through REPLACE, which is what makes a late result from
// an older request unable to overwrite a newer one.
class ObAiColumnTaskHelper
{
public:
  enum ObAiColumnTaskState
  {
    OB_AI_COLUMN_TASK_PENDING = 1,
    OB_AI_COLUMN_TASK_RETRY = 3,
  };
  static const int64_t MAX_ROWKEY_PREDICATE_LENGTH = 512;
  static const int64_t LOAD_TASK_BATCH_SIZE = 128;

  // SQL expression that renders the rowkey identity of a row, e.g.
  // concat('`id` = ', cast(`id` as char)).
  static int build_rowkey_predicate_expr(common::ObIAllocator &alloc,
                                         const schema::ObTableSchema &table_schema,
                                         common::ObString &predicate_expr);
  // Request id shared by all the AI columns of one statement.
  static int generate_request_id(common::ObIAllocator &alloc, common::ObString &request_id);
  // One task per row whose AI column is still null. A null value is the only state that needs
  // the model, so non-null rows never pay for an extra task.
  static int build_enqueue_sql(common::ObIAllocator &alloc,
                               const uint64_t table_id,
                               const uint64_t column_id,
                               const common::ObString &database_name,
                               const common::ObString &table_name,
                               const common::ObString &column_name,
                               const common::ObString &predicate_expr,
                               const common::ObString &request_id,
                               common::ObString &sql);
  // Drops tasks whose row is gone or whose AI column now has a value.
  static int build_cleanup_sql(common::ObIAllocator &alloc,
                               const uint64_t table_id,
                               const uint64_t column_id,
                               const common::ObString &database_name,
                               const common::ObString &table_name,
                               const common::ObString &column_name,
                               const common::ObString &predicate_expr,
                               common::ObString &sql);
  static int build_load_tasks_sql(common::ObIAllocator &alloc, common::ObString &sql);
  // Recomputes every AI column of one (row, request) group in a single statement so that the
  // columns of a group become visible together.
  static int build_worker_update_sql(common::ObIAllocator &alloc,
                                     const common::ObIArray<ObAiColumnTaskRow> &group,
                                     common::ObString &sql);
  static int build_group_delete_sql(common::ObIAllocator &alloc,
                                    const common::ObIArray<ObAiColumnTaskRow> &rows,
                                    common::ObString &sql);
  static int build_group_retry_sql(common::ObIAllocator &alloc,
                                   const common::ObIArray<ObAiColumnTaskRow> &rows,
                                   common::ObString &sql);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_TASK_H_

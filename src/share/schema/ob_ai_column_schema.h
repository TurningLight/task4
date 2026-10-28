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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_

#include "lib/ob_define.h"
#include "lib/string/ob_string.h"
#include "lib/utility/ob_print_utils.h"
#include "lib/utility/ob_unify_serialize.h"
#include "lib/allocator/ob_allocator.h"
#include "share/ob_errno.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// Physical metadata of one AI column.  One AI column owns three hidden
// physical columns (state / request / result_spec) whose column ids are
// recorded here.  The content of this struct is persisted as one row of
// the inner table `__all_ai_column`.
class ObAiColumnSchema
{
public:
  static const int64_t OB_INVALID_AI_COLUMN_ID = -1;
  enum SyncMode
  {
    SYNC_MODE_INVALID = 0,
    SYNC_MODE_ASYNC = 1,
  };
public:
  ObAiColumnSchema();
  ~ObAiColumnSchema() {}
  void reset();
  // shallow copy, the ObString members point to the same memory as `other`
  int assign(const ObAiColumnSchema &other);
  // deep copy, every ObString member is duplicated into `allocator`
  int deep_copy(common::ObIAllocator &allocator, const ObAiColumnSchema &other);
  bool is_valid() const;

  int64_t get_serialize_size() const;
  int serialize(char *buf, const int64_t buf_len, int64_t &pos) const;
  int deserialize(const char *buf, const int64_t data_len, int64_t &pos);

  // getters / setters, the names mirror the catalog columns
  uint64_t get_table_id() const { return table_id_; }
  void set_table_id(const uint64_t table_id) { table_id_ = table_id; }
  uint64_t get_column_id() const { return column_id_; }
  void set_column_id(const uint64_t column_id) { column_id_ = column_id; }
  const common::ObString &get_column_name() const { return column_name_; }
  void set_column_name(const common::ObString &name) { column_name_ = name; }
  int64_t get_max_retries() const { return max_retries_; }
  void set_max_retries(const int64_t max_retries) { max_retries_ = max_retries; }
  SyncMode get_sync_mode() const { return sync_mode_; }
  void set_sync_mode(const SyncMode sync_mode) { sync_mode_ = sync_mode; }
  uint64_t get_state_column_id() const { return state_column_id_; }
  void set_state_column_id(const uint64_t id) { state_column_id_ = id; }
  uint64_t get_request_column_id() const { return request_column_id_; }
  void set_request_column_id(const uint64_t id) { request_column_id_ = id; }
  uint64_t get_result_spec_column_id() const { return result_spec_column_id_; }
  void set_result_spec_column_id(const uint64_t id) { result_spec_column_id_ = id; }
  const common::ObString &get_normalized_expr() const { return normalized_expr_; }
  void set_normalized_expr(const common::ObString &expr) { normalized_expr_ = expr; }

  TO_STRING_KV(K_(table_id), K_(column_id), K_(column_name), K_(max_retries),
               "sync_mode", static_cast<int64_t>(sync_mode_),
               K_(state_column_id), K_(request_column_id), K_(result_spec_column_id),
               K_(normalized_expr));
private:
  uint64_t table_id_;
  uint64_t column_id_;
  common::ObString column_name_;
  int64_t max_retries_;
  SyncMode sync_mode_;
  uint64_t state_column_id_;
  uint64_t request_column_id_;
  uint64_t result_spec_column_id_;
  common::ObString normalized_expr_;
};

inline ObAiColumnSchema::ObAiColumnSchema()
{
  reset();
}

inline void ObAiColumnSchema::reset()
{
  table_id_ = common::OB_INVALID_ID;
  column_id_ = common::OB_INVALID_ID;
  column_name_.reset();
  max_retries_ = 0;
  sync_mode_ = SYNC_MODE_ASYNC;
  state_column_id_ = common::OB_INVALID_ID;
  request_column_id_ = common::OB_INVALID_ID;
  result_spec_column_id_ = common::OB_INVALID_ID;
  normalized_expr_.reset();
}

inline int ObAiColumnSchema::assign(const ObAiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  table_id_ = other.table_id_;
  column_id_ = other.column_id_;
  column_name_ = other.column_name_;
  max_retries_ = other.max_retries_;
  sync_mode_ = other.sync_mode_;
  state_column_id_ = other.state_column_id_;
  request_column_id_ = other.request_column_id_;
  result_spec_column_id_ = other.result_spec_column_id_;
  normalized_expr_ = other.normalized_expr_;
  return ret;
}

inline int ObAiColumnSchema::deep_copy(common::ObIAllocator &allocator,
                                       const ObAiColumnSchema &other)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(assign(other))) {
  } else if (OB_FAIL(common::ob_write_string(allocator, other.column_name_, column_name_))) {
  } else if (OB_FAIL(common::ob_write_string(allocator, other.normalized_expr_, normalized_expr_))) {
  } else { /*do nothing*/ }
  return ret;
}

inline bool ObAiColumnSchema::is_valid() const
{
  // neither table_id_ nor column_id_ is final when the resolver builds this
  // struct (the RS assigns them), so only the name is required here.
  return !column_name_.empty();
}

inline int64_t ObAiColumnSchema::get_serialize_size() const
{
  int64_t len = 0;
  int64_t sync_mode = static_cast<int64_t>(sync_mode_);
  OB_UNIS_ADD_LEN(table_id_);
  OB_UNIS_ADD_LEN(column_id_);
  OB_UNIS_ADD_LEN(column_name_);
  OB_UNIS_ADD_LEN(max_retries_);
  OB_UNIS_ADD_LEN(sync_mode);
  OB_UNIS_ADD_LEN(state_column_id_);
  OB_UNIS_ADD_LEN(request_column_id_);
  OB_UNIS_ADD_LEN(result_spec_column_id_);
  OB_UNIS_ADD_LEN(normalized_expr_);
  return len;
}

inline int ObAiColumnSchema::serialize(char *buf, const int64_t buf_len, int64_t &pos) const
{
  int ret = OB_SUCCESS;
  int64_t sync_mode = static_cast<int64_t>(sync_mode_);
  OB_UNIS_ENCODE(table_id_);
  OB_UNIS_ENCODE(column_id_);
  OB_UNIS_ENCODE(column_name_);
  OB_UNIS_ENCODE(max_retries_);
  OB_UNIS_ENCODE(sync_mode);
  OB_UNIS_ENCODE(state_column_id_);
  OB_UNIS_ENCODE(request_column_id_);
  OB_UNIS_ENCODE(result_spec_column_id_);
  OB_UNIS_ENCODE(normalized_expr_);
  return ret;
}

inline int ObAiColumnSchema::deserialize(const char *buf, const int64_t data_len, int64_t &pos)
{
  int ret = OB_SUCCESS;
  int64_t sync_mode = 0;
  OB_UNIS_DECODE(table_id_);
  OB_UNIS_DECODE(column_id_);
  OB_UNIS_DECODE(column_name_);
  OB_UNIS_DECODE(max_retries_);
  OB_UNIS_DECODE(sync_mode);
  if (OB_SUCC(ret)) {
    sync_mode_ = static_cast<SyncMode>(sync_mode);
  }
  OB_UNIS_DECODE(state_column_id_);
  OB_UNIS_DECODE(request_column_id_);
  OB_UNIS_DECODE(result_spec_column_id_);
  OB_UNIS_DECODE(normalized_expr_);
  return ret;
}

} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_SCHEMA_H_

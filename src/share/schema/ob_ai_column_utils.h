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

#ifndef OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTILS_H_
#define OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTILS_H_

#include "lib/string/ob_string.h"
#include "lib/string/ob_string_buffer.h"
#include "lib/container/ob_se_array.h"
#include "lib/utility/ob_print_utils.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"

namespace oceanbase
{
namespace share
{
namespace schema
{

// AI column support.
//
// An AI column is a regular user column whose value is produced asynchronously by a model
// call.  Business SQL can read it but can never assign it.  Everything the runtime needs to
// know about one derivation is kept next to the column schemas of the same table:
//
//   * the AI result column carries a metadata entry in its extended_type_info;
//   * every AI column owns three state columns of the same table (invisible to users):
//       __ai_chk_<column>   stored generated column, fingerprint of the derivation inputs
//       __ai_done_<column>  fingerprint that produced the value currently stored
//       __ai_err_<column>   failure message of the last attempt (NULL when not failed)
//
// The fingerprint is derived from the AI expression definition plus the values of the
// columns the expression depends on.  Two rows agree on it iff their derivation inputs
// agree, therefore "an input changed" is decided by the storage engine itself and the
// derived state is always updated transactionally together with the row.
namespace ai_column
{

static const char *const AI_META_MAGIC = "~AIC1~";
static const char *const AI_CHK_PREFIX = "__ai_chk_";
static const char *const AI_DONE_PREFIX = "__ai_done_";
static const char *const AI_ERR_PREFIX = "__ai_err_";
static const char *const AI_FUNC_COMPLETE = "AI_COMPLETE";
static const char *const AI_FUNC_EMBED = "AI_EMBED";
static const char *const AI_SYNC_MODE_ASYNC = "async";
static const char *const AI_SYNC_MODE_SYNC = "sync";
// placeholder used by the fingerprint expression for a NULL input
static const char *const AI_NULL_PLACEHOLDER = "<NULL>";

// state column kind, encoded as a single character inside the metadata string
enum ObAIColumnStateKind
{
  AI_STATE_NONE = 0,
  AI_STATE_AI_COLUMN = 1,   // 'A': the AI result column itself
  AI_STATE_FINGERPRINT = 2, // 'C': stored generated fingerprint column
  AI_STATE_DONE = 3,        // 'D': fingerprint that produced the stored value
  AI_STATE_ERROR = 4,       // 'E': last failure message
};

const int64_t AI_FINGERPRINT_LENGTH = 64;
const int64_t AI_ERROR_LENGTH = 512;
const int64_t AI_MAX_RETRIES_LIMIT = 100;
const int64_t AI_DEFAULT_MAX_RETRIES = 2;

struct ObAIColumnDef
{
  ObAIColumnDef() { reset(); }
  void reset()
  {
    kind_ = AI_STATE_NONE;
    owner_ = ObString();
    func_name_ = ObString();
    model_ = ObString();
    sync_mode_ = ObString();
    deps_ = ObString();
    expr_ = ObString();
    max_retries_ = AI_DEFAULT_MAX_RETRIES;
  }
  bool is_valid() const
  {
    return AI_STATE_NONE != kind_ && !func_name_.empty() && !expr_.empty();
  }
  ObAIColumnStateKind kind_;
  // name of the AI result column this definition belongs to
  ObString owner_;
  ObString func_name_;
  ObString model_;
  ObString sync_mode_;
  // comma separated dependency column names, empty when nothing is referenced
  ObString deps_;
  // text of the AI function call, as written by the user
  ObString expr_;
  int64_t max_retries_;
};

inline char state_kind_char(const ObAIColumnStateKind kind)
{
  char c = 'A';
  switch (kind) {
    case AI_STATE_FINGERPRINT: c = 'C'; break;
    case AI_STATE_DONE: c = 'D'; break;
    case AI_STATE_ERROR: c = 'E'; break;
    default: c = 'A'; break;
  }
  return c;
}

inline ObAIColumnStateKind state_kind_from_char(const char c)
{
  ObAIColumnStateKind kind = AI_STATE_NONE;
  switch (c) {
    case 'C': kind = AI_STATE_FINGERPRINT; break;
    case 'D': kind = AI_STATE_DONE; break;
    case 'E': kind = AI_STATE_ERROR; break;
    case 'A': kind = AI_STATE_AI_COLUMN; break;
    default: kind = AI_STATE_NONE; break;
  }
  return kind;
}

inline bool is_ai_func_name(const ObString &name)
{
  return 0 == name.case_compare(AI_FUNC_COMPLETE) || 0 == name.case_compare(AI_FUNC_EMBED);
}

inline bool starts_with(const ObString &str, const char *prefix)
{
  const int64_t len = static_cast<int64_t>(strlen(prefix));
  return str.length() >= len && 0 == STRNCMP(str.ptr(), prefix, len);
}

inline int append_decimal(ObStringBuffer &buf, const int64_t value)
{
  int ret = OB_SUCCESS;
  char tmp[32];
  const int64_t len = snprintf(tmp, sizeof(tmp), "%ld", value);
  if (OB_UNLIKELY(len <= 0 || len >= static_cast<int64_t>(sizeof(tmp)))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(buf.append(tmp, static_cast<uint64_t>(len)))) {
  }
  return ret;
}

// The metadata is one self describing string.  Every field but the last never contains the
// separator, and the expression text (which may contain anything) is stored last, so no
// escaping is needed and the string is never modified in place:
//   ~AIC1~<kind>|<owner>|<func>|<model>|<sync_mode>|<max_retries>|<deps>|<expr>
inline int build_ai_meta_string(ObIAllocator &allocator,
                                const ObAIColumnDef &def,
                                ObString &meta)
{
  int ret = OB_SUCCESS;
  ObStringBuffer buf(&allocator);
  const char kind_char = state_kind_char(def.kind_);
  const char *sync_mode = def.sync_mode_.empty() ? AI_SYNC_MODE_ASYNC : NULL;
  if (OB_UNLIKELY(!def.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(buf.append(AI_META_MAGIC))) {
  } else if (OB_FAIL(buf.append(&kind_char, 1))) {
  } else if (OB_FAIL(buf.append("|")) || OB_FAIL(buf.append(def.owner_))) {
  } else if (OB_FAIL(buf.append("|")) || OB_FAIL(buf.append(def.func_name_))) {
  } else if (OB_FAIL(buf.append("|")) || OB_FAIL(buf.append(def.model_))) {
  } else if (OB_FAIL(buf.append("|"))) {
  } else if (OB_FAIL(NULL != sync_mode ? buf.append(sync_mode) : buf.append(def.sync_mode_))) {
  } else if (OB_FAIL(buf.append("|")) || OB_FAIL(append_decimal(buf, def.max_retries_))) {
  } else if (OB_FAIL(buf.append("|")) || OB_FAIL(buf.append(def.deps_))) {
  } else if (OB_FAIL(buf.append("|")) || OB_FAIL(buf.append(def.expr_))) {
  } else {
    meta = buf.string();
  }
  return ret;
}

// Returns OB_HASH_NOT_EXIST when the string is not an AI column metadata entry.
inline int parse_ai_meta_string(const ObString &meta, ObAIColumnDef &def)
{
  int ret = OB_SUCCESS;
  def.reset();
  const int64_t magic_len = static_cast<int64_t>(strlen(AI_META_MAGIC));
  if (!starts_with(meta, AI_META_MAGIC) || meta.length() < magic_len + 2) {
    ret = OB_HASH_NOT_EXIST;
  } else {
    const char *ptr = meta.ptr() + magic_len;
    const char *end = meta.ptr() + meta.length();
    def.kind_ = state_kind_from_char(*ptr);
    ++ptr;
    if (AI_STATE_NONE == def.kind_ || ptr >= end || '|' != *ptr) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      ++ptr;
      const char *field_begin = ptr;
      int64_t field_idx = 0;
      while (OB_SUCC(ret) && field_idx <= 6) {
        if (ptr == end) {
          // the expression text is the last field: end of string terminates it
          if (6 == field_idx) {
            def.expr_.assign_ptr(field_begin, static_cast<int32_t>(ptr - field_begin));
          } else {
            ret = OB_INVALID_ARGUMENT;
          }
          break;
        } else if ('|' == *ptr) {
          const int64_t len = ptr - field_begin;
          switch (field_idx) {
            case 0: def.owner_.assign_ptr(field_begin, static_cast<int32_t>(len)); break;
            case 1: def.func_name_.assign_ptr(field_begin, static_cast<int32_t>(len)); break;
            case 2: def.model_.assign_ptr(field_begin, static_cast<int32_t>(len)); break;
            case 3: def.sync_mode_.assign_ptr(field_begin, static_cast<int32_t>(len)); break;
            case 4: {
              int64_t value = 0;
              for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
                const char c = field_begin[i];
                if (c < '0' || c > '9') {
                  ret = OB_INVALID_ARGUMENT;
                } else {
                  value = value * 10 + (c - '0');
                }
              }
              if (OB_SUCC(ret)) {
                def.max_retries_ = value;
              }
              break;
            }
            case 5: def.deps_.assign_ptr(field_begin, static_cast<int32_t>(len)); break;
            case 6: def.expr_.assign_ptr(field_begin, static_cast<int32_t>(len)); break;
            default: ret = OB_INVALID_ARGUMENT; break;
          }
          ++field_idx;
          field_begin = ptr + 1;
        }
        ++ptr;
      }
    }
  }
  return ret;
}

// Finds the AI metadata entry inside a column's extended type info.
// Returns OB_HASH_NOT_EXIST when the column does not carry one.
inline int get_ai_meta(const ObColumnSchemaV2 &column, ObString &meta)
{
  int ret = OB_HASH_NOT_EXIST;
  const ObIArray<ObString> &type_info = column.get_extended_type_info();
  for (int64_t i = 0; OB_HASH_NOT_EXIST == ret && i < type_info.count(); ++i) {
    if (starts_with(type_info.at(i), AI_META_MAGIC)) {
      meta = type_info.at(i);
      ret = OB_SUCCESS;
    }
  }
  return ret;
}

inline bool is_ai_column(const ObColumnSchemaV2 &column)
{
  ObString meta;
  ObAIColumnDef def;
  return OB_SUCCESS == get_ai_meta(column, meta)
         && OB_SUCCESS == parse_ai_meta_string(meta, def)
         && AI_STATE_AI_COLUMN == def.kind_;
}

// true for the three internal state columns owned by an AI column
inline bool is_ai_state_column(const ObColumnSchemaV2 &column)
{
  ObString meta;
  ObAIColumnDef def;
  return OB_SUCCESS == get_ai_meta(column, meta)
         && OB_SUCCESS == parse_ai_meta_string(meta, def)
         && AI_STATE_AI_COLUMN != def.kind_;
}

inline int get_ai_column_def(const ObColumnSchemaV2 &column, ObAIColumnDef &def)
{
  int ret = OB_SUCCESS;
  ObString meta;
  if (OB_FAIL(get_ai_meta(column, meta))) {
  } else if (OB_FAIL(parse_ai_meta_string(meta, def))) {
  }
  return ret;
}

inline const char *state_column_prefix(const int64_t kind)
{
  const char *prefix = AI_CHK_PREFIX;
  switch (kind) {
    case AI_STATE_DONE: prefix = AI_DONE_PREFIX; break;
    case AI_STATE_ERROR: prefix = AI_ERR_PREFIX; break;
    default: prefix = AI_CHK_PREFIX; break;
  }
  return prefix;
}

// <prefix><ai column name>
inline int build_state_column_name(ObIAllocator &allocator,
                                   const int64_t kind,
                                   const ObString &ai_column_name,
                                   ObString &name)
{
  int ret = OB_SUCCESS;
  const char *prefix = state_column_prefix(kind);
  const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
  char *buf = static_cast<char *>(allocator.alloc(prefix_len + ai_column_name.length()));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, prefix, prefix_len);
    MEMCPY(buf + prefix_len, ai_column_name.ptr(), ai_column_name.length());
    name.assign_ptr(buf, static_cast<int32_t>(prefix_len + ai_column_name.length()));
  }
  return ret;
}

inline const ObColumnSchemaV2 *find_ai_state_column(const ObTableSchema &table,
                                                    const ObString &ai_column_name,
                                                    const int64_t kind)
{
  const ObColumnSchemaV2 *result = NULL;
  char buf[OB_MAX_COLUMN_NAME_LENGTH];
  const char *prefix = state_column_prefix(kind);
  const int64_t prefix_len = static_cast<int64_t>(strlen(prefix));
  const int64_t len = prefix_len + ai_column_name.length();
  if (len > OB_MAX_COLUMN_NAME_LENGTH) {
    // column names are validated on creation, cannot happen
  } else {
    MEMCPY(buf, prefix, prefix_len);
    MEMCPY(buf + prefix_len, ai_column_name.ptr(), ai_column_name.length());
    result = table.get_column_schema(ObString(static_cast<int32_t>(len), buf));
  }
  return result;
}

// Builds the fingerprint expression of an AI column:
//   md5(concat_ws('|', '<definition text>', '<model>', <dep values>...))
// The definition text makes a changed prompt/model invalidate previous results, the values
// of the dependency columns make a changed input invalidate them as well.
inline int build_ai_fingerprint_expr(ObIAllocator &allocator,
                                     const ObAIColumnDef &def,
                                     ObString &expr)
{
  int ret = OB_SUCCESS;
  ObStringBuffer buf(&allocator);
  if (OB_FAIL(buf.append("md5(concat_ws('|', '"))) {
  } else {
    // the definition text may contain quotes, keep it as a hex literal
    static const char *hex_digits = "0123456789abcdef";
    for (int64_t i = 0; OB_SUCC(ret) && i < def.expr_.length(); ++i) {
      char hex[2];
      const unsigned char c = static_cast<unsigned char>(def.expr_[i]);
      hex[0] = hex_digits[(c >> 4) & 0x0f];
      hex[1] = hex_digits[c & 0x0f];
      if (OB_FAIL(buf.append(hex, 2))) {
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(buf.append("', '"))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(buf.append(def.model_))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(buf.append("'"))) {
  }
  if (OB_SUCC(ret)) {
    // dependencies, as back quoted identifiers
    const char *deps_begin = def.deps_.ptr();
    const char *deps_end = def.deps_.ptr() + def.deps_.length();
    const char *ptr = deps_begin;
    while (OB_SUCC(ret) && deps_begin < deps_end) {
      if (ptr == deps_end || ',' == *ptr) {
        const int64_t len = ptr - deps_begin;
        if (len > 0) {
          if (OB_FAIL(buf.append(", ifnull(cast(`"))) {
          } else if (OB_FAIL(buf.append(ObString(static_cast<int32_t>(len), deps_begin)))) {
          } else if (OB_FAIL(buf.append("` as char), '"))) {
          } else if (OB_FAIL(buf.append(AI_NULL_PLACEHOLDER))) {
          } else if (OB_FAIL(buf.append("')"))) {
          }
        }
        deps_begin = ptr + 1;
      }
      ++ptr;
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(buf.append("))"))) {
  }
  if (OB_SUCC(ret)) {
    expr = buf.string();
  }
  return ret;
}

// AI columns of a table, in column order.
inline int get_ai_columns(const ObTableSchema &table, ObIArray<const ObColumnSchemaV2 *> &columns)
{
  int ret = OB_SUCCESS;
  for (ObTableSchema::const_column_iterator iter = table.column_begin();
       OB_SUCC(ret) && iter != table.column_end(); ++iter) {
    const ObColumnSchemaV2 *column = *iter;
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (is_ai_column(*column)) {
      if (OB_FAIL(columns.push_back(column))) {
      }
    }
  }
  return ret;
}

inline bool table_has_ai_column(const ObTableSchema &table)
{
  bool found = false;
  for (ObTableSchema::const_column_iterator iter = table.column_begin();
       !found && iter != table.column_end(); ++iter) {
    const ObColumnSchemaV2 *column = *iter;
    found = OB_NOT_NULL(column) && is_ai_column(*column);
  }
  return found;
}

// Initializes the schema of one state column of an AI column.
inline int init_ai_state_column_schema(ObIAllocator &allocator,
                                       const ObColumnSchemaV2 &ai_column,
                                       const ObAIColumnDef &ai_def,
                                       const int64_t kind,
                                       const uint64_t column_id,
                                       ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  ObString column_name;
  ObString meta;
  ObAIColumnDef state_def;
  ObString fingerprint_expr;
  if (OB_FAIL(build_state_column_name(allocator, kind, ai_column.get_column_name_str(), column_name))) {
  } else if (OB_FAIL(column.set_column_name(column_name))) {
  } else if (FALSE_IT(column.set_column_id(column_id))) {
  } else if (FALSE_IT(column.set_table_id(ai_column.get_table_id()))) {
  } else if (FALSE_IT(column.set_data_type(ObVarcharType))) {
  } else if (FALSE_IT(column.set_charset_type(CHARSET_UTF8MB4))) {
  } else if (FALSE_IT(column.set_collation_type(CS_TYPE_UTF8MB4_BIN))) {
  } else if (FALSE_IT(column.set_nullable(true))) {
  } else if (FALSE_IT(column.add_column_flag(INVISIBLE_COLUMN_FLAG))) {
  } else if (AI_STATE_FINGERPRINT == kind) {
    ObAccuracy accuracy(static_cast<ObLength>(AI_FINGERPRINT_LENGTH));
    ObObj expr_obj;
    if (OB_FAIL(build_ai_fingerprint_expr(allocator, ai_def, fingerprint_expr))) {
    } else if (FALSE_IT(column.set_accuracy(accuracy))) {
    } else if (FALSE_IT(column.add_column_flag(STORED_GENERATED_COLUMN_FLAG))) {
    } else {
      expr_obj.set_varchar(fingerprint_expr);
      expr_obj.set_collation_type(CS_TYPE_UTF8MB4_BIN);
      expr_obj.set_collation_level(CS_LEVEL_COERCIBLE);
      if (OB_FAIL(column.set_cur_default_value(expr_obj, false))) {
      } else if (OB_FAIL(column.set_orig_default_value(expr_obj))) {
      }
    }
  } else if (AI_STATE_DONE == kind) {
    ObAccuracy accuracy(static_cast<ObLength>(AI_FINGERPRINT_LENGTH));
    column.set_accuracy(accuracy);
  } else {
    ObAccuracy accuracy(static_cast<ObLength>(AI_ERROR_LENGTH));
    column.set_accuracy(accuracy);
  }
  if (OB_SUCC(ret)) {
    state_def.kind_ = static_cast<ObAIColumnStateKind>(kind);
    state_def.owner_ = ai_column.get_column_name_str();
    state_def.func_name_ = ai_def.func_name_;
    state_def.model_ = ai_def.model_;
    state_def.sync_mode_ = ai_def.sync_mode_;
    state_def.max_retries_ = ai_def.max_retries_;
    state_def.deps_ = ai_def.deps_;
    state_def.expr_ = ai_def.expr_;
    if (OB_FAIL(build_ai_meta_string(allocator, state_def, meta))) {
    } else {
      ObSEArray<ObString, 1> type_info;
      if (OB_FAIL(type_info.push_back(meta))) {
      } else if (OB_FAIL(column.set_extended_type_info(type_info))) {
      }
    }
  }
  return ret;
}

// Appends an AI metadata entry to a column that may already carry extended type info
// (vector columns keep their dimension there).
inline int append_ai_meta(ObIAllocator &allocator,
                          const ObAIColumnDef &def,
                          ObColumnSchemaV2 &column)
{
  int ret = OB_SUCCESS;
  ObString meta;
  ObSEArray<ObString, 2> type_info;
  if (OB_FAIL(build_ai_meta_string(allocator, def, meta))) {
  } else {
    const ObIArray<ObString> &old_type_info = column.get_extended_type_info();
    for (int64_t i = 0; OB_SUCC(ret) && i < old_type_info.count(); ++i) {
      if (OB_FAIL(type_info.push_back(old_type_info.at(i)))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(type_info.push_back(meta))) {
    } else if (OB_SUCC(ret) && OB_FAIL(column.set_extended_type_info(type_info))) {
    }
  }
  return ret;
}

} // namespace ai_column
} // namespace schema
} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_SCHEMA_OB_AI_COLUMN_UTILS_H_

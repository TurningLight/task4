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

#define USING_LOG_PREFIX SQL_ENG

#include "ob_file_table_op.h"
#include "sql/engine/expr/ob_expr_util.h"
#include "share/rc/ob_server_runtime.h"
#include "share/system_variable/ob_system_variable_alias.h"

namespace oceanbase
{
namespace sql
{

OB_SERIALIZE_MEMBER((ObFileTableSpec, ObOpSpec),
                    format_, path_, secure_file_priv_, delimiter_, quote_, escape_,
                    has_header_, null_str_, is_glob_);

DEF_TO_STRING(ObFileTableSpec)
{
  int64_t pos = 0;
  J_OBJ_START();
  J_NAME("op_spec");
  J_COLON();
  pos += ObOpSpec::to_string(buf + pos, buf_len - pos);
  J_COMMA();
  J_KV(K_(format), K_(path), K_(delimiter), K_(quote), K_(escape), K_(has_header),
       K_(is_glob));
  J_OBJ_END();
  return pos;
}

int ObFileTableOp::inner_open()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!initialized_)) {
    const ObString path = MY_SPEC.path_;
    if (path.empty()) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "file table function requires a file path");
    } else if (MY_SPEC.is_glob_) {
      kind_ = SCAN_GLOB;
    } else if (0 == MY_SPEC.format_.case_compare("csv")) {
      kind_ = SCAN_CSV;
    } else if (0 == MY_SPEC.format_.case_compare("json") ||
               0 == MY_SPEC.format_.case_compare("jsonl") ||
               0 == MY_SPEC.format_.case_compare("ndjson")) {
      kind_ = SCAN_JSON;
    } else if (0 == MY_SPEC.format_.case_compare("parquet")) {
      kind_ = SCAN_PARQUET;
    } else {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "unknown file table format");
    }
    initialized_ = true;
  }
  if (OB_SUCC(ret) && SCAN_GLOB != kind_) {
    if (OB_FAIL(open_next_file())) {
    }
  }
  return ret;
}

int ObFileTableOp::open_next_file()
{
  int ret = OB_SUCCESS;
  release_scanner();
  // glob() synthesizes its rows from the resolved path list instead of reading
  // file contents, so it is handled without a scanner.
  if (SCAN_GLOB == kind_) {
    return ret;
  }
  if (SCAN_CSV == kind_) {
    ObFileReadOptions options;
    options.has_header_ = MY_SPEC.has_header_;
    options.delimiter_ = MY_SPEC.delimiter_;
    options.quote_ = MY_SPEC.quote_;
    options.escape_ = MY_SPEC.escape_;
    options.null_str_ = MY_SPEC.null_str_;
    ObFileCsvScanner *csv = static_cast<ObFileCsvScanner *>(
        ctx_.get_allocator().alloc(sizeof(ObFileCsvScanner)));
    if (OB_ISNULL(csv)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      scanner_ = new (csv) ObFileCsvScanner();
      if (OB_FAIL(scanner_->init(ctx_.get_allocator(), MY_SPEC.path_, options, schema_))) {
      }
    }
  } else if (SCAN_JSON == kind_) {
    ObFileJsonScanner *json = static_cast<ObFileJsonScanner *>(
        ctx_.get_allocator().alloc(sizeof(ObFileJsonScanner)));
    if (OB_ISNULL(json)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      json_scanner_ = new (json) ObFileJsonScanner();
      if (OB_FAIL(json_scanner_->init(ctx_.get_allocator(), MY_SPEC.path_, schema_))) {
      }
    }
  }
  return ret;
}

void ObFileTableOp::release_scanner()
{
  // The scanners are arena-allocated; releasing them only runs the destructor so
  // their internal containers are unwound.
  if (OB_NOT_NULL(scanner_)) {
    scanner_->~ObFileCsvScanner();
    scanner_ = nullptr;
  }
  if (OB_NOT_NULL(json_scanner_)) {
    json_scanner_->~ObFileJsonScanner();
    json_scanner_ = nullptr;
  }
  if (OB_NOT_NULL(parquet_scanner_)) {
    parquet_scanner_->~ObFileParquetScanner();
    parquet_scanner_ = nullptr;
  }
}

int ObFileTableOp::cast_cell(ObExpr *expr, const ObString &cell, const bool is_null,
                             const ObFileColumnType type)
{
  int ret = OB_SUCCESS;
  ObDatum &datum = expr->locate_expr_datum(eval_ctx_);
  if (is_null) {
    datum.set_null();
  } else if (OB_FILE_TYPE_BIGINT == type) {
    // Values were classified as integers at inference time, so this parse cannot
    // silently truncate.
    char *end_ptr = nullptr;
    const int64_t v = strtoll(cell.ptr(), &end_ptr, 10);
    datum.set_int(v);
  } else if (OB_FILE_TYPE_DOUBLE == type) {
    char *end_ptr = nullptr;
    const double v = strtod(cell.ptr(), &end_ptr);
    datum.set_double(v);
  } else if (OB_FILE_TYPE_BOOLEAN == type) {
    const bool v = cell.length() == 4 && 0 == strncasecmp(cell.ptr(), "true", 4);
    datum.set_bool(v);
  } else {
    // Strings, dates and timestamps all travel as text and are converted by the
    // column's own cast on the way out.
    char *buf = static_cast<char *>(ctx_.get_allocator().alloc(cell.length()));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      if (cell.length() > 0) {
        MEMCPY(buf, cell.ptr(), cell.length());
      }
      datum.set_string(buf, cell.length());
    }
  }
  if (OB_SUCC(ret)) {
    expr->get_eval_info(eval_ctx_).evaluated_ = true;
  }
  return ret;
}

int ObFileTableOp::emit_row(common::ObIArray<common::ObString> &fields,
                            common::ObIArray<bool> &is_null)
{
  int ret = OB_SUCCESS;
  const int64_t output_count = MY_SPEC.output_.count();
  if (OB_UNLIKELY(fields.count() < output_count)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file row has fewer columns than the plan expects", K(ret),
             K(fields.count()), K(output_count));
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < output_count; ++i) {
      ObExpr *expr = MY_SPEC.output_.at(i);
      const ObFileColumnType type = i < schema_.columns_.count()
          ? schema_.columns_.at(i).type_ : OB_FILE_TYPE_VARCHAR;
      if (OB_FAIL(cast_cell(expr, fields.at(i), is_null.at(i), type))) {
      }
    }
  }
  return ret;
}

int ObFileTableOp::inner_get_next_row()
{
  int ret = OB_SUCCESS;
  if (SCAN_GLOB == kind_) {
    // glob() yields a single `file` column per matched path.
    if (glob_index_ >= glob_paths_.count()) {
      ret = OB_ITER_END;
    } else if (MY_SPEC.output_.count() < 1) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      ObExpr *expr = MY_SPEC.output_.at(0);
      ObDatum &datum = expr->locate_expr_datum(eval_ctx_);
      const ObString &path = glob_paths_.at(glob_index_++);
      char *buf = static_cast<char *>(ctx_.get_allocator().alloc(path.length()));
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        if (path.length() > 0) {
          MEMCPY(buf, path.ptr(), path.length());
        }
        datum.set_string(buf, path.length());
        expr->get_eval_info(eval_ctx_).evaluated_ = true;
      }
    }
  } else if (SCAN_CSV == kind_ && OB_NOT_NULL(scanner_)) {
    common::ObSEArray<common::ObString, 24> fields;
    common::ObSEArray<bool, 24> is_null;
    if (OB_FAIL(scanner_->next_row(fields, is_null))) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to read csv row", K(ret));
      }
    } else if (OB_UNLIKELY(fields.count() != schema_.columns_.count())) {
      ret = OB_INVALID_ARGUMENT;
      LOG_USER_ERROR(OB_INVALID_ARGUMENT, "csv row has a different field count");
    } else if (OB_FAIL(emit_row(fields, is_null))) {
    }
  } else if (SCAN_JSON == kind_ && OB_NOT_NULL(json_scanner_)) {
    common::ObSEArray<common::ObString, 24> fields;
    common::ObSEArray<bool, 24> is_null;
    if (OB_FAIL(json_scanner_->next_row(fields, is_null))) {
      if (OB_ITER_END != ret) {
        LOG_WARN("failed to read json row", K(ret));
      }
    } else if (OB_FAIL(emit_row(fields, is_null))) {
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table scanner is not initialized", K(ret), K_(kind));
  }
  return ret;
}

int ObFileTableOp::inner_get_next_batch(const int64_t max_row_cnt)
{
  int ret = OB_SUCCESS;
  int64_t row_count = 0;
  while (OB_SUCC(ret) && row_count < max_row_cnt) {
    if (OB_FAIL(inner_get_next_row())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("failed to get next row", K(ret));
      }
      break;
    } else {
      ++row_count;
    }
  }
  brs_.size_ = row_count;
  if (row_count == 0) {
    ret = OB_SUCCESS;
    brs_.end_ = true;
  }
  return ret;
}

int ObFileTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  release_scanner();
  glob_index_ = 0;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else if (OB_FAIL(inner_open())) {
  }
  return ret;
}

int ObFileTableOp::inner_close()
{
  int ret = OB_SUCCESS;
  release_scanner();
  glob_index_ = 0;
  return ret;
}

} // namespace sql
} // namespace oceanbase

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
#include "ob_json_table_op.h"
#include "sql/engine/expr/ob_expr_multi_mode_func_helper.h"
#include "sql/engine/expr/ob_expr_json_value.h"
#include "sql/engine/expr/ob_expr_json_query.h"
#include "common/xml/ob_binary_aggregate.h"
#include "sql/engine/expr/ob_array_expr_utils.h"
#include "common/timezone/ob_time_convert.h"

// system headers first: ob_parquet_reader.h uses std::FILE without including <cstdio>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "sql/engine/basic/file/ob_csv_scanner.h"
#include "sql/engine/basic/file/ob_jsonl_scanner.h"
#include "sql/engine/basic/file/ob_parquet_reader.h"

namespace oceanbase
{
using namespace common;
namespace sql
{

/* json value  mismatch type { MISSING : 0, EXTRA : 1, TYPE : 2, EMPTY : 3} */
const static int32_t JSN_VALUE_TYPE_MISSING_DATA    = 0;
const static int32_t JSN_VALUE_TYPE_EXTRA_DATA      = 1;
const static int32_t JSN_VALUE_TYPE_TYPE_ERROR      = 2;
const static int32_t JSN_VALUE_TYPE_IMPLICIT        = 3;

/* json exists */
const static int32_t JSN_EXIST_FALSE = 0;
const static int32_t JSN_EXIST_TRUE  = 1;
const static int32_t JSN_EXIST_ERROR = 2;
const static int32_t JSN_EXIST_DEFAULT = 3;

#define SET_COVER_ERROR(jt_ctx_ptr, error_code) \
{\
  if (!(jt_ctx_ptr)->is_cover_error_) { \
    (jt_ctx_ptr)->is_cover_error_ = true; \
    (jt_ctx_ptr)->error_code_ = error_code; \
  } \
}

// ---------------------------------------------------------------------------
// File table mode: read_csv / read_json / read_parquet / glob
//
// The resolver keeps the file path as the single const expression in
// json_table_def_->doc_exprs_, which reaches the operator as
// spec.value_exprs_.at(0) (ob_join_order.cpp appends it to JsonTablePath's
// value_exprs_, and the code generator turns it into a runtime expression).
// The inferred columns arrive as spec.cols_def_ in exactly the order the
// scanner produced them, and their output expressions as spec.column_exprs_ --
// but only for the columns the query actually references.
//
// The function name itself is not carried into the execute stage (it only
// exists in the parse tree), so the scanner is selected from the path: a
// wildcard pattern means glob(), a known file extension wins, otherwise the
// file magic decides (PAR1 -> parquet, first non-blank byte '{' -> jsonl).
// ---------------------------------------------------------------------------
struct ObFileTableScanState
{
  enum Format
  {
    FMT_NONE = 0,
    FMT_CSV,
    FMT_JSONL,
    FMT_PARQUET,
    FMT_GLOB
  };
  ObFileTableScanState()
    : fmt_(FMT_NONE), path_(), csv_(), jsonl_(), parquet_(),
      parquet_cols_(), parquet_rows_(0), cur_row_(0),
      files_(), cur_file_(0), csv_row_(), jsonl_row_(), col_cnt_(0) {}
  ~ObFileTableScanState() {}

  Format fmt_;
  std::string path_;
  ObCsvScanner csv_;
  ObJsonlScanner jsonl_;
  obparquet::Reader parquet_;
  std::vector<obparquet::ColumnData> parquet_cols_;
  int64_t parquet_rows_;
  int64_t cur_row_;
  std::vector<std::string> files_;
  int64_t cur_file_;
  std::vector<ObCsvValue> csv_row_;
  std::vector<ObJsonlValue> jsonl_row_;
  int64_t col_cnt_;
};

// '*' matches any run of characters, '?' exactly one.
static bool fs_glob_match(const char *pat, const char *name)
{
  bool matched = true;
  const char *star_pat = NULL;  // pattern position just after the last '*'
  const char *star_name = NULL; // name position that '*' was matched against
  while ('\0' != *name) {
    if ('*' == *pat) {
      star_pat = ++pat;
      star_name = name;
    } else if ('?' == *pat || *pat == *name) {
      ++pat;
      ++name;
    } else if (NULL != star_pat) {
      pat = star_pat;
      name = ++star_name;
    } else {
      matched = false;
      break;
    }
  }
  if (matched) {
    while ('*' == *pat) {
      ++pat;
    }
    matched = ('\0' == *pat);
  }
  return matched;
}

static bool fs_has_glob_meta(const std::string &path)
{
  return std::string::npos != path.find('*') || std::string::npos != path.find('?');
}

static bool fs_ends_with(const std::string &path, const char *suffix)
{
  const size_t len = std::strlen(suffix);
  if (path.size() < len) {
    return false;
  }
  return 0 == path.compare(path.size() - len, len, suffix);
}

// Lower-cased copy of the extension (including the dot), empty when there is none.
static std::string fs_extension(const std::string &path)
{
  std::string ext;
  const std::string::size_type slash = path.find_last_of('/');
  const std::string::size_type dot = path.find_last_of('.');
  if (std::string::npos != dot && (std::string::npos == slash || dot > slash)) {
    ext = path.substr(dot);
  }
  for (size_t i = 0; i < ext.size(); ++i) {
    ext[i] = static_cast<char>(::tolower(static_cast<unsigned char>(ext[i])));
  }
  return ext;
}

static ObFileTableScanState::Format fs_sniff_format(const std::string &path)
{
  ObFileTableScanState::Format fmt = ObFileTableScanState::FMT_CSV;
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (NULL != file) {
    char buf[8] = {0};
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, file);
    std::fclose(file);
    if (n >= 4 && 0 == std::memcmp(buf, "PAR1", 4)) {
      fmt = ObFileTableScanState::FMT_PARQUET;
    } else {
      size_t i = 0;
      while (i < n && (' ' == buf[i] || '\t' == buf[i] || '\r' == buf[i] || '\n' == buf[i])) {
        ++i;
      }
      fmt = (i < n && '{' == buf[i]) ? ObFileTableScanState::FMT_JSONL
                                     : ObFileTableScanState::FMT_CSV;
    }
  }
  return fmt;
}

static ObFileTableScanState::Format fs_detect_format(const std::string &path)
{
  ObFileTableScanState::Format fmt = ObFileTableScanState::FMT_NONE;
  const std::string ext = fs_extension(path);
  if (fs_has_glob_meta(path)) {
    fmt = ObFileTableScanState::FMT_GLOB;
  } else if (".csv" == ext || ".tsv" == ext || ".txt" == ext) {
    fmt = ObFileTableScanState::FMT_CSV;
  } else if (".jsonl" == ext || ".ndjson" == ext || ".json" == ext) {
    fmt = ObFileTableScanState::FMT_JSONL;
  } else if (".parquet" == ext || ".parq" == ext || ".pq" == ext) {
    fmt = ObFileTableScanState::FMT_PARQUET;
  } else if (fs_ends_with(path, "/")) {
    fmt = ObFileTableScanState::FMT_NONE;
  } else {
    fmt = fs_sniff_format(path);
  }
  return fmt;
}

static int fs_glob_collect(const std::string &pattern, std::vector<std::string> *out)
{
  int ret = OB_SUCCESS;
  std::string dir;
  std::string file_pat;
  const std::string::size_type slash = pattern.find_last_of('/');
  if (std::string::npos == slash) {
    dir = ".";
    file_pat = pattern;
  } else {
    dir = pattern.substr(0, slash);
    file_pat = pattern.substr(slash + 1);
    if (dir.empty()) {
      dir = "/";
    }
  }
  if (file_pat.empty()) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("glob pattern has no file name part", "pattern", pattern.c_str());
  } else {
    DIR *dirp = ::opendir(dir.c_str());
    if (NULL == dirp) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("open directory failed", "dir", dir.c_str());
    } else {
      struct dirent *ent = NULL;
      while (NULL != (ent = ::readdir(dirp))) {
        if (0 == std::strcmp(ent->d_name, ".") || 0 == std::strcmp(ent->d_name, "..")) {
          continue;
        }
        // A leading dot marks a hidden entry. Shell and DuckDB wildcards never
        // match one unless the pattern itself starts with a dot, so `glob('/d/*')`
        // must not return `._basic.csv` / `.DS_Store` style metadata files.
        if ('.' == ent->d_name[0] && '.' != file_pat[0]) {
          continue;
        }
        if (!fs_glob_match(file_pat.c_str(), ent->d_name)) {
          continue;
        }
        std::string full = dir;
        if ('/' != full[full.length() - 1]) {
          full += "/";
        }
        full += ent->d_name;
        struct stat st;
        if (0 != ::stat(full.c_str(), &st) || !S_ISREG(st.st_mode)) {
          continue;
        }
        out->push_back(full);
      }
      ::closedir(dirp);
      // deterministic order for reproducible results
      std::sort(out->begin(), out->end());
    }
  }
  return ret;
}

// The resolver inferred the plan schema with the pinned defaults (header present,
// sample the whole file); the runtime pass has to reproduce it column by column,
// so the same defaults are used here.
static int fs_open_csv_scan(ObFileTableScanState *state)
{
  int ret = OB_SUCCESS;
  std::string err;
  state->csv_.set_header(true);
  state->csv_.set_sample_size(-1);
  if (!state->csv_.open(state->path_, &err)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("open csv file failed", "path", state->path_.c_str(), "err", err.c_str());
  } else if (!state->csv_.infer_schema(&err)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("infer csv schema failed", "path", state->path_.c_str(), "err", err.c_str());
  }
  return ret;
}

static int fs_open_jsonl_scan(ObFileTableScanState *state)
{
  int ret = OB_SUCCESS;
  std::string err;
  state->jsonl_.set_sample_size(-1);
  if (!state->jsonl_.open(state->path_, &err)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("open jsonl file failed", "path", state->path_.c_str(), "err", err.c_str());
  } else if (!state->jsonl_.infer_schema(&err)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("infer jsonl schema failed", "path", state->path_.c_str(), "err", err.c_str());
  }
  return ret;
}

static int fs_open_parquet_scan(ObFileTableScanState *state)
{
  int ret = OB_SUCCESS;
  std::string err;
  if (!state->parquet_.Open(state->path_, &err)) {
    ret = OB_FILE_NOT_EXIST;
    LOG_WARN("open parquet file failed", "path", state->path_.c_str(), "err", err.c_str());
  } else {
    const size_t column_count = state->parquet_.columns().size();
    state->parquet_cols_.clear();
    state->parquet_cols_.resize(column_count);
    // The reader materialises one whole column at a time (column-at-a-time, not
    // row-at-a-time): every projected column is decoded once here and rows are
    // then served from memory. Correct for M1, but memory is O(file) per column;
    // a row-group iterator is the fix for very large files.
    for (size_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
      if (!state->parquet_.ReadColumn(i, &state->parquet_cols_[i], &err)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("read parquet column failed", "path", state->path_.c_str(),
                 "column", i, "err", err.c_str());
      }
    }
    if (OB_SUCC(ret)) {
      state->parquet_rows_ = state->parquet_.NumRows();
      state->cur_row_ = 0;
    }
  }
  return ret;
}

// spec.cols_def_.at(j)->output_column_idx_ is rewritten by the code generator to the
// index of this column's expression in spec.column_exprs_ -- but only for columns the
// query actually references (count(*) references none). Unreferenced columns keep the
// resolver-time value, i.e. the column id (OB_APP_MIN_COLUMN_ID + j), which is out of
// range for any realistic column count and therefore read as "not projected".
static ObExpr *fs_target_expr(const ObJsonTableSpec &spec, int64_t col_idx)
{
  ObExpr *expr = NULL;
  if (col_idx >= 0 && col_idx < spec.cols_def_.count()) {
    const int64_t expr_idx = spec.cols_def_.at(col_idx)->output_column_idx_;
    if (expr_idx >= 0 && expr_idx < spec.column_exprs_.count()) {
      expr = spec.column_exprs_.at(expr_idx);
    }
  }
  return expr;
}

// glob() produces one VARCHAR column named `file`; that shape is the only signal that
// the plan was built by glob() when the pattern has no wildcard (see open_file_scan).
static bool fs_plan_is_glob_column(const ObJsonTableSpec &spec)
{
  return 1 == spec.cols_def_.count()
      && ObVarcharType == spec.cols_def_.at(0)->data_type_.get_obj_type()
      && 0 == spec.cols_def_.at(0)->col_name_.case_compare("file");
}

// The resolver inferred the plan schema by scanning this same file, so the scanner must
// reproduce it column by column. A mismatch means this is not the mode the plan was
// built for (glob() on a plain path) or that the file changed under us; both are better
// reported than rendered as garbage.
static bool fs_schema_matches(const ObJsonTableSpec &spec,
                              const ObFileTableScanState *state)
{
  bool matched = true;
  int64_t scanned_count = 0;
  if (ObFileTableScanState::FMT_CSV == state->fmt_) {
    scanned_count = static_cast<int64_t>(state->csv_.schema().size());
  } else if (ObFileTableScanState::FMT_JSONL == state->fmt_) {
    scanned_count = static_cast<int64_t>(state->jsonl_.schema().size());
  } else {
    scanned_count = static_cast<int64_t>(state->parquet_.columns().size());
  }
  if (scanned_count != spec.cols_def_.count()) {
    matched = false;
  }
  for (int64_t i = 0; matched && i < scanned_count; ++i) {
    ObObjType scanned_type = ObMaxType;
    const std::string *scanned_name = NULL;
    if (ObFileTableScanState::FMT_CSV == state->fmt_) {
      const ObCsvColumnSchema &col = state->csv_.schema().at(static_cast<size_t>(i));
      scanned_name = &col.name;
      switch (col.type) {
        case OB_CSV_TYPE_BIGINT:   scanned_type = ObIntType;      break;
        case OB_CSV_TYPE_DOUBLE:   scanned_type = ObDoubleType;   break;
        case OB_CSV_TYPE_BOOLEAN:  scanned_type = ObTinyIntType;  break;
        case OB_CSV_TYPE_DATE:     scanned_type = ObDateType;     break;
        case OB_CSV_TYPE_DATETIME: scanned_type = ObDateTimeType; break;
        default:                   scanned_type = ObVarcharType;  break;
      }
    } else if (ObFileTableScanState::FMT_JSONL == state->fmt_) {
      const ObJsonlColumnSchema &col = state->jsonl_.schema().at(static_cast<size_t>(i));
      scanned_name = &col.name;
      switch (col.type) {
        case OB_JSONL_TYPE_BIGINT:  scanned_type = ObIntType;     break;
        case OB_JSONL_TYPE_DOUBLE:  scanned_type = ObDoubleType;  break;
        case OB_JSONL_TYPE_BOOLEAN: scanned_type = ObTinyIntType; break;
        default:                    scanned_type = ObVarcharType; break;
      }
    } else {
      const obparquet::ColumnInfo &col = state->parquet_.columns().at(static_cast<size_t>(i));
      scanned_name = &col.name;
      switch (col.physical_type) {
        case obparquet::PT_INT32:
        case obparquet::PT_INT64:  scanned_type = ObIntType;    break;
        case obparquet::PT_FLOAT:
        case obparquet::PT_DOUBLE: scanned_type = ObDoubleType; break;
        default:                   scanned_type = ObVarcharType; break;
      }
    }
    if (scanned_type != spec.cols_def_.at(i)->data_type_.get_obj_type()) {
      matched = false;
    } else if (NULL != scanned_name) {
      // names too: a different header mode (header = false) keeps the column count but
      // renames the columns and would silently drop the first data row.
      const ObString plan_name = spec.cols_def_.at(i)->col_name_;
      const ObString scanner_name(static_cast<int32_t>(scanned_name->size()),
                                  scanned_name->c_str());
      if (0 != plan_name.case_compare(scanner_name)) {
        matched = false;
      }
    }
  }
  return matched;
}

// NOTE: the varchar payload points into the scanner's row buffer, which stays valid
// until the next next_row() call. That is enough here because this operator is not
// vectorized (ObOpSpec::is_vectorized() == false for PHY_JSON_TABLE), so the framework
// pulls exactly one row per batch and consumes it before asking for the next one.
static int fs_write_csv_value(ObEvalCtx &ctx, ObExpr *expr, const ObCsvValue &value,
                              const ObTimeConvertCtx &cvrt_ctx)
{
  int ret = OB_SUCCESS;
  ObDatum &datum = expr->locate_datum_for_write(ctx);
  if (value.is_null) {
    datum.set_null();
  } else {
    switch (value.type) {
      case OB_CSV_TYPE_BIGINT:
        datum.set_int(value.int_value);
        break;
      case OB_CSV_TYPE_DOUBLE:
        datum.set_double(value.double_value);
        break;
      case OB_CSV_TYPE_BOOLEAN:
        datum.set_bool(value.bool_value);
        break;
      case OB_CSV_TYPE_DATE: {
        int32_t date_value = 0;
        const ObString text(static_cast<int32_t>(value.string_value.size()),
                            value.string_value.c_str());
        if (OB_FAIL(ObTimeConverter::str_to_date(text, date_value))) {
          LOG_WARN("convert csv date failed", K(text), K(ret));
          datum.set_null();
          ret = OB_SUCCESS;
        } else {
          datum.set_date(date_value);
        }
        break;
      }
      case OB_CSV_TYPE_DATETIME: {
        int64_t datetime_value = 0;
        const ObString text(static_cast<int32_t>(value.string_value.size()),
                            value.string_value.c_str());
        if (OB_FAIL(ObTimeConverter::str_to_datetime(text, cvrt_ctx, datetime_value))) {
          LOG_WARN("convert csv datetime failed", K(text), K(ret));
          datum.set_null();
          ret = OB_SUCCESS;
        } else {
          datum.set_datetime(datetime_value);
        }
        break;
      }
      case OB_CSV_TYPE_VARCHAR:
      case OB_CSV_TYPE_SQL_NULL:
      default:
        datum.set_string(value.string_value.c_str(),
                         static_cast<uint32_t>(value.string_value.size()));
        break;
    }
  }
  if (OB_SUCC(ret)) {
    expr->get_eval_info(ctx).evaluated_ = true;
  }
  return ret;
}

static int fs_write_jsonl_value(ObEvalCtx &ctx, ObExpr *expr, const ObJsonlValue &value)
{
  int ret = OB_SUCCESS;
  ObDatum &datum = expr->locate_datum_for_write(ctx);
  if (value.is_null) {
    datum.set_null();
  } else {
    switch (value.type) {
      case OB_JSONL_TYPE_BIGINT:
        datum.set_int(value.int_value);
        break;
      case OB_JSONL_TYPE_DOUBLE:
        datum.set_double(value.double_value);
        break;
      case OB_JSONL_TYPE_BOOLEAN:
        datum.set_bool(value.bool_value);
        break;
      case OB_JSONL_TYPE_VARCHAR:
      case OB_JSONL_TYPE_SQL_NULL:
      default:
        datum.set_string(value.string_value.c_str(),
                         static_cast<uint32_t>(value.string_value.size()));
        break;
    }
  }
  if (OB_SUCC(ret)) {
    expr->get_eval_info(ctx).evaluated_ = true;
  }
  return ret;
}

static int fs_write_parquet_value(ObEvalCtx &ctx, ObExpr *expr,
                                  const obparquet::ColumnData &column, int64_t row)
{
  int ret = OB_SUCCESS;
  ObDatum &datum = expr->locate_datum_for_write(ctx);
  const bool is_null = row >= static_cast<int64_t>(column.is_null.size())
                       || 0 != column.is_null.at(static_cast<size_t>(row));
  if (is_null) {
    datum.set_null();
  } else if (row < static_cast<int64_t>(column.values.size())) {
    datum.set_int(column.values.at(static_cast<size_t>(row)));
  } else if (row < static_cast<int64_t>(column.doubles.size())) {
    datum.set_double(column.doubles.at(static_cast<size_t>(row)));
  } else if (row < static_cast<int64_t>(column.bytes.size())) {
    const std::string &text = column.bytes.at(static_cast<size_t>(row));
    datum.set_string(text.c_str(), static_cast<uint32_t>(text.size()));
  } else {
    datum.set_null();
  }
  if (OB_SUCC(ret)) {
    expr->get_eval_info(ctx).evaluated_ = true;
  }
  return ret;
}

#define EVAL_COVER_CODE(jt_ctx_ptr, error_code) \
{\
  if ((jt_ctx_ptr)->is_cover_error_) { \
    error_code = (jt_ctx_ptr)->error_code_; \
  } \
}

#define RESET_COVER_CODE(jt_ctx_ptr) \
{\
  if ((jt_ctx_ptr)->is_cover_error_) { \
    (jt_ctx_ptr)->is_cover_error_ = false; \
    (jt_ctx_ptr)->error_code_ = 0; \
  } \
}

ObJtColInfo::ObJtColInfo()
  : col_type_(0),
    truncate_(0),
    format_json_(0),
    wrapper_(0),
    allow_scalar_(0),
    output_column_idx_(-1),
    empty_expr_id_(-1),
    error_expr_id_(-1),
    col_name_(),
    path_(),
    on_empty_(3),
    on_error_(3),
    on_mismatch_(3),
    on_mismatch_type_(3),
    data_type_(),
    parent_id_(-1),
    id_(-1) {}

ObJtColInfo::ObJtColInfo(const ObJtColInfo& info)
  : col_type_(info.col_type_),
    truncate_(info.truncate_),
    format_json_(info.format_json_),
    wrapper_(info.wrapper_),
    allow_scalar_(info.allow_scalar_),
    output_column_idx_(info.output_column_idx_),
    empty_expr_id_(info.empty_expr_id_),
    error_expr_id_(info.error_expr_id_),
    col_name_(info.col_name_),
    path_(info.path_),
    on_empty_(info.on_empty_),
    on_error_(info.on_error_),
    on_mismatch_(info.on_mismatch_),
    on_mismatch_type_(info.on_mismatch_type_),
    data_type_(info.data_type_),
    parent_id_(info.parent_id_),
    id_(info.id_) {}


int ObJtColInfo::deep_copy(const ObJtColInfo& src, ObIAllocator* allocator) 
{
  int ret = OB_SUCCESS;
  if (src.col_name_.length() > 0) {
    void *name_buf = allocator->alloc(src.col_name_.length());
    if (OB_ISNULL(name_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(name_buf, src.col_name_.ptr(), src.col_name_.length());
      col_name_.assign(static_cast<char*>(name_buf), src.col_name_.length());
    }
  }
  
  if (OB_SUCC(ret) && src.path_.length() > 0) {
    void *path_buf = allocator->alloc(src.path_.length());
    if (OB_ISNULL(path_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      MEMCPY(path_buf, src.path_.ptr(), src.path_.length());
      path_.assign(static_cast<char*>(path_buf), src.path_.length());
    }
  }

  if (OB_SUCC(ret)) {
    col_type_ = src.col_type_;
    truncate_ = src.truncate_;
    format_json_ = src.format_json_;
    wrapper_ = src.wrapper_;
    allow_scalar_ = src.allow_scalar_;
    output_column_idx_ = src.output_column_idx_;
    empty_expr_id_ = src.empty_expr_id_;
    error_expr_id_ = src.error_expr_id_;
    on_empty_ = src.on_empty_;
    on_error_ = src.on_error_;
    on_mismatch_ = src.on_mismatch_;
    on_mismatch_type_ = src.on_mismatch_type_;
    data_type_ = src.data_type_;
    parent_id_ = src.parent_id_;
    id_ = src.id_;
  }
  return ret;
}

int ObJtColInfo::serialize(char *buf, int64_t buf_len, int64_t &pos) const
{
  INIT_SUCC(ret);
  OB_UNIS_ENCODE(col_type_);
  OB_UNIS_ENCODE(truncate_);
  OB_UNIS_ENCODE(format_json_);
  OB_UNIS_ENCODE(wrapper_);
  OB_UNIS_ENCODE(allow_scalar_);
  OB_UNIS_ENCODE(output_column_idx_);
  OB_UNIS_ENCODE(empty_expr_id_);
  OB_UNIS_ENCODE(error_expr_id_);
  OB_UNIS_ENCODE(col_name_);
  OB_UNIS_ENCODE(path_);
  OB_UNIS_ENCODE(on_empty_);
  OB_UNIS_ENCODE(on_error_);
  OB_UNIS_ENCODE(on_mismatch_);
  OB_UNIS_ENCODE(on_mismatch_type_);
  OB_UNIS_ENCODE(data_type_);
  OB_UNIS_ENCODE(parent_id_);
  OB_UNIS_ENCODE(id_);

  return ret;
}

int ObJtColInfo::deserialize(const char *buf, const int64_t data_len, int64_t &pos)
{
  INIT_SUCC(ret);
  OB_UNIS_DECODE(col_type_);
  OB_UNIS_DECODE(truncate_);
  OB_UNIS_DECODE(format_json_);
  OB_UNIS_DECODE(wrapper_);
  OB_UNIS_DECODE(allow_scalar_);
  OB_UNIS_DECODE(output_column_idx_);
  OB_UNIS_DECODE(empty_expr_id_);
  OB_UNIS_DECODE(error_expr_id_);
  OB_UNIS_DECODE(col_name_);
  OB_UNIS_DECODE(path_);
  OB_UNIS_DECODE(on_empty_);
  OB_UNIS_DECODE(on_error_);
  OB_UNIS_DECODE(on_mismatch_);
  OB_UNIS_DECODE(on_mismatch_type_);
  OB_UNIS_DECODE(data_type_);
  OB_UNIS_DECODE(parent_id_);
  OB_UNIS_DECODE(id_);
  return ret;
}

int64_t ObJtColInfo::get_serialize_size() const
{
  int64_t len = 0;
  OB_UNIS_ADD_LEN(col_type_);
  OB_UNIS_ADD_LEN(truncate_);
  OB_UNIS_ADD_LEN(format_json_);
  OB_UNIS_ADD_LEN(wrapper_);
  OB_UNIS_ADD_LEN(allow_scalar_);
  OB_UNIS_ADD_LEN(output_column_idx_);
  OB_UNIS_ADD_LEN(empty_expr_id_);
  OB_UNIS_ADD_LEN(error_expr_id_);
  OB_UNIS_ADD_LEN(col_name_);
  OB_UNIS_ADD_LEN(path_);
  OB_UNIS_ADD_LEN(on_empty_);
  OB_UNIS_ADD_LEN(on_error_);
  OB_UNIS_ADD_LEN(on_mismatch_);
  OB_UNIS_ADD_LEN(on_mismatch_type_);
  OB_UNIS_ADD_LEN(data_type_);
  OB_UNIS_ADD_LEN(parent_id_);
  OB_UNIS_ADD_LEN(id_);

  return len;
}

int ObJtColInfo::from_JtColBaseInfo(const ObJtColBaseInfo& info)
{
  INIT_SUCC(ret);
  col_type_ = info.col_type_;
  truncate_ = info.truncate_;
  format_json_ = info.format_json_;
  wrapper_ = info.wrapper_;
  allow_scalar_ = info.allow_scalar_;
  output_column_idx_ = info.output_column_idx_;
  empty_expr_id_ = info.empty_expr_id_;
  error_expr_id_ = info.error_expr_id_;
  col_name_ = info.col_name_;
  path_ = info.path_;
  on_empty_ = info.on_empty_;
  on_error_ = info.on_error_;
  on_mismatch_ = info.on_mismatch_;
  on_mismatch_type_ = info.on_mismatch_type_;
  data_type_ = info.data_type_;
  parent_id_ = info.parent_id_;
  id_ = info.id_;

  return ret;
}

static int construct_table_func_join_node(ObIAllocator* allocator,
                                          const ObJtColInfo& col_info,
                                          JoinNode*& jt_node)
{
  INIT_SUCC(ret);
  void* node_buf = static_cast<void*>(jt_node);
  if (OB_ISNULL(node_buf)) {
    node_buf = allocator->alloc(sizeof(JoinNode));
    if (OB_ISNULL(node_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    jt_node = static_cast<JoinNode*>(new(node_buf)JoinNode());
  } else {
    jt_node = static_cast<JoinNode*>(new(node_buf)JoinNode());
  }
  return ret;
}

static int construct_table_func_reg_node(ObIAllocator* allocator,
                                        const ObJtColInfo& col_info,
                                        ObRegCol*& jt_node)
{
  INIT_SUCC(ret);
  void* node_buf = allocator->alloc(sizeof(ObRegCol));
  if (OB_ISNULL(node_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    jt_node = static_cast<ObRegCol*>(new(node_buf)ObRegCol(col_info));
  }
  return ret;
}

static int construct_table_func_union_node(ObIAllocator* allocator,
                                          const ObJtColInfo& col_info,
                                          UnionNode*& jt_node)
{
  INIT_SUCC(ret);
  void* node_buf = allocator->alloc(sizeof(UnionNode));
  if (OB_ISNULL(node_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    jt_node = static_cast<UnionNode*>(new(node_buf)UnionNode());
  }
  return ret;
}

static int construct_table_func_scan_node(ObIAllocator* allocator,
                                          const ObJtColInfo& col_info,
                                          ScanNode*& jt_node)
{
  INIT_SUCC(ret);
  void* node_buf = allocator->alloc(sizeof(ScanNode));
  if (OB_ISNULL(node_buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    jt_node = static_cast<ScanNode*>(new(node_buf)ScanNode(col_info));
  }
  return ret;
}

void JtColTreeNode::destroy()
{
  regular_cols_.reset();
  for (size_t i = 0; i < nested_cols_.count(); ++i) {
    JtColTreeNode* tmp_node = nested_cols_.at(i);
    tmp_node->destroy();
  }
  nested_cols_.reset();
}

OB_DEF_SERIALIZE(ObJsonTableSpec)
{
  INIT_SUCC(ret);
  BASE_SER((ObJsonTableSpec, ObOpSpec));
  OB_UNIS_ENCODE(value_exprs_.at(0));
  OB_UNIS_ENCODE(column_exprs_);
  OB_UNIS_ENCODE(emp_default_exprs_);
  OB_UNIS_ENCODE(err_default_exprs_);
  OB_UNIS_ENCODE(has_correlated_expr_);
  int32_t column_count = cols_def_.count();
  OB_UNIS_ENCODE(column_count);
  for (size_t i = 0; OB_SUCC(ret) && i < cols_def_.count(); ++i) {
    const ObJtColInfo& info = *cols_def_.at(i);
    OB_UNIS_ENCODE(info);
  }
  if (OB_FAIL(ret)) {
  } else if (table_type_ == MulModeTableType::OB_UNNEST_TABLE_TYPE) {
    OB_UNIS_ENCODE(table_type_);
    int32_t value_exprs_count = value_exprs_.count() - 1;
    OB_UNIS_ENCODE(value_exprs_count);
    for (size_t i = 1; OB_SUCC(ret) && i < value_exprs_.count(); ++i) {
      OB_UNIS_ENCODE(value_exprs_.at(i));
    }
  }

  // File tables (read_csv/read_json/read_parquet/glob) are a fourth mode. Their
  // whole payload is value_exprs_.at(0) (the path) plus column_exprs_/cols_def_,
  // all encoded above, so the only extra wire field is the mode itself. Appending
  // it unconditionally after the mode-specific blocks keeps the json-table and
  // unnest encodings unchanged apart from this trailing int32, and keeps
  // encode/serialize_size/deserialize symmetric for the file case.
  int32_t ser_table_type = static_cast<int32_t>(table_type_);
  OB_UNIS_ENCODE(ser_table_type);
  
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObJsonTableSpec)
{
  int64_t len = 0;
  BASE_ADD_LEN((ObJsonTableSpec, ObOpSpec));
  OB_UNIS_ADD_LEN(value_exprs_.at(0));
  OB_UNIS_ADD_LEN(column_exprs_);
  OB_UNIS_ADD_LEN(emp_default_exprs_);
  OB_UNIS_ADD_LEN(err_default_exprs_);
  OB_UNIS_ADD_LEN(has_correlated_expr_);
  
  int32_t column_count = cols_def_.count();
  OB_UNIS_ADD_LEN(column_count);
  for (size_t i = 0; i < cols_def_.count(); ++i) {
    const ObJtColInfo& info = *cols_def_.at(i);
    OB_UNIS_ADD_LEN(info);
  }
  if (table_type_ == MulModeTableType::OB_UNNEST_TABLE_TYPE) {
    OB_UNIS_ADD_LEN(table_type_);
    int32_t value_exprs_count = value_exprs_.count() - 1;
    OB_UNIS_ADD_LEN(value_exprs_count);
    for (size_t i = 1; i < value_exprs_.count(); ++i) {
      OB_UNIS_ADD_LEN(value_exprs_.at(i));
    }
  }
  // trailing file-table mode flag, see OB_DEF_SERIALIZE(ObJsonTableSpec)
  int32_t ser_table_type = static_cast<int32_t>(table_type_);
  OB_UNIS_ADD_LEN(ser_table_type);
  return len;
}

OB_DEF_DESERIALIZE(ObJsonTableSpec)
{
  INIT_SUCC(ret);
  BASE_DESER((ObJsonTableSpec, ObOpSpec));
  ObExpr* value_expr = NULL;
  OB_UNIS_DECODE(value_expr);
  OB_UNIS_DECODE(column_exprs_);
  OB_UNIS_DECODE(emp_default_exprs_);
  OB_UNIS_DECODE(err_default_exprs_);
  OB_UNIS_DECODE(has_correlated_expr_);
  
  int32_t column_count = 0;
  int8_t table_type_flag = OB_JSON_TABLE;
  OB_UNIS_DECODE(column_count);
  
  if (OB_SUCC(ret) && OB_FAIL(cols_def_.init(column_count))) {
  }

  for (size_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
    void* col_info_buf = alloc_->alloc(sizeof(ObJtColInfo));
    if (OB_ISNULL(col_info_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ObJtColInfo* col_info = static_cast<ObJtColInfo*>(new (col_info_buf) ObJtColInfo());
      ObJtColInfo& tmp_col_info = *col_info;
      OB_UNIS_DECODE(tmp_col_info);
      *col_info = tmp_col_info;
      if (OB_FAIL(cols_def_.push_back(col_info))) {
      } else if (col_info->col_type_ == COL_TYPE_UNNEST) {
        table_type_flag = OB_UNNEST_TABLE;
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (table_type_flag == OB_UNNEST_TABLE) {
    OB_UNIS_DECODE(table_type_);
    int32_t value_exprs_count = 0;
    OB_UNIS_DECODE(value_exprs_count);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(value_exprs_.init(value_exprs_count + 1))) {
    } else if (OB_FAIL(value_exprs_.push_back(value_expr))) {
    }
    for (size_t i = 0; OB_SUCC(ret) && i < value_exprs_count; ++i) {
      OB_UNIS_DECODE(value_expr);
      if (OB_SUCC(ret) && OB_FAIL(value_exprs_.push_back(value_expr))) {
      }
    }
  } else if (OB_FAIL(value_exprs_.init(1))) {
  } else if (OB_FAIL(value_exprs_.push_back(value_expr))) {
  }

  // trailing file-table mode flag, see OB_DEF_SERIALIZE(ObJsonTableSpec)
  int32_t ser_table_type = 0;
  if (OB_FAIL(ret)) {
  } else {
    OB_UNIS_DECODE(ser_table_type);
    if (ser_table_type == static_cast<int32_t>(MulModeTableType::OB_FILE_TABLE_TYPE)) {
      table_type_ = MulModeTableType::OB_FILE_TABLE_TYPE;
    }
  }
  return ret;
}

int ObJsonTableOp::generate_table_exec_tree(ObIAllocator* allocator,
                                            const JtColTreeNode& orig_col,
                                            JoinNode*& join_col)
{
  INIT_SUCC(ret);
  
  int reg_count = orig_col.regular_cols_.count();
  int nest_count = orig_col.nested_cols_.count();
  ScanNode* scan_col = nullptr;
  
  if (OB_FAIL(construct_table_func_join_node(allocator, orig_col.col_base_info_, join_col))) {
  } else if (OB_FAIL(construct_table_func_scan_node(allocator, orig_col.col_base_info_, scan_col))) {
  } else {
    join_col->set_left(scan_col);
  }
  
  for (int i = 0; OB_SUCC(ret) && i < reg_count; ++i) {
    ObRegCol* reg_node = nullptr;
    if (OB_FAIL(construct_table_func_reg_node(allocator, orig_col.regular_cols_.at(i)->col_base_info_, reg_node))) {
    } else {
      if (OB_FAIL(scan_col->add_reg_column_node(reg_node))) {
      }
    } 
  }

  if (OB_SUCC(ret) && nest_count > 0) {
    common::ObArray<UnionNode*> ji_nodes;
    for (size_t i = 0; OB_SUCC(ret) && i < nest_count; ++i) {
      UnionNode* ji_node = nullptr;
      if (OB_FAIL(construct_table_func_union_node(allocator, orig_col.nested_cols_.at(i)->col_base_info_, ji_node))) {
      } else if (OB_FAIL(ji_nodes.push_back(ji_node))) {
      }
    }

    if (OB_SUCC(ret)) {
      int j = 0;
      UnionNode* last_node = nullptr;
      join_col->set_right(ji_nodes.at(j));
      last_node = ji_nodes.at(j);
      ++j;

      while (j < nest_count) { // nested col/union node in right child
        UnionNode* cur_node = ji_nodes.at(j);
        last_node->set_right(cur_node);
        last_node = cur_node;
        ++j;
      };

      for (int i = 0; OB_SUCC(ret) && i < nest_count; ++i) {
        JoinNode* col_node = nullptr;
        if (OB_FAIL(generate_table_exec_tree(allocator, *orig_col.nested_cols_.at(i), col_node))) {
        } else {
          ji_nodes.at(i)->set_left(col_node);
        }
      }
    }
  }
  
  return ret;
}

int ObJsonTableOp::generate_table_exec_tree()
{
  INIT_SUCC(ret);
  if (OB_FAIL(generate_column_trees(def_root_))) {
  } else if (OB_FAIL(generate_table_exec_tree(allocator_, *def_root_, root_))) {
  }
  return ret;
}

int ObJsonTableSpec::dup_origin_column_defs(ObIArray<ObJtColBaseInfo*>& columns)
{
  INIT_SUCC(ret);

  for (size_t i = 0; OB_SUCC(ret) && i < columns.count(); ++i) {
    void* col_info_buf = alloc_->alloc(sizeof(ObJtColInfo));
    if (OB_ISNULL(col_info_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      ObJtColInfo col_info;
      if (OB_FAIL(col_info.from_JtColBaseInfo(*columns.at(i)))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObJtColInfo* col = static_cast<ObJtColInfo*>(new (col_info_buf) ObJtColInfo());
        if (OB_FAIL(col->deep_copy(col_info, alloc_))) {
        } else if (OB_FAIL(cols_def_.push_back(col))) {
        }
      }
    }
  }

  return ret;
}

int ObJsonTableOp::find_column(int32_t id, JtColTreeNode* root, JtColTreeNode*& col)
{
  INIT_SUCC(ret);
  common::ObArray<JtColTreeNode*> col_stack;
  if (OB_FAIL(col_stack.push_back(root))) {
  }

  bool exists = false;

  while (OB_SUCC(ret) && !exists && col_stack.count() > 0) {
    JtColTreeNode* cur_col = col_stack.at(col_stack.count() - 1);
    if (cur_col->col_base_info_.id_ == id) {
      exists = true;
      col = cur_col;
    } else if (cur_col->col_base_info_.parent_id_ < 0 
               || cur_col->col_base_info_.col_type_ == static_cast<int32_t>(NESTED_COL_TYPE)) {
      col_stack.remove(col_stack.count() - 1);
      for (size_t i = 0; !exists && i < cur_col->nested_cols_.count(); ++i) {
        JtColTreeNode* nest_col = cur_col->nested_cols_.at(i);
        if (nest_col->col_base_info_.id_ == id) {
          exists = true;
          col = nest_col;
        } else if (nest_col->col_base_info_.col_type_ == static_cast<int32_t>(NESTED_COL_TYPE) 
                  && OB_FAIL(col_stack.push_back(nest_col))) {
        }
      }
    }
  }

  if (OB_SUCC(ret) && !exists) {
    ret = OB_ERR_UNEXPECTED;
  } 
  return ret;
}

int ObJsonTableOp::generate_column_trees(JtColTreeNode*& root)
{
  INIT_SUCC(ret);
  
  const ObJsonTableSpec* spec_ptr = reinterpret_cast<const ObJsonTableSpec*>(&spec_);
  const ObIArray<ObJtColInfo*>& plain_def = spec_ptr->cols_def_;
  
  for (size_t i = 0; OB_SUCC(ret) && i < plain_def.count(); ++i) {
    const ObJtColInfo& info = *plain_def.at(i);
    JtColTreeNode* col_def = static_cast<JtColTreeNode*>(allocator_->alloc(sizeof(JtColTreeNode)));
    if (OB_ISNULL(col_def)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      col_def = new (col_def) JtColTreeNode(info);
    }

    if (OB_FAIL(ret)) {
    } else {
      JtColTreeNode* parent = nullptr;
      if (info.parent_id_ < 0) {
        root = col_def;
      } else if (OB_FAIL(find_column(info.parent_id_, root, parent))) {
      } else if (info.col_type_ == static_cast<int32_t>(NESTED_COL_TYPE)) {
        if (OB_FAIL(parent->nested_cols_.push_back(col_def))) {
        }
      } else if (OB_FAIL(parent->regular_cols_.push_back(col_def))) {
      }
    }
  }

  return ret;
}

int ObJsonTableOp::inner_open()
{
  INIT_SUCC(ret);
  if (OB_FAIL(init())) {
  } else if (MY_SPEC.value_exprs_.empty()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (jt_ctx_.is_file_table_func()) {
    // file mode: no json/unnest exec tree, the scanner is opened instead
    if (OB_FAIL(open_file_scan())) {
    } else {
      is_evaled_ = false;
    }
  } else if (OB_FAIL(root_->open(&jt_ctx_))) {
  } else {
    is_evaled_ = false;
  }

  return ret;
}

int ObJsonTableOp::inner_rescan()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ObOperator::inner_rescan())) {
  } else {
    jt_ctx_.row_alloc_.reuse();
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(reset_variable())) {
  }
  return ret;
}

int ObJsonTableOp::reset_variable()
{
  INIT_SUCC(ret);
  jt_ctx_.is_cover_error_ = false;
  jt_ctx_.error_code_ = 0;
  jt_ctx_.is_need_end_ = 0;
  
  if (OB_FAIL(ret)) {
  } else if (MY_SPEC.value_exprs_.empty()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (jt_ctx_.is_file_table_func()) {
    // file mode has no json/unnest root to reset; rewind or reopen the scanner
    if (OB_FAIL(reset_file_scan())) {
    } else {
      is_evaled_ = false;
    }
  } else if (OB_FAIL(root_->reset(&jt_ctx_))) {
  } else {
    is_evaled_ = false;
  }

  return ret;
}

int ObJsonTableOp::switch_iterator()
{
  INIT_SUCC(ret);
  return OB_ITER_END;
}

int ObJsonTableOp::init()
{
  INIT_SUCC(ret);
  
  if (!is_inited_) {
    const ObJsonTableSpec* spec_ptr = reinterpret_cast<const ObJsonTableSpec*>(&spec_);
    jt_ctx_.spec_ptr_ = const_cast<ObJsonTableSpec*>(spec_ptr);
    if (jt_ctx_.is_file_table_func()) {
      // File mode: no json document to shred, so no exec tree and no table func
      // object. The shared JtScanCtx fields are still initialised because the
      // datum writers use eval_ctx_ and row_alloc_ lives there.
      const sql::ObSQLSessionInfo *session = get_exec_ctx().get_my_session();
      if (OB_ISNULL(session)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        is_inited_ = true;
        jt_ctx_.eval_ctx_ = &eval_ctx_;
        jt_ctx_.exec_ctx_ = &get_exec_ctx();
        jt_ctx_.op_exec_alloc_ = allocator_;
        jt_ctx_.is_evaled_ = false;
        jt_ctx_.is_charset_converted_ = false;
        jt_ctx_.res_obj_ = nullptr;
        jt_ctx_.jt_op_ = this;
        jt_ctx_.is_const_input_ = !MY_SPEC.has_correlated_expr_;
      }
    } else if (OB_FAIL(generate_table_exec_tree())) {
    } else {
      const sql::ObSQLSessionInfo *session = get_exec_ctx().get_my_session();
      if (OB_ISNULL(session)) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        is_inited_ = true;
        jt_ctx_.spec_ptr_ = const_cast<ObJsonTableSpec*>(spec_ptr);
        jt_ctx_.eval_ctx_ = &eval_ctx_;
        jt_ctx_.exec_ctx_ = &get_exec_ctx();
        
        jt_ctx_.op_exec_alloc_ = allocator_;
        jt_ctx_.is_evaled_ = false;
        jt_ctx_.is_charset_converted_ = false;
        jt_ctx_.res_obj_ = nullptr;
        jt_ctx_.jt_op_ = this;
        jt_ctx_.is_const_input_ = !MY_SPEC.has_correlated_expr_;
      }
    }
    void* table_func_buf = NULL;
    if (OB_FAIL(ret)) {
    } else if (jt_ctx_.is_json_table_func()) {
      table_func_buf = jt_ctx_.op_exec_alloc_->alloc(sizeof(JsonTableFunc));
      if (OB_ISNULL(table_func_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_ISNULL(jt_ctx_.table_func_ = new (table_func_buf) JsonTableFunc())) {
        ret = OB_ERR_UNEXPECTED;
      }
    } else if (jt_ctx_.is_unnest_table_func()) {
      table_func_buf = jt_ctx_.op_exec_alloc_->alloc(sizeof(UnnestTableFunc));
      if (OB_ISNULL(table_func_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_ISNULL(jt_ctx_.table_func_ = new (table_func_buf) UnnestTableFunc())) {
        ret = OB_ERR_UNEXPECTED;
      }
    }
  }
  jt_ctx_.is_cover_error_ = false;
  jt_ctx_.error_code_ = 0;
  jt_ctx_.is_need_end_ = 0;
  return ret;
}

int ObJsonTableOp::inner_close()
{
  INIT_SUCC(ret);
  if (OB_NOT_NULL(root_)) {
    if (OB_FAIL(root_->reset(&jt_ctx_))) {
    }
    root_->destroy();
  }
  if (OB_NOT_NULL(def_root_)) { 
    def_root_->destroy();
  }
  jt_ctx_.row_alloc_.clear();
  if (OB_NOT_NULL(jt_ctx_.table_func_) && OB_NOT_NULL(jt_ctx_.op_exec_alloc_)) {
    jt_ctx_.op_exec_alloc_->free(jt_ctx_.table_func_);
  }
  destroy_file_scan();
  return ret;
}

void ObJsonTableOp::reset_columns()
{
  for (size_t i = 0; i < col_count_; ++i) {
    ObExpr* col_expr = jt_ctx_.spec_ptr_->column_exprs_.at(i);
    col_expr->locate_datum_for_write(*jt_ctx_.eval_ctx_).reset();
    col_expr->locate_datum_for_write(*jt_ctx_.eval_ctx_).set_null();
    col_expr->get_eval_info(*jt_ctx_.eval_ctx_).evaluated_ = true;
  }
}

void ScanNode::reset_reg_columns(JtScanCtx* ctx)
{
  for (size_t i = 0; i < reg_column_count(); ++i) {
    ObExpr* col_expr = ctx->spec_ptr_->column_exprs_.at(reg_col_node(i)->col_info_.output_column_idx_);
    col_expr->locate_datum_for_write(*ctx->eval_ctx_).reset();
    col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
    col_expr->get_eval_info(*ctx->eval_ctx_).evaluated_ = true;
  }
}

void ObJsonTableOp::destroy()
{
  destroy_file_scan();
  ObOperator::destroy();
}

int RegularCol::check_item_method_json(ObRegCol &col_node, JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  ObExpr* expr = ctx->spec_ptr_->column_exprs_.at(col_node.col_info_.output_column_idx_);
  if (col_node.type() == COL_TYPE_QUERY) {
    if (col_node.expr_param_.dst_type_ != ObVarcharType 
         && col_node.expr_param_.dst_type_ != ObLongTextType 
         && col_node.expr_param_.dst_type_ != ObJsonType) {
      ret = OB_ERR_INVALID_DATA_TYPE_RETURNING;
      LOG_USER_ERROR(OB_ERR_INVALID_DATA_TYPE_RETURNING);
    } else if (OB_ISNULL(col_node.expr_param_.json_path_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (col_node.expr_param_.json_path_->is_last_func() 
                && OB_FAIL( ObJsonExprHelper::check_item_func_with_return(col_node.expr_param_.json_path_->get_last_node_type(), 
                              col_node.expr_param_.dst_type_, expr->datum_meta_.cs_type_, 1))) {
      if (ret == OB_ERR_INVALID_DATA_TYPE_RETURNING) {
        ret = OB_ERR_INVALID_DATA_TYPE;
        LOG_USER_ERROR(OB_ERR_INVALID_DATA_TYPE, "JSON_TABLE");
      }
    } else if (OB_FAIL(ObExprJsonQuery::check_item_method_valid_with_wrapper(col_node.expr_param_.json_path_,
                                                                             col_node.expr_param_.wrapper_))) {
    }
  } else if (col_node.type() == COL_TYPE_VALUE) {
    if (OB_ISNULL(col_node.expr_param_.json_path_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (col_node.expr_param_.json_path_->is_last_func() 
        && OB_FAIL( ObJsonExprHelper::check_item_func_with_return(col_node.expr_param_.json_path_->get_last_node_type(), 
                                                                  col_node.expr_param_.dst_type_, expr->datum_meta_.cs_type_, 0))) {
      if (ret == OB_ERR_INVALID_DATA_TYPE_RETURNING) {
        ret = OB_ERR_INVALID_DATA_TYPE;
        LOG_USER_ERROR(OB_ERR_INVALID_DATA_TYPE, "JSON_TABLE");
      }
    }
  }
  return ret;
}

int RegularCol::eval_query_col(ObRegCol &col_node, JtScanCtx* ctx, ObExpr* col_expr, bool& is_null)
{
  INIT_SUCC(ret);
  ObJsonArray* in = static_cast<ObJsonArray*>(col_node.curr_);
  is_null = false;
  int8_t use_wrapper = 0;
  if (in->element_count() == 0) {
    is_null = true;
  } else if (in->element_count() == 1 
              && OB_FAIL(ObExprJsonQuery::get_single_obj_wrapper(col_node.col_info_.wrapper_, 
                                          use_wrapper, in[0][0]->json_type(), col_node.col_info_.allow_scalar_))) {
    SET_COVER_ERROR(ctx, ret);
  } else if (in->element_count() > 1 
              && OB_FAIL(ObExprJsonQuery::get_multi_scalars_wrapper_type(col_node.col_info_.wrapper_, use_wrapper))) {
    SET_COVER_ERROR(ctx, ret);
  } else if (!use_wrapper) {
    col_node.curr_ = in[0][0];
  }
  return ret;
}

int RegularCol::eval_value_col(ObRegCol &col_node, JtScanCtx* ctx, ObExpr* col_expr, bool& is_null)
{
  INIT_SUCC(ret);
  is_null = false;
  uint8_t is_type_mismatch = 0;
  ObIJsonBase* in = static_cast<ObIJsonBase*>(col_node.curr_);
  
  if (ob_is_json(col_expr->datum_meta_.type_)) {
  } else if (in->json_type() == ObJsonNodeType::J_OBJECT
             || in->json_type() == ObJsonNodeType::J_ARRAY) {
    ret = OB_ERR_JSON_VALUE_NO_SCALAR;
    SET_COVER_ERROR(ctx, ret);
  } else if ((in->json_type() == ObJsonNodeType::J_INT || in->json_type() == ObJsonNodeType::J_INT)
             && (ob_is_datetime_tc(col_node.col_info_.data_type_.get_obj_type())
                 || ob_is_mysql_datetime_tc(col_node.col_info_.data_type_.get_obj_type()))) {
    char* res_ptr = ctx->buf;
    int len = snprintf(ctx->buf, sizeof(ctx->buf), "%ld", in->get_int());
    if (len > 0) {
      ObJsonString* j_string = nullptr;
      if (OB_ISNULL(j_string = static_cast<ObJsonString*>(ctx->row_alloc_.alloc(sizeof(ObJsonString))))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
        RESET_COVER_CODE(ctx);
      } else {
        col_node.curr_ = new(j_string) ObJsonString(ctx->buf, len);
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      RESET_COVER_CODE(ctx);
    }
  }
  return ret;
}
int RegularCol::eval_exist_col(ObRegCol &col_node, JtScanCtx* ctx, ObExpr* col_expr, bool& is_null)
{
  INIT_SUCC(ret);
  is_null = true;
  if (ob_is_string_type(col_node.col_info_.data_type_.get_obj_type())) {
    ObString value("1");
    void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonString));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      col_node.curr_ = static_cast<ObJsonString*>(new(buf)ObJsonString(value.ptr(), value.length()));
      is_null = false;
    }
  } else {
    void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonInt));
    if (OB_ISNULL(buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      col_node.curr_ = static_cast<ObJsonInt*>(new(buf)ObJsonInt(1));
      is_null = false;
    }
  }
  return ret;
}

int RegularCol::eval_unnest_col(ObRegCol &col_node, void* in, JtScanCtx* ctx, ObExpr* col_expr)
{
  INIT_SUCC(ret);
  col_node.cur_pos_++;
  ObIArrayType **arr_stucts = reinterpret_cast<ObIArrayType**>(in);
  ObIArrayType *arr_obj = arr_stucts[col_node.col_info_.output_column_idx_];
  int32_t idx = col_node.cur_pos_;
  ObExpr* expr = ctx->spec_ptr_->column_exprs_.at(col_node.col_info_.output_column_idx_);
  ObDatum& res_datum = expr->locate_datum_for_write(*ctx->eval_ctx_);

  if (OB_ISNULL(arr_obj) || arr_obj->is_null(idx)) {
    res_datum.set_null();
  } else if (arr_obj->get_format() == Nested_Array) {
    ObArrayNested *arr = static_cast<ObArrayNested*>(arr_obj);
    ObIArrayType* child_arr = NULL;
    ObString res_str;
    if (OB_FAIL(ObArrayTypeObjFactory::construct(*ctx->op_exec_alloc_, *dynamic_cast<const ObCollectionArrayType*>(arr_obj->get_array_type())->element_type_, child_arr))) {
    } else if (OB_FAIL(arr->at(idx, *child_arr))) {
    } else if (OB_FAIL(ObArrayExprUtils::set_array_res(child_arr, child_arr->get_raw_binary_len(), *expr, *ctx->eval_ctx_, res_str))) {
    } else {
      res_datum.set_string(res_str);
    }
  } else {
    ObObj elem_obj;
    if (OB_FAIL(arr_obj->elem_at(idx, elem_obj))) {
    } else {
      res_datum.from_obj(elem_obj);
    }
  }

  return ret;
}

int UnnestTableFunc::eval_input(ObJsonTableOp &jt, JtScanCtx &ctx, ObEvalCtx &eval_ctx)
{
  INIT_SUCC(ret);
  ObIArrayType **arr_objs = NULL;
  int64_t col_num = ctx.spec_ptr_->value_exprs_.count();
  bool is_all_null = true;
  if (!ctx.is_unnest_table_func()) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(arr_objs = static_cast<ObIArrayType**>(ctx.row_alloc_.alloc(col_num * sizeof(ObIArrayType*))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    jt.reset_columns();
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < col_num; ++i) {
    ObExpr *value_expr = ctx.spec_ptr_->value_exprs_.at(i);
    ObDataType data_type = ctx.spec_ptr_->cols_def_.at(i)->data_type_;
    ObObjType obj_type = value_expr->datum_meta_.type_;
    ObDatum *datum = NULL;
    const uint16_t meta_id = value_expr->obj_meta_.get_subschema_id();
    ObIArrayType *arr_obj = NULL;
    bool is_null = false;
    if (obj_type == ObNullType) {
      is_null = true;
    } else if (!ob_is_collection_sql_type(obj_type)) {
      ret = OB_ERR_INVALID_TYPE_FOR_ARGUMENT;
    } else if (OB_FAIL(value_expr->eval(eval_ctx, datum))) {
    } else if (datum->is_null()) {
      is_null = true;
    } else if (OB_FAIL(ObArrayExprUtils::get_array_obj(ctx.row_alloc_, eval_ctx, meta_id, datum->get_string(), arr_obj))) {
    }
    if (OB_SUCC(ret)) {
      arr_objs[i] = arr_obj;
      is_all_null &= is_null;
    }
  }
  if (OB_FAIL(ret)) {
  } else if (is_all_null) {
    ret = OB_ITER_END;
  } else {
    jt.input_ = arr_objs;
  }
  return ret;
}

int UnnestTableFunc::reset_ctx(ObRegCol &scan_node, JtScanCtx*& ctx)
{
  INIT_SUCC(ret);
  return ret;
}

int UnnestTableFunc::init_ctx(ObRegCol &scan_node, JtScanCtx*& ctx)
{
  INIT_SUCC(ret);
  scan_node.tab_type_ = MulModeTableType::OB_UNNEST_TABLE_TYPE;
  return ret;
}

int UnnestTableFunc::reset_path_iter(ObRegCol &scan_node, void* in, JtScanCtx*& ctx, ScanType init_flag, bool &is_null_value)
{
  INIT_SUCC(ret);
  scan_node.iter_ = in;
  if (OB_FAIL(get_iter_value(scan_node, ctx, is_null_value))) {
  }
  return ret;
}

int UnnestTableFunc::get_iter_value(ObRegCol &col_node, JtScanCtx* ctx, bool &is_null_value)
{
  UNUSED(is_null_value);
  INIT_SUCC(ret);
  bool all_iter_end = true;
  col_node.cur_pos_++;
  ObIArrayType **arr_objs = reinterpret_cast<ObIArrayType **>(col_node.iter_);
  int64_t col_num = ctx->spec_ptr_->value_exprs_.count();
  for (int64_t i = 0; OB_SUCC(ret) && i < col_num; ++i) {
    ObIArrayType *arr_obj = arr_objs[i];
    if (OB_ISNULL(arr_obj)) {
      // do nothing
    } else if (col_node.cur_pos_ < arr_obj->size()) {
      all_iter_end = false;
    } else {
      arr_objs[i] = NULL;
    }
  }
  if (OB_SUCC(ret) && all_iter_end) {
    ret = OB_ITER_END;
  }
  return ret;
}

// default value cast type check
bool RegularCol::check_cast_allowed(const ObObjType orig_type,
                                    const ObCollationType orig_cs_type,
                                    const ObObjType expect_type,
                                    const ObCollationType expect_cs_type,
                                    const bool is_explicit_cast)
{
  UNUSED(expect_cs_type);
  bool res = true;
  ObObjTypeClass ori_tc = ob_obj_type_class(orig_type);
  ObObjTypeClass expect_tc = ob_obj_type_class(expect_type);
  bool is_expect_lob_tc = (ObLobTC == expect_tc || ObTextTC == expect_tc);
  bool is_ori_lob_tc = (ObLobTC == ori_tc || ObTextTC == ori_tc);
  return res;
}

// scan node function implement


int ScanNode::add_reg_column_node(ObRegCol* node, bool add_idx)
{ 
  INIT_SUCC(ret);
  if (OB_FAIL(reg_col_defs_.push_back(node))) {
  }
  return ret;
}

// basic function
int ObRegCol::open(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ctx->table_func_->init_ctx(*this, ctx))) {
  } else {
    ord_val_ = -1;
    cur_pos_ = -1;
  }

  return ret;
}

int ObRegCol::reset(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ctx->table_func_->reset_ctx(*this, ctx))) {
  } else {
    ord_val_ = -1;
    cur_pos_ = -1;
  }
  return ret;
}

int UnionNode::open(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ObMultiModeTableNode::open(ctx))) {
  } else if (left_ && OB_FAIL(left_->open(ctx))) {
  } else if (right_ && OB_FAIL(right_->open(ctx))) {
  }
  return ret;
}

int UnionNode::reset(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ObMultiModeTableNode::reset(ctx))) {
  } else if (left_ && OB_FAIL(left_->reset(ctx))) {
  } else if (right_ && OB_FAIL(right_->reset(ctx))) {
  } else {
    is_left_iter_end_ = false;
    is_right_iter_end_ = true;
  }
  return ret;
}

int ScanNode::open(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ObMultiModeTableNode::open(ctx))) {
  } else if (OB_FAIL(seek_node_.open(ctx))) {
  } else {
    for (int i = 0; OB_SUCC(ret) && i < reg_col_defs_.count(); ++i) {
      ObRegCol* node = reg_col_defs_.at(i);
      if (OB_FAIL(node->open(ctx))) {
      }
    }
  }
  return ret;
}

int ScanNode::reset(JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  if (OB_FAIL(ObMultiModeTableNode::reset(ctx))) {
  } else if (OB_FAIL(seek_node_.reset(ctx))) {
  }
  for (int i = 0; OB_SUCC(ret) && i < reg_col_defs_.count(); ++i) {
    ObRegCol* node = reg_col_defs_.at(i);
    ret = node->reset(ctx);
  }
  return ret;
}

void ObMultiModeTableNode::destroy()
{
  // do nothing
}

void ObRegCol::destroy()
{
  // path destory
  if (is_path_evaled_ && OB_NOT_NULL(path_)) {
    is_path_evaled_ = false;
    if (tab_type_ == OB_ORA_JSON_TABLE_TYPE) { // do nothing current
      ObJsonPath* json_path = static_cast<ObJsonPath*>(path_);
      json_path->~ObJsonPath();
    }
  }
}

void UnionNode::destroy()
{
  if (OB_NOT_NULL(left_)) {
    left_->destroy();
  }

  if (OB_NOT_NULL(right_)) {
    right_->destroy();
  }
}

void ScanNode::destroy()
{
  seek_node_.destroy();
  for (size_t i = 0; i < reg_col_defs_.count(); ++i) {
    reg_col_defs_.at(i)->destroy();
  }
  
  reg_col_defs_.reset();
  child_idx_.reset();
}

// common logical : iter
int ObRegCol::eval_regular_col(void *in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);
  JtColType col_type = type();
  is_null_value = false;
  bool is_null_res = false;
  ObExpr* col_expr = ctx->spec_ptr_->column_exprs_.at(col_info_.output_column_idx_);
  ctx->res_obj_ = &col_expr->locate_datum_for_write(*ctx->eval_ctx_);
  bool need_cast_res = true;
  bool enable_error = true;

  if (OB_ISNULL(in)) {
    is_null_value = true;
    need_cast_res = false;
    curr_ = nullptr;
    col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
  } else if (col_type == COL_TYPE_UNNEST) {
    if (OB_FAIL(RegularCol::eval_unnest_col(*this, in, ctx, col_expr))) {
    }
  } else if (col_type == COL_TYPE_ORDINALITY) {
    if (OB_ISNULL(in)) {
      col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
    } else {
      col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_int(ctx->ord_val_);
    }
    col_expr->get_eval_info(*ctx->eval_ctx_).evaluated_ = true;
  } else {
    if (OB_FAIL(ctx->table_func_->col_res_type_check(*this, ctx))) {
    } else if (OB_ISNULL(in)) {
      is_null_value = true;
      curr_ = nullptr;
      EVAL_COVER_CODE(ctx, ret);
    } else if (OB_FAIL(ctx->table_func_->reset_path_iter(*this, in, ctx, ScanType::COL_NODE_TYPE, is_null_res))) {
      RESET_COVER_CODE(ctx);
    } else if (is_null_res) {
      is_null_value = true;
    } else if (OB_FAIL(ctx->table_func_->eval_seek_col(*this, in, ctx, is_null_res, need_cast_res))) {
      SET_COVER_ERROR(ctx, ret);
    } else if (curr_ == nullptr || is_null_res) {
      is_null_value = true;
    } else {
      is_null_value = false;
      if (col_type == COL_TYPE_QUERY) {
        if (OB_FAIL(RegularCol::eval_query_col(*this, ctx, col_expr, is_null_value))) {
        }
      } else if (col_type == COL_TYPE_VALUE) {
        if (OB_FAIL(RegularCol::eval_value_col(*this, ctx, col_expr, is_null_value))) {
        }
      } else if (col_type == COL_TYPE_EXISTS) {
        if (OB_FAIL(RegularCol::eval_exist_col(*this, ctx, col_expr, is_null_value))) {
        }
      }
    }
  }

  if (OB_FAIL(ret)) { // deal empty value
  } else if (col_type == COL_TYPE_EXISTS 
              || col_type == COL_TYPE_QUERY 
              || col_type == COL_TYPE_VALUE) {
    if (is_null_value) {
      if (OB_FAIL(ctx->table_func_->set_on_empty(*this, ctx, need_cast_res, is_null_value))) {
      }
    } else if (ctx->is_json_table_func()
              && curr_ && NOT_DATUM == res_flag_
              && static_cast<ObIJsonBase*>(curr_)->json_type() == ObJsonNodeType::J_NULL) {
      curr_ = nullptr;
    }
  }
  // deal error value
  if (OB_FAIL(ret)) {
    if (ctx->is_cover_error_) {
      enable_error = false;
      int tmp_ret = ctx->table_func_->set_on_error(*this, ctx, ret);
      if (tmp_ret != OB_SUCCESS) {
      }
    }
  }
  // cast_to_res
  if (OB_FAIL(ret)) {
  } else if (col_type == COL_TYPE_EXISTS 
              || col_type == COL_TYPE_QUERY 
              || col_type == COL_TYPE_VALUE) {
    if (OB_ISNULL(curr_)) {
      is_null_value = true;
      col_expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
    } else if (need_cast_res && OB_FAIL(ctx->table_func_->cast_to_result(*this, ctx, enable_error))) {
    } else if (OB_ISNULL(curr_)) {
      is_null_value = true;
    }
  }
  if (OB_SUCC(ret)) {
    res_flag_ = NOT_DATUM;
    col_expr->get_eval_info(*ctx->eval_ctx_).evaluated_ = true;
  }

  return ret;
}

int ScanNode::get_next_iter(void* in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);
  is_null_value = false;
  bool is_null_iter = false;
  if (!is_evaled_ || in_ != in) {
    if (OB_FAIL(seek_node_.reset(ctx))) {
    } else {
      in_ = in;
      is_null_result_ = false;
      if (OB_ISNULL(in_)) {
        is_null_value = is_null_result_ = true;
        seek_node_.curr_ = seek_node_.iter_ = nullptr;
        seek_node_.cur_pos_ = 0;
        seek_node_.total_ = 0;
      } else if (OB_FAIL(ctx->table_func_->reset_path_iter(seek_node_, in_, ctx, ScanType::SCAN_NODE_TYPE, is_null_iter))) {   // reset path & get first result
        RESET_COVER_CODE(ctx);
      } else if (is_null_iter) {
        is_null_value = is_null_result_ = true;
        seek_node_.curr_ = seek_node_.iter_ = nullptr;
        seek_node_.total_ = 1;
        seek_node_.cur_pos_ = 0;
        // 1. if root node seek result is NULL, but input(in) not null,then return end.
        if (seek_node_.col_info_.parent_id_ == common::OB_INVALID_ID 
            || (ctx->jt_op_->get_root_param() == in  // 2. if path == '$' && root scan node not have regular column, 
                && ctx->jt_op_->get_root_entry()->get_scan_node()->reg_column_count() == 0 
                && ctx->jt_op_->get_root_entry()->get_scan_node() == this)) {
          ret = OB_ITER_END;
        }
      }
      if (OB_SUCC(ret)) {
        is_evaled_= true;
        seek_node_.cur_pos_ = 0;
      }
    }
  } else {
    is_null_iter = false;
    if (OB_FAIL(ctx->table_func_->get_iter_value(seek_node_, ctx, is_null_iter))) {
      if (ret == OB_ITER_END) {
        seek_node_.curr_ = seek_node_.iter_ = nullptr;
      } else {
      }
    } else if (is_null_iter) {
      is_null_value = is_null_result_ = true;
      seek_node_.curr_ = seek_node_.iter_ = nullptr;
    } 
  }
  return ret;
}

int ScanNode::get_next_row(void* in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);
  bool is_empty_node = false;
  is_null_value = false;
  if (OB_FAIL(get_next_iter(in, ctx, is_empty_node))) {
  }
  is_null_value = is_empty_node;
  // need reset column 
  reset_reg_columns(ctx);

  // eval regular column
  if (OB_SUCC(ret)) {
    uint32_t reg_count = reg_col_defs_.count();
    bool tmp_is_null = false;
    for (uint32_t i = 0; OB_SUCC(ret) && i < reg_count && is_evaled_; ++i) {
      ObRegCol* cur_node = reg_col_defs_.at(i);
      if (cur_node->type() == COL_TYPE_ORDINALITY) {
        ctx->ord_val_ = seek_node_.cur_pos_ + 1;
      }
      if (OB_FAIL(cur_node->eval_regular_col(seek_node_.iter_, ctx, tmp_is_null))) {
      } else {
        is_null_value &= tmp_is_null;
      }
    } // eval scan node in join node
  }

  return ret;
}

int JoinNode::get_next_row(void* in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);
  is_null_value = false;

  ObMultiModeTableNode* left_node = left();
  ObMultiModeTableNode* right_node = right();

  bool is_left_null = false;
  bool is_right_null = false;
  if (!is_right_iter_end_) {  // right node can expand more value,
  } else if (OB_NOT_NULL(left_)) {
    is_right_iter_end_ = false;
    ret = left_node->get_next_row(in, ctx, is_left_null);
    if (OB_FAIL(ret) && ret != OB_ITER_END) {
    } else if (OB_SUCC(ret)) {
      is_null_value = is_left_null;
    }
  } else {
    ret = OB_ERR_UNEXPECTED;
  }

  bool is_sub_result_null = false; // nested child result is null then 
  if (OB_SUCC(ret)) {
    // child : join ->right child_ = union node 
    if (right_node) {
      if (OB_FAIL(right_node->get_next_row(get_curr_iter_value(), ctx, is_sub_result_null))) {
        if (OB_FAIL(ret) && ret != OB_ITER_END) {
        } else if (ret == OB_ITER_END) {
          is_right_iter_end_ = true;
        }
      } else if (OB_SUCC(ret)) {
        is_null_value &= is_sub_result_null;
        if (is_sub_result_null) {
          is_right_iter_end_ = true;
        }
      }
    } else {
      is_right_iter_end_ = true;
    }
  }
  if (OB_SUCC(ret) && OB_ISNULL(get_curr_iter_value()) 
      && ctx->jt_op_->get_root_entry() == this) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(ret) && is_right_iter_end_) { // nested column evaled finis should get next iter
    if (!is_evaled_) {
      ret = OB_SUCCESS; // ignore only one null result
    } else if (OB_FAIL(get_next_row(in, ctx, is_null_value))) {
      if (ret != OB_ITER_END) {
      }
    }
  } else if (OB_FAIL(ret)) { // if return fail, need reset flag
    is_right_iter_end_ = true;
  }
  is_evaled_ = true;

  return ret;
}

int UnionNode::get_next_row(void* in, JtScanCtx* ctx, bool& is_null_value)
{
  INIT_SUCC(ret);

  ObMultiModeTableNode* left_node = left();
  ObMultiModeTableNode* right_node = right();
  is_null_value = false;
  bool is_left_null = false;
  bool is_right_null = false;
  if (in != in_) {
    is_left_iter_end_ = false;
  }
  if (OB_NOT_NULL(left_node) && !is_left_iter_end_) {
    ret = left_node->get_next_row(in, ctx, is_left_null);
    if (OB_FAIL(ret)) {
      if (ret != OB_ITER_END) {
      } else {
        is_left_iter_end_ = true;
      }
    } else {
      is_null_value = is_left_null;
      if (is_left_null) {
        is_left_iter_end_ = true;
      }
    }
  } else {
    ret = OB_ITER_END;
  }

  if (OB_SUCC(ret)) {
    if (is_left_null && OB_NOT_NULL(right_node)) {
      ret = right_node->get_next_row(in, ctx, is_right_null);
      if (OB_FAIL(ret) &&  ret != OB_ITER_END) {
      } else if (OB_SUCC(ret)) {
        if (!is_right_null) {
          is_null_value = false;
        }
      }
    }
  } else if (OB_NOT_NULL(right_node) && (ret == OB_ITER_END)) {
    ret = right_node->get_next_row(in, ctx, is_right_null);
    if (OB_FAIL(ret) &&  ret != OB_ITER_END) {
    } else if (OB_SUCC(ret) && is_right_null) {
      if (in_ == in) {
        ret = OB_ITER_END;
      }
    } else if (OB_SUCC(ret) && !is_right_null) {
      is_null_value = false;
    }
  } 
  
  in_ = in;
  return ret;
}

// json/xml table function inner
int ObJsonTableOp::inner_get_next_row()
{
  INIT_SUCC(ret);
  bool is_root_null = false;
  // file table mode is dispatched before the json/unnest check: it never builds a
  // root_ or a table_func_, and must not touch either.
  if (jt_ctx_.is_file_table_func()) {
    ret = inner_get_next_row_file();
  } else if (!(jt_ctx_.is_json_table_func()
        || jt_ctx_.is_unnest_table_func())) {
    ret = OB_ERR_UNEXPECTED;
  } else if (is_evaled_) {
    clear_evaluated_flag();
    if (OB_FAIL(root_->get_next_row(input_, &jt_ctx_, is_root_null))) {
      if (ret != OB_ITER_END) {
      }
    }
  } else {
    clear_evaluated_flag();
    if (OB_FAIL(jt_ctx_.table_func_->eval_input(*this, jt_ctx_, *jt_ctx_.eval_ctx_))) {  // get input value
      if (ret != OB_ITER_END) {
      }
    } else if (OB_FAIL(root_->get_next_row(input_, &jt_ctx_, is_root_null))) {
      if (ret != OB_ITER_END) {
      }
    } else {
      is_evaled_ = true;
    }
  }

  return ret;
}

// ---------------------------------------------------------------------------
// File table mode (read_csv / read_json / read_parquet / glob)
// ---------------------------------------------------------------------------

int ObJsonTableOp::open_file_scan()
{
  INIT_SUCC(ret);
  const ObJsonTableSpec &spec = MY_SPEC;
  ObDatum *path_datum = NULL;
  ObString path;
  if (!is_inited_) {
    ret = OB_NOT_INIT;
    LOG_WARN("json table op is not inited", K(ret));
  } else if (OB_ISNULL(file_state_)) {
    void *buf = NULL;
    if (OB_ISNULL(allocator_)) {
      ret = OB_NOT_INIT;
      LOG_WARN("operator allocator is null", K(ret));
    } else if (OB_ISNULL(buf = allocator_->alloc(sizeof(ObFileTableScanState)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("allocate file scan state failed", K(ret));
    } else {
      file_state_ = new (buf) ObFileTableScanState();
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_UNLIKELY(spec.value_exprs_.empty())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file table has no path expression", K(ret));
  } else if (OB_FAIL(spec.value_exprs_.at(0)->eval(eval_ctx_, path_datum))) {
    LOG_WARN("evaluate file path expression failed", K(ret));
  } else if (OB_ISNULL(path_datum) || path_datum->is_null()) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file path is null", K(ret));
  } else if (FALSE_IT(path = path_datum->get_string())) {
  } else if (OB_UNLIKELY(path.empty())) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file path is empty", K(ret));
  } else {
    file_state_->path_.assign(path.ptr(), static_cast<size_t>(path.length()));
    file_state_->fmt_ = fs_detect_format(file_state_->path_);
    file_state_->col_cnt_ = spec.cols_def_.count();
    file_state_->cur_row_ = 0;
    file_state_->cur_file_ = 0;
  }
  if (OB_FAIL(ret)) {
  } else if (ObFileTableScanState::FMT_GLOB == file_state_->fmt_) {
    ret = fs_glob_collect(file_state_->path_, &file_state_->files_);
  } else if (ObFileTableScanState::FMT_CSV == file_state_->fmt_) {
    ret = fs_open_csv_scan(file_state_);
  } else if (ObFileTableScanState::FMT_JSONL == file_state_->fmt_) {
    ret = fs_open_jsonl_scan(file_state_);
  } else if (ObFileTableScanState::FMT_PARQUET == file_state_->fmt_) {
    ret = fs_open_parquet_scan(file_state_);
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("cannot detect file table format", "path", file_state_->path_.c_str());
  }
  if (OB_SUCC(ret) && ObFileTableScanState::FMT_GLOB != file_state_->fmt_
      && !fs_schema_matches(spec, file_state_)) {
    // The plan schema was inferred from this file, so it must match. When it does not,
    // the plan was most likely built by glob() on a path that has no wildcard: only the
    // path travels into the plan, so such a call is indistinguishable from
    // read_<format>() on the same path. glob's single varchar `file` column identifies
    // it; fall back to glob semantics instead of returning mismatched values.
    if (fs_plan_is_glob_column(spec)) {
      file_state_->csv_.close();
      file_state_->jsonl_.close();
      file_state_->parquet_.Close();
      file_state_->parquet_cols_.clear();
      file_state_->fmt_ = ObFileTableScanState::FMT_GLOB;
      ret = fs_glob_collect(file_state_->path_, &file_state_->files_);
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("file schema does not match the inferred plan schema, file changed "
               "between resolve and execute?",
               "path", file_state_->path_.c_str());
    }
  }
  LOG_TRACE("open file scan", K(ret), "path", file_state_->path_.c_str());
  return ret;
}

int ObJsonTableOp::reset_file_scan()
{
  INIT_SUCC(ret);
  if (OB_ISNULL(file_state_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("file scan state is null", K(ret));
  } else if (ObFileTableScanState::FMT_GLOB == file_state_->fmt_) {
    file_state_->cur_file_ = 0;
  } else if (ObFileTableScanState::FMT_PARQUET == file_state_->fmt_) {
    file_state_->cur_row_ = 0;
  } else if (ObFileTableScanState::FMT_CSV == file_state_->fmt_) {
    file_state_->csv_.close();
    ret = fs_open_csv_scan(file_state_);
  } else if (ObFileTableScanState::FMT_JSONL == file_state_->fmt_) {
    file_state_->jsonl_.close();
    ret = fs_open_jsonl_scan(file_state_);
  } else {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("cannot reset file scan", "path", file_state_->path_.c_str());
  }
  return ret;
}

void ObJsonTableOp::destroy_file_scan()
{
  if (OB_NOT_NULL(file_state_)) {
    file_state_->csv_.close();
    file_state_->jsonl_.close();
    file_state_->parquet_.Close();
    file_state_->~ObFileTableScanState();
    if (OB_NOT_NULL(allocator_)) {
      allocator_->free(file_state_);
    }
    file_state_ = nullptr;
  }
}

int ObJsonTableOp::inner_get_next_row_file()
{
  INIT_SUCC(ret);
  const ObJsonTableSpec &spec = MY_SPEC;
  const ObSQLSessionInfo *session = get_exec_ctx().get_my_session();
  const ObTimeZoneInfo *tz_info = NULL;
  std::string err;
  if (NULL != session) {
    tz_info = session->get_timezone_info();
  }
  const ObTimeConvertCtx cvrt_ctx(tz_info, false);
  if (OB_ISNULL(file_state_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("file scan state is null", K(ret));
  } else {
    clear_evaluated_flag();
    if (ObFileTableScanState::FMT_CSV == file_state_->fmt_) {
      if (!file_state_->csv_.next_row(&file_state_->csv_row_, &err)) {
        if (file_state_->csv_.eof()) {
          ret = OB_ITER_END;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("read csv row failed", K(ret), "path", file_state_->path_.c_str(),
                   "err", err.c_str());
        }
      } else {
        const int64_t value_count = static_cast<int64_t>(file_state_->csv_row_.size());
        for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
          ObExpr *expr = fs_target_expr(spec, i);
          if (NULL != expr && OB_FAIL(fs_write_csv_value(eval_ctx_, expr,
                                                         file_state_->csv_row_.at(static_cast<size_t>(i)),
                                                         cvrt_ctx))) {
            LOG_WARN("write csv value failed", K(ret), K(i));
          }
        }
      }
    } else if (ObFileTableScanState::FMT_JSONL == file_state_->fmt_) {
      if (!file_state_->jsonl_.next_row(&file_state_->jsonl_row_, &err)) {
        if (file_state_->jsonl_.eof()) {
          ret = OB_ITER_END;
        } else {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("read jsonl row failed", K(ret), "path", file_state_->path_.c_str(),
                   "err", err.c_str());
        }
      } else {
        const int64_t value_count = static_cast<int64_t>(file_state_->jsonl_row_.size());
        for (int64_t i = 0; OB_SUCC(ret) && i < value_count; ++i) {
          ObExpr *expr = fs_target_expr(spec, i);
          if (NULL != expr && OB_FAIL(fs_write_jsonl_value(eval_ctx_, expr,
                                                           file_state_->jsonl_row_.at(static_cast<size_t>(i))))) {
            LOG_WARN("write jsonl value failed", K(ret), K(i));
          }
        }
      }
    } else if (ObFileTableScanState::FMT_PARQUET == file_state_->fmt_) {
      if (file_state_->cur_row_ >= file_state_->parquet_rows_) {
        ret = OB_ITER_END;
      } else {
        const int64_t row = file_state_->cur_row_;
        const int64_t column_count = static_cast<int64_t>(file_state_->parquet_cols_.size());
        for (int64_t i = 0; OB_SUCC(ret) && i < column_count; ++i) {
          ObExpr *expr = fs_target_expr(spec, i);
          if (NULL != expr && OB_FAIL(fs_write_parquet_value(eval_ctx_, expr,
                                                             file_state_->parquet_cols_.at(static_cast<size_t>(i)),
                                                             row))) {
            LOG_WARN("write parquet value failed", K(ret), K(i), K(row));
          }
        }
        if (OB_SUCC(ret)) {
          ++file_state_->cur_row_;
        }
      }
    } else if (ObFileTableScanState::FMT_GLOB == file_state_->fmt_) {
      if (file_state_->cur_file_ >= static_cast<int64_t>(file_state_->files_.size())) {
        ret = OB_ITER_END;
      } else {
        // glob() has exactly one VARCHAR column named `file`
        ObExpr *expr = fs_target_expr(spec, 0);
        if (NULL != expr) {
          const std::string &file = file_state_->files_.at(static_cast<size_t>(file_state_->cur_file_));
          ObDatum &datum = expr->locate_datum_for_write(eval_ctx_);
          datum.set_string(file.c_str(), static_cast<uint32_t>(file.size()));
          expr->get_eval_info(eval_ctx_).evaluated_ = true;
        }
        ++file_state_->cur_file_;
      }
    } else {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("unknown file table format", K(ret), "path", file_state_->path_.c_str());
    }
  }
  return ret;
}

int JsonTableFunc::col_res_type_check(ObRegCol &col_node, JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  UNUSED(col_node);
  UNUSED(ctx);
  return ret;
}

int JsonTableFunc::check_default_value(JtScanCtx* ctx, ObRegCol &col_node, ObExpr* expr)
{
  INIT_SUCC(ret);
  // in mysql mode, should check default value with parse json
  if (OB_FAIL(RegularCol::check_default_value_mysql(col_node, ctx, expr))) {
  }
  return ret;
}

int RegularCol::parse_default_value_2json(ObExpr* default_expr, 
                                          JtScanCtx* ctx,
                                          ObDatum*& tmp_datum,
                                          ObIJsonBase *&res)
{
  INIT_SUCC(ret);
  ObObjType val_type = default_expr->datum_meta_.type_;
  ObCollationType cs_type = default_expr->datum_meta_.cs_type_;
  ObDatum converted_datum;
  converted_datum.set_datum(*tmp_datum);
  ObString origin_str = converted_datum.get_string();
  // convert string charset if needed
  if (ob_is_string_type(val_type) && ObCharset::charset_type_by_coll(cs_type) != CHARSET_UTF8MB4) {
    ObString converted_str;
    if (OB_FAIL(ObExprUtil::convert_string_collation(origin_str, cs_type, converted_str, 
                                                      CS_TYPE_UTF8MB4_BIN, ctx->row_alloc_))) {
    } else {
      converted_datum.set_string(converted_str);
      cs_type = CS_TYPE_UTF8MB4_BIN;
    }
  }
  origin_str = converted_datum.get_string();
  if (OB_SUCC(ret) 
      && OB_FAIL(ObJsonExprHelper::get_json_val(converted_datum, *ctx->exec_ctx_, default_expr,
                                                ctx->op_exec_alloc_, val_type, cs_type, res))) {
  }
  return ret;
}

int RegularCol::check_default_value_mysql(ObRegCol &col_node, JtScanCtx* ctx, ObExpr* expr)
{
  INIT_SUCC(ret);
  ctx->is_cover_error_ = false; 
  ObIJsonBase* j_res = NULL;
  
  if (col_node.col_info_.on_empty_ == JSN_VALUE_DEFAULT) {
    j_res = static_cast<ObIJsonBase*>(col_node.emp_val_);
    ObExpr* default_expr = ctx->spec_ptr_->emp_default_exprs_.at(col_node.col_info_.empty_expr_id_);
    if (OB_FAIL(RegularCol::check_default_value_inner_mysql(ctx, col_node, default_expr, expr, j_res))) {
    }
  }

  if (OB_SUCC(ret) && col_node.col_info_.on_error_ == JSN_VALUE_DEFAULT) {
    j_res = static_cast<ObIJsonBase*>(col_node.err_val_);
    ObExpr* default_expr = ctx->spec_ptr_->err_default_exprs_.at(col_node.col_info_.error_expr_id_);
    if (OB_FAIL(RegularCol::check_default_value_inner_mysql(ctx, col_node, default_expr, expr, j_res))) {
    }
  } 
  return ret;
}

int RegularCol::check_default_value_inner_mysql(JtScanCtx* ctx,
                                                ObRegCol &col_node,
                                                ObExpr* default_expr,
                                                ObExpr* expr,
                                                ObIJsonBase* j_base)
{
  INIT_SUCC(ret);
  ObDatum res;
  char datum_buff[OBJ_DATUM_STRING_RES_SIZE] = {0};
  res.ptr_ = datum_buff;
  
  ObDatum* tmp_datum = nullptr;
  uint8_t is_type_mismatch = 0;
  ObAccuracy accuracy = col_node.col_info_.data_type_.get_accuracy();
  char col_str[col_node.col_info_.col_name_.length() + 1];
  ObObjType dst_type = expr->datum_meta_.type_;
  ObJsonCastParam cast_param(dst_type, default_expr->datum_meta_.cs_type_, expr->datum_meta_.cs_type_, false);
  cast_param.is_only_check_ = true;
  cast_param.relaxed_time_convert_ = true;
  cast_param.rt_expr_ = expr;

  if (OB_ISNULL(j_base)) {
      ret = OB_ERR_UNEXPECTED;
  } else if ((dst_type != ObJsonType && !j_base->is_json_scalar(j_base->json_type()))) {
    ret = OB_INVALID_DEFAULT;
    LOG_USER_ERROR(OB_INVALID_DEFAULT, col_node.col_info_.col_name_.length(), col_node.col_info_.col_name_.ptr());
  } else if (OB_FAIL(ObJsonUtil::cast_to_res(&ctx->row_alloc_, *ctx->eval_ctx_, 
                            j_base, accuracy, cast_param, res, is_type_mismatch))) {
    ret = OB_OPERATE_OVERFLOW;
    databuff_printf(col_str, col_node.col_info_.col_name_.length() + 1, "%s", col_node.col_info_.col_name_.ptr());
    LOG_USER_ERROR(OB_OPERATE_OVERFLOW, "JSON_TABLE", col_str);
  }
  return ret;
}                                                

int JsonTableFunc::set_expr_exec_param(ObRegCol& col_node, JtScanCtx* ctx)
{
  INIT_SUCC(ret);
  ObExpr* expr = ctx->spec_ptr_->column_exprs_.at(col_node.col_info_.output_column_idx_);

  ObObjType dst_type = expr->datum_meta_.type_;
  ObAccuracy accuracy = col_node.col_info_.data_type_.get_accuracy();
  col_node.expr_param_.truncate_ = col_node.col_info_.truncate_;
  col_node.expr_param_.format_json_ = col_node.col_info_.format_json_;
  col_node.expr_param_.wrapper_ = col_node.col_info_.wrapper_;
  col_node.expr_param_.empty_type_ = col_node.col_info_.on_empty_;
  col_node.expr_param_.error_type_ = col_node.col_info_.on_error_;
  col_node.expr_param_.accuracy_ = accuracy;
  col_node.expr_param_.dst_type_ = dst_type;
  col_node.expr_param_.pretty_type_ = 0;
  col_node.expr_param_.ascii_type_ = 0;
  col_node.expr_param_.scalars_type_ = col_node.col_info_.allow_scalar_;
  if (OB_FAIL(col_node.expr_param_.on_mismatch_.push_back(col_node.col_info_.on_mismatch_))) {
  } else if (OB_FAIL(col_node.expr_param_.on_mismatch_type_.push_back(col_node.col_info_.on_mismatch_type_))) {
  }
  return ret;
}

int JsonTableFunc::get_empty_option(int8_t option_on_empty, bool& res_val) 
{
  INIT_SUCC(ret);
  switch (option_on_empty) {
    // The on empty clause has two problems
    // 1. The on empty clause of json_exists, its track diagram is inconsistent with the document description:
    // a. The on empty clause in the document includes: NULL ON EMPTY, ERROR ON EMPTY, DEFAULT literal ON EMPTY
    // b. The on empty clause in the track diagram includes: ERROR ON EMPTY, FALSE ON EMPTY, TRUE ON EMPTY
    // c. During the actual test, it is found that inputting NULL ON EMPTY will report a syntax error, and the modification clause cannot be recognized, so implement it according to the track diagram
    // 2. According to the documentation, the on error of json_exists mainly checks whether the json data is wrong, and returns the response result.
    // But the on empty clause cannot find the correct documentation for the corresponding clause due to reason 1.
    // According to: whether the query result is empty / whether the path expression is empty / whether the json data is empty These three situations have been tried
    // It turns out that error/false/true on empty behaves exactly the same in three cases:
    // a. The query result or json data is empty, and all three on empty clauses return false
    // b. The path expression is empty, and the three on empty clauses all return the error code when the path is empty
    // Therefore, the implementation does not distinguish the three clauses:
    // as long as there is no true result, it returns false.
    case OB_JSON_ERROR_ON_EMPTY:
    case OB_JSON_FALSE_ON_EMPTY: 
    case OB_JSON_TRUE_ON_EMPTY:
    case OB_JSON_DEFAULT_ON_ERROR: {
      res_val = false;
      break;
    } 
    default: {
      ret = OB_ERR_UNEXPECTED;
    }
  }
  return ret;
}

int JsonTableFunc::set_on_empty(ObRegCol& col_node, JtScanCtx* ctx, bool &need_cast, bool& is_null)
{
  INIT_SUCC(ret);
  JtColType col_type = col_node.type();
  bool is_cover_by_error = true;
  if (col_type == COL_TYPE_QUERY) {
    is_null = false;
    bool is_json_arr = false;
    bool is_json_obj = false;
    if (OB_FAIL(ObExprJsonQuery::get_empty_option(is_cover_by_error, 
                          col_node.col_info_.on_empty_, is_null, is_json_arr,
                          is_json_obj))) {
      if (is_cover_by_error) {
        SET_COVER_ERROR(ctx, ret);
      }
    } else if (is_null) {
      col_node.curr_ = nullptr;
      ret = OB_SUCCESS;
    } else if (is_json_arr) {
      col_node.curr_ = ctx->jt_op_->get_js_array();
    } else if (is_json_obj) {
      col_node.curr_ = ctx->jt_op_->get_js_object();
    }
  } else if (col_type == COL_TYPE_VALUE) {
    is_null = false;
    ObDatum *t_res = NULL;
    if (OB_FAIL(ObExprJsonValue::get_empty_option(t_res, is_cover_by_error, col_node.col_info_.on_empty_,
                                                static_cast<ObDatum*>(col_node.err_val_), is_null))) {
      if (is_cover_by_error) {
        SET_COVER_ERROR(ctx, ret);
      }
    } else if (is_null) {
      col_node.curr_ = nullptr;
    } else {
      if (OB_ISNULL(col_node.emp_val_) || !col_node.is_emp_evaled_) {
        ret = OB_ERR_UNEXPECTED;
      } else { // mysql mode
        col_node.curr_ = col_node.emp_val_;
        col_node.res_flag_ = ResultType::NOT_DATUM;
      }
    }
  } else if (col_type == COL_TYPE_EXISTS) {
    bool res_val = false;
    if (OB_FAIL(get_empty_option(col_node.col_info_.on_empty_, res_val))) {
    } else if (!res_val) { // result will return false currently, not return true
      if (ob_is_string_type(col_node.col_info_.data_type_.get_obj_type())) {
        ObString value = "0";
        void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonString));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          col_node.curr_ = static_cast<ObJsonString*>(new(buf)ObJsonString(value.ptr(), value.length()));
          is_null = false;
        }
      } else {
        void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonInt));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          col_node.curr_ = static_cast<ObJsonInt*>(new(buf)ObJsonInt(0));
          is_null = false;
        }
      }
    }
  }
  return ret;
}

int JsonTableFunc::set_on_error(ObRegCol& col_node, JtScanCtx* ctx, int& ret)
{
  INIT_SUCC(tmp_ret);
  if (ret == OB_SUCCESS) {
  } else {
    bool is_null = false;
    const ObJtColInfo& info = col_node.col_info_;
    JtColType col_type = col_node.type();
    ObIJsonBase* t_val = nullptr;
    bool has_default_val = false;
    ObExpr* expr = ctx->spec_ptr_->column_exprs_.at(col_node.col_info_.output_column_idx_);
    if (col_type == COL_TYPE_VALUE) {
      ObExprJsonValue::get_error_option(info.on_error_, is_null, has_default_val);
      if (is_null) {
        col_node.curr_ = nullptr;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      } else if (has_default_val) {
        if (OB_ISNULL(col_node.err_val_) || !col_node.is_err_evaled_) {
          ret = OB_ERR_UNEXPECTED;
        } else { // mysql mode
          col_node.curr_ = col_node.err_val_;
          col_node.res_flag_ = ResultType::NOT_DATUM;
          ret = OB_SUCCESS;
        }
      } else if (ret != OB_SUCCESS) {
        EVAL_COVER_CODE(ctx, ret) ;
        if (ctx->is_need_end_ && OB_SUCC(ret)) {
          ret = OB_ITER_END;
        }
      }
    } else if (col_type == COL_TYPE_QUERY) {
      t_val = NULL;
      if (col_node.expr_param_.error_type_ == JSN_QUERY_ERROR) {
      } else if (OB_FAIL(ObExprJsonQuery::get_error_option(col_node.expr_param_.error_type_, t_val, 
                  ctx->jt_op_->get_js_array(), ctx->jt_op_->get_js_object(), is_null))) {
      } else if (is_null) {
        if (OB_FAIL(ObExprJsonQuery::get_mismatch_option(col_node.expr_param_.on_mismatch_[0], ctx->error_code_))) {
        } else {
          is_null = true;
          col_node.curr_ = nullptr;
          ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
        }
      } else {
        col_node.curr_ = t_val;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      }
    } else if (col_type == COL_TYPE_EXISTS) {
      int is_true = 0;
      if (info.on_error_ == JSN_EXIST_ERROR) {
        ret = ctx->error_code_;
        if (OB_SUCC(ret) && ctx->is_need_end_) {
          ret = OB_ITER_END;
        }
      } else if (info.on_error_ == JSN_EXIST_DEFAULT || info.on_error_ == JSN_EXIST_FALSE) {
        is_null = false;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      } else if (info.on_error_ == JSN_EXIST_TRUE) {
        is_true = 0;
        is_null = false;
        ret = ctx->is_need_end_ ? OB_ITER_END : OB_SUCCESS;
      }
      
      if (OB_FAIL(ret)) {
      } else if (ob_is_string_type(info.data_type_.get_obj_type())) {
        ObString value = is_true ? ObString("1") : ObString("0");
        void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonString));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          col_node.curr_ = static_cast<ObJsonString*>(new(buf)ObJsonString(value.ptr(), value.length()));
          is_null = false;
        }
      } else {
        void* buf = ctx->row_alloc_.alloc(sizeof(ObJsonInt));
        if (OB_ISNULL(buf)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          col_node.curr_ = static_cast<ObJsonInt*>(new(buf)ObJsonInt(is_true));
          is_null = false;
        }
      }
    }

    if (OB_SUCC(ret) && is_null) {
      expr->locate_datum_for_write(*ctx->eval_ctx_).set_null();
    }
  }
  return ret;
}

int JsonTableFunc::cast_to_result(ObRegCol& col_node, JtScanCtx* ctx, bool enable_error, bool is_pack_result)
{
  INIT_SUCC(ret);
  ObIJsonBase *js_val = static_cast<ObIJsonBase*>(col_node.curr_);
  ObJtColInfo& col_info = col_node.get_column_def();
  bool is_truncate = static_cast<bool>(col_info.truncate_);
  JtColType col_type = col_node.type();

  ObExpr* expr = ctx->spec_ptr_->column_exprs_.at(col_info.output_column_idx_);
  ObDatum& res = expr->locate_datum_for_write(*ctx->eval_ctx_);
  ctx->res_obj_ = &res;
  uint8_t is_type_mismatch = false;
  uint8_t ascii_type = false;

  ObObjType dst_type = expr->datum_meta_.type_;
  ObCollationType coll_type = expr->datum_meta_.cs_type_;
  ObAccuracy accuracy = col_info.data_type_.get_accuracy();
  ObCollationType dst_coll_type = col_info.data_type_.get_collation_type();
  ObCollationType in_coll_type = ctx->is_charset_converted_
                                 ? CS_TYPE_UTF8MB4_BIN
                                 : ctx->spec_ptr_->value_exprs_.at(0)->datum_meta_.cs_type_;
  ObCollationLevel dst_coll_level = col_info.data_type_.get_collation_level();
  bool is_quote = (col_info.col_type_ == COL_TYPE_QUERY && js_val->json_type() == ObJsonNodeType::J_STRING);
  ObJsonCastParam cast_param(dst_type, in_coll_type, dst_coll_type, ascii_type);
  cast_param.is_const_ = ctx->is_const_input_;
  cast_param.is_trunc_ = is_truncate;
  cast_param.relaxed_time_convert_ = true;
  cast_param.rt_expr_ = expr;
  switch (col_type) {
    case JtColType::COL_TYPE_VALUE: {
      if (col_node.res_flag_ != ResultType::NOT_DATUM) {
        ObDatum *js_val = static_cast<ObDatum*>(col_node.curr_);
        ObExpr* default_expr;
        if (col_node.res_flag_ == ResultType::EMPTY_DATUM) {
          default_expr = ctx->spec_ptr_->emp_default_exprs_.at(col_node.col_info_.empty_expr_id_);
        } else if (col_node.res_flag_ == ResultType::ERROR_DATUM) {
          default_expr = ctx->spec_ptr_->err_default_exprs_.at(col_node.col_info_.error_expr_id_);
        }
        
        if (OB_ISNULL(js_val)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(ObJsonExprHelper::cast_to_res(ctx->row_alloc_, *js_val, *expr, *default_expr, *ctx->eval_ctx_, res, true))) {
          enable_error = false;
        }
      } else {
        ret = ObJsonUtil::cast_to_res(&ctx->row_alloc_, *ctx->eval_ctx_, 
                  js_val, accuracy, cast_param, res, is_type_mismatch);
      }
      break;
    }
    case JtColType::COL_TYPE_QUERY: {
      cast_param.is_quote_ = true;
      ret = ObJsonUtil::cast_to_res(&ctx->row_alloc_, *ctx->eval_ctx_, 
                  js_val, accuracy, cast_param, res, is_type_mismatch);
      break;
    }
    case JtColType::COL_TYPE_EXISTS: {
      ret = ObJsonUtil::cast_to_res(&ctx->row_alloc_, *ctx->eval_ctx_, 
                js_val, accuracy, cast_param, res, is_type_mismatch);
      break;
    }
    default: {
      ret = OB_ERR_UNEXPECTED;
      break;
    }
  }
  if (OB_FAIL(ret) && enable_error) {
    int tmp_ret = set_on_error(col_node, ctx, ret);
    if (tmp_ret != OB_SUCCESS) {
    } else if (OB_SUCC(tmp_ret) && OB_NOT_NULL(col_node.curr_)
               && OB_FAIL(cast_to_result(col_node, ctx, false, false))) { // due of without type calc, so use cast transform default value to res.
    }
  }

  if (OB_SUCC(ret) && is_pack_result && is_lob_storage(dst_type) && (col_node.res_flag_ == ResultType::NOT_DATUM) && !res.is_null()) {
    ObString val = res.get_string();
    if (OB_FAIL(ObJsonExprHelper::pack_json_str_res(*expr, *ctx->eval_ctx_, res, val, &ctx->row_alloc_))) {
    }
  }
  return ret;
}

int JsonTableFunc::eval_seek_col(ObRegCol &col_node, void* in, JtScanCtx* ctx, bool &is_null_value, bool &need_cast_res)
{
  INIT_SUCC(ret);
  ObJsonSeekResult hit;
  ObIJsonBase* in_val = static_cast<ObIJsonBase*>(in);
  ObJsonPath* json_path = static_cast<ObJsonPath*>(col_node.path_);
  ObExpr* col_expr = ctx->spec_ptr_->column_exprs_.at(col_node.col_info_.output_column_idx_);
  in_val->set_allocator(&ctx->row_alloc_);
  if (OB_FAIL(in_val->seek(*json_path, json_path->path_node_cnt(), true, false, hit))) {
    SET_COVER_ERROR(ctx, ret);
  } else if (hit.size() == 0) {
    col_node.curr_ = nullptr;
    is_null_value = true;
  } else if (col_node.type() == COL_TYPE_EXISTS && hit.size() > 0) {
    is_null_value = false;
    col_node.curr_ = hit[0];
  } else if (col_node.type() != COL_TYPE_QUERY && hit.size() == 1) {
    is_null_value = false;
    col_node.curr_ = hit[0];
  } else if (col_node.type() == COL_TYPE_VALUE
             && !ob_is_json(col_expr->datum_meta_.type_)) {
    ret = OB_ERR_JSON_VALUE_NO_SCALAR;
    SET_COVER_ERROR(ctx, ret);
  } else if (col_node.type() == COL_TYPE_QUERY ||
             (col_node.type() == COL_TYPE_VALUE
              && ob_is_json(col_expr->datum_meta_.type_))) {
    void* js_arr_buf = ctx->row_alloc_.alloc(sizeof(ObJsonArray));
    ObIJsonBase* js_arr_ptr = nullptr;
    if (OB_ISNULL(js_arr_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else if (OB_ISNULL(js_arr_ptr = new (js_arr_buf) ObJsonArray(&ctx->row_alloc_))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ObExprJsonQuery::append_node_into_res(js_arr_ptr, json_path, hit, &ctx->row_alloc_))) {
    }
    
    if (OB_SUCC(ret)) {
      is_null_value = false;
      col_node.curr_ = js_arr_ptr;
    }
  }
  return ret;
}

int JsonTableFunc::init_ctx(ObRegCol &scan_node, JtScanCtx*& ctx)
{
  INIT_SUCC(ret);
  ObJsonPath* js_path = NULL;
  scan_node.tab_type_ = MulModeTableType::OB_ORA_JSON_TABLE_TYPE;
  bool need_eval = false;    // flag of eval default value
  bool need_datum = false;
  if (!scan_node.is_path_evaled_ && OB_ISNULL(scan_node.path_) 
      && (scan_node.node_type() == REG_TYPE 
          || scan_node.node_type() == SCAN_TYPE) 
      && !scan_node.col_info_.path_.empty()) {
    void* path_buf = ctx->op_exec_alloc_->alloc(sizeof(ObJsonPath));
    if (OB_ISNULL(path_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      js_path = new (path_buf) ObJsonPath(scan_node.col_info_.path_, ctx->op_exec_alloc_);
      if (OB_FAIL(js_path->parse_path())) {
        ret = OB_ERR_JSON_PATH_EXPRESSION_SYNTAX_ERROR;
        LOG_USER_ERROR(OB_ERR_JSON_PATH_EXPRESSION_SYNTAX_ERROR, scan_node.col_info_.path_.length(), scan_node.col_info_.path_.ptr());
      } else {
        scan_node.expr_param_.json_path_ = js_path;
        scan_node.path_ = js_path;
        scan_node.is_path_evaled_ = true;
      }
    }
  }
  if (OB_FAIL(ret)) {
  } else if (scan_node.node_type() == REG_TYPE) {
    if (!scan_node.is_emp_evaled_) {
      need_eval = false;
      if (scan_node.type() == COL_TYPE_VALUE) {
        if (scan_node.col_info_.on_empty_ == JSN_VALUE_IMPLICIT 
            && scan_node.col_info_.on_error_ == JSN_VALUE_DEFAULT) {
          ObExpr* default_expr = ctx->spec_ptr_->err_default_exprs_.at(scan_node.col_info_.error_expr_id_);
          if (OB_FAIL(eval_default_value(ctx, default_expr, scan_node.err_val_, need_datum))) {
            ret = OB_INVALID_DEFAULT;
            LOG_USER_ERROR(OB_INVALID_DEFAULT, scan_node.col_info_.col_name_.length(), scan_node.col_info_.col_name_.ptr());
          } else {
            scan_node.is_err_evaled_ = true;
          }
        } else if (scan_node.col_info_.on_empty_ == JSN_VALUE_DEFAULT) {
          ObExpr* default_expr = ctx->spec_ptr_->emp_default_exprs_.at(scan_node.col_info_.empty_expr_id_);
          if (OB_FAIL(eval_default_value(ctx, default_expr, scan_node.emp_val_, need_datum))) {
            ret = OB_INVALID_DEFAULT;
            LOG_USER_ERROR(OB_INVALID_DEFAULT, scan_node.col_info_.col_name_.length(), scan_node.col_info_.col_name_.ptr());
          } else {
            scan_node.is_emp_evaled_ = true;
          }
        }
      }
    }
    if (OB_SUCC(ret) && !scan_node.is_err_evaled_) {
      need_eval = false;
      if (scan_node.type() == COL_TYPE_VALUE) {
        if (scan_node.col_info_.on_error_ == JSN_VALUE_DEFAULT) {
          need_eval = true;
        }
        if (need_eval) {
          ObExpr* default_expr = ctx->spec_ptr_->err_default_exprs_.at(scan_node.col_info_.error_expr_id_);
          if (OB_FAIL(eval_default_value(ctx, default_expr, scan_node.err_val_, need_datum))) {
            ret = OB_INVALID_DEFAULT;
            LOG_USER_ERROR(OB_INVALID_DEFAULT, scan_node.col_info_.col_name_.length(), scan_node.col_info_.col_name_.ptr());
          } else {
            scan_node.is_err_evaled_ = true;
          }
        }
      }
    }
    scan_node.res_flag_ = ResultType::NOT_DATUM;
    ObExpr* col_expr = ctx->spec_ptr_->column_exprs_.at(scan_node.col_info_.output_column_idx_);
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(set_expr_exec_param(scan_node, ctx))) {
    } else if (OB_FAIL(RegularCol::check_item_method_json(scan_node, ctx))) {
    } else if (OB_FAIL(check_default_value(ctx, scan_node, col_expr))) {
    }
  }
  return ret;
}

int JsonTableFunc::eval_default_value(JtScanCtx*& ctx, ObExpr*& default_expr, void*& res, bool need_datum)
{
  INIT_SUCC(ret);
  ObDatum* emp_datum = nullptr;
  ObIJsonBase* emp_json = nullptr;
  if (OB_FAIL(default_expr->eval(*ctx->eval_ctx_, emp_datum))) {
  } else if (need_datum) {
    res = emp_datum;
  } else if (OB_FAIL(RegularCol::parse_default_value_2json(default_expr, ctx, emp_datum, emp_json))) {
  } else {
    res = emp_json;
  }
  return ret;
}

int JsonTableFunc::reset_path_iter(ObRegCol &scan_node, void* in, JtScanCtx*& ctx, ScanType init_flag, bool &is_null_value)
{
  INIT_SUCC(ret);
  if (init_flag == SCAN_NODE_TYPE) {
    ObJsonSeekResult hit;
    ObIJsonBase* in_val = static_cast<ObIJsonBase*>(in);
    ObJsonPath* js_path = static_cast<ObJsonPath*>(scan_node.path_);
    in_val->set_allocator(&ctx->row_alloc_);
    if (OB_ISNULL(in_val) || OB_ISNULL(js_path)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(in_val->seek(*js_path, js_path->path_node_cnt(), true, false, hit))) {
      SET_COVER_ERROR(ctx, ret);
    } else if (hit.size() == 0) {
      scan_node.cur_pos_ = 0;
      scan_node.total_ = 1;
      is_null_value = true;
      scan_node.curr_ = scan_node.iter_ = nullptr;
    } else if (hit.size() == 1) {
      scan_node.iter_ = scan_node.curr_ = hit[0];
      is_null_value = false;
      scan_node.cur_pos_ = 0;
      scan_node.total_ = 1;
    } else {
      is_null_value = false;
      void* js_arr_buf = ctx->row_alloc_.alloc(sizeof(ObJsonArray));
      ObJsonArray* js_arr_ptr = nullptr;
      if (OB_ISNULL(js_arr_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_ISNULL(js_arr_ptr = new (js_arr_buf) ObJsonArray(&ctx->row_alloc_))) {
        ret = OB_ERR_UNEXPECTED;
      } else {
        ObJsonNode *j_node = NULL;
        ObIJsonBase *jb_node = NULL;
        for (int32_t i = 0; OB_SUCC(ret) && i < hit.size(); i++) {
          if (ObJsonBaseFactory::transform(&ctx->row_alloc_, hit[i], ObJsonInType::JSON_TREE, jb_node)) { // to tree
          } else {
            j_node = static_cast<ObJsonNode *>(jb_node);
            if (OB_FAIL(js_arr_ptr->array_append(j_node->clone(&ctx->row_alloc_)))) {
            }
          }
        }

        if (OB_SUCC(ret)) {
          scan_node.curr_ = js_arr_ptr;
          scan_node.total_ = hit.size();
          ObIJsonBase* iter = NULL;
          if (OB_FAIL(js_arr_ptr->get_array_element(0, iter))) {
          } else {
            scan_node.iter_ = iter;
          }
        }
      }
    }
  } else {
    // do nothing in col node
  }
  return ret;
}

int JsonTableFunc::get_iter_value(ObRegCol &col_node, JtScanCtx* ctx, bool &is_null_value)
{
  INIT_SUCC(ret);

  if (col_node.cur_pos_ + 1 < col_node.total_) {
    col_node.cur_pos_++;
    if (OB_FAIL(container_at(col_node.curr_, col_node.iter_, col_node.cur_pos_))) {
    }
  } else {
    ret = OB_ITER_END;
  }
  return ret;
}

int JsonTableFunc::eval_input(ObJsonTableOp &jt, JtScanCtx& ctx, ObEvalCtx &eval_ctx)
{
  INIT_SUCC(ret);
  common::ObObjMeta& doc_obj_datum = ctx.spec_ptr_->value_exprs_.at(0)->obj_meta_;
  ObDatumMeta& doc_datum = ctx.spec_ptr_->value_exprs_.at(0)->datum_meta_;
  ObObjType doc_type = doc_datum.type_;
  ObCollationType doc_cs_type = doc_datum.cs_type_;
  ObString j_str;
  bool is_null = false;
  ObIJsonBase* in = NULL;
  MultimodeAlloctor tmp_allocator(ctx.row_alloc_);

  if (doc_type == ObNullType) {
    ret = OB_ITER_END;
  } else if (!(doc_type == ObJsonType
                || ob_is_string_type(doc_type))) {
    ret = OB_ERR_INPUT_JSON_TABLE;
  } else {
    jt.reset_columns();
    if (OB_FAIL(ObJsonExprHelper::get_json_or_str_data(ctx.spec_ptr_->value_exprs_.at(0), eval_ctx,
                                                        tmp_allocator, j_str, is_null))) {
      ret = OB_ERR_INPUT_JSON_TABLE;
    } else if (is_null) {
      ret = OB_ITER_END;
    } else if (ob_is_string_type(doc_type)
                && (doc_cs_type != CS_TYPE_BINARY)
                && (ObCharset::charset_type_by_coll(doc_cs_type) != CHARSET_UTF8MB4)) {
      // need convert to utf8 first, we are using GenericInsituStringStream<UTF8<> >
      char *buf = nullptr;
      const int64_t factor = 2;
      int64_t buf_len = j_str.length() * factor;
      uint32_t result_len = 0;

      if (OB_ISNULL(buf = static_cast<char*>(tmp_allocator.alloc(buf_len)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (OB_FAIL(ObCharset::charset_convert(doc_cs_type, j_str.ptr(),
                                                    j_str.length(), CS_TYPE_UTF8MB4_BIN, buf,
                                                    buf_len, result_len))) {
      } else {
        ctx.is_charset_converted_ = true;
        j_str.assign_ptr(buf, result_len);
      }
    }
    ObJsonInType j_in_type = ObJsonExprHelper::get_json_internal_type(doc_type);
    ObJsonInType expect_type = ObJsonInType::JSON_TREE;
    uint32_t parse_flag = ObJsonParser::JSN_DEFAULT_FLAG;
    
    // json type input, or has is json check
    bool is_ensure_json = false;


    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&tmp_allocator, j_str, j_in_type, expect_type, in, parse_flag, ObJsonExprHelper::get_json_max_depth_config()))
                || (in->json_type() != ObJsonNodeType::J_ARRAY && in->json_type() != ObJsonNodeType::J_OBJECT)) {
      if (OB_FAIL(ret) || (is_ensure_json)) {
        in= nullptr;
        ret = OB_ERR_JSON_SYNTAX_ERROR;
        SET_COVER_ERROR(&ctx, ret);
        ctx.is_need_end_ = 1;
      } else {
        ret = OB_SUCCESS;
      }
    }
    if (OB_SUCC(ret)) {
      jt.input_ = in;
    }
  }
  return ret;
}

int JsonTableFunc::container_at(void* in, void *&out, int32_t pos) {
  INIT_SUCC(ret);
  ObIJsonBase* in_ = static_cast<ObIJsonBase*>(in);
  ObIJsonBase* res = nullptr;

  if (in_->json_type() == ObJsonNodeType::J_ARRAY) {
    if (OB_FAIL(in_->get_array_element(pos, res))) {
    }
  } else if (in_->json_type() == ObJsonNodeType::J_OBJECT) {
    if (OB_FAIL(in_->get_object_value(pos, res))) {
    }
  }
  if (OB_SUCC(ret)) {
    out = res;
  }

  return ret;
}

} // end namespace sql
} // end namespace oceanbase

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

#define USING_LOG_PREFIX SQL_RESV

#include "sql/resolver/dml/ob_file_sql.h"

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/json_type/ob_json_base.h"
#include "common/json_type/ob_json_tree.h"
#include "lib/string/ob_string_buffer.h"
#include "share/ob_errno.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{

namespace
{

const int64_t FILE_SQL_MAX_COLUMN_CNT = 4096;
const int64_t FILE_SQL_DEFAULT_VARCHAR_LEN = 256;
// JSON_TABLE cannot cast a >4GB document, keep the generated document bounded.
const int64_t FILE_SQL_MAX_DOC_LEN = 1024L * 1024L * 1024L;

////////////////////////////////////////////////////////////////
// small helpers
////////////////////////////////////////////////////////////////

struct FileSqlCsvCell
{
  const char *ptr_;
  int32_t len_;
  bool is_null_;
};

struct FileSqlCsvRows
{
  FileSqlCsvRows() : cells_(), column_cnt_(0), row_cnt_(0) {}
  ObSEArray<FileSqlCsvCell, 64> cells_;
  int64_t column_cnt_;
  int64_t row_cnt_;
  const FileSqlCsvCell &at(const int64_t row, const int64_t col) const
  {
    return cells_.at(row * column_cnt_ + col);
  }
};

bool file_sql_is_digit(const char c) { return c >= '0' && c <= '9'; }

bool file_sql_is_ident_char(const char c)
{
  return file_sql_is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool file_sql_ci_equal(const char *str1, const int64_t len1, const char *str2, const int64_t len2)
{
  bool bret = (NULL != str1 && NULL != str2 && len1 == len2);
  for (int64_t i = 0; bret && i < len1; ++i) {
    char left = str1[i];
    char right = str2[i];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<char>(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<char>(right - 'A' + 'a');
    }
    if (left != right) {
      bret = false;
    }
  }
  return bret;
}

// YYYY-MM-DD
bool file_sql_is_date(const char *p, const int64_t len)
{
  bool bret = (10 == len && '-' == p[4] && '-' == p[7]);
  for (int64_t i = 0; bret && i < len; ++i) {
    if (4 != i && 7 != i && !file_sql_is_digit(p[i])) {
      bret = false;
    }
  }
  if (bret) {
    const int year = (p[0] - '0') * 1000 + (p[1] - '0') * 100 + (p[2] - '0') * 10 + (p[3] - '0');
    const int month = (p[5] - '0') * 10 + (p[6] - '0');
    const int day = (p[8] - '0') * 10 + (p[9] - '0');
    bret = (year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= 31);
  }
  return bret;
}

// YYYY-MM-DD[ T]HH:MM:SS[.ffffff]
bool file_sql_is_datetime(const char *p, const int64_t len)
{
  bool bret = (len >= 19 && (' ' == p[10] || 'T' == p[10]) && ':' == p[13] && ':' == p[16]);
  if (bret && !file_sql_is_date(p, 10)) {
    bret = false;
  }
  for (int64_t i = 11; bret && i < 19; ++i) {
    if (13 != i && 16 != i && !file_sql_is_digit(p[i])) {
      bret = false;
    }
  }
  if (bret) {
    const int hour = (p[11] - '0') * 10 + (p[12] - '0');
    const int minute = (p[14] - '0') * 10 + (p[15] - '0');
    const int second = (p[17] - '0') * 10 + (p[18] - '0');
    bret = (hour < 24 && minute < 60 && second < 60);
  }
  if (bret && len > 19) {
    bret = ('.' == p[19]);
    for (int64_t i = 20; bret && i < len; ++i) {
      if (!file_sql_is_digit(p[i])) {
        bret = false;
      }
    }
  }
  return bret;
}

bool file_sql_is_int(const char *p, const int64_t len)
{
  bool bret = (len > 0);
  int64_t i = ('+' == p[0] || '-' == p[0]) ? 1 : 0;
  if (i >= len) {
    bret = false;
  }
  for (; bret && i < len; ++i) {
    if (!file_sql_is_digit(p[i])) {
      bret = false;
    }
  }
  if (bret) {
    char *end_ptr = NULL;
    int err = 0;
    ObCharset::strntoll(p, len, 10, &end_ptr, &err);
    bret = (0 == err && end_ptr == p + len);
  }
  return bret;
}

bool file_sql_is_double(const char *p, const int64_t len)
{
  bool bret = (len > 0);
  bool has_sep = false;
  bool has_digit = false;
  int64_t i = ('+' == p[0] || '-' == p[0]) ? 1 : 0;
  for (; bret && i < len; ++i) {
    const char c = p[i];
    if (file_sql_is_digit(c)) {
      has_digit = true;
    } else if ('.' == c || 'e' == c || 'E' == c || '+' == c || '-' == c) {
      has_sep = true;
    } else {
      bret = false;
    }
  }
  if (bret) {
    char *end_ptr = NULL;
    int err = 0;
    ObCharset::strntod(p, len, &end_ptr, &err);
    bret = (has_digit && has_sep && 0 == err && end_ptr == p + len);
  }
  return bret;
}

bool file_sql_is_bool(const char *p, const int64_t len, bool &value)
{
  bool bret = true;
  if (4 == len && file_sql_ci_equal(p, len, "true", 4)) {
    value = true;
  } else if (5 == len && file_sql_ci_equal(p, len, "false", 5)) {
    value = false;
  } else if (1 == len && ('1' == p[0] || '0' == p[0])) {
    value = ('1' == p[0]);
  } else {
    bret = false;
  }
  return bret;
}

int file_sql_append(ObStringBuffer &buf, const char *str, const int64_t len)
{
  return buf.append(str, static_cast<uint64_t>(len));
}

int file_sql_append_str(ObStringBuffer &buf, const char *str)
{
  return buf.append(str);
}

// newline delimited json holds one object per line, while a json document has
// to be a single array: join the non empty lines with commas.
int file_sql_append_json_lines(ObStringBuffer &buf, const ObString &content)
{
  int ret = OB_SUCCESS;
  const char *p = content.ptr();
  const char *end = p + content.length();
  bool appended = false;
  if (OB_FAIL(file_sql_append(buf, "[", 1))) {
  }
  while (OB_SUCC(ret) && p < end) {
    const char *line_end = p;
    while (line_end < end && '\n' != *line_end) {
      ++line_end;
    }
    const char *begin = p;
    const char *stop = line_end;
    while (begin < stop && (' ' == *begin || '\t' == *begin || '\r' == *begin)) {
      ++begin;
    }
    while (stop > begin && (' ' == stop[-1] || '\t' == stop[-1] || '\r' == stop[-1])) {
      --stop;
    }
    if (stop > begin) {
      if (appended && OB_FAIL(file_sql_append(buf, ",", 1))) {
      } else if (OB_FAIL(file_sql_append(buf, begin, stop - begin))) {
      } else {
        appended = true;
      }
    }
    p = line_end < end ? line_end + 1 : end;
  }
  if (OB_FAIL(ret)) {
  } else if (!appended) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("json file is empty", K(ret));
  } else if (OB_FAIL(file_sql_append(buf, "]", 1))) {
  }
  return ret;
}

int file_sql_append_int(ObStringBuffer &buf, const int64_t value)
{
  char tmp[64];
  const int64_t len = snprintf(tmp, sizeof(tmp), "%ld", value);
  return (len > 0 && len < static_cast<int64_t>(sizeof(tmp)))
          ? file_sql_append(buf, tmp, len) : OB_ERR_UNEXPECTED;
}

int file_sql_append_double(ObStringBuffer &buf, const double value)
{
  char tmp[64];
  // %.17g round trips a double and is still a valid JSON number
  const int64_t len = snprintf(tmp, sizeof(tmp), "%.17g", value);
  return (len > 0 && len < static_cast<int64_t>(sizeof(tmp)))
          ? file_sql_append(buf, tmp, len) : OB_ERR_UNEXPECTED;
}

int file_sql_append_json_string(ObStringBuffer &buf, const char *str, const int64_t len)
{
  int ret = OB_SUCCESS;
  char tmp[8];
  if (OB_FAIL(file_sql_append(buf, "\"", 1))) {
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < len; ++i) {
    const unsigned char c = static_cast<unsigned char>(str[i]);
    switch (c) {
      case '"': ret = file_sql_append(buf, "\\\"", 2); break;
      case '\\': ret = file_sql_append(buf, "\\\\", 2); break;
      case '\b': ret = file_sql_append(buf, "\\b", 2); break;
      case '\f': ret = file_sql_append(buf, "\\f", 2); break;
      case '\n': ret = file_sql_append(buf, "\\n", 2); break;
      case '\r': ret = file_sql_append(buf, "\\r", 2); break;
      case '\t': ret = file_sql_append(buf, "\\t", 2); break;
      default: {
        if (c < 0x20) {
          const int64_t n = snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<int>(c));
          ret = file_sql_append(buf, tmp, n);
        } else {
          ret = file_sql_append(buf, reinterpret_cast<const char *>(&str[i]), 1);
        }
        break;
      }
    }
  }
  if (OB_SUCC(ret)) {
    ret = file_sql_append(buf, "\"", 1);
  }
  return ret;
}

int file_sql_read_file(ObIAllocator &alloc, const ObString &path, ObString &content)
{
  int ret = OB_SUCCESS;
  int fd = -1;
  char *path_buf = NULL;
  char *buf = NULL;
  struct stat st;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty file path", K(ret));
  } else if (OB_ISNULL(path_buf = static_cast<char *>(alloc.alloc(path.length() + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(path_buf, path.ptr(), path.length());
    path_buf[path.length()] = '\0';
    if ((fd = ::open(path_buf, O_RDONLY)) < 0) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open file", K(ret), K(path));
    } else if (0 != ::fstat(fd, &st)) {
      ret = OB_IO_ERROR;
      LOG_WARN("failed to stat file", K(ret), K(path));
    } else if (!S_ISREG(st.st_mode)) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("not a regular file", K(ret), K(path));
    } else if (st.st_size > FILE_SQL_MAX_DOC_LEN) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("file is too large", K(ret), K(path), K(st.st_size));
    } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(st.st_size + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t read_len = 0;
      while (OB_SUCC(ret) && read_len < st.st_size) {
        const ssize_t n = ::read(fd, buf + read_len, st.st_size - read_len);
        if (n < 0) {
          if (EINTR == errno) {
            continue;
          }
          ret = OB_IO_ERROR;
          LOG_WARN("failed to read file", K(ret), K(path), K(errno));
        } else if (0 == n) {
          break;
        } else {
          read_len += n;
        }
      }
      if (OB_SUCC(ret)) {
        buf[read_len] = '\0';
        content.assign_ptr(buf, static_cast<int32_t>(read_len));
      }
    }
  }
  if (fd >= 0) {
    ::close(fd);
  }
  return ret;
}

////////////////////////////////////////////////////////////////
// CSV
////////////////////////////////////////////////////////////////

// Append one parsed field, un-escaping doubled quotes when needed.
int file_sql_push_csv_cell(ObIAllocator &alloc,
                           const char *field_start,
                           const int64_t field_len,
                           const bool quoted,
                           const bool has_escape,
                           const bool next_is_delim,
                           FileSqlCsvCell &cell)
{
  int ret = OB_SUCCESS;
  cell.is_null_ = false;
  if (!quoted) {
    // an unquoted empty field is NULL, a quoted one is an empty string
    if (0 == field_len) {
      cell.is_null_ = true;
      cell.ptr_ = field_start;
      cell.len_ = 0;
    } else {
      cell.ptr_ = field_start;
      cell.len_ = static_cast<int32_t>(field_len);
    }
  } else if (!has_escape) {
    cell.ptr_ = field_start;
    cell.len_ = static_cast<int32_t>(field_len);
  } else {
    char *buf = NULL;
    if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(field_len + 1)))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    } else {
      int64_t out = 0;
      for (int64_t i = 0; i < field_len; ++i) {
        buf[out++] = field_start[i];
        if ('"' == field_start[i] && i + 1 < field_len && '"' == field_start[i + 1]) {
          ++i;
        }
      }
      buf[out] = '\0';
      cell.ptr_ = buf;
      cell.len_ = static_cast<int32_t>(out);
    }
  }
  UNUSED(next_is_delim);
  return ret;
}

int file_sql_parse_csv(ObIAllocator &alloc, const ObString &content, FileSqlCsvRows &rows)
{
  int ret = OB_SUCCESS;
  const char *p = content.ptr();
  const int64_t n = content.length();
  int64_t i = 0;
  int64_t row_start = 0;
  int64_t field_cnt = 0;
  bool in_quote = false;
  bool quoted = false;
  bool has_escape = false;
  int64_t field_content_start = 0;
  rows.cells_.reuse();
  rows.column_cnt_ = 0;
  rows.row_cnt_ = 0;
  if (NULL == p && n > 0) {
    ret = OB_INVALID_ARGUMENT;
  }
  while (OB_SUCC(ret) && (i <= n)) {
    const bool at_end = (i == n);
    const char c = at_end ? '\n' : p[i];
    if (in_quote) {
      if (at_end) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("unterminated quoted csv field", K(ret));
      } else if ('"' == c) {
        if (i + 1 < n && '"' == p[i + 1]) {
          has_escape = true;
          ++i;
        } else {
          in_quote = false;
        }
      }
      ++i;
    } else if ('"' == c && i == field_content_start - 1 + (quoted ? 0 : 1) && !quoted) {
      // quote at the very beginning of a field
      quoted = true;
      in_quote = true;
      has_escape = false;
      field_content_start = i + 1;
      ++i;
    } else if (',' == c || '\n' == c) {
      int64_t field_len = (quoted ? i - 1 : i) - field_content_start;
      if (quoted && field_len < 0) {
        field_len = 0;
      }
      if (!quoted && field_len > 0 && field_content_start + field_len - 1 < n
          && (',' == c) && '\r' == p[field_content_start + field_len - 1]) {
        --field_len;
      }
      FileSqlCsvCell cell;
      if (OB_FAIL(file_sql_push_csv_cell(alloc, p + field_content_start, field_len,
                                         quoted, has_escape, true, cell))) {
      } else if (OB_FAIL(rows.cells_.push_back(cell))) {
      } else {
        ++field_cnt;
      }
      if (OB_FAIL(ret)) {
      } else if (',' == c) {
        quoted = false;
        has_escape = false;
        field_content_start = i + 1;
        ++i;
      } else {
        // end of record
        const bool empty_last_row = at_end && 1 == field_cnt && rows.cells_.at(rows.cells_.count() - 1).is_null_;
        if (!empty_last_row) {
          if (0 == rows.column_cnt_) {
            rows.column_cnt_ = field_cnt;
            if (rows.column_cnt_ > FILE_SQL_MAX_COLUMN_CNT) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("too many csv columns", K(ret), K(rows.column_cnt_));
            }
          }
          if (OB_SUCC(ret)) {
            if (field_cnt != rows.column_cnt_) {
              ret = OB_INVALID_ARGUMENT;
              LOG_WARN("csv field count mismatch", K(ret), K(field_cnt), K(rows.column_cnt_));
            } else {
              ++rows.row_cnt_;
            }
          }
        } else {
          rows.cells_.pop_back();
        }
        if (OB_SUCC(ret)) {
          field_cnt = 0;
          quoted = false;
          has_escape = false;
          field_content_start = i + 1;
          row_start = i + 1;
          ++i;
        }
      }
    } else {
      ++i;
    }
  }
  UNUSED(row_start);
  return ret;
}

bool file_sql_csv_has_header(const FileSqlCsvRows &rows)
{
  bool bret = (rows.row_cnt_ > 1 && rows.column_cnt_ > 0);
  for (int64_t c = 0; bret && c < rows.column_cnt_; ++c) {
    const FileSqlCsvCell &cell = rows.at(0, c);
    if (cell.is_null_ || cell.len_ <= 0 || !(cell.ptr_[0] == '_' || isalpha(cell.ptr_[0]))) {
      bret = false;
    }
    for (int32_t i = 0; bret && i < cell.len_; ++i) {
      if (!file_sql_is_ident_char(cell.ptr_[i])) {
        bret = false;
      }
    }
  }
  return bret;
}

enum FileSqlCsvKind
{
  FILE_SQL_CSV_INT = 1,
  FILE_SQL_CSV_DOUBLE = 1 << 1,
  FILE_SQL_CSV_BOOL = 1 << 2,
  FILE_SQL_CSV_DATE = 1 << 3,
  FILE_SQL_CSV_DATETIME = 1 << 4,
  FILE_SQL_CSV_STRING = 1 << 5,
};

int file_sql_csv_cell_kind(const FileSqlCsvCell &cell, int32_t &kind)
{
  int ret = OB_SUCCESS;
  bool bool_value = false;
  if (cell.is_null_) {
    kind = 0;
  } else if (file_sql_is_int(cell.ptr_, cell.len_)) {
    kind = FILE_SQL_CSV_INT;
  } else if (file_sql_is_double(cell.ptr_, cell.len_)) {
    kind = FILE_SQL_CSV_DOUBLE;
  } else if (file_sql_is_bool(cell.ptr_, cell.len_, bool_value)) {
    kind = FILE_SQL_CSV_BOOL;
  } else if (file_sql_is_datetime(cell.ptr_, cell.len_)) {
    kind = FILE_SQL_CSV_DATETIME;
  } else if (file_sql_is_date(cell.ptr_, cell.len_)) {
    kind = FILE_SQL_CSV_DATE;
  } else {
    kind = FILE_SQL_CSV_STRING;
  }
  return ret;
}

int file_sql_csv_append_value(ObStringBuffer &buf, const FileSqlCsvCell &cell, const ObObjType type)
{
  int ret = OB_SUCCESS;
  if (cell.is_null_) {
    ret = file_sql_append_str(buf, "null");
  } else if (ObIntType == type) {
    char *end_ptr = NULL;
    int err = 0;
    const int64_t value = ObCharset::strntoll(cell.ptr_, cell.len_, 10, &end_ptr, &err);
    if (0 != err || end_ptr != cell.ptr_ + cell.len_) {
      ret = file_sql_append_json_string(buf, cell.ptr_, cell.len_);
    } else {
      ret = file_sql_append_int(buf, value);
    }
  } else if (ObDoubleType == type) {
    char *int_end = NULL;
    char *real_end = NULL;
    int int_err = 0;
    int real_err = 0;
    int64_t int_value = 0;
    const double value = ObCharset::strntod(cell.ptr_, cell.len_, &real_end, &real_err);
    int_value = ObCharset::strntoll(cell.ptr_, cell.len_, 10, &int_end, &int_err);
    if (0 == int_err && int_end == cell.ptr_ + cell.len_) {
      ret = file_sql_append_int(buf, int_value);
    } else if (0 != real_err || real_end != cell.ptr_ + cell.len_) {
      ret = file_sql_append_json_string(buf, cell.ptr_, cell.len_);
    } else {
      ret = file_sql_append_double(buf, value);
    }
  } else if (ObTinyIntType == type) {
    bool bool_value = false;
    if (file_sql_is_bool(cell.ptr_, cell.len_, bool_value)) {
      ret = file_sql_append_str(buf, bool_value ? "true" : "false");
    } else {
      ret = file_sql_append_str(buf, "null");
    }
  } else if (ObVarcharType == type) {
    ret = file_sql_append_json_string(buf, cell.ptr_, cell.len_);
  } else {
    // date / datetime columns take the original text and are cast by JSON_TABLE
    ret = file_sql_append_json_string(buf, cell.ptr_, cell.len_);
  }
  return ret;
}

int file_sql_read_csv(ObIAllocator &alloc,
                      const ObString &path,
                      const ObFileSqlOptions &options,
                      ObFileSqlTable &table)
{
  int ret = OB_SUCCESS;
  ObString content;
  FileSqlCsvRows rows;
  bool has_header = false;
  int64_t data_row_begin = 0;
  ObStringBuffer doc(&alloc);
  if (OB_FAIL(file_sql_read_file(alloc, path, content))) {
  } else if (OB_FAIL(file_sql_parse_csv(alloc, content, rows))) {
  } else if (0 == rows.row_cnt_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("csv file has no data row", K(ret), K(path));
  } else if (FALSE_IT(has_header = options.header_set_ ? options.header_ : file_sql_csv_has_header(rows))) {
  } else if (FALSE_IT(data_row_begin = has_header ? 1 : 0)) {
  } else if (data_row_begin >= rows.row_cnt_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("csv file has no data row", K(ret), K(path));
  } else if (OB_FAIL(file_sql_append_str(doc, "["))) {
  } else {
    // 1. infer column types from all non null values
    for (int64_t c = 0; OB_SUCC(ret) && c < rows.column_cnt_; ++c) {
      int32_t kinds = 0;
      int64_t max_len = 0;
      bool nullable = false;
      for (int64_t r = data_row_begin; OB_SUCC(ret) && r < rows.row_cnt_; ++r) {
        const FileSqlCsvCell &cell = rows.at(r, c);
        int32_t kind = 0;
        if (OB_FAIL(file_sql_csv_cell_kind(cell, kind))) {
        } else if (cell.is_null_) {
          nullable = true;
        } else {
          kinds |= kind;
          max_len = MAX(max_len, static_cast<int64_t>(cell.len_));
        }
      }
      if (OB_SUCC(ret)) {
        ObFileSqlColumn column;
        if (0 == kinds) {
          column.type_ = ObVarcharType;
          column.length_ = FILE_SQL_DEFAULT_VARCHAR_LEN;
        } else if (FILE_SQL_CSV_BOOL == kinds) {
          column.type_ = ObTinyIntType;
        } else if (FILE_SQL_CSV_INT == kinds) {
          column.type_ = ObIntType;
        } else if (0 == (kinds & ~(FILE_SQL_CSV_INT | FILE_SQL_CSV_DOUBLE))) {
          column.type_ = ObDoubleType;
        } else if (FILE_SQL_CSV_DATE == kinds) {
          column.type_ = ObDateType;
        } else if (FILE_SQL_CSV_DATETIME == kinds
                   || (0 == (kinds & ~(FILE_SQL_CSV_DATE | FILE_SQL_CSV_DATETIME)))) {
          column.type_ = ObDateTimeType;
        } else {
          column.type_ = ObVarcharType;
          column.length_ = MAX(max_len, static_cast<int64_t>(1));
        }
        column.nullable_ = nullable;
        if (OB_FAIL(ret)) {
        } else if (has_header) {
          const FileSqlCsvCell &name_cell = rows.at(0, c);
          ObString name;
          // duplicate header names are made unique by appending __<ordinal>
          int64_t dup = 1;
          for (int64_t p = 0; p < c; ++p) {
            const FileSqlCsvCell &prev = rows.at(0, p);
            if (prev.len_ == name_cell.len_
                && 0 == strncmp(prev.ptr_, name_cell.ptr_, name_cell.len_)) {
              ++dup;
            }
          }
          if (dup > 1) {
            ObStringBuffer name_buf(&alloc);
            char suffix[32];
            const int64_t suffix_len = snprintf(suffix, sizeof(suffix), "__%ld", dup);
            if (OB_FAIL(name_buf.append(name_cell.ptr_, name_cell.len_))) {
            } else if (OB_FAIL(name_buf.append(suffix, suffix_len))) {
            } else if (OB_FAIL(name_buf.get_result_string(name))) {
            } else {
              column.name_ = name;
            }
          } else {
            column.name_.assign_ptr(name_cell.ptr_, name_cell.len_);
          }
        } else {
          char tmp[32];
          const int64_t len = snprintf(tmp, sizeof(tmp), "column%ld", c);
          char *name_buf = static_cast<char *>(alloc.alloc(len));
          if (OB_ISNULL(name_buf)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            MEMCPY(name_buf, tmp, len);
            column.name_.assign_ptr(name_buf, static_cast<int32_t>(len));
          }
        }
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(table.columns_.push_back(column))) {
        }
      }
    }
    // 2. build the JSON document
    for (int64_t r = data_row_begin; OB_SUCC(ret) && r < rows.row_cnt_; ++r) {
      if (OB_FAIL(file_sql_append_str(doc, (r == data_row_begin) ? "{" : ",{"))) {
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < rows.column_cnt_; ++c) {
        char key[32];
        const int64_t key_len = snprintf(key, sizeof(key), "%s\"c%ld\":", (0 == c) ? "" : ",", c);
        if (OB_FAIL(file_sql_append(doc, key, key_len))) {
        } else if (OB_FAIL(file_sql_csv_append_value(doc, rows.at(r, c), table.columns_.at(c).type_))) {
        }
      }
      if (OB_SUCC(ret)) {
        ret = file_sql_append_str(doc, "}");
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(file_sql_append_str(doc, "]"))) {
      } else if (OB_FAIL(doc.get_result_string(table.json_doc_))) {
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////
// JSON / JSONL
////////////////////////////////////////////////////////////////

struct FileSqlJsonColumn
{
  FileSqlJsonColumn()
    : key_(), is_bool_(true), is_int_(true), is_number_(true), is_date_(true),
      is_datetime_(true), seen_value_(false), nullable_(false), max_len_(0) {}
  ObString key_;
  bool is_bool_;
  bool is_int_;
  bool is_number_;
  bool is_date_;
  bool is_datetime_;
  bool seen_value_;
  bool nullable_;
  int64_t max_len_;
};

int file_sql_json_track_value(FileSqlJsonColumn &column, ObIJsonBase *value)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(value)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObJsonNodeType json_type = value->json_type();
    if (ObJsonNodeType::J_NULL == json_type) {
      column.nullable_ = true;
    } else {
      column.seen_value_ = true;
      switch (json_type) {
        case ObJsonNodeType::J_BOOLEAN: {
          column.is_int_ = false;
          column.is_number_ = false;
          column.is_date_ = false;
          column.is_datetime_ = false;
          break;
        }
        case ObJsonNodeType::J_INT: {
          column.is_bool_ = false;
          column.is_date_ = false;
          column.is_datetime_ = false;
          break;
        }
        case ObJsonNodeType::J_UINT:
        case ObJsonNodeType::J_DOUBLE:
        case ObJsonNodeType::J_DECIMAL: {
          column.is_bool_ = false;
          column.is_int_ = false;
          column.is_date_ = false;
          column.is_datetime_ = false;
          break;
        }
        default: {
          column.is_bool_ = false;
          column.is_int_ = false;
          column.is_number_ = false;
          if (ObJsonNodeType::J_STRING == json_type) {
            const char *str = value->get_data();
            const int64_t str_len = static_cast<int64_t>(value->get_data_length());
            column.max_len_ = MAX(column.max_len_, str_len);
            if (!file_sql_is_date(str, str_len)) {
              column.is_date_ = false;
            }
            if (!file_sql_is_datetime(str, str_len)) {
              column.is_datetime_ = false;
            }
          } else {
            column.is_date_ = false;
            column.is_datetime_ = false;
          }
          break;
        }
      }
    }
  }
  return ret;
}

int file_sql_read_json(ObIAllocator &alloc,
                       const ObString &path,
                       const ObFileSqlOptions &options,
                       ObFileSqlTable &table)
{
  int ret = OB_SUCCESS;
  ObString content;
  ObString json_text;
  ObStringBuffer doc(&alloc);
  ObIJsonBase *root = NULL;
  if (!options.format_.empty()
      && !file_sql_ci_equal(options.format_.ptr(), options.format_.length(),
                            "newline_delimited", 17)) {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported json format", K(ret), K(options.format_));
    LOG_USER_ERROR(OB_NOT_SUPPORTED, "json format other than newline_delimited");
  } else if (OB_FAIL(file_sql_read_file(alloc, path, content))) {
  } else if (OB_FAIL(file_sql_append_json_lines(doc, content))) {
  } else if (OB_FAIL(doc.get_result_string(json_text))) {
  } else if (OB_FAIL(ObJsonBaseFactory::get_json_base(&alloc, json_text,
                                                       ObJsonInType::JSON_TREE,
                                                       ObJsonInType::JSON_TREE, root, 0))) {
  } else if (OB_ISNULL(root) || ObJsonNodeType::J_ARRAY != root->json_type()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("json file should contain newline delimited objects", K(ret));
  } else {
    ObSEArray<FileSqlJsonColumn, 8> columns;
    const uint64_t row_cnt = root->element_count();
    for (uint64_t r = 0; OB_SUCC(ret) && r < row_cnt; ++r) {
      ObIJsonBase *row = NULL;
      if (OB_FAIL(root->get_array_element(r, row))) {
      } else if (OB_ISNULL(row) || ObJsonNodeType::J_OBJECT != row->json_type()) {
        ret = OB_INVALID_ARGUMENT;
        LOG_WARN("json row should be an object", K(ret), K(r));
      } else {
        const uint64_t member_cnt = row->element_count();
        for (uint64_t i = 0; OB_SUCC(ret) && i < member_cnt; ++i) {
          ObIJsonBase *member = NULL;
          ObString key;
          if (OB_FAIL(row->get_object_value(i, key, member))) {
          } else if (OB_ISNULL(member)) {
            ret = OB_ERR_UNEXPECTED;
          } else {
            int64_t column_idx = -1;
            for (int64_t c = 0; c < columns.count() && column_idx < 0; ++c) {
              if (0 == ObCharset::strcmp(CS_TYPE_UTF8MB4_BIN, key, columns.at(c).key_)) {
                column_idx = c;
              }
            }
            if (column_idx < 0) {
              FileSqlJsonColumn column;
              if (OB_FAIL(ob_write_string(alloc, key, column.key_))) {
              } else if (columns.count() >= FILE_SQL_MAX_COLUMN_CNT) {
                ret = OB_NOT_SUPPORTED;
              } else if (OB_FAIL(columns.push_back(column))) {
              } else {
                column_idx = columns.count() - 1;
              }
            }
            if (OB_SUCC(ret)) {
              ret = file_sql_json_track_value(columns.at(column_idx), member);
            }
          }
        }
      }
    }
    for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
      FileSqlJsonColumn &column = columns.at(c);
      ObFileSqlColumn column_info;
      if (!column.seen_value_) {
        column_info.type_ = ObVarcharType;
        column_info.length_ = FILE_SQL_DEFAULT_VARCHAR_LEN;
      } else if (column.is_bool_) {
        column_info.type_ = ObTinyIntType;
      } else if (column.is_int_) {
        column_info.type_ = ObIntType;
      } else if (column.is_number_) {
        column_info.type_ = ObDoubleType;
      } else if (column.is_datetime_) {
        column_info.type_ = ObDateTimeType;
      } else if (column.is_date_) {
        column_info.type_ = ObDateType;
      } else {
        column_info.type_ = ObVarcharType;
        column_info.length_ = MAX(column.max_len_, static_cast<int64_t>(1));
      }
      column_info.nullable_ = column.nullable_;
      column_info.name_ = column.key_;
      // json objects keep their own keys, so the document cannot use c<ordinal>
      column_info.doc_key_ = column.key_;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(table.columns_.push_back(column_info))) {
      }
    }
    if (OB_SUCC(ret)) {
      table.json_doc_ = json_text;
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////
// Parquet
////////////////////////////////////////////////////////////////

// Minimal thrift compact protocol reader, enough for the parquet footer.
class FileSqlThriftReader
{
public:
  FileSqlThriftReader() : buf_(NULL), len_(0), pos_(0) {}
  void init(const char *buf, const int64_t len, const int64_t pos)
  {
    buf_ = buf;
    len_ = len;
    pos_ = pos;
  }
  int64_t pos() const { return pos_; }
  int read_field_header(int8_t &type, int16_t &field_id, bool &stop, int16_t &last_id)
  {
    int ret = OB_SUCCESS;
    uint8_t byte = 0;
    stop = false;
    if (OB_FAIL(read_byte(byte))) {
    } else {
      type = byte & 0x0f;
      if (0 == type) {
        stop = true;
        last_id = 0;
      } else {
        const int16_t delta = (byte >> 4) & 0x0f;
        if (0 == delta) {
          int64_t id = 0;
          if (OB_FAIL(read_zigzag(id))) {
          } else {
            field_id = static_cast<int16_t>(id);
          }
        } else {
          field_id = static_cast<int16_t>(last_id + delta);
        }
        if (OB_SUCC(ret)) {
          last_id = field_id;
        }
      }
    }
    return ret;
  }
  int read_i32(int32_t &value)
  {
    int ret = OB_SUCCESS;
    int64_t tmp = 0;
    if (OB_FAIL(read_zigzag(tmp))) {
    } else {
      value = static_cast<int32_t>(tmp);
    }
    return ret;
  }
  int read_i64(int64_t &value) { return read_zigzag(value); }
  int read_byte(uint8_t &value)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      value = static_cast<uint8_t>(buf_[pos_++]);
    }
    return ret;
  }
  int read_bool(bool &value)
  {
    int ret = OB_SUCCESS;
    uint8_t byte = 0;
    if (OB_FAIL(read_byte(byte))) {
    } else {
      value = (1 == byte);
    }
    return ret;
  }
  int read_binary(ObString &value)
  {
    int ret = OB_SUCCESS;
    uint64_t len = 0;
    if (OB_FAIL(read_varint(len))) {
    } else if (len > static_cast<uint64_t>(len_ - pos_) || pos_ + len > len_) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      value.assign_ptr(buf_ + pos_, static_cast<int32_t>(len));
      pos_ += len;
    }
    return ret;
  }
  int read_list_header(int8_t &elem_type, int32_t &size)
  {
    int ret = OB_SUCCESS;
    uint8_t byte = 0;
    if (OB_FAIL(read_byte(byte))) {
    } else {
      elem_type = byte & 0x0f;
      size = (byte >> 4) & 0x0f;
      if (15 == size) {
        uint64_t len = 0;
        if (OB_FAIL(read_varint(len))) {
        } else {
          size = static_cast<int32_t>(len);
        }
      }
    }
    return ret;
  }
  int skip(const int8_t type)
  {
    int ret = OB_SUCCESS;
    switch (type) {
      case 1:
      case 2: {
        break;
      }
      case 3: {
        uint8_t byte = 0;
        ret = read_byte(byte);
        break;
      }
      case 4:
      case 5:
      case 6: {
        int64_t value = 0;
        ret = read_zigzag(value);
        break;
      }
      case 7: {
        if (pos_ + 8 > len_) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          pos_ += 8;
        }
        break;
      }
      case 8: {
        ObString str;
        ret = read_binary(str);
        break;
      }
      case 9:
      case 10: {
        int8_t elem_type = 0;
        int32_t size = 0;
        if (OB_FAIL(read_list_header(elem_type, size))) {
        } else {
          for (int32_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            ret = skip(elem_type);
          }
        }
        break;
      }
      case 11: {
        uint8_t byte = 0;
        if (OB_FAIL(read_byte(byte))) {
        } else {
          const int8_t key_type = byte & 0x0f;
          const int8_t value_type = (byte >> 4) & 0x0f;
          uint64_t size = 0;
          if (0 == (byte >> 4)) {
            if (OB_FAIL(read_varint(size))) {
            }
          } else {
            size = (byte >> 4) & 0x0f;
            if (15 == size) {
              if (OB_FAIL(read_varint(size))) {
              }
            }
          }
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            if (OB_FAIL(skip(key_type))) {
            } else {
              ret = skip(value_type);
            }
          }
        }
        break;
      }
      case 12: {
        int16_t last_id = 0;
        bool stop = false;
        while (OB_SUCC(ret) && !stop) {
          int8_t field_type = 0;
          int16_t field_id = 0;
          if (OB_FAIL(read_field_header(field_type, field_id, stop, last_id))) {
          } else if (!stop) {
            ret = skip(field_type);
          }
        }
        break;
      }
      default: {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
    }
    return ret;
  }
private:
  int read_varint(uint64_t &value)
  {
    int ret = OB_SUCCESS;
    value = 0;
    int32_t shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      uint8_t byte = 0;
      if (OB_FAIL(read_byte(byte))) {
      } else {
        if (shift >= 64) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          value |= (static_cast<uint64_t>(byte & 0x7f)) << shift;
          shift += 7;
          done = (0 == (byte & 0x80));
        }
      }
    }
    return ret;
  }
  int read_zigzag(int64_t &value)
  {
    int ret = OB_SUCCESS;
    uint64_t raw = 0;
    if (OB_FAIL(read_varint(raw))) {
    } else {
      value = static_cast<int64_t>((raw >> 1) ^ (~(raw & 1) + 1));
    }
    return ret;
  }
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

struct FileSqlParquetColumn
{
  FileSqlParquetColumn()
    : name_(), physical_type_(-1), converted_type_(-1), optional_(false),
      data_page_offset_(-1), dict_page_offset_(-1), num_values_(0), codec_(0) {}
  ObString name_;
  int32_t physical_type_;
  int32_t converted_type_;
  bool optional_;
  int64_t data_page_offset_;
  int64_t dict_page_offset_;
  int64_t num_values_;
  int32_t codec_;
};

struct FileSqlParquetValues
{
  FileSqlParquetValues() : ints_(), reals_(), strs_(), nulls_() {}
  void reuse()
  {
    ints_.reuse();
    reals_.reuse();
    strs_.reuse();
    nulls_.reuse();
  }
  ObSEArray<int64_t, 32> ints_;
  ObSEArray<double, 32> reals_;
  ObSEArray<ObString, 32> strs_;
  ObSEArray<int8_t, 32> nulls_;
};

enum FileSqlParquetPhysicalType
{
  PARQUET_BOOLEAN = 0,
  PARQUET_INT32 = 1,
  PARQUET_INT64 = 2,
  PARQUET_INT96 = 3,
  PARQUET_FLOAT = 4,
  PARQUET_DOUBLE = 5,
  PARQUET_BYTE_ARRAY = 6,
  PARQUET_FIXED_LEN_BYTE_ARRAY = 7,
};

enum FileSqlParquetConvertedType
{
  PARQUET_UTF8 = 0,
  PARQUET_DATE = 6,
  PARQUET_TIMESTAMP_MILLIS = 9,
  PARQUET_TIMESTAMP_MICROS = 10,
};

int file_sql_snappy_uncompress(const char *in, const int64_t in_len, char *out,
                               const int64_t out_cap, int64_t &out_len)
{
  int ret = OB_SUCCESS;
  int64_t ipos = 0;
  int64_t opos = 0;
  uint64_t expect_len = 0;
  int32_t shift = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    if (ipos >= in_len || shift >= 64) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const uint8_t byte = static_cast<uint8_t>(in[ipos++]);
      expect_len |= (static_cast<uint64_t>(byte & 0x7f)) << shift;
      shift += 7;
      done = (0 == (byte & 0x80));
    }
  }
  if (OB_SUCC(ret) && expect_len > static_cast<uint64_t>(out_cap)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("snappy output buffer too small", K(ret), K(expect_len), K(out_cap));
  }
  while (OB_SUCC(ret) && ipos < in_len) {
    const uint8_t tag = static_cast<uint8_t>(in[ipos++]);
    const uint8_t type = tag & 0x03;
    if (0 == type) {
      int64_t len = (tag >> 2) + 1;
      if (len > 60) {
        const int32_t extra = static_cast<int32_t>(len - 60);
        if (ipos + extra > in_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          len = 0;
          for (int32_t i = 0; i < extra; ++i) {
            len |= static_cast<int64_t>(static_cast<uint8_t>(in[ipos + i])) << (8 * i);
          }
          ++len;
          ipos += extra;
        }
      }
      if (OB_SUCC(ret)) {
        if (ipos + len > in_len || opos + len > out_cap) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          MEMCPY(out + opos, in + ipos, len);
          ipos += len;
          opos += len;
        }
      }
    } else {
      int64_t len = 0;
      int64_t offset = 0;
      if (1 == type) {
        if (ipos >= in_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          len = ((tag >> 2) & 0x07) + 4;
          offset = ((static_cast<int64_t>(tag >> 5) & 0x07) << 8)
                   | static_cast<uint8_t>(in[ipos]);
          ++ipos;
        }
      } else if (2 == type) {
        if (ipos + 2 > in_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          len = (tag >> 2) + 1;
          offset = static_cast<uint8_t>(in[ipos])
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[ipos + 1])) << 8);
          ipos += 2;
        }
      } else {
        if (ipos + 4 > in_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          len = (tag >> 2) + 1;
          offset = static_cast<uint8_t>(in[ipos])
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[ipos + 1])) << 8)
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[ipos + 2])) << 16)
                   | (static_cast<int64_t>(static_cast<uint8_t>(in[ipos + 3])) << 24);
          ipos += 4;
        }
      }
      if (OB_SUCC(ret)) {
        if (offset <= 0 || offset > opos || opos + len > out_cap) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          for (int64_t i = 0; i < len; ++i) {
            out[opos] = out[opos - offset];
            ++opos;
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    out_len = opos;
  }
  return ret;
}

int file_sql_unpack_bits(const uint8_t *src, const int32_t bit_width, const int64_t cnt,
                         const int64_t src_len, int64_t *out)
{
  int ret = OB_SUCCESS;
  if (bit_width < 0 || bit_width > 32) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == bit_width) {
    for (int64_t i = 0; i < cnt; ++i) {
      out[i] = 0;
    }
  } else {
    const int64_t need = (cnt * bit_width + 7) >> 3;
    if (need > src_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      const uint64_t mask = (1ULL << bit_width) - 1;
      for (int64_t i = 0; OB_SUCC(ret) && i < cnt; ++i) {
        const int64_t bit = i * bit_width;
        const int64_t byte_idx = bit >> 3;
        const int32_t shift = static_cast<int32_t>(bit & 7);
        const int32_t bytes = (shift + bit_width + 7) >> 3;
        uint64_t value = 0;
        for (int32_t b = 0; b < bytes; ++b) {
          value |= static_cast<uint64_t>(src[byte_idx + b]) << (8 * b);
        }
        out[i] = static_cast<int64_t>((value >> shift) & mask);
      }
    }
  }
  return ret;
}

// RLE / bit-packed hybrid decoder, used for levels and dictionary indices.
int file_sql_read_hybrid(const char *buf, const int64_t buf_len, int64_t &pos,
                         const int32_t bit_width, const int64_t value_cnt,
                         ObIArray<int64_t> &values)
{
  int ret = OB_SUCCESS;
  int64_t decoded = 0;
  int64_t *scratch = NULL;
  void *tmp = NULL;
  ObArenaAllocator tmp_alloc;
  if (OB_ISNULL(tmp = tmp_alloc.alloc(sizeof(int64_t) * MAX(value_cnt, static_cast<int64_t>(8))))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    scratch = static_cast<int64_t *>(tmp);
  }
  while (OB_SUCC(ret) && decoded < value_cnt) {
    if (pos >= buf_len) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      // read uvarint header
      uint64_t header = 0;
      int32_t shift = 0;
      bool done = false;
      while (OB_SUCC(ret) && !done) {
        if (pos >= buf_len || shift >= 64) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          const uint8_t byte = static_cast<uint8_t>(buf[pos++]);
          header |= (static_cast<uint64_t>(byte & 0x7f)) << shift;
          shift += 7;
          done = (0 == (byte & 0x80));
        }
      }
      if (OB_FAIL(ret)) {
      } else if (0 == (header & 1)) {
        const int64_t run = static_cast<int64_t>(header >> 1);
        const int32_t byte_width = (bit_width + 7) >> 3;
        if (pos + byte_width > buf_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          int64_t value = 0;
          for (int32_t i = 0; i < byte_width; ++i) {
            value |= static_cast<int64_t>(static_cast<uint8_t>(buf[pos + i])) << (8 * i);
          }
          pos += byte_width;
          for (int64_t i = 0; OB_SUCC(ret) && i < run && decoded < value_cnt; ++i) {
            if (OB_FAIL(values.push_back(value))) {
            } else {
              ++decoded;
            }
          }
        }
      } else {
        const int64_t groups = static_cast<int64_t>(header >> 1);
        const int64_t cnt = groups * 8;
        const int64_t bytes = (cnt * bit_width + 7) >> 3;
        if (pos + bytes > buf_len) {
          ret = OB_INVALID_ARGUMENT;
        } else if (OB_FAIL(file_sql_unpack_bits(reinterpret_cast<const uint8_t *>(buf + pos),
                                                bit_width, cnt, bytes, scratch))) {
        } else {
          pos += bytes;
          for (int64_t i = 0; OB_SUCC(ret) && i < cnt && decoded < value_cnt; ++i) {
            if (OB_FAIL(values.push_back(scratch[i]))) {
            } else {
              ++decoded;
            }
          }
        }
      }
    }
  }
  return ret;
}

struct FileSqlParquetPageHeader
{
  FileSqlParquetPageHeader()
    : type_(-1), uncompressed_page_size_(0), compressed_page_size_(0),
      num_values_(0), encoding_(0), dict_num_values_(0),
      v2_num_values_(0), v2_num_nulls_(0), v2_num_rows_(0), v2_encoding_(0),
      v2_def_len_(0), v2_rep_len_(0), v2_is_compressed_(true) {}
  int32_t type_;
  int32_t uncompressed_page_size_;
  int32_t compressed_page_size_;
  int32_t num_values_;
  int32_t encoding_;
  int32_t dict_num_values_;
  int32_t v2_num_values_;
  int32_t v2_num_nulls_;
  int32_t v2_num_rows_;
  int32_t v2_encoding_;
  int32_t v2_def_len_;
  int32_t v2_rep_len_;
  bool v2_is_compressed_;
};

int file_sql_parquet_read_page_header(FileSqlThriftReader &reader, FileSqlParquetPageHeader &header)
{
  int ret = OB_SUCCESS;
  int16_t last_id = 0;
  bool stop = false;
  while (OB_SUCC(ret) && !stop) {
    int8_t field_type = 0;
    int16_t field_id = 0;
    if (OB_FAIL(reader.read_field_header(field_type, field_id, stop, last_id))) {
    } else if (stop) {
    } else if (1 == field_id || 2 == field_id || 3 == field_id) {
      int32_t value = 0;
      if (5 != field_type) {
        ret = OB_INVALID_ARGUMENT;
      } else if (OB_FAIL(reader.read_i32(value))) {
      } else if (1 == field_id) {
        header.type_ = value;
      } else if (2 == field_id) {
        header.uncompressed_page_size_ = value;
      } else {
        header.compressed_page_size_ = value;
      }
    } else if (5 == field_id || 8 == field_id) {
      // DataPageHeader / DataPageHeaderV2
      int16_t sub_last_id = 0;
      bool sub_stop = false;
      while (OB_SUCC(ret) && !sub_stop) {
        int8_t sub_type = 0;
        int16_t sub_id = 0;
        if (OB_FAIL(reader.read_field_header(sub_type, sub_id, sub_stop, sub_last_id))) {
        } else if (sub_stop) {
        } else if (8 == field_id && 7 == sub_id) {
          if (1 == sub_type || 2 == sub_type) {
            header.v2_is_compressed_ = (1 == sub_type);
          } else {
            bool value = false;
            if (OB_FAIL(reader.read_bool(value))) {
            } else {
              header.v2_is_compressed_ = value;
            }
          }
        } else if (5 == sub_type) {
          int32_t value = 0;
          if (OB_FAIL(reader.read_i32(value))) {
          } else if (5 == field_id) {
            if (1 == sub_id) {
              header.num_values_ = value;
            } else if (2 == sub_id) {
              header.encoding_ = value;
            }
          } else {
            if (1 == sub_id) {
              header.v2_num_values_ = value;
            } else if (2 == sub_id) {
              header.v2_num_nulls_ = value;
            } else if (3 == sub_id) {
              header.v2_num_rows_ = value;
            } else if (4 == sub_id) {
              header.v2_encoding_ = value;
            } else if (5 == sub_id) {
              header.v2_def_len_ = value;
            } else if (6 == sub_id) {
              header.v2_rep_len_ = value;
            }
          }
        } else if (OB_FAIL(reader.skip(sub_type))) {
        }
      }
    } else if (7 == field_id) {
      // DictionaryPageHeader
      int16_t sub_last_id = 0;
      bool sub_stop = false;
      while (OB_SUCC(ret) && !sub_stop) {
        int8_t sub_type = 0;
        int16_t sub_id = 0;
        if (OB_FAIL(reader.read_field_header(sub_type, sub_id, sub_stop, sub_last_id))) {
        } else if (sub_stop) {
        } else if (1 == sub_id && 5 == sub_type) {
          int32_t value = 0;
          if (OB_FAIL(reader.read_i32(value))) {
          } else {
            header.dict_num_values_ = value;
          }
        } else if (OB_FAIL(reader.skip(sub_type))) {
        }
      }
    } else if (OB_FAIL(reader.skip(field_type))) {
    }
  }
  return ret;
}

int file_sql_parquet_plain_values(const char *buf, const int64_t buf_len, int64_t &pos,
                                  const int32_t physical_type, const int64_t value_cnt,
                                  FileSqlParquetValues &values)
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < value_cnt; ++i) {
    if (PARQUET_BOOLEAN == physical_type) {
      if (pos + 1 > buf_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        const uint8_t byte = static_cast<uint8_t>(buf[pos]);
        const int64_t bit = i & 0x07;
        if (0 == bit) {
          ++pos;
        }
        if (OB_FAIL(values.ints_.push_back((byte >> bit) & 0x01))) {
        }
      }
    } else if (PARQUET_INT32 == physical_type || PARQUET_INT64 == physical_type) {
      const int32_t width = (PARQUET_INT32 == physical_type) ? 4 : 8;
      if (pos + width > buf_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        int64_t value = 0;
        for (int32_t b = 0; b < width; ++b) {
          value |= static_cast<int64_t>(static_cast<uint8_t>(buf[pos + b])) << (8 * b);
        }
        if (4 == width) {
          value = static_cast<int32_t>(value);
        }
        pos += width;
        if (OB_FAIL(values.ints_.push_back(value))) {
        }
      }
    } else if (PARQUET_FLOAT == physical_type || PARQUET_DOUBLE == physical_type) {
      const int32_t width = (PARQUET_FLOAT == physical_type) ? 4 : 8;
      if (pos + width > buf_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        double value = 0;
        if (4 == width) {
          float tmp = 0;
          MEMCPY(&tmp, buf + pos, 4);
          value = tmp;
        } else {
          MEMCPY(&value, buf + pos, 8);
        }
        pos += width;
        if (OB_FAIL(values.reals_.push_back(value))) {
        }
      }
    } else if (PARQUET_BYTE_ARRAY == physical_type) {
      if (pos + 4 > buf_len) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        int32_t len = 0;
        MEMCPY(&len, buf + pos, 4);
        pos += 4;
        if (len < 0 || pos + len > buf_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          ObString value;
          value.assign_ptr(buf + pos, len);
          pos += len;
          if (OB_FAIL(values.strs_.push_back(value))) {
          }
        }
      }
    } else if (PARQUET_FIXED_LEN_BYTE_ARRAY == physical_type) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("fixed len byte array is not supported", K(ret));
    } else {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("unsupported parquet physical type", K(ret), K(physical_type));
    }
  }
  return ret;
}

int file_sql_parquet_decode_chunk(ObIAllocator &alloc,
                                  const ObString &file,
                                  FileSqlParquetColumn &column,
                                  FileSqlParquetValues &values)
{
  int ret = OB_SUCCESS;
  const char *buf = file.ptr();
  const int64_t buf_len = file.length();
  int64_t pos = (column.dict_page_offset_ >= 0) ? column.dict_page_offset_ : column.data_page_offset_;
  int64_t decoded = 0;
  int64_t dict_cnt = 0;
  FileSqlParquetValues dict;
  ObArenaAllocator page_alloc;
  UNUSED(alloc);
  if (pos < 0 || pos >= buf_len) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid parquet column offset", K(ret), K(pos), K(buf_len));
  }
  while (OB_SUCC(ret) && decoded < column.num_values_) {
    FileSqlThriftReader reader;
    FileSqlParquetPageHeader header;
    reader.init(buf, buf_len, pos);
    if (OB_FAIL(file_sql_parquet_read_page_header(reader, header))) {
      LOG_WARN("failed to read parquet page header", K(ret), K(reader.pos()));
    } else if (header.compressed_page_size_ < 0 || header.uncompressed_page_size_ < 0) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid parquet page size", K(ret), K(header.compressed_page_size_),
               K(header.uncompressed_page_size_));
    } else if (reader.pos() + header.compressed_page_size_ > buf_len) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("parquet page is out of file", K(ret), K(reader.pos()),
               K(header.compressed_page_size_), K(buf_len));
    } else {
      const char *payload = buf + reader.pos();
      const int64_t payload_len = header.compressed_page_size_;
      const char *page = payload;
      int64_t page_len = payload_len;
      pos = reader.pos() + payload_len;
      if (2 == header.type_) {
        // dictionary page, always compressed as a whole
        if (0 != column.codec_) {
          int64_t out_len = 0;
          char *out = static_cast<char *>(page_alloc.alloc(header.uncompressed_page_size_));
          if (1 != column.codec_) {
            ret = OB_NOT_SUPPORTED;
            LOG_WARN("unsupported parquet compression codec", K(ret), K(column.codec_));
          } else if (OB_ISNULL(out)) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else if (OB_FAIL(file_sql_snappy_uncompress(payload, payload_len, out,
                                                        header.uncompressed_page_size_, out_len))) {
          } else {
            page = out;
            page_len = out_len;
          }
        }
        int64_t dict_pos = 0;
        dict.reuse();
        if (OB_SUCC(ret) && OB_FAIL(file_sql_parquet_plain_values(page, page_len, dict_pos,
                                                                  column.physical_type_,
                                                                  header.dict_num_values_, dict))) {
        } else {
          dict_cnt = header.dict_num_values_;
        }
      } else if (0 == header.type_ || 3 == header.type_) {
        const bool is_v2 = (3 == header.type_);
        const int32_t num_values = is_v2 ? header.v2_num_values_ : header.num_values_;
        const int32_t encoding = is_v2 ? header.v2_encoding_ : header.encoding_;
        const int32_t def_len = is_v2 ? header.v2_def_len_ : 0;
        const int32_t rep_len = is_v2 ? header.v2_rep_len_ : 0;
        int64_t level_pos = 0;
        int64_t data_pos = rep_len + def_len;
        int64_t data_len = payload_len - data_pos;
        if (!is_v2) {
          // V1 pages store levels as (len, rle) blocks; skip repetition levels
          uint32_t rep_size = 0;
          MEMCPY(&rep_size, payload, 4);
          level_pos = 4 + rep_size;
          uint32_t def_size = 0;
          if (level_pos + 4 > payload_len) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            MEMCPY(&def_size, payload + level_pos, 4);
            level_pos += 4;
            data_pos = level_pos + def_size;
            data_len = payload_len - data_pos;
          }
        }
        if (OB_FAIL(ret)) {
        } else if (data_pos < 0 || data_pos > payload_len) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          const char *data = payload + data_pos;
          int64_t out_len = data_len;
          const bool need_decompress = is_v2 ? (0 != column.codec_ && header.v2_is_compressed_)
                                             : (0 != column.codec_);
          if (need_decompress) {
            // the levels are stored uncompressed, only the values are compressed
            const int64_t out_cap = header.uncompressed_page_size_ - data_pos;
            char *out = (out_cap > 0)
                ? static_cast<char *>(page_alloc.alloc(out_cap)) : NULL;
            if (1 != column.codec_) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("unsupported parquet compression codec", K(ret), K(column.codec_));
            } else if (OB_ISNULL(out)) {
              ret = OB_ALLOCATE_MEMORY_FAILED;
            } else if (OB_FAIL(file_sql_snappy_uncompress(data, data_len, out,
                                                          out_cap, out_len))) {
            } else {
              data = out;
            }
          }
          // definition levels: 0 means NULL for optional columns
          ObSEArray<int64_t, 32> def_levels;
          int64_t null_cnt = 0;
          if (column.optional_) {
            int64_t def_pos = is_v2 ? rep_len : level_pos;
            const char *def_buf = is_v2 ? (payload + rep_len) : (payload + def_pos);
            int64_t def_buf_len = is_v2 ? def_len : (data_pos - 4 - rep_len - 4);
            int64_t rel_pos = 0;
            if (def_buf_len < 0) {
              ret = OB_INVALID_ARGUMENT;
            } else if (OB_FAIL(file_sql_read_hybrid(def_buf, def_buf_len, rel_pos, 1,
                                                    num_values, def_levels))) {
            } else {
              for (int64_t i = 0; i < def_levels.count(); ++i) {
                if (0 == def_levels.at(i)) {
                  ++null_cnt;
                }
              }
            }
          }
          const int64_t value_cnt = num_values - null_cnt;
          FileSqlParquetValues page_values;
          const int64_t value_begin = values.ints_.count() + values.reals_.count()
                                      + values.strs_.count();
          if (OB_FAIL(ret)) {
          } else if (0 == encoding) {
            int64_t value_pos = 0;
            if (OB_FAIL(file_sql_parquet_plain_values(data, out_len, value_pos,
                                                      column.physical_type_, value_cnt,
                                                      page_values))) {
            }
          } else if (2 == encoding || 8 == encoding) {
            if (out_len <= 0) {
              ret = OB_INVALID_ARGUMENT;
            } else {
              const int32_t bit_width = static_cast<uint8_t>(data[0]);
              int64_t idx_pos = 1;
              ObSEArray<int64_t, 32> indices;
              if (OB_FAIL(file_sql_read_hybrid(data, out_len, idx_pos, bit_width,
                                               value_cnt, indices))) {
              }
              for (int64_t i = 0; OB_SUCC(ret) && i < indices.count(); ++i) {
                const int64_t idx = indices.at(i);
                if (idx < 0 || idx >= dict_cnt) {
                  ret = OB_INVALID_ARGUMENT;
                } else if (PARQUET_BYTE_ARRAY == column.physical_type_) {
                  if (dict_cnt > dict.strs_.count()) {
                    ret = OB_INVALID_ARGUMENT;
                  } else {
                    ret = page_values.strs_.push_back(dict.strs_.at(idx));
                  }
                } else if (PARQUET_FLOAT == column.physical_type_
                           || PARQUET_DOUBLE == column.physical_type_) {
                  ret = page_values.reals_.push_back(dict.reals_.at(idx));
                } else {
                  ret = page_values.ints_.push_back(dict.ints_.at(idx));
                }
              }
            }
          } else {
            ret = OB_NOT_SUPPORTED;
            LOG_WARN("unsupported parquet encoding", K(ret), K(encoding));
          }
          if (OB_FAIL(ret)) {
          } else {
            // merge nulls and values into the column buffers
            int64_t value_idx = 0;
            for (int64_t i = 0; OB_SUCC(ret) && i < num_values; ++i) {
              const bool is_null = column.optional_ && (0 == def_levels.at(i));
              if (OB_FAIL(values.nulls_.push_back(is_null ? 1 : 0))) {
              } else if (!is_null) {
                if (PARQUET_BYTE_ARRAY == column.physical_type_) {
                  ret = values.strs_.push_back(page_values.strs_.at(value_idx));
                } else if (PARQUET_FLOAT == column.physical_type_
                           || PARQUET_DOUBLE == column.physical_type_) {
                  ret = values.reals_.push_back(page_values.reals_.at(value_idx));
                } else {
                  ret = values.ints_.push_back(page_values.ints_.at(value_idx));
                }
                ++value_idx;
              }
            }
            decoded += num_values;
          }
          UNUSED(value_begin);
        }
      } else if (1 == header.type_) {
        // index pages carry no values
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet page type", K(ret), K(header.type_));
      }
    }
  }
  if (OB_SUCC(ret) && decoded < column.num_values_) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("parquet column values are truncated", K(ret), K(decoded), K(column.num_values_));
  }
  return ret;
}

int file_sql_parquet_read_footer(ObIAllocator &alloc,
                                 const ObString &file,
                                 ObIArray<FileSqlParquetColumn> &columns,
                                 int64_t &row_cnt)
{
  int ret = OB_SUCCESS;
  const char *buf = file.ptr();
  const int64_t buf_len = file.length();
  int64_t footer_len = 0;
  int32_t name_cnt = 0;
  if (buf_len < 12 || 0 != MEMCMP(buf, "PAR1", 4)
      || 0 != MEMCMP(buf + buf_len - 4, "PAR1", 4)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid parquet file header or tail", K(ret), K(buf_len));
  } else {
    MEMCPY(&footer_len, buf + buf_len - 8, 4);
    if (footer_len <= 0 || footer_len > buf_len - 12) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid parquet footer length", K(ret), K(footer_len));
    }
  }
  if (OB_SUCC(ret)) {
    FileSqlThriftReader reader;
    reader.init(buf, buf_len - 8, buf_len - 8 - footer_len);
    int16_t last_id = 0;
    bool stop = false;
    ObSEArray<ObString, 8> column_names;
    while (OB_SUCC(ret) && !stop) {
      int8_t field_type = 0;
      int16_t field_id = 0;
      if (OB_FAIL(reader.read_field_header(field_type, field_id, stop, last_id))) {
      } else if (stop) {
      } else if (2 == field_id && 9 == field_type) {
        // schema
        int8_t elem_type = 0;
        int32_t elem_cnt = 0;
        if (OB_FAIL(reader.read_list_header(elem_type, elem_cnt))) {
        }
        for (int32_t i = 0; OB_SUCC(ret) && i < elem_cnt; ++i) {
          int16_t schema_last_id = 0;
          bool schema_stop = false;
          FileSqlParquetColumn column;
          int32_t num_children = 0;
          bool is_root = (0 == i);
          while (OB_SUCC(ret) && !schema_stop) {
            int8_t sub_type = 0;
            int16_t sub_id = 0;
            if (OB_FAIL(reader.read_field_header(sub_type, sub_id, schema_stop,
                                                 schema_last_id))) {
            } else if (schema_stop) {
            } else if (1 == sub_id && 5 == sub_type) {
              if (OB_FAIL(reader.read_i32(column.physical_type_))) {
              }
            } else if (3 == sub_id && 5 == sub_type) {
              int32_t repetition = 0;
              if (OB_FAIL(reader.read_i32(repetition))) {
              } else {
                column.optional_ = (1 == repetition);
              }
            } else if (4 == sub_id && 8 == sub_type) {
              if (OB_FAIL(reader.read_binary(column.name_))) {
              }
            } else if (5 == sub_id && 5 == sub_type) {
              if (OB_FAIL(reader.read_i32(num_children))) {
              }
            } else if (6 == sub_id && 5 == sub_type) {
              if (OB_FAIL(reader.read_i32(column.converted_type_))) {
              }
            } else if (OB_FAIL(reader.skip(sub_type))) {
            }
          }
          if (OB_SUCC(ret)) {
            if (!is_root && num_children > 0) {
              ret = OB_NOT_SUPPORTED;
              LOG_WARN("nested parquet schema is not supported", K(ret));
            } else if (!is_root && OB_FAIL(columns.push_back(column))) {
            }
          }
        }
        UNUSED(name_cnt);
      } else if (3 == field_id && 6 == field_type) {
        if (OB_FAIL(reader.read_i64(row_cnt))) {
        }
      } else if (4 == field_id && 9 == field_type) {
        // row groups
        int8_t elem_type = 0;
        int32_t elem_cnt = 0;
        if (OB_FAIL(reader.read_list_header(elem_type, elem_cnt))) {
        }
        for (int32_t i = 0; OB_SUCC(ret) && i < elem_cnt; ++i) {
          int16_t rg_last_id = 0;
          bool rg_stop = false;
          while (OB_SUCC(ret) && !rg_stop) {
            int8_t sub_type = 0;
            int16_t sub_id = 0;
            if (OB_FAIL(reader.read_field_header(sub_type, sub_id, rg_stop, rg_last_id))) {
            } else if (rg_stop) {
            } else if (1 == sub_id && 9 == sub_type) {
              int8_t chunk_type = 0;
              int32_t chunk_cnt = 0;
              if (OB_FAIL(reader.read_list_header(chunk_type, chunk_cnt))) {
              }
              for (int32_t c = 0; OB_SUCC(ret) && c < chunk_cnt; ++c) {
                int16_t chunk_last_id = 0;
                bool chunk_stop = false;
                FileSqlParquetColumn chunk_column;
                while (OB_SUCC(ret) && !chunk_stop) {
                  int8_t chunk_sub_type = 0;
                  int16_t chunk_sub_id = 0;
                  if (OB_FAIL(reader.read_field_header(chunk_sub_type, chunk_sub_id,
                                                       chunk_stop, chunk_last_id))) {
                  } else if (chunk_stop) {
                  } else if (3 == chunk_sub_id && 12 == chunk_sub_type) {
                    // ColumnMetaData
                    int16_t meta_last_id = 0;
                    bool meta_stop = false;
                    while (OB_SUCC(ret) && !meta_stop) {
                      int8_t meta_type = 0;
                      int16_t meta_id = 0;
                      if (OB_FAIL(reader.read_field_header(meta_type, meta_id, meta_stop,
                                                           meta_last_id))) {
                      } else if (meta_stop) {
                      } else if (1 == meta_id && 5 == meta_type) {
                        if (OB_FAIL(reader.read_i32(chunk_column.physical_type_))) {
                        }
                      } else if (4 == meta_id && 5 == meta_type) {
                        if (OB_FAIL(reader.read_i32(chunk_column.codec_))) {
                        }
                      } else if (5 == meta_id && 6 == meta_type) {
                        if (OB_FAIL(reader.read_i64(chunk_column.num_values_))) {
                        }
                      } else if (9 == meta_id && 6 == meta_type) {
                        if (OB_FAIL(reader.read_i64(chunk_column.data_page_offset_))) {
                        }
                      } else if (11 == meta_id && 6 == meta_type) {
                        if (OB_FAIL(reader.read_i64(chunk_column.dict_page_offset_))) {
                        }
                      } else if (3 == meta_id && 9 == meta_type) {
                        int8_t path_type = 0;
                        int32_t path_cnt = 0;
                        if (OB_FAIL(reader.read_list_header(path_type, path_cnt))) {
                        }
                        for (int32_t p = 0; OB_SUCC(ret) && p < path_cnt; ++p) {
                          ObString path;
                          if (OB_FAIL(reader.read_binary(path))) {
                          }
                        }
                      } else if (OB_FAIL(reader.skip(meta_type))) {
                      }
                    }
                  } else if (OB_FAIL(reader.skip(chunk_sub_type))) {
                  }
                }
                if (OB_SUCC(ret)) {
                  // merge chunk offsets into the matching schema column
                  if (c >= columns.count()) {
                    ret = OB_INVALID_ARGUMENT;
                    LOG_WARN("parquet column chunk does not match schema", K(ret), K(c));
                  } else {
                    FileSqlParquetColumn &column = columns.at(c);
                    column.data_page_offset_ = chunk_column.data_page_offset_;
                    column.dict_page_offset_ = chunk_column.dict_page_offset_;
                    column.num_values_ += chunk_column.num_values_;
                    column.codec_ = chunk_column.codec_;
                  }
                }
              }
            } else if (OB_FAIL(reader.skip(sub_type))) {
            }
          }
        }
      } else if (OB_FAIL(reader.skip(field_type))) {
      }
    }
    UNUSED(alloc);
  }
  return ret;
}

int file_sql_parquet_append_value(ObStringBuffer &buf, FileSqlParquetColumn &column,
                                  FileSqlParquetValues &values, const int64_t idx)
{
  int ret = OB_SUCCESS;
  if (PARQUET_BOOLEAN == column.physical_type_) {
    ret = file_sql_append_str(buf, (0 != values.ints_.at(idx)) ? "true" : "false");
  } else if (PARQUET_INT32 == column.physical_type_ || PARQUET_INT64 == column.physical_type_) {
    const int64_t value = values.ints_.at(idx);
    if (PARQUET_DATE == column.converted_type_ && PARQUET_INT32 == column.physical_type_) {
      char tmp[32];
      const int64_t days = value;
      // days since epoch -> YYYY-MM-DD, computed with the civil date algorithm
      int64_t z = days + 719468;
      const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
      const int64_t doe = z - era * 146097;
      const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
      const int64_t y = yoe + era * 400;
      const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
      const int64_t mp = (5 * doy + 2) / 153;
      const int64_t d = doy - (153 * mp + 2) / 5 + 1;
      const int64_t m = (mp < 10) ? mp + 3 : mp - 9;
      const int64_t year = (m <= 2) ? y + 1 : y;
      const int64_t len = snprintf(tmp, sizeof(tmp), "\"%04ld-%02ld-%02ld\"",
                                   year, m, d);
      ret = file_sql_append(buf, tmp, len);
    } else {
      ret = file_sql_append_int(buf, value);
    }
  } else if (PARQUET_FLOAT == column.physical_type_ || PARQUET_DOUBLE == column.physical_type_) {
    ret = file_sql_append_double(buf, values.reals_.at(idx));
  } else if (PARQUET_BYTE_ARRAY == column.physical_type_) {
    const ObString &str = values.strs_.at(idx);
    ret = file_sql_append_json_string(buf, str.ptr(), str.length());
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int file_sql_read_parquet(ObIAllocator &alloc,
                          const ObString &path,
                          ObFileSqlTable &table)
{
  int ret = OB_SUCCESS;
  ObString content;
  ObSEArray<FileSqlParquetColumn, 8> columns;
  ObSEArray<FileSqlParquetValues, 8> all_values;
  int64_t row_cnt = 0;
  ObStringBuffer doc(&alloc);
  if (OB_FAIL(file_sql_read_file(alloc, path, content))) {
  } else if (OB_FAIL(file_sql_parquet_read_footer(alloc, content, columns, row_cnt))) {
  } else if (columns.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("parquet file has no column", K(ret));
  } else {
    for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
      FileSqlParquetValues values;
      if (OB_FAIL(file_sql_parquet_decode_chunk(alloc, content, columns.at(c), values))) {
      } else if (OB_FAIL(all_values.push_back(values))) {
      }
    }
    for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
      FileSqlParquetColumn &column = columns.at(c);
      ObFileSqlColumn column_info;
      int64_t max_len = 0;
      bool nullable = false;
      for (int64_t i = 0; OB_SUCC(ret) && i < all_values.at(c).nulls_.count(); ++i) {
        if (0 != all_values.at(c).nulls_.at(i)) {
          nullable = true;
        }
      }
      if (PARQUET_BOOLEAN == column.physical_type_) {
        column_info.type_ = ObTinyIntType;
      } else if (PARQUET_INT32 == column.physical_type_ || PARQUET_INT64 == column.physical_type_) {
        column_info.type_ = (PARQUET_DATE == column.converted_type_) ? ObDateType : ObIntType;
      } else if (PARQUET_FLOAT == column.physical_type_ || PARQUET_DOUBLE == column.physical_type_) {
        column_info.type_ = ObDoubleType;
      } else if (PARQUET_BYTE_ARRAY == column.physical_type_) {
        column_info.type_ = ObVarcharType;
        for (int64_t i = 0; i < all_values.at(c).strs_.count(); ++i) {
          max_len = MAX(max_len, static_cast<int64_t>(all_values.at(c).strs_.at(i).length()));
        }
        column_info.length_ = MAX(max_len, static_cast<int64_t>(1));
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet column type", K(ret), K(column.physical_type_));
      }
      column_info.nullable_ = nullable;
      column_info.name_ = column.name_;
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(table.columns_.push_back(column_info))) {
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(file_sql_append_str(doc, "["))) {
    }
    for (int64_t r = 0; OB_SUCC(ret) && r < row_cnt; ++r) {
      if (OB_FAIL(file_sql_append_str(doc, (0 == r) ? "{" : ",{"))) {
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
        char key[32];
        const int64_t key_len = snprintf(key, sizeof(key), "%s\"c%ld\":", (0 == c) ? "" : ",", c);
        if (OB_FAIL(file_sql_append(doc, key, key_len))) {
        } else if (0 != all_values.at(c).nulls_.at(r)) {
          ret = file_sql_append_str(doc, "null");
        } else {
          ret = file_sql_parquet_append_value(doc, columns.at(c), all_values.at(c), r);
        }
      }
      if (OB_SUCC(ret)) {
        ret = file_sql_append_str(doc, "}");
      }
    }
    if (OB_SUCC(ret)) {
      if (OB_FAIL(file_sql_append_str(doc, "]"))) {
      } else if (OB_FAIL(doc.get_result_string(table.json_doc_))) {
      }
    }
  }
  return ret;
}

////////////////////////////////////////////////////////////////
// glob
////////////////////////////////////////////////////////////////

bool file_sql_glob_match(const char *pat, const int64_t pat_len,
                         const char *name, const int64_t name_len)
{
  int64_t p = 0;
  int64_t n = 0;
  int64_t star_p = -1;
  int64_t star_n = 0;
  while (n < name_len) {
    if (p < pat_len && ('?' == pat[p] || pat[p] == name[n])) {
      ++p;
      ++n;
    } else if (p < pat_len && '*' == pat[p]) {
      star_p = p++;
      star_n = n;
    } else if (star_p >= 0) {
      p = star_p + 1;
      n = ++star_n;
    } else {
      return false;
    }
  }
  while (p < pat_len && '*' == pat[p]) {
    ++p;
  }
  return p == pat_len;
}

int file_sql_glob_push_column(ObIAllocator &alloc, ObFileSqlTable &table)
{
  int ret = OB_SUCCESS;
  ObFileSqlColumn column;
  if (OB_FAIL(ob_write_string(alloc, ObString::make_string("file"), column.name_))) {
  } else {
    column.type_ = ObVarcharType;
    column.length_ = FILE_SQL_DEFAULT_VARCHAR_LEN;
    column.nullable_ = false;
    ret = table.columns_.push_back(column);
  }
  return ret;
}

int file_sql_read_glob(ObIAllocator &alloc, const ObString &pattern, ObFileSqlTable &table)
{
  int ret = OB_SUCCESS;
  ObStringBuffer doc(&alloc);
  char *path_buf = NULL;
  char *base_pattern = NULL;
  ObString dir_name;
  int64_t base_len = 0;
  bool found = false;
  if (OB_ISNULL(path_buf = static_cast<char *>(alloc.alloc(pattern.length() + 1)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(path_buf, pattern.ptr(), pattern.length());
    path_buf[pattern.length()] = '\0';
    if (NULL == strpbrk(path_buf, "*?")) {
      // a plain path matches itself
      if (OB_FAIL(file_sql_glob_push_column(alloc, table))) {
      } else if (OB_FAIL(file_sql_append_str(doc, "[{\"c0\":"))) {
      } else if (OB_FAIL(file_sql_append_json_string(doc, path_buf,
                                                     static_cast<int64_t>(strlen(path_buf))))) {
      } else if (OB_FAIL(file_sql_append_str(doc, "}]"))) {
      } else if (OB_FAIL(doc.get_result_string(table.json_doc_))) {
      }
    } else {
      int64_t len = pattern.length();
      while (len > 0 && '/' == path_buf[len - 1]) {
        --len;
      }
      int64_t slash = len;
      while (slash > 0 && '/' != path_buf[slash - 1]) {
        --slash;
      }
      // the part after the last slash selects the names inside the directory
      base_pattern = path_buf + slash;
      base_len = len - slash;
      len = slash > 0 ? slash - 1 : 0;
      path_buf[len] = '\0';
      dir_name.assign_ptr(path_buf, static_cast<int32_t>(len));
      DIR *dir = ::opendir(len > 0 ? path_buf : ".");
      if (NULL == dir) {
        ret = OB_FILE_NOT_EXIST;
        LOG_WARN("failed to open directory", K(ret), K(dir_name));
      } else if (OB_FAIL(file_sql_append_str(doc, "["))) {
        ::closedir(dir);
      } else {
        struct dirent *entry = NULL;
        while (OB_SUCC(ret) && NULL != (entry = ::readdir(dir))) {
          const int64_t name_len = static_cast<int64_t>(strlen(entry->d_name));
          if (0 == strcmp(entry->d_name, ".") || 0 == strcmp(entry->d_name, "..")
              || !file_sql_glob_match(base_pattern, base_len, entry->d_name, name_len)) {
            continue;
          }
          char full_path[4096];
          const int64_t full_len = (len > 0)
              ? snprintf(full_path, sizeof(full_path), "%.*s/%s", static_cast<int32_t>(len),
                         path_buf, entry->d_name)
              : snprintf(full_path, sizeof(full_path), "%s", entry->d_name);
          if (!found && OB_FAIL(file_sql_glob_push_column(alloc, table))) {
          } else if (OB_FAIL(file_sql_append_str(doc, found ? ",{\"c0\":" : "{\"c0\":"))) {
          } else if (OB_FAIL(file_sql_append_json_string(doc, full_path, full_len))) {
          } else if (OB_FAIL(file_sql_append_str(doc, "}"))) {
          } else {
            found = true;
          }
        }
        ::closedir(dir);
        if (OB_FAIL(ret)) {
        } else if (!found) {
          // no matched file, still expose the single file column
          if (OB_FAIL(file_sql_glob_push_column(alloc, table))) {
          }
        }
        if (OB_SUCC(ret)) {
          if (OB_FAIL(file_sql_append_str(doc, "]"))) {
          } else if (OB_FAIL(doc.get_result_string(table.json_doc_))) {
          }
        }
      }
    }
  }
  return ret;
}

} // namespace

bool ObFileSqlReader::is_file_table_function(const ObString &fn_name, ObFileSqlType &type)
{
  bool bret = true;
  if (file_sql_ci_equal(fn_name.ptr(), fn_name.length(), "read_csv", 8)) {
    type = FILE_SQL_CSV;
  } else if (file_sql_ci_equal(fn_name.ptr(), fn_name.length(), "read_json", 9)) {
    type = FILE_SQL_JSON;
  } else if (file_sql_ci_equal(fn_name.ptr(), fn_name.length(), "read_parquet", 12)) {
    type = FILE_SQL_PARQUET;
  } else if (file_sql_ci_equal(fn_name.ptr(), fn_name.length(), "glob", 4)) {
    type = FILE_SQL_GLOB;
  } else {
    bret = false;
  }
  return bret;
}

int ObFileSqlReader::read_table(ObIAllocator &alloc,
                                const ObString &path,
                                const ObFileSqlType type,
                                const ObFileSqlOptions &options,
                                ObFileSqlTable &table)
{
  int ret = OB_SUCCESS;
  if (path.empty()) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("empty file path", K(ret));
  } else if (FILE_SQL_CSV == type) {
    ret = file_sql_read_csv(alloc, path, options, table);
  } else if (FILE_SQL_JSON == type) {
    ret = file_sql_read_json(alloc, path, options, table);
  } else if (FILE_SQL_PARQUET == type) {
    ret = file_sql_read_parquet(alloc, path, table);
  } else if (FILE_SQL_GLOB == type) {
    ret = file_sql_read_glob(alloc, path, table);
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported file table type", K(ret), K(type));
  }
  return ret;
}

} // namespace sql
} // namespace oceanbase

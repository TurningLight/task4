#include "ob_csv_scanner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <limits>
#include <set>

namespace oceanbase {
namespace sql {

namespace {

const size_t OB_CSV_IO_BUFFER_SIZE = 64 * 1024;

inline bool ob_ascii_digit(char c) { return c >= '0' && c <= '9'; }

inline char ob_ascii_lower(char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool ob_iequals(const std::string &text, const char *lower_literal)
{
  const size_t n = ::strlen(lower_literal);
  if (text.size() != n) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (ob_ascii_lower(text[i]) != lower_literal[i]) {
      return false;
    }
  }
  return true;
}

bool ob_is_space(char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c;
}

std::string ob_trim(const std::string &text)
{
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && ob_is_space(text[begin])) {
    ++begin;
  }
  while (end > begin && ob_is_space(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

// Strict base-10 int64 parse. Returns false on empty input, junk, or overflow
// (an overflowing integer literal legitimately falls back to DOUBLE).
bool ob_parse_i64(const std::string &text, int64_t *value)
{
  const size_t n = text.size();
  size_t i = 0;
  bool negative = false;
  if (0 == n) {
    return false;
  }
  if ('+' == text[0] || '-' == text[0]) {
    negative = ('-' == text[0]);
    i = 1;
  }
  if (i >= n) {
    return false;
  }
  const uint64_t positive_limit =
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
  const uint64_t limit = negative ? positive_limit + 1ULL : positive_limit;
  uint64_t magnitude = 0;
  for (; i < n; ++i) {
    if (!ob_ascii_digit(text[i])) {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
    if (magnitude > (limit - digit) / 10ULL) {
      return false;  // would overflow
    }
    magnitude = magnitude * 10ULL + digit;
  }
  if (!negative) {
    *value = static_cast<int64_t>(magnitude);
  } else if (magnitude == positive_limit + 1ULL) {
    *value = std::numeric_limits<int64_t>::min();
  } else {
    *value = -static_cast<int64_t>(magnitude);
  }
  return true;
}

// Accepts [+-]? (digits [. digits?] | . digits) ([eE] [+-]? digits)?
// Deliberately rejects "nan"/"inf"/hex so a stray word never becomes a DOUBLE.
bool ob_looks_like_double(const std::string &text)
{
  const size_t n = text.size();
  if (0 == n) {
    return false;
  }
  size_t i = 0;
  if ('+' == text[i] || '-' == text[i]) {
    ++i;
  }
  size_t int_digits = 0;
  size_t frac_digits = 0;
  while (i < n && ob_ascii_digit(text[i])) {
    ++i;
    ++int_digits;
  }
  if (i < n && '.' == text[i]) {
    ++i;
    while (i < n && ob_ascii_digit(text[i])) {
      ++i;
      ++frac_digits;
    }
  }
  if (0 == int_digits + frac_digits) {
    return false;
  }
  if (i < n && ('e' == text[i] || 'E' == text[i])) {
    ++i;
    if (i < n && ('+' == text[i] || '-' == text[i])) {
      ++i;
    }
    size_t exp_digits = 0;
    while (i < n && ob_ascii_digit(text[i])) {
      ++i;
      ++exp_digits;
    }
    if (0 == exp_digits) {
      return false;
    }
  }
  return i == n;
}

int ob_days_in_month(int year, int month)
{
  static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) {
    return 0;
  }
  if (2 == month) {
    const bool leap = (0 == year % 4 && 0 != year % 100) || 0 == year % 400;
    return leap ? 29 : 28;
  }
  return days[month - 1];
}

int ob_two_digits(const std::string &text, size_t offset)
{
  return (text[offset] - '0') * 10 + (text[offset + 1] - '0');
}

// "YYYY-MM-DD" with a calendar-valid day.
bool ob_is_date_text(const std::string &text)
{
  if (text.size() != 10) {
    return false;
  }
  for (size_t i = 0; i < 4; ++i) {
    if (!ob_ascii_digit(text[i])) {
      return false;
    }
  }
  if ('-' != text[4] || '-' != text[7]) {
    return false;
  }
  for (size_t i = 5; i < 7; ++i) {
    if (!ob_ascii_digit(text[i])) {
      return false;
    }
  }
  for (size_t i = 8; i < 10; ++i) {
    if (!ob_ascii_digit(text[i])) {
      return false;
    }
  }
  int year = 0;
  for (size_t i = 0; i < 4; ++i) {
    year = year * 10 + (text[i] - '0');
  }
  const int month = ob_two_digits(text, 5);
  const int day = ob_two_digits(text, 8);
  if (month < 1 || month > 12 || day < 1) {
    return false;
  }
  return day <= ob_days_in_month(year, month);
}

// "YYYY-MM-DD[ T]HH:MM:SS[.fraction]" - both separators are accepted, which is
// what the pinned fixture requires ("..." vs "...T...").
bool ob_is_datetime_text(const std::string &text)
{
  if (text.size() < 19) {
    return false;
  }
  if (!ob_is_date_text(text.substr(0, 10))) {
    return false;
  }
  if (' ' != text[10] && 'T' != text[10]) {
    return false;
  }
  if (!ob_ascii_digit(text[11]) || !ob_ascii_digit(text[12]) || ':' != text[13]) {
    return false;
  }
  if (!ob_ascii_digit(text[14]) || !ob_ascii_digit(text[15]) || ':' != text[16]) {
    return false;
  }
  if (!ob_ascii_digit(text[17]) || !ob_ascii_digit(text[18])) {
    return false;
  }
  const int hour = ob_two_digits(text, 11);
  const int minute = ob_two_digits(text, 14);
  const int second = ob_two_digits(text, 17);
  if (hour > 23 || minute > 59 || second > 59) {
    return false;
  }
  if (text.size() > 19) {
    if ('.' != text[19] || 20 == text.size()) {
      return false;
    }
    for (size_t i = 20; i < text.size(); ++i) {
      if (!ob_ascii_digit(text[i])) {
        return false;
      }
    }
  }
  return true;
}

// Canonical DATETIME rendering: the 'T' separator is folded to a space so that
// both spellings of the same instant print identically.
std::string ob_normalize_datetime(const std::string &text)
{
  std::string out = text;
  if (out.size() > 10 && 'T' == out[10]) {
    out[10] = ' ';
  }
  return out;
}

// Shortest representation that round-trips, preferring plain decimal notation
// for everyday magnitudes (so 87.0 prints "87", not "8.7e+01").
std::string ob_format_double(double value)
{
  if (value != value) {
    return "nan";
  }
  if (value > std::numeric_limits<double>::max() ||
      value < -std::numeric_limits<double>::max()) {
    return value < 0 ? "-inf" : "inf";
  }
  if (0.0 == value) {
    return "0";
  }
  char buffer[64];
  const double magnitude = ::fabs(value);
  if (magnitude >= 1e15 || magnitude < 1e-4) {
    for (int precision = 1; precision <= 17; ++precision) {
      ::snprintf(buffer, sizeof(buffer), "%.*g", precision, value);
      if (::strtod(buffer, NULL) == value) {
        return std::string(buffer);
      }
    }
    ::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return std::string(buffer);
  }
  for (int precision = 0; precision <= 17; ++precision) {
    ::snprintf(buffer, sizeof(buffer), "%.*f", precision, value);
    if (::strtod(buffer, NULL) == value) {
      return std::string(buffer);
    }
  }
  ::snprintf(buffer, sizeof(buffer), "%.17g", value);
  return std::string(buffer);
}

void ob_set_error(std::string *err, const std::string &message)
{
  if (NULL != err) {
    *err = message;
  }
}

// ---------------------------------------------------------------------------
// Buffered record reader: the only place that touches the file. It keeps a
// fixed 64 KiB window plus the record being assembled, so memory does not grow
// with file size.
// ---------------------------------------------------------------------------
class ObCsvRecordReader {
 public:
  ObCsvRecordReader()
      : buf_pos_(0), buf_len_(0), at_eof_(false), line_(1), record_line_(1) {}

  bool open(const std::string &path, std::string *err)
  {
    buf_.resize(OB_CSV_IO_BUFFER_SIZE);
    in_.open(path.c_str(), std::ios::in | std::ios::binary);
    if (!in_.is_open()) {
      ob_set_error(err, "cannot open file for reading: " + path);
      return false;
    }
    buf_pos_ = 0;
    buf_len_ = 0;
    at_eof_ = false;
    line_ = 1;
    record_line_ = 1;
    if (fill()) {
      // Tolerate a UTF-8 BOM written by spreadsheet exporters.
      if (buf_len_ >= 3 && static_cast<unsigned char>(buf_[0]) == 0xEF &&
          static_cast<unsigned char>(buf_[1]) == 0xBB &&
          static_cast<unsigned char>(buf_[2]) == 0xBF) {
        buf_pos_ = 3;
      }
    }
    return true;
  }

  void close() { in_.close(); }

  uint64_t record_line() const { return record_line_; }

  // 1 => record produced, 0 => clean EOF, -1 => error.
  int next_record(std::vector<std::string> *fields, std::vector<bool> *quoted,
                  std::string *err)
  {
    for (;;) {
      const int result = next_record_impl(fields, quoted, err);
      if (1 != result) {
        return result;
      }
      // Skip blank lines (and the trailing newline of the file).
      if (1 == fields->size() && fields->at(0).empty() && !quoted->at(0)) {
        continue;
      }
      return 1;
    }
  }

 private:
  bool fill()
  {
    if (at_eof_) {
      return false;
    }
    buf_pos_ = 0;
    buf_len_ = 0;
    in_.read(&buf_[0], static_cast<std::streamsize>(buf_.size()));
    const std::streamsize got = in_.gcount();
    if (got <= 0) {
      at_eof_ = true;
      return false;
    }
    buf_len_ = static_cast<size_t>(got);
    return true;
  }

  int peek()
  {
    if (buf_pos_ >= buf_len_ && !fill()) {
      return -1;
    }
    return static_cast<unsigned char>(buf_[buf_pos_]);
  }

  int get()
  {
    const int c = peek();
    if (c >= 0) {
      ++buf_pos_;
    }
    return c;
  }

  int next_record_impl(std::vector<std::string> *fields,
                       std::vector<bool> *quoted, std::string *err)
  {
    fields->clear();
    quoted->clear();
    record_line_ = line_;
    if (peek() < 0) {
      return 0;
    }
    for (;;) {
      std::string field;
      bool field_quoted = false;
      const int lead = peek();
      if ('"' == lead) {
        field_quoted = true;
        get();
        for (;;) {
          const int c = get();
          if (c < 0) {
            ob_set_error(err, "unterminated quoted field starting on line " +
                                   std::to_string(record_line_));
            return -1;
          }
          if ('"' == c) {
            if ('"' == peek()) {
              get();
              field.push_back('"');  // "" -> "
            } else {
              break;  // closing quote
            }
          } else {
            if ('\n' == c) {
              ++line_;  // embedded newline inside a quoted field
            }
            field.push_back(static_cast<char>(c));
          }
        }
      } else {
        for (;;) {
          const int c = peek();
          if (c < 0 || ',' == c || '\n' == c || '\r' == c) {
            break;
          }
          get();
          field.push_back(static_cast<char>(c));
        }
      }

      const int terminator = peek();
      if (terminator < 0) {
        fields->push_back(field);
        quoted->push_back(field_quoted);
        return 1;
      }
      if (',' == terminator) {
        get();
        fields->push_back(field);
        quoted->push_back(field_quoted);
        continue;
      }
      if ('\r' == terminator) {
        get();
        if ('\n' == peek()) {
          get();
        }
        ++line_;
        fields->push_back(field);
        quoted->push_back(field_quoted);
        return 1;
      }
      if ('\n' == terminator) {
        get();
        ++line_;
        fields->push_back(field);
        quoted->push_back(field_quoted);
        return 1;
      }
      ob_set_error(err, std::string("unexpected character '") +
                            static_cast<char>(terminator) +
                            "' after a closing quote on line " +
                            std::to_string(line_));
      return -1;
    }
  }

  std::ifstream in_;
  std::vector<char> buf_;
  size_t buf_pos_;
  size_t buf_len_;
  bool at_eof_;
  uint64_t line_;
  uint64_t record_line_;
};

// One cell -> its narrowest candidate type. Quoted cells are strings by the
// writer's intent, so "2026-08-01" in quotes stays VARCHAR.
ObCsvType ob_classify_cell(const std::string &text, bool quoted)
{
  if (text.empty()) {
    return quoted ? OB_CSV_TYPE_VARCHAR : OB_CSV_TYPE_SQL_NULL;
  }
  if (quoted) {
    return OB_CSV_TYPE_VARCHAR;
  }
  int64_t ignored = 0;
  if (ob_parse_i64(text, &ignored)) {
    return OB_CSV_TYPE_BIGINT;
  }
  if (ob_looks_like_double(text)) {
    return OB_CSV_TYPE_DOUBLE;
  }
  if (ob_iequals(text, "true") || ob_iequals(text, "false")) {
    return OB_CSV_TYPE_BOOLEAN;
  }
  if (ob_is_date_text(text)) {
    return OB_CSV_TYPE_DATE;
  }
  if (ob_is_datetime_text(text)) {
    return OB_CSV_TYPE_DATETIME;
  }
  return OB_CSV_TYPE_VARCHAR;
}

// Order-insensitive widening lattice.
ObCsvType ob_merge_types(ObCsvType left, ObCsvType right)
{
  if (left == right) {
    return left;
  }
  if (OB_CSV_TYPE_SQL_NULL == left) {
    return right;
  }
  if (OB_CSV_TYPE_SQL_NULL == right) {
    return left;
  }
  if ((OB_CSV_TYPE_BIGINT == left && OB_CSV_TYPE_DOUBLE == right) ||
      (OB_CSV_TYPE_DOUBLE == left && OB_CSV_TYPE_BIGINT == right)) {
    return OB_CSV_TYPE_DOUBLE;
  }
  if ((OB_CSV_TYPE_DATE == left && OB_CSV_TYPE_DATETIME == right) ||
      (OB_CSV_TYPE_DATETIME == left && OB_CSV_TYPE_DATE == right)) {
    return OB_CSV_TYPE_DATETIME;
  }
  return OB_CSV_TYPE_VARCHAR;
}

std::string ob_make_unique_name(const std::string &base,
                                std::set<std::string> *used)
{
  std::string candidate = base;
  int suffix = 2;
  while (used->count(candidate) > 0) {
    candidate = base + "__" + std::to_string(suffix);
    ++suffix;
  }
  used->insert(candidate);
  return candidate;
}

void ob_build_columns(const std::vector<std::string> &names,
                      std::vector<ObCsvColumnSchema> *columns)
{
  columns->clear();
  std::set<std::string> used;
  for (size_t i = 0; i < names.size(); ++i) {
    std::string base = ob_trim(names[i]);
    if (base.empty()) {
      base = "column" + std::to_string(i);
    }
    ObCsvColumnSchema column;
    column.name = ob_make_unique_name(base, &used);
    column.type = OB_CSV_TYPE_SQL_NULL;
    column.nullable = false;  // flipped on the first NULL seen
    columns->push_back(column);
  }
}

void ob_build_generated_columns(size_t count,
                                std::vector<ObCsvColumnSchema> *columns)
{
  columns->clear();
  for (size_t i = 0; i < count; ++i) {
    ObCsvColumnSchema column;
    column.name = "column" + std::to_string(i);
    column.type = OB_CSV_TYPE_SQL_NULL;
    column.nullable = false;
    columns->push_back(column);
  }
}

bool ob_all_varchar(const std::vector<std::string> &fields,
                    const std::vector<bool> &quoted)
{
  for (size_t i = 0; i < fields.size(); ++i) {
    if (OB_CSV_TYPE_VARCHAR != ob_classify_cell(fields[i], quoted[i])) {
      return false;
    }
  }
  return true;
}

bool ob_convert_cell(const std::string &text, bool quoted, ObCsvType type,
                     ObCsvValue *value, std::string *error)
{
  value->type = type;
  value->is_null = false;
  value->int_value = 0;
  value->double_value = 0.0;
  value->bool_value = false;
  value->string_value.clear();

  if (OB_CSV_TYPE_SQL_NULL == type) {
    if (!text.empty() || quoted) {
      ob_set_error(error, "non-NULL value \"" + text + "\" in a NULL column");
      return false;
    }
    value->is_null = true;
    return true;
  }
  if (text.empty() && !quoted) {
    value->is_null = true;  // empty unquoted field
    return true;
  }
  switch (type) {
    case OB_CSV_TYPE_BIGINT:
      if (!ob_parse_i64(text, &value->int_value)) {
        ob_set_error(error, "cannot convert \"" + text + "\" to BIGINT");
        return false;
      }
      return true;
    case OB_CSV_TYPE_DOUBLE: {
      if (!ob_looks_like_double(text)) {
        ob_set_error(error, "cannot convert \"" + text + "\" to DOUBLE");
        return false;
      }
      char *end = NULL;
      const double parsed = ::strtod(text.c_str(), &end);
      if (NULL == end || end != text.c_str() + text.size()) {
        ob_set_error(error, "cannot convert \"" + text + "\" to DOUBLE");
        return false;
      }
      value->double_value = parsed;
      return true;
    }
    case OB_CSV_TYPE_BOOLEAN:
      if (ob_iequals(text, "true")) {
        value->bool_value = true;
      } else if (ob_iequals(text, "false")) {
        value->bool_value = false;
      } else {
        ob_set_error(error, "cannot convert \"" + text + "\" to BOOLEAN");
        return false;
      }
      return true;
    case OB_CSV_TYPE_DATE:
      if (!ob_is_date_text(text)) {
        ob_set_error(error, "cannot convert \"" + text + "\" to DATE");
        return false;
      }
      value->string_value = text;
      return true;
    case OB_CSV_TYPE_DATETIME:
      if (ob_is_datetime_text(text)) {
        value->string_value = ob_normalize_datetime(text);
      } else if (ob_is_date_text(text)) {
        value->string_value = text + " 00:00:00";  // a DATE in a DATETIME column
      } else {
        ob_set_error(error, "cannot convert \"" + text + "\" to DATETIME");
        return false;
      }
      return true;
    case OB_CSV_TYPE_VARCHAR:
      value->string_value = text;
      return true;
    default:
      ob_set_error(error, "unexpected inferred column type");
      return false;
  }
}

}  // namespace

const char *ob_csv_type_name(ObCsvType type)
{
  switch (type) {
    case OB_CSV_TYPE_BIGINT:
      return "BIGINT";
    case OB_CSV_TYPE_DOUBLE:
      return "DOUBLE";
    case OB_CSV_TYPE_BOOLEAN:
      return "BOOLEAN";
    case OB_CSV_TYPE_VARCHAR:
      return "VARCHAR";
    case OB_CSV_TYPE_DATE:
      return "DATE";
    case OB_CSV_TYPE_DATETIME:
      return "DATETIME";
    case OB_CSV_TYPE_SQL_NULL:
    default:
      return "NULL";
  }
}

std::string ob_csv_value_to_string(const ObCsvValue &value)
{
  if (value.is_null || OB_CSV_TYPE_SQL_NULL == value.type) {
    return "NULL";
  }
  switch (value.type) {
    case OB_CSV_TYPE_BIGINT:
      return std::to_string(value.int_value);
    case OB_CSV_TYPE_DOUBLE:
      return ob_format_double(value.double_value);
    case OB_CSV_TYPE_BOOLEAN:
      return value.bool_value ? "true" : "false";
    case OB_CSV_TYPE_VARCHAR:
    case OB_CSV_TYPE_DATE:
    case OB_CSV_TYPE_DATETIME:
      return value.string_value;
    default:
      return "NULL";
  }
}

struct ObCsvScanner::Impl {
  std::string path;
  ObCsvHeaderMode header_mode;
  int64_t sample_size;
  bool opened;
  bool has_header;
  bool schema_ready;
  bool eof;
  int64_t inferred_rows;
  std::vector<ObCsvColumnSchema> columns;
  std::unique_ptr<ObCsvRecordReader> reader;

  Impl()
      : header_mode(OB_CSV_HEADER_PRESENT),
        sample_size(-1),
        opened(false),
        has_header(false),
        schema_ready(false),
        eof(false),
        inferred_rows(0) {}
};

ObCsvScanner::ObCsvScanner() : impl_(new Impl()) {}

ObCsvScanner::~ObCsvScanner() {}

void ObCsvScanner::set_header_mode(ObCsvHeaderMode mode)
{
  impl_->header_mode = mode;
}

ObCsvScanner::ObCsvHeaderMode ObCsvScanner::header_mode() const
{
  return impl_->header_mode;
}

void ObCsvScanner::set_header(bool has_header)
{
  impl_->header_mode = has_header ? OB_CSV_HEADER_PRESENT : OB_CSV_HEADER_ABSENT;
}

void ObCsvScanner::set_sample_size(int64_t sample_size)
{
  impl_->sample_size = sample_size;
}

int64_t ObCsvScanner::sample_size() const { return impl_->sample_size; }

bool ObCsvScanner::open(const std::string &path, std::string *err)
{
  std::string sink;
  std::string *error = (NULL == err) ? &sink : err;
  error->clear();
  ObCsvRecordReader probe;
  if (!probe.open(path, error)) {
    return false;
  }
  probe.close();
  impl_->path = path;
  impl_->opened = true;
  impl_->schema_ready = false;
  impl_->eof = false;
  impl_->inferred_rows = 0;
  impl_->columns.clear();
  return true;
}

bool ObCsvScanner::detect_header(bool *has_header, std::string *err)
{
  std::string sink;
  std::string *error = (NULL == err) ? &sink : err;
  error->clear();
  if (NULL == has_header) {
    ob_set_error(error, "null has_header output");
    return false;
  }
  *has_header = false;
  if (!impl_->opened) {
    ob_set_error(error, "open() must be called before detect_header()");
    return false;
  }
  ObCsvRecordReader reader;
  if (!reader.open(impl_->path, error)) {
    return false;
  }
  std::vector<std::string> first;
  std::vector<bool> first_quoted;
  const int first_rc = reader.next_record(&first, &first_quoted, error);
  if (first_rc < 0) {
    return false;
  }
  if (0 == first_rc) {
    *has_header = false;  // empty file: nothing to sniff
    return true;
  }
  std::vector<std::string> second;
  std::vector<bool> second_quoted;
  const int second_rc = reader.next_record(&second, &second_quoted, error);
  if (second_rc < 0) {
    return false;
  }
  if (0 == second_rc) {
    *has_header = false;  // a single record cannot prove a header
    return true;
  }
  // A header is plausible when row 1 is entirely textual and row 2 is not.
  *has_header = ob_all_varchar(first, first_quoted) &&
                !ob_all_varchar(second, second_quoted);
  return true;
}

bool ObCsvScanner::infer_schema(std::string *err)
{
  std::string sink;
  std::string *error = (NULL == err) ? &sink : err;
  error->clear();
  Impl *impl = impl_.get();
  if (!impl->opened) {
    ob_set_error(error, "open() must be called before infer_schema()");
    return false;
  }
  if (0 == impl->sample_size) {
    ob_set_error(error,
                 "sample_size = 0 is not supported; use -1 (all rows) or N > 0");
    return false;
  }
  bool has_header = false;
  if (OB_CSV_HEADER_PRESENT == impl->header_mode) {
    has_header = true;
  } else if (OB_CSV_HEADER_ABSENT == impl->header_mode) {
    has_header = false;
  } else if (!detect_header(&has_header, error)) {
    return false;
  }

  ObCsvRecordReader reader;
  if (!reader.open(impl->path, error)) {
    return false;
  }

  std::vector<std::string> fields;
  std::vector<bool> quoted;
  std::vector<ObCsvColumnSchema> columns;
  if (has_header) {
    const int rc = reader.next_record(&fields, &quoted, error);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      ob_set_error(error, "file is empty: no header row");
      return false;
    }
    ob_build_columns(fields, &columns);
  }

  int64_t rows = 0;
  while (impl->sample_size < 0 || rows < impl->sample_size) {
    const int rc = reader.next_record(&fields, &quoted, error);
    if (rc < 0) {
      return false;
    }
    if (0 == rc) {
      break;
    }
    if (columns.empty()) {
      ob_build_generated_columns(fields.size(), &columns);  // headerless file
    }
    if (fields.size() != columns.size()) {
      ob_set_error(error, "row on line " + std::to_string(reader.record_line()) +
                              " has " + std::to_string(fields.size()) +
                              " fields but the file has " +
                              std::to_string(columns.size()) + " columns");
      return false;
    }
    for (size_t i = 0; i < fields.size(); ++i) {
      const ObCsvType cell = ob_classify_cell(fields[i], quoted[i]);
      columns[i].type = ob_merge_types(columns[i].type, cell);
      if (OB_CSV_TYPE_SQL_NULL == cell) {
        columns[i].nullable = true;
      }
    }
    ++rows;
  }
  for (size_t i = 0; i < columns.size(); ++i) {
    if (OB_CSV_TYPE_SQL_NULL == columns[i].type) {
      columns[i].nullable = true;  // an all-NULL column is by definition nullable
    }
  }

  impl->columns.swap(columns);
  impl->has_header = has_header;
  impl->inferred_rows = rows;
  impl->schema_ready = true;
  impl->eof = false;
  impl->reader.reset(new ObCsvRecordReader());
  if (!impl->reader->open(impl->path, error)) {
    impl->schema_ready = false;
    return false;
  }
  if (has_header) {
    const int rc = impl->reader->next_record(&fields, &quoted, error);
    if (rc <= 0) {
      if (0 == rc) {
        ob_set_error(error, "unexpected end of file while skipping the header");
      }
      impl->schema_ready = false;
      return false;
    }
  }
  return true;
}

const std::vector<ObCsvColumnSchema> &ObCsvScanner::schema() const
{
  return impl_->columns;
}

bool ObCsvScanner::has_header() const { return impl_->has_header; }

bool ObCsvScanner::next_row(std::vector<ObCsvValue> *row, std::string *err)
{
  std::string sink;
  std::string *error = (NULL == err) ? &sink : err;
  error->clear();
  Impl *impl = impl_.get();
  if (!impl->schema_ready) {
    ob_set_error(error, "infer_schema() must be called before next_row()");
    return false;
  }
  if (NULL == row) {
    ob_set_error(error, "null row output");
    return false;
  }
  if (impl->eof) {
    return false;
  }
  std::vector<std::string> fields;
  std::vector<bool> quoted;
  const int rc = impl->reader->next_record(&fields, &quoted, error);
  if (rc < 0) {
    return false;
  }
  if (0 == rc) {
    impl->eof = true;
    return false;
  }
  if (fields.size() != impl->columns.size()) {
    ob_set_error(error, "row on line " +
                            std::to_string(impl->reader->record_line()) + " has " +
                            std::to_string(fields.size()) +
                            " fields but the schema has " +
                            std::to_string(impl->columns.size()) + " columns");
    return false;
  }
  row->clear();
  row->resize(impl->columns.size());
  for (size_t i = 0; i < fields.size(); ++i) {
    std::string conversion_error;
    if (!ob_convert_cell(fields[i], quoted[i], impl->columns[i].type, &(*row)[i],
                         &conversion_error)) {
      ob_set_error(error, "line " + std::to_string(impl->reader->record_line()) +
                              ", column " + std::to_string(i + 1) + " (" +
                              impl->columns[i].name + "): " + conversion_error);
      return false;
    }
  }
  return true;
}

bool ObCsvScanner::eof() const { return impl_->eof; }

int64_t ObCsvScanner::inferred_row_count() const { return impl_->inferred_rows; }

void ObCsvScanner::close()
{
  if (impl_->reader) {
    impl_->reader->close();
    impl_->reader.reset();
  }
  impl_->opened = false;
  impl_->schema_ready = false;
  impl_->eof = false;
}

}  // namespace sql
}  // namespace oceanbase

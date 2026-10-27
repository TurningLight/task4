#include "ob_jsonl_scanner.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace oceanbase {
namespace sql {

namespace {

void ob_set_error(std::string *err, const std::string &message)
{
  if (NULL != err) {
    *err = message;
  }
}

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
    if (text[i] < '0' || text[i] > '9') {
      return false;
    }
    const uint64_t digit = static_cast<uint64_t>(text[i] - '0');
    if (magnitude > (limit - digit) / 10ULL) {
      return false;
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

// Shortest round-trip rendering, plain decimal for everyday magnitudes.
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

// ---------------------------------------------------------------------------
// Minimal self-contained JSON value parser.
//
// Nodes live in a flat pool and reference children by index, so the recursive
// type problem is avoided (C++11 has no recursive containers guarantee) and
// deep documents cannot corrupt already-built nodes.
// ---------------------------------------------------------------------------
enum ObJsonNodeKind {
  OB_JSON_NODE_NULL = 0,
  OB_JSON_NODE_BOOL = 1,
  OB_JSON_NODE_NUMBER = 2,
  OB_JSON_NODE_STRING = 3,
  OB_JSON_NODE_ARRAY = 4,
  OB_JSON_NODE_OBJECT = 5
};

struct ObJsonNode {
  ObJsonNodeKind kind;
  bool bool_value;
  bool integral;  // NUMBER token carries no '.' and no exponent
  std::string text;  // STRING payload (unescaped) or the raw NUMBER token
  std::vector<std::string> keys;  // OBJECT member names, in document order
  std::vector<size_t> children;   // ARRAY elements / OBJECT values

  ObJsonNode() : kind(OB_JSON_NODE_NULL), bool_value(false), integral(false) {}
};

struct ObJsonDocument {
  std::vector<ObJsonNode> nodes;
  size_t root;

  ObJsonDocument() : root(0) {}
};

class ObJsonParser {
 public:
  static const int OB_JSON_MAX_DEPTH = 64;

  ObJsonParser(const std::string &text, ObJsonDocument *doc)
      : text_(text), doc_(doc), pos_(0) {}

  bool parse_document(std::string *err)
  {
    doc_->nodes.clear();
    doc_->root = 0;
    pos_ = 0;
    skip_whitespace();
    if (pos_ >= text_.size()) {
      return fail(err, "empty JSON document");
    }
    size_t root = 0;
    if (!parse_value(&root, 0, err)) {
      return false;
    }
    skip_whitespace();
    if (pos_ != text_.size()) {
      return fail(err, "trailing data after the top-level value");
    }
    doc_->root = root;
    return true;
  }

 private:
  bool fail(std::string *err, const std::string &message)
  {
    if (NULL != err) {
      *err = "offset " + std::to_string(pos_) + ": " + message;
    }
    return false;
  }

  size_t add_node(ObJsonNodeKind kind)
  {
    doc_->nodes.push_back(ObJsonNode());
    doc_->nodes.back().kind = kind;
    return doc_->nodes.size() - 1;
  }

  void skip_whitespace()
  {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (' ' == c || '\t' == c || '\n' == c || '\r' == c) {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool parse_value(size_t *out_index, int depth, std::string *err)
  {
    if (depth > OB_JSON_MAX_DEPTH) {
      return fail(err, "document nested too deeply");
    }
    skip_whitespace();
    if (pos_ >= text_.size()) {
      return fail(err, "unexpected end of input");
    }
    const char c = text_[pos_];
    if ('{' == c) {
      return parse_object(out_index, depth, err);
    }
    if ('[' == c) {
      return parse_array(out_index, depth, err);
    }
    if ('"' == c) {
      std::string text;
      if (!parse_string(&text, err)) {
        return false;
      }
      const size_t index = add_node(OB_JSON_NODE_STRING);
      doc_->nodes[index].text = text;
      *out_index = index;
      return true;
    }
    if ('t' == c) {
      if (!parse_literal("true", err)) {
        return false;
      }
      const size_t index = add_node(OB_JSON_NODE_BOOL);
      doc_->nodes[index].bool_value = true;
      *out_index = index;
      return true;
    }
    if ('f' == c) {
      if (!parse_literal("false", err)) {
        return false;
      }
      const size_t index = add_node(OB_JSON_NODE_BOOL);
      doc_->nodes[index].bool_value = false;
      *out_index = index;
      return true;
    }
    if ('n' == c) {
      if (!parse_literal("null", err)) {
        return false;
      }
      *out_index = add_node(OB_JSON_NODE_NULL);
      return true;
    }
    return parse_number(out_index, err);
  }

  bool parse_literal(const char *literal, std::string *err)
  {
    const size_t n = ::strlen(literal);
    if (text_.compare(pos_, n, literal) != 0) {
      return fail(err, std::string("invalid literal, expected \"") + literal + "\"");
    }
    pos_ += n;
    return true;
  }

  bool parse_object(size_t *out_index, int depth, std::string *err)
  {
    ++pos_;  // '{'
    const size_t index = add_node(OB_JSON_NODE_OBJECT);
    std::vector<std::string> keys;
    std::vector<size_t> children;
    skip_whitespace();
    if (pos_ < text_.size() && '}' == text_[pos_]) {
      ++pos_;
      doc_->nodes[index].keys.swap(keys);
      doc_->nodes[index].children.swap(children);
      *out_index = index;
      return true;
    }
    for (;;) {
      skip_whitespace();
      if (pos_ >= text_.size() || '"' != text_[pos_]) {
        return fail(err, "expected a string key in an object");
      }
      std::string key;
      if (!parse_string(&key, err)) {
        return false;
      }
      skip_whitespace();
      if (pos_ >= text_.size() || ':' != text_[pos_]) {
        return fail(err, "expected ':' after an object key");
      }
      ++pos_;
      size_t child = 0;
      if (!parse_value(&child, depth + 1, err)) {
        return false;
      }
      keys.push_back(key);
      children.push_back(child);
      skip_whitespace();
      if (pos_ >= text_.size()) {
        return fail(err, "unexpected end of input inside an object");
      }
      if (',' == text_[pos_]) {
        ++pos_;
        continue;
      }
      if ('}' == text_[pos_]) {
        ++pos_;
        break;
      }
      return fail(err, "expected ',' or '}' in an object");
    }
    doc_->nodes[index].keys.swap(keys);
    doc_->nodes[index].children.swap(children);
    *out_index = index;
    return true;
  }

  bool parse_array(size_t *out_index, int depth, std::string *err)
  {
    ++pos_;  // '['
    const size_t index = add_node(OB_JSON_NODE_ARRAY);
    std::vector<size_t> children;
    skip_whitespace();
    if (pos_ < text_.size() && ']' == text_[pos_]) {
      ++pos_;
      doc_->nodes[index].children.swap(children);
      *out_index = index;
      return true;
    }
    for (;;) {
      size_t child = 0;
      if (!parse_value(&child, depth + 1, err)) {
        return false;
      }
      children.push_back(child);
      skip_whitespace();
      if (pos_ >= text_.size()) {
        return fail(err, "unexpected end of input inside an array");
      }
      if (',' == text_[pos_]) {
        ++pos_;
        continue;
      }
      if (']' == text_[pos_]) {
        ++pos_;
        break;
      }
      return fail(err, "expected ',' or ']' in an array");
    }
    doc_->nodes[index].children.swap(children);
    *out_index = index;
    return true;
  }

  bool parse_string(std::string *out, std::string *err)
  {
    out->clear();
    if (pos_ >= text_.size() || '"' != text_[pos_]) {
      return fail(err, "expected a string");
    }
    ++pos_;
    for (;;) {
      if (pos_ >= text_.size()) {
        return fail(err, "unterminated string");
      }
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if ('"' == c) {
        ++pos_;
        return true;
      }
      if ('\\' == c) {
        ++pos_;
        if (pos_ >= text_.size()) {
          return fail(err, "unterminated escape sequence");
        }
        const char esc = text_[pos_];
        ++pos_;
        switch (esc) {
          case '"':
            out->push_back('"');
            break;
          case '\\':
            out->push_back('\\');
            break;
          case '/':
            out->push_back('/');
            break;
          case 'b':
            out->push_back('\b');
            break;
          case 'f':
            out->push_back('\f');
            break;
          case 'n':
            out->push_back('\n');
            break;
          case 'r':
            out->push_back('\r');
            break;
          case 't':
            out->push_back('\t');
            break;
          case 'u': {
            uint32_t code_point = 0;
            if (!parse_hex4(&code_point, err)) {
              return false;
            }
            if (code_point >= 0xD800 && code_point <= 0xDBFF) {
              if (pos_ + 1 >= text_.size() || '\\' != text_[pos_] ||
                  'u' != text_[pos_ + 1]) {
                return fail(err, "unpaired high surrogate in a \\u escape");
              }
              pos_ += 2;
              uint32_t low = 0;
              if (!parse_hex4(&low, err)) {
                return false;
              }
              if (low < 0xDC00 || low > 0xDFFF) {
                return fail(err, "invalid low surrogate in a \\u escape");
              }
              code_point =
                  0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
            } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
              return fail(err, "unpaired low surrogate in a \\u escape");
            }
            append_utf8(code_point, out);
            break;
          }
          default:
            return fail(err, std::string("invalid escape '\\") + esc + "'");
        }
        continue;
      }
      if (c < 0x20) {
        return fail(err, "raw control character inside a string");
      }
      out->push_back(static_cast<char>(c));
      ++pos_;
    }
  }

  bool parse_hex4(uint32_t *value, std::string *err)
  {
    if (pos_ + 4 > text_.size()) {
      return fail(err, "truncated \\u escape");
    }
    uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + i];
      int digit = -1;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return fail(err, "invalid hex digit in a \\u escape");
      }
      result = (result << 4) | static_cast<uint32_t>(digit);
    }
    pos_ += 4;
    *value = result;
    return true;
  }

  static void append_utf8(uint32_t code_point, std::string *out)
  {
    if (code_point <= 0x7F) {
      out->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (code_point >> 6)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else if (code_point <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (code_point >> 12)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (code_point >> 18)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
    }
  }

  bool parse_number(size_t *out_index, std::string *err)
  {
    const size_t start = pos_;
    if (pos_ < text_.size() && '-' == text_[pos_]) {
      ++pos_;
    }
    if (pos_ >= text_.size()) {
      return fail(err, "truncated number");
    }
    if ('0' == text_[pos_]) {
      ++pos_;
      if (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        return fail(err, "leading zero in a number");
      }
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      return fail(err, "invalid number");
    }
    bool integral = true;
    if (pos_ < text_.size() && '.' == text_[pos_]) {
      integral = false;
      ++pos_;
      const size_t fraction_start = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
      if (pos_ == fraction_start) {
        return fail(err, "missing fraction digits");
      }
    }
    if (pos_ < text_.size() && ('e' == text_[pos_] || 'E' == text_[pos_])) {
      integral = false;
      ++pos_;
      if (pos_ < text_.size() && ('+' == text_[pos_] || '-' == text_[pos_])) {
        ++pos_;
      }
      const size_t exponent_start = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
      if (pos_ == exponent_start) {
        return fail(err, "missing exponent digits");
      }
    }
    const size_t index = add_node(OB_JSON_NODE_NUMBER);
    doc_->nodes[index].text = text_.substr(start, pos_ - start);
    doc_->nodes[index].integral = integral;
    *out_index = index;
    return true;
  }

  const std::string &text_;
  ObJsonDocument *doc_;
  size_t pos_;
};

ObJsonlType ob_classify_node(const ObJsonNode &node)
{
  switch (node.kind) {
    case OB_JSON_NODE_NULL:
      return OB_JSONL_TYPE_SQL_NULL;
    case OB_JSON_NODE_BOOL:
      return OB_JSONL_TYPE_BOOLEAN;
    case OB_JSON_NODE_STRING:
      return OB_JSONL_TYPE_VARCHAR;  // date-looking strings stay VARCHAR
    case OB_JSON_NODE_NUMBER: {
      int64_t ignored = 0;
      if (node.integral && ob_parse_i64(node.text, &ignored)) {
        return OB_JSONL_TYPE_BIGINT;
      }
      return OB_JSONL_TYPE_DOUBLE;
    }
    default:
      return OB_JSONL_TYPE_VARCHAR;  // arrays/objects are rejected by the caller
  }
}

ObJsonlType ob_merge_types(ObJsonlType left, ObJsonlType right)
{
  if (left == right) {
    return left;
  }
  if (OB_JSONL_TYPE_SQL_NULL == left) {
    return right;
  }
  if (OB_JSONL_TYPE_SQL_NULL == right) {
    return left;
  }
  if ((OB_JSONL_TYPE_BIGINT == left && OB_JSONL_TYPE_DOUBLE == right) ||
      (OB_JSONL_TYPE_DOUBLE == left && OB_JSONL_TYPE_BIGINT == right)) {
    return OB_JSONL_TYPE_DOUBLE;
  }
  return OB_JSONL_TYPE_VARCHAR;
}

// Text form of a scalar, used when a column widened to VARCHAR.
std::string ob_scalar_text(const ObJsonNode &node)
{
  switch (node.kind) {
    case OB_JSON_NODE_STRING:
      return node.text;
    case OB_JSON_NODE_NUMBER:
      return node.text;
    case OB_JSON_NODE_BOOL:
      return node.bool_value ? "true" : "false";
    default:
      return std::string();
  }
}

bool ob_json_node_to_value(const ObJsonNode &node, ObJsonlType type,
                           ObJsonlValue *value, std::string *error)
{
  value->type = type;
  value->is_null = false;
  value->int_value = 0;
  value->double_value = 0.0;
  value->bool_value = false;
  value->string_value.clear();

  if (OB_JSONL_TYPE_SQL_NULL == type) {
    if (OB_JSON_NODE_NULL != node.kind) {
      ob_set_error(error, "non-null JSON value in a column inferred as NULL");
      return false;
    }
    value->is_null = true;
    return true;
  }
  if (OB_JSON_NODE_NULL == node.kind) {
    value->is_null = true;
    return true;
  }
  switch (type) {
    case OB_JSONL_TYPE_BIGINT:
      if (OB_JSON_NODE_NUMBER != node.kind ||
          !ob_parse_i64(node.text, &value->int_value)) {
        ob_set_error(error, "cannot convert \"" + ob_scalar_text(node) +
                                "\" to BIGINT");
        return false;
      }
      return true;
    case OB_JSONL_TYPE_DOUBLE: {
      if (OB_JSON_NODE_NUMBER != node.kind) {
        ob_set_error(error, "cannot convert \"" + ob_scalar_text(node) +
                                "\" to DOUBLE");
        return false;
      }
      char *end = NULL;
      const double parsed = ::strtod(node.text.c_str(), &end);
      if (NULL == end || end != node.text.c_str() + node.text.size()) {
        ob_set_error(error, "cannot convert \"" + node.text + "\" to DOUBLE");
        return false;
      }
      value->double_value = parsed;
      return true;
    }
    case OB_JSONL_TYPE_BOOLEAN:
      if (OB_JSON_NODE_BOOL != node.kind) {
        ob_set_error(error, "cannot convert \"" + ob_scalar_text(node) +
                                "\" to BOOLEAN");
        return false;
      }
      value->bool_value = node.bool_value;
      return true;
    case OB_JSONL_TYPE_VARCHAR:
      value->string_value = ob_scalar_text(node);
      return true;
    default:
      ob_set_error(error, "unexpected inferred column type");
      return false;
  }
}

bool ob_check_flat_object(const ObJsonDocument &doc, uint64_t line_no,
                          std::string *error)
{
  const ObJsonNode &root = doc.nodes[doc.root];
  if (OB_JSON_NODE_OBJECT != root.kind) {
    ob_set_error(error, "line " + std::to_string(line_no) +
                            ": the top-level JSON value must be an object");
    return false;
  }
  for (size_t i = 0; i < root.children.size(); ++i) {
    const ObJsonNodeKind kind = doc.nodes[root.children[i]].kind;
    if (OB_JSON_NODE_OBJECT == kind || OB_JSON_NODE_ARRAY == kind) {
      ob_set_error(error, "line " + std::to_string(line_no) + ": field \"" +
                              root.keys[i] +
                              "\" is a nested object/array; only flat objects "
                              "are supported");
      return false;
    }
  }
  return true;
}

// Reads the next non-blank line. Returns false only on an I/O error; at EOF it
// returns true with *have == false.
bool ob_read_record_line(std::ifstream *in, std::string *line, uint64_t *line_no,
                         bool *have)
{
  *have = false;
  std::string raw;
  while (std::getline(*in, raw)) {
    ++(*line_no);
    if (!raw.empty() && '\r' == raw[raw.size() - 1]) {
      raw.erase(raw.size() - 1);
    }
    if (1 == *line_no && raw.size() >= 3 &&
        static_cast<unsigned char>(raw[0]) == 0xEF &&
        static_cast<unsigned char>(raw[1]) == 0xBB &&
        static_cast<unsigned char>(raw[2]) == 0xBF) {
      raw.erase(0, 3);  // UTF-8 BOM
    }
    bool blank = true;
    for (size_t i = 0; i < raw.size(); ++i) {
      const char c = raw[i];
      if (' ' != c && '\t' != c && '\r' != c) {
        blank = false;
        break;
      }
    }
    if (blank) {
      continue;
    }
    *line = raw;
    *have = true;
    return true;
  }
  return !in->bad();
}

}  // namespace

const char *ob_jsonl_type_name(ObJsonlType type)
{
  switch (type) {
    case OB_JSONL_TYPE_BIGINT:
      return "BIGINT";
    case OB_JSONL_TYPE_DOUBLE:
      return "DOUBLE";
    case OB_JSONL_TYPE_BOOLEAN:
      return "BOOLEAN";
    case OB_JSONL_TYPE_VARCHAR:
      return "VARCHAR";
    case OB_JSONL_TYPE_SQL_NULL:
    default:
      return "NULL";
  }
}

std::string ob_jsonl_value_to_string(const ObJsonlValue &value)
{
  if (value.is_null || OB_JSONL_TYPE_SQL_NULL == value.type) {
    return "NULL";
  }
  switch (value.type) {
    case OB_JSONL_TYPE_BIGINT:
      return std::to_string(value.int_value);
    case OB_JSONL_TYPE_DOUBLE:
      return ob_format_double(value.double_value);
    case OB_JSONL_TYPE_BOOLEAN:
      return value.bool_value ? "true" : "false";
    case OB_JSONL_TYPE_VARCHAR:
      return value.string_value;
    default:
      return "NULL";
  }
}

struct ObJsonlScanner::Impl {
  std::string path;
  int64_t sample_size;
  bool opened;
  bool schema_ready;
  bool eof;
  int64_t inferred_rows;
  uint64_t line_no;
  std::vector<ObJsonlColumnSchema> columns;
  std::map<std::string, size_t> index;
  std::ifstream reader;

  Impl()
      : sample_size(-1),
        opened(false),
        schema_ready(false),
        eof(false),
        inferred_rows(0),
        line_no(0) {}
};

ObJsonlScanner::ObJsonlScanner() : impl_(new Impl()) {}

ObJsonlScanner::~ObJsonlScanner() {}

void ObJsonlScanner::set_sample_size(int64_t sample_size)
{
  impl_->sample_size = sample_size;
}

int64_t ObJsonlScanner::sample_size() const { return impl_->sample_size; }

bool ObJsonlScanner::open(const std::string &path, std::string *err)
{
  std::string sink;
  std::string *error = (NULL == err) ? &sink : err;
  error->clear();
  std::ifstream probe(path.c_str(), std::ios::in | std::ios::binary);
  if (!probe.is_open()) {
    ob_set_error(error, "cannot open file for reading: " + path);
    return false;
  }
  probe.close();
  impl_->path = path;
  impl_->opened = true;
  impl_->schema_ready = false;
  impl_->eof = false;
  impl_->inferred_rows = 0;
  impl_->line_no = 0;
  impl_->columns.clear();
  impl_->index.clear();
  return true;
}

bool ObJsonlScanner::infer_schema(std::string *err)
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
                 "sample_size = 0 is not supported; use -1 (all records) or N > 0");
    return false;
  }

  std::ifstream in(impl->path.c_str(), std::ios::in | std::ios::binary);
  if (!in.is_open()) {
    ob_set_error(error, "cannot open file for reading: " + impl->path);
    return false;
  }

  std::vector<ObJsonlColumnSchema> columns;
  std::map<std::string, size_t> index;
  uint64_t line_no = 0;
  int64_t records = 0;
  std::string line;
  for (;;) {
    if (impl->sample_size > 0 && records >= impl->sample_size) {
      break;
    }
    bool have = false;
    if (!ob_read_record_line(&in, &line, &line_no, &have)) {
      ob_set_error(error, "I/O error while reading " + impl->path);
      return false;
    }
    if (!have) {
      break;
    }
    ObJsonDocument doc;
    ObJsonParser parser(line, &doc);
    std::string parse_error;
    if (!parser.parse_document(&parse_error)) {
      ob_set_error(error, "line " + std::to_string(line_no) + ": " + parse_error);
      return false;
    }
    if (!ob_check_flat_object(doc, line_no, error)) {
      return false;
    }
    const ObJsonNode &root = doc.nodes[doc.root];

    // Last occurrence wins for duplicate keys, but the key keeps the position
    // of its first appearance.
    std::vector<std::pair<std::string, size_t> > members;
    for (size_t i = 0; i < root.children.size(); ++i) {
      bool replaced = false;
      for (size_t m = 0; m < members.size(); ++m) {
        if (members[m].first == root.keys[i]) {
          members[m].second = root.children[i];
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        members.push_back(std::make_pair(root.keys[i], root.children[i]));
      }
    }

    std::set<std::string> present;
    for (size_t m = 0; m < members.size(); ++m) {
      const std::string &name = members[m].first;
      const ObJsonNode &node = doc.nodes[members[m].second];
      present.insert(name);
      std::map<std::string, size_t>::iterator found = index.find(name);
      size_t column_index = 0;
      if (index.end() == found) {
        ObJsonlColumnSchema column;
        column.name = name;
        column.type = OB_JSONL_TYPE_SQL_NULL;
        column.nullable = (records > 0);  // absent from earlier records
        column_index = columns.size();
        columns.push_back(column);
        index[name] = column_index;
      } else {
        column_index = found->second;
      }
      const ObJsonlType cell = ob_classify_node(node);
      columns[column_index].type = ob_merge_types(columns[column_index].type, cell);
      if (OB_JSONL_TYPE_SQL_NULL == cell) {
        columns[column_index].nullable = true;
      }
    }
    for (size_t c = 0; c < columns.size(); ++c) {
      if (present.end() == present.find(columns[c].name)) {
        columns[c].nullable = true;  // missing field => NULL
      }
    }
    ++records;
  }
  for (size_t c = 0; c < columns.size(); ++c) {
    if (OB_JSONL_TYPE_SQL_NULL == columns[c].type) {
      columns[c].nullable = true;
    }
  }
  in.close();

  impl->columns.swap(columns);
  impl->index.swap(index);
  impl->inferred_rows = records;
  impl->schema_ready = true;
  impl->eof = false;
  impl->line_no = 0;
  impl->reader.close();
  impl->reader.clear();
  impl->reader.open(impl->path.c_str(), std::ios::in | std::ios::binary);
  if (!impl->reader.is_open()) {
    impl->schema_ready = false;
    ob_set_error(error, "cannot reopen file for reading: " + impl->path);
    return false;
  }
  return true;
}

const std::vector<ObJsonlColumnSchema> &ObJsonlScanner::schema() const
{
  return impl_->columns;
}

bool ObJsonlScanner::next_row(std::vector<ObJsonlValue> *row, std::string *err)
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
  std::string line;
  bool have = false;
  for (;;) {
    if (!ob_read_record_line(&impl->reader, &line, &impl->line_no, &have)) {
      ob_set_error(error, "I/O error while reading " + impl->path);
      return false;
    }
    if (!have) {
      impl->eof = true;
      return false;
    }
    break;
  }
  const uint64_t record_line = impl->line_no;
  ObJsonDocument doc;
  ObJsonParser parser(line, &doc);
  std::string parse_error;
  if (!parser.parse_document(&parse_error)) {
    ob_set_error(error, "line " + std::to_string(record_line) + ": " + parse_error);
    return false;
  }
  if (!ob_check_flat_object(doc, record_line, error)) {
    return false;
  }
  const ObJsonNode &root = doc.nodes[doc.root];

  row->clear();
  row->resize(impl->columns.size());
  for (size_t c = 0; c < impl->columns.size(); ++c) {
    (*row)[c].type = impl->columns[c].type;
    (*row)[c].is_null = true;
  }
  for (size_t i = 0; i < root.children.size(); ++i) {
    const std::string &name = root.keys[i];
    const std::map<std::string, size_t>::const_iterator found = impl->index.find(name);
    if (impl->index.end() == found) {
      ob_set_error(error, "line " + std::to_string(record_line) + ": field \"" +
                              name +
                              "\" is not in the inferred schema (a bounded "
                              "sample_size may be unrepresentative)");
      return false;
    }
    std::string conversion_error;
    if (!ob_json_node_to_value(doc.nodes[root.children[i]],
                               impl->columns[found->second].type,
                               &(*row)[found->second], &conversion_error)) {
      ob_set_error(error, "line " + std::to_string(record_line) + ", field \"" +
                              name + "\": " + conversion_error);
      return false;
    }
  }
  return true;
}

bool ObJsonlScanner::eof() const { return impl_->eof; }

int64_t ObJsonlScanner::inferred_row_count() const { return impl_->inferred_rows; }

void ObJsonlScanner::close()
{
  impl_->reader.close();
  impl_->reader.clear();
  impl_->opened = false;
  impl_->schema_ready = false;
  impl_->eof = false;
}

}  // namespace sql
}  // namespace oceanbase

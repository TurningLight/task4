// ob_jsonl_scanner.h
//
// Streaming JSONL (newline-delimited JSON) scanner with a union schema.
//
// Design notes:
//   * One line is one record; lines are parsed one at a time, so peak memory is
//     O(longest line), not O(file).
//   * The schema is the union of the keys of all sampled lines, in first-seen
//     order. A key missing from a line yields NULL, which makes the column
//     nullable.
//   * Only flat objects are accepted: a top-level field whose value is an
//     object or an array is an error.
//   * JSON numbers that are integral become BIGINT, otherwise DOUBLE. true and
//     false become BOOLEAN. JSON strings always stay VARCHAR - dates are
//     deliberately NOT inferred from JSON strings (pinned semantics), unlike
//     CSV.
//   * The bundled JSON value parser is self-contained: objects, arrays,
//     strings, numbers, true/false/null, plus \" \\ \/ \b \f \n \r \t and
//     \uXXXX escapes (with surrogate pairs).
#ifndef OCEANBASE_SQL_ENGINE_BASIC_FILE_OB_JSONL_SCANNER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_FILE_OB_JSONL_SCANNER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {

// Logical type inferred for one JSONL column. DATE/DATETIME are intentionally
// absent: a JSON string is a string.
enum ObJsonlType {
  OB_JSONL_TYPE_SQL_NULL = 0,  // every observed value was null
  OB_JSONL_TYPE_BIGINT = 1,
  OB_JSONL_TYPE_DOUBLE = 2,
  OB_JSONL_TYPE_BOOLEAN = 3,
  OB_JSONL_TYPE_VARCHAR = 4
};

const char *ob_jsonl_type_name(ObJsonlType type);

struct ObJsonlColumnSchema {
  std::string name;
  ObJsonlType type;
  bool nullable;

  ObJsonlColumnSchema() : type(OB_JSONL_TYPE_SQL_NULL), nullable(true) {}
};

struct ObJsonlValue {
  ObJsonlType type;
  bool is_null;
  int64_t int_value;
  double double_value;
  bool bool_value;
  std::string string_value;

  ObJsonlValue()
      : type(OB_JSONL_TYPE_SQL_NULL),
        is_null(true),
        int_value(0),
        double_value(0.0),
        bool_value(false) {}
};

// Render one value: "NULL" for NULL, "true"/"false" for BOOLEAN, a shortest
// round-trip rendering for DOUBLE, raw text otherwise.
std::string ob_jsonl_value_to_string(const ObJsonlValue &value);

class ObJsonlScanner {
 public:
  ObJsonlScanner();
  ~ObJsonlScanner();

  // -1 (default) => infer over every line; N > 0 => infer over the first N
  // records. 0 is rejected: a zero-record sample cannot describe any column.
  void set_sample_size(int64_t sample_size);
  int64_t sample_size() const;

  bool open(const std::string &path, std::string *err);

  // Pass 1: builds the union schema in first-seen order, then rewinds.
  bool infer_schema(std::string *err);

  const std::vector<ObJsonlColumnSchema> &schema() const;

  // Pass 2: one record at a time. Returns false at EOF (see eof()) or on error
  // (err non-empty). The output row has schema().size() cells.
  bool next_row(std::vector<ObJsonlValue> *row, std::string *err);
  bool eof() const;

  // Number of records consumed by the last infer_schema() call.
  int64_t inferred_row_count() const;

  void close();

 private:
  ObJsonlScanner(const ObJsonlScanner &);
  ObJsonlScanner &operator=(const ObJsonlScanner &);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_ENGINE_BASIC_FILE_OB_JSONL_SCANNER_H_

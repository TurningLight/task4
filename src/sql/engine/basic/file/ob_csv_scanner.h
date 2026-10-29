// ob_csv_scanner.h
//
// Streaming CSV scanner with DuckDB-style schema inference.
//
// Design notes:
//   * The file is never slurped: a 64 KiB buffered byte reader produces one
//     record at a time; inference and row iteration are two independent passes
//     over the same path, so peak memory is O(longest record), not O(file).
//   * Inference is done over ALL rows when sample_size == -1 (the pinned
//     acceptance mode) or over the first N data rows for a bounded sample.
//   * Type lattice: BIGINT -> DOUBLE -> VARCHAR, plus BOOLEAN / DATE /
//     DATETIME, with DATE + DATETIME widening to DATETIME and any other
//     mismatch widening to VARCHAR. A column with only empty fields ends up
//     as SQL_NULL (DuckDB's NULL type) and is nullable.
//   * Quoted fields may contain ',', newlines, and "" escapes. An empty
//     *unquoted* field is NULL; an empty *quoted* field ("") is the empty
//     string and is therefore non-NULL.
#ifndef OCEANBASE_SQL_ENGINE_BASIC_FILE_OB_CSV_SCANNER_H_
#define OCEANBASE_SQL_ENGINE_BASIC_FILE_OB_CSV_SCANNER_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {

// Logical type inferred for one CSV column.
enum ObCsvType {
  OB_CSV_TYPE_SQL_NULL = 0,  // every value in the column was NULL
  OB_CSV_TYPE_BIGINT = 1,
  OB_CSV_TYPE_DOUBLE = 2,
  OB_CSV_TYPE_BOOLEAN = 3,
  OB_CSV_TYPE_VARCHAR = 4,
  OB_CSV_TYPE_DATE = 5,
  OB_CSV_TYPE_DATETIME = 6
};

const char *ob_csv_type_name(ObCsvType type);

struct ObCsvColumnSchema {
  std::string name;
  ObCsvType type;
  bool nullable;

  ObCsvColumnSchema() : type(OB_CSV_TYPE_SQL_NULL), nullable(true) {}
};

// One typed cell. The active member is selected by `type`; DATE/DATETIME are
// carried as normalized text ("YYYY-MM-DD" / "YYYY-MM-DD HH:MM:SS[.frac]")
// because the scan layer only needs a canonical rendering.
struct ObCsvValue {
  ObCsvType type;
  bool is_null;
  int64_t int_value;
  double double_value;
  bool bool_value;
  std::string string_value;

  ObCsvValue()
      : type(OB_CSV_TYPE_SQL_NULL),
        is_null(true),
        int_value(0),
        double_value(0.0),
        bool_value(false) {}
};

// Render one value: "NULL" for NULL, "true"/"false" for BOOLEAN, a shortest
// round-trip rendering for DOUBLE, raw text otherwise.
std::string ob_csv_value_to_string(const ObCsvValue &value);

class ObCsvScanner {
 public:
  enum ObCsvHeaderMode {
    OB_CSV_HEADER_PRESENT = 0,  // first record is the header (pinned default)
    OB_CSV_HEADER_ABSENT = 1,   // no header; columns are named column0..columnN
    OB_CSV_HEADER_AUTO = 2      // sniff: header iff row 1 is all-VARCHAR and row 2 is not
  };

  ObCsvScanner();
  ~ObCsvScanner();

  void set_header_mode(ObCsvHeaderMode mode);
  ObCsvHeaderMode header_mode() const;
  void set_header(bool has_header);  // convenience: true -> PRESENT, false -> ABSENT

  // -1 (default) => infer over the whole file; N > 0 => infer over the first N
  // data rows. 0 is rejected: a zero-row sample cannot describe any column.
  void set_sample_size(int64_t sample_size);
  int64_t sample_size() const;

  bool open(const std::string &path, std::string *err);

  // Header sniffing heuristic, also used by HEADER_AUTO during infer_schema().
  bool detect_header(bool *has_header, std::string *err);

  // Full pass 1: resolves the header, infers every column type plus
  // nullability, then rewinds the stream to the first data row.
  bool infer_schema(std::string *err);

  const std::vector<ObCsvColumnSchema> &schema() const;
  bool has_header() const;

  // Pass 2: one row at a time. Returns false at EOF (see eof()) or on error
  // (err non-empty). The output row has schema().size() cells.
  bool next_row(std::vector<ObCsvValue> *row, std::string *err);
  bool eof() const;

  // Number of data rows consumed by the last infer_schema() call.
  int64_t inferred_row_count() const;

  void close();

 private:
  ObCsvScanner(const ObCsvScanner &);
  ObCsvScanner &operator=(const ObCsvScanner &);

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sql
}  // namespace oceanbase

#endif  // OCEANBASE_SQL_ENGINE_BASIC_FILE_OB_CSV_SCANNER_H_

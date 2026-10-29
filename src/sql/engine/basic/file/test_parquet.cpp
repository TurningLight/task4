// test_parquet.cpp
//
// Standalone acceptance test for ob_parquet_reader.
//
//   g++ -std=c++17 -O2 -o test_parquet test_parquet.cpp ob_parquet_reader.cpp
//   ./test_parquet /tmp/simple.parquet
//
// Prints every column with its physical type, the row count, and the values.

#include "ob_parquet_reader.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

void PrintValues(const obparquet::ColumnData& data, int64_t max_inline) {
  int64_t n = data.NumRows();
  std::printf("  values (%lld):", static_cast<long long>(n));
  int64_t shown = 0;
  for (int64_t i = 0; i < n; ++i) {
    if (data.is_null[static_cast<size_t>(i)]) {
      std::printf(" NULL");
    } else if (data.physical_type == obparquet::PT_INT32 ||
               data.physical_type == obparquet::PT_INT64 ||
               data.physical_type == obparquet::PT_BOOLEAN) {
      std::printf(" %lld", static_cast<long long>(data.values[static_cast<size_t>(i)]));
    } else if (data.physical_type == obparquet::PT_FLOAT ||
               data.physical_type == obparquet::PT_DOUBLE) {
      std::printf(" %.17g", data.doubles[static_cast<size_t>(i)]);
    } else {
      const std::string& s = data.bytes[static_cast<size_t>(i)];
      std::printf(" \"%s\"", s.c_str());
    }
    ++shown;
    if (max_inline > 0 && shown >= max_inline && i + 1 < n) {
      std::printf(" ... (%lld more)", static_cast<long long>(n - shown));
      break;
    }
  }
  std::printf("\n");
}

// Verifies the documented invariant: row-aligned vectors, one entry per row.
bool CheckRowAlignment(const obparquet::ColumnData& data) {
  const size_t rows = data.is_null.size();
  if (data.physical_type == obparquet::PT_INT32 || data.physical_type == obparquet::PT_INT64 ||
      data.physical_type == obparquet::PT_BOOLEAN) {
    return data.values.size() == rows;
  }
  if (data.physical_type == obparquet::PT_FLOAT || data.physical_type == obparquet::PT_DOUBLE) {
    return data.doubles.size() == rows;
  }
  return data.bytes.size() == rows;
}

int64_t CountNulls(const obparquet::ColumnData& data) {
  int64_t nulls = 0;
  for (size_t i = 0; i < data.is_null.size(); ++i) {
    if (data.is_null[i]) ++nulls;
  }
  return nulls;
}

}  // namespace

int main(int argc, char** argv) {
  const char* path = NULL;
  const char* only_column = NULL;
  bool dump_meta = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--meta") == 0) {
      dump_meta = true;
    } else if (std::strcmp(argv[i], "--column") == 0 && i + 1 < argc) {
      only_column = argv[++i];
    } else if (path == NULL) {
      path = argv[i];
    } else {
      std::fprintf(stderr, "usage: %s [--meta] [--column NAME] <file.parquet>\n", argv[0]);
      return 2;
    }
  }
  if (path == NULL) {
    std::fprintf(stderr, "usage: %s [--meta] [--column NAME] <file.parquet>\n", argv[0]);
    return 2;
  }

  obparquet::Reader reader;
  std::string err;
  if (!reader.Open(path, &err)) {
    std::fprintf(stderr, "FAIL open %s: %s\n", path, err.c_str());
    return 1;
  }

  std::printf("=== Parquet reader test ===\n");
  std::printf("file        : %s\n", path);
  std::printf("columns     : %zu\n", reader.NumColumns());
  std::printf("row_count   : %lld\n", static_cast<long long>(reader.NumRows()));
  std::printf("row_groups  : %zu\n", reader.metadata().row_groups.size());
  std::printf("created_by  : \"%s\"\n", reader.metadata().created_by.c_str());
  std::printf("\n");

  std::printf("column list:\n");
  for (size_t i = 0; i < reader.NumColumns(); ++i) {
    const obparquet::ColumnInfo& c = reader.Column(i);
    std::printf("  [%zu] name=%s physical_type=%s repetition=%s codec=%s"
                " max_def=%d max_rep=%d\n",
                i, c.name.c_str(), obparquet::PhysicalTypeName(c.physical_type),
                obparquet::RepetitionName(c.repetition_type),
                obparquet::CodecName(reader.metadata().row_groups.empty()
                                         ? -1
                                         : reader.metadata()
                                               .row_groups[0]
                                               .columns[i]
                                               .meta_data.codec),
                c.max_definition_level, c.max_repetition_level);
  }
  std::printf("\n");

  if (dump_meta) {
    std::printf("--- decoded FileMetaData ---\n%s\n", reader.DescribeMetadata().c_str());
  }

  bool all_ok = true;
  int only_index = -1;
  if (only_column != NULL) {
    only_index = reader.FindColumn(only_column);
    if (only_index < 0) {
      std::fprintf(stderr, "FAIL: no column named \"%s\"\n", only_column);
      reader.Close();
      return 1;
    }
    std::printf("reading only column \"%s\" (column projection; other chunks stay untouched)\n\n",
                only_column);
  }

  for (size_t i = 0; i < reader.NumColumns(); ++i) {
    if (only_index >= 0 && static_cast<int>(i) != only_index) continue;
    const obparquet::ColumnInfo& c = reader.Column(i);
    std::printf("--- column %zu/%zu: \"%s\" (%s) ---\n", i + 1, reader.NumColumns(),
                c.name.c_str(), obparquet::PhysicalTypeName(c.physical_type));
    obparquet::ColumnData data;
    std::string cerr_str;
    if (!reader.ReadColumn(i, &data, &cerr_str)) {
      std::printf("  READ FAILED: %s\n", cerr_str.c_str());
      all_ok = false;
      continue;
    }
    int64_t nulls = CountNulls(data);
    std::printf("  rows: %lld\n", static_cast<long long>(data.NumRows()));
    std::printf("  nulls: %lld\n", static_cast<long long>(nulls));
    if (!CheckRowAlignment(data)) {
      std::printf("  FAILED: value vectors are not row aligned\n");
      all_ok = false;
      continue;
    }
    PrintValues(data, 0);
    std::printf("\n");
  }

  std::printf("RESULT: %s\n", all_ok ? "PASS" : "FAIL");
  reader.Close();
  return all_ok ? 0 : 1;
}

// ob_parquet_reader.h
//
// A minimal, self-contained Parquet reader for OceanBase-style use:
//   * no external libraries, no third-party dependencies, C++11 only
//   * Thrift compact protocol decoder (generic field-id walker)
//   * FileMetaData / SchemaElement / RowGroup / ColumnChunk / ColumnMetaData
//   * PageHeader: DATA_PAGE, DICTIONARY_PAGE, DATA_PAGE_V2
//   * Encodings: PLAIN, PLAIN_DICTIONARY/RLE_DICTIONARY, RLE, BIT_PACKED
//   * Codec: UNCOMPRESSED, SNAPPY (built-in raw-block decompressor);
//            other codecs are detected and reported by name, never faked.
//
// Design notes:
//   * Only the footer is held in memory. Column reads stream one row group at a
//     time and only touch the requested column chunk (column projection).
//   * Intended as a read path inside OceanBase; error strings are descriptive so
//     the SQL layer can surface "unsupported encoding/codec X".

#ifndef OB_PARQUET_READER_H
#define OB_PARQUET_READER_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace obparquet {

// ---- Parquet enumerations (values match parquet.thrift) --------------------

enum PhysicalType : int32_t {
  PT_BOOLEAN = 0,
  PT_INT32 = 1,
  PT_INT64 = 2,
  PT_INT96 = 3,
  PT_FLOAT = 4,
  PT_DOUBLE = 5,
  PT_BYTE_ARRAY = 6,
  PT_FIXED_LEN_BYTE_ARRAY = 7
};

enum Encoding : int32_t {
  ENC_PLAIN = 0,
  ENC_PLAIN_DICTIONARY = 2,
  ENC_RLE = 3,
  ENC_BIT_PACKED = 4,
  ENC_DELTA_BINARY_PACKED = 5,
  ENC_DELTA_LENGTH_BYTE_ARRAY = 6,
  ENC_DELTA_BYTE_ARRAY = 7,
  ENC_RLE_DICTIONARY = 8,
  ENC_BYTE_STREAM_SPLIT = 9
};

enum CompressionCodec : int32_t {
  CODEC_UNCOMPRESSED = 0,
  CODEC_SNAPPY = 1,
  CODEC_GZIP = 2,
  CODEC_LZO = 3,
  CODEC_BROTLI = 4,
  CODEC_LZ4 = 5,
  CODEC_ZSTD = 6,
  CODEC_LZ4_RAW = 7
};

enum RepetitionType : int32_t {
  REP_REQUIRED = 0,
  REP_OPTIONAL = 1,
  REP_REPEATED = 2
};

enum PageType : int32_t {
  PAGE_DATA = 0,
  PAGE_INDEX = 1,
  PAGE_DICTIONARY = 2,
  PAGE_DATA_V2 = 3
};

// ---- Human readable helpers ----------------------------------------------

const char* PhysicalTypeName(int32_t t);
const char* EncodingName(int32_t e);
const char* CodecName(int32_t c);
const char* PageTypeName(int32_t t);
const char* RepetitionName(int32_t r);

// ---- Metadata structures --------------------------------------------------

struct KeyValue {
  std::string key;
  std::string value;
  bool has_value = false;
};

struct SchemaElement {
  bool has_type = false;
  int32_t type = -1;  // PhysicalType
  bool has_type_length = false;
  int32_t type_length = -1;
  bool has_repetition_type = false;
  int32_t repetition_type = -1;  // RepetitionType
  std::string name;
  bool has_num_children = false;
  int32_t num_children = -1;
  bool has_converted_type = false;
  int32_t converted_type = -1;
  bool has_field_id = false;
  int32_t field_id = -1;
};

struct StatisticsInfo {
  bool has_max = false;        // deprecated field 1
  bool has_min = false;        // deprecated field 2
  bool has_null_count = false;  // field 3
  bool has_distinct_count = false;
  bool has_max_value = false;  // field 5 (order-insensitive encoding)
  bool has_min_value = false;  // field 6
  int64_t null_count = 0;
  int64_t distinct_count = 0;
  std::string max, min, max_value, min_value;
};

struct ColumnMetaData {
  bool has_type = false;
  int32_t type = -1;  // PhysicalType
  std::vector<int32_t> encodings;
  std::vector<std::string> path_in_schema;
  bool has_codec = false;
  int32_t codec = -1;
  int64_t num_values = 0;
  int64_t total_uncompressed_size = 0;
  int64_t total_compressed_size = 0;
  bool has_data_page_offset = false;
  int64_t data_page_offset = 0;
  bool has_index_page_offset = false;
  int64_t index_page_offset = 0;
  bool has_dictionary_page_offset = false;
  int64_t dictionary_page_offset = 0;
  bool has_statistics = false;
  StatisticsInfo statistics;
  std::vector<KeyValue> key_value_metadata;
};

struct ColumnChunk {
  std::string file_path;
  bool has_file_path = false;
  int64_t file_offset = 0;
  bool has_meta_data = false;
  ColumnMetaData meta_data;
};

struct RowGroup {
  std::vector<ColumnChunk> columns;
  int64_t total_byte_size = 0;
  int64_t num_rows = 0;
  bool has_file_offset = false;
  int64_t file_offset = 0;
  bool has_total_compressed_size = false;
  int64_t total_compressed_size = 0;
  bool has_ordinal = false;
  int32_t ordinal = 0;
};

struct FileMetaData {
  int32_t version = 0;
  std::vector<SchemaElement> schema;
  int64_t num_rows = 0;
  std::vector<RowGroup> row_groups;
  std::vector<KeyValue> key_value_metadata;
  std::string created_by;
};

// ---- Page headers --------------------------------------------------------

struct DataPageHeader {
  int32_t num_values = 0;
  int32_t encoding = -1;
  int32_t definition_level_encoding = -1;
  int32_t repetition_level_encoding = -1;
  bool has_statistics = false;
  StatisticsInfo statistics;
};

struct DictionaryPageHeader {
  int32_t num_values = 0;
  int32_t encoding = -1;
  bool is_sorted = false;
};

struct DataPageHeaderV2 {
  int32_t num_values = 0;
  int32_t num_nulls = 0;
  int32_t num_rows = 0;
  int32_t encoding = -1;
  int32_t definition_levels_byte_length = 0;
  int32_t repetition_levels_byte_length = 0;
  bool is_compressed = true;
  bool has_statistics = false;
  StatisticsInfo statistics;
};

struct PageHeader {
  int32_t type = -1;
  int32_t uncompressed_page_size = 0;
  int32_t compressed_page_size = 0;
  bool has_crc = false;
  int32_t crc = 0;
  bool has_data_page_header = false;
  DataPageHeader data_page_header;
  bool has_dictionary_page_header = false;
  DictionaryPageHeader dictionary_page_header;
  bool has_data_page_header_v2 = false;
  DataPageHeaderV2 data_page_header_v2;
  size_t header_size = 0;  // bytes consumed by the thrift-encoded header
};

// ---- Column descriptor exposed to callers --------------------------------

struct ColumnInfo {
  std::string name;                 // leaf name (SchemaElement.name)
  std::vector<std::string> path;    // full path_in_schema
  int32_t physical_type = -1;       // PhysicalType of the leaf
  int32_t type_length = -1;         // for FIXED_LEN_BYTE_ARRAY
  int32_t repetition_type = -1;     // RepetitionType of the leaf
  int32_t converted_type = -1;      // legacy converted type, -1 unset
  int max_definition_level = 0;
  int max_repetition_level = 0;
  int schema_index = -1;            // index into schema elements
  int column_index = -1;            // index among RowGroup.columns
};

// ---- Column data ---------------------------------------------------------

// A decoded column. Exactly one of `values`, `doubles`, `bytes` is used,
// depending on the physical type:
//   BOOLEAN, INT32, INT64            -> values  (0/1 for BOOLEAN)
//   FLOAT, DOUBLE                    -> doubles
//   BYTE_ARRAY, FLBA, INT96          -> bytes
//
// All containers are ROW ALIGNED: they have exactly one entry per row
// (NumRows() == is_null.size() == the active value vector's size), so a row can
// be read with a single index. `is_null[i] == 1` marks a NULL row, whose value
// slot holds a default (0 / 0.0 / empty string) and must not be used.
struct ColumnData {
  int32_t physical_type = -1;
  std::vector<int64_t> values;
  std::vector<double> doubles;
  std::vector<std::string> bytes;
  std::vector<uint8_t> is_null;

  void Clear() {
    physical_type = -1;
    values.clear();
    doubles.clear();
    bytes.clear();
    is_null.clear();
  }
  int64_t NumRows() const { return static_cast<int64_t>(is_null.size()); }
};

// ---- Reader --------------------------------------------------------------

class Reader {
 public:
  Reader();
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  // Parses magic + footer. Only the footer stays resident.
  bool Open(const std::string& path, std::string* err);
  void Close();
  bool is_open() const { return file_ != NULL; }

  const std::string& path() const { return path_; }
  const FileMetaData& metadata() const { return meta_; }
  const std::vector<ColumnInfo>& columns() const { return columns_; }
  size_t NumColumns() const { return columns_.size(); }
  int64_t NumRows() const { return meta_.num_rows; }

  // Returns the column index, or -1 when the name is unknown.
  int FindColumn(const std::string& name) const;
  const ColumnInfo& Column(size_t i) const { return columns_[i]; }

  // Reads every value of one leaf column, row group by row group.
  // Only the requested column chunk is read from disk (column projection).
  bool ReadColumn(const std::string& name, ColumnData* out, std::string* err);
  bool ReadColumn(size_t index, ColumnData* out, std::string* err);

  // Convenience for the common INT32/INT64 case used by the engine.
  bool ReadInt64Column(const std::string& name, std::vector<int64_t>* values,
                       std::vector<uint8_t>* is_null, std::string* err);

  // Renders the decoded footer, for debugging / for the RESULT.md evidence.
  std::string DescribeMetadata() const;

 private:
  bool ReadAt(int64_t offset, size_t len, std::string* buf, std::string* err) const;
  bool ReadChunk(const ColumnMetaData& cm, const ColumnInfo& info, ColumnData* out,
                 std::string* err) const;
  // Decodes one data page body into `out`, returning the number of values seen.
  bool DecodeDataPage(const PageHeader& ph, const uint8_t* body, size_t body_len,
                      const ColumnInfo& info, const ColumnMetaData& cm,
                      const ColumnData& dict, bool has_dict, ColumnData* out,
                      int64_t* values_seen, std::string* err) const;
  bool DecodeDictionaryPage(const PageHeader& ph, const uint8_t* body, size_t body_len,
                            const ColumnMetaData& cm, int32_t type_length, ColumnData* dict,
                            std::string* err) const;
  bool BuildColumns(std::string* err);

  std::FILE* file_;
  std::string path_;
  int64_t file_size_;
  FileMetaData meta_;
  std::vector<ColumnInfo> columns_;
};

}  // namespace obparquet

#endif  // OB_PARQUET_READER_H

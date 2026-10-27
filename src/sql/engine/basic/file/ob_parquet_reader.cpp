// ob_parquet_reader.cpp
//
// Minimal self-contained Parquet reader. C++11, no external dependencies.
// See ob_parquet_reader.h for the supported feature matrix.

#include "ob_parquet_reader.h"

#include <cstdio>
#include <cstring>
#include <sstream>

namespace obparquet {

// ===========================================================================
// Enum names
// ===========================================================================

const char* PhysicalTypeName(int32_t t) {
  switch (t) {
    case PT_BOOLEAN: return "BOOLEAN";
    case PT_INT32: return "INT32";
    case PT_INT64: return "INT64";
    case PT_INT96: return "INT96";
    case PT_FLOAT: return "FLOAT";
    case PT_DOUBLE: return "DOUBLE";
    case PT_BYTE_ARRAY: return "BYTE_ARRAY";
    case PT_FIXED_LEN_BYTE_ARRAY: return "FIXED_LEN_BYTE_ARRAY";
    default: return "UNKNOWN_PHYSICAL_TYPE";
  }
}

const char* EncodingName(int32_t e) {
  switch (e) {
    case ENC_PLAIN: return "PLAIN";
    case ENC_PLAIN_DICTIONARY: return "PLAIN_DICTIONARY";
    case ENC_RLE: return "RLE";
    case ENC_BIT_PACKED: return "BIT_PACKED";
    case ENC_DELTA_BINARY_PACKED: return "DELTA_BINARY_PACKED";
    case ENC_DELTA_LENGTH_BYTE_ARRAY: return "DELTA_LENGTH_BYTE_ARRAY";
    case ENC_DELTA_BYTE_ARRAY: return "DELTA_BYTE_ARRAY";
    case ENC_RLE_DICTIONARY: return "RLE_DICTIONARY";
    case ENC_BYTE_STREAM_SPLIT: return "BYTE_STREAM_SPLIT";
    default: return "UNKNOWN_ENCODING";
  }
}

const char* CodecName(int32_t c) {
  switch (c) {
    case CODEC_UNCOMPRESSED: return "UNCOMPRESSED";
    case CODEC_SNAPPY: return "SNAPPY";
    case CODEC_GZIP: return "GZIP";
    case CODEC_LZO: return "LZO";
    case CODEC_BROTLI: return "BROTLI";
    case CODEC_LZ4: return "LZ4";
    case CODEC_ZSTD: return "ZSTD";
    case CODEC_LZ4_RAW: return "LZ4_RAW";
    default: return "UNKNOWN_CODEC";
  }
}

const char* PageTypeName(int32_t t) {
  switch (t) {
    case PAGE_DATA: return "DATA_PAGE";
    case PAGE_INDEX: return "INDEX_PAGE";
    case PAGE_DICTIONARY: return "DICTIONARY_PAGE";
    case PAGE_DATA_V2: return "DATA_PAGE_V2";
    default: return "UNKNOWN_PAGE_TYPE";
  }
}

const char* RepetitionName(int32_t r) {
  switch (r) {
    case REP_REQUIRED: return "REQUIRED";
    case REP_OPTIONAL: return "OPTIONAL";
    case REP_REPEATED: return "REPEATED";
    default: return "UNSET";
  }
}

// ===========================================================================
// Thrift compact protocol decoder
//
// Wire format recap (field-header oriented):
//   field header byte: high nibble = field-id delta (0 => zigzag varint id
//   follows), low nibble = type. type 0 == STOP.
//   types: 1 bool-true, 2 bool-false, 3 i8, 4 i16, 5 i32, 6 i64, 7 double,
//          8 binary, 9 list, 10 set, 11 map, 12 struct.
//   integers are zigzag varints. Field ids are scoped per struct.
// ===========================================================================

namespace {

enum ThriftType : int32_t {
  TT_STOP = 0,
  TT_BOOL_TRUE = 1,
  TT_BOOL_FALSE = 2,
  TT_BYTE = 3,
  TT_I16 = 4,
  TT_I32 = 5,
  TT_I64 = 6,
  TT_DOUBLE = 7,
  TT_BINARY = 8,
  TT_LIST = 9,
  TT_SET = 10,
  TT_MAP = 11,
  TT_STRUCT = 12
};

class CompactReader {
 public:
  CompactReader(const uint8_t* data, size_t len)
      : data_(data), len_(len), pos_(0), ok_(true), last_field_id_(0) {}

  bool ok() const { return ok_; }
  size_t pos() const { return pos_; }
  const char* error() const { return error_; }
  void set_pos(size_t p) { pos_ = p; }

  // Struct nesting: field-id delta state must be saved/restored.
  class StructScope {
   public:
    explicit StructScope(CompactReader* r) : r_(r), saved_(r->last_field_id_) {
      r_->last_field_id_ = 0;
    }
    ~StructScope() { r_->last_field_id_ = saved_; }

   private:
    CompactReader* r_;
    int32_t saved_;
  };

  bool Skip(size_t n) {
    if (!ok_ || pos_ + n > len_) return Fail("truncated thrift buffer");
    pos_ += n;
    return true;
  }

  // Returns false on error; sets *stop when a STOP field was read.
  bool ReadFieldHeader(int32_t* field_id, int32_t* type, bool* stop) {
    *stop = false;
    if (!ok_) return false;
    if (pos_ >= len_) return Fail("truncated thrift field header");
    uint8_t b = data_[pos_++];
    int32_t t = b & 0x0f;
    if (t == TT_STOP) {
      *stop = true;
      *field_id = 0;
      *type = TT_STOP;
      return true;
    }
    int32_t delta = (b >> 4) & 0x0f;
    int32_t fid;
    if (delta == 0) {
      int64_t v = 0;
      if (!ReadZigZag(&v)) return false;
      fid = static_cast<int32_t>(v);
    } else {
      fid = last_field_id_ + delta;
    }
    last_field_id_ = fid;
    *field_id = fid;
    *type = t;
    return true;
  }

  bool ReadBool(int32_t type, bool* value) {
    if (type == TT_BOOL_TRUE) {
      *value = true;
      return true;
    }
    if (type == TT_BOOL_FALSE) {
      *value = false;
      return true;
    }
    return Fail("expected thrift bool");
  }

  bool ReadI32(int32_t* value) {
    int64_t v = 0;
    if (!ReadZigZag(&v)) return false;
    *value = static_cast<int32_t>(v);
    return true;
  }

  bool ReadI16(int32_t* value) {
    int64_t v = 0;
    if (!ReadZigZag(&v)) return false;
    *value = static_cast<int32_t>(v);
    return true;
  }

  bool ReadI64(int64_t* value) {
    int64_t v = 0;
    if (!ReadZigZag(&v)) return false;
    *value = v;
    return true;
  }

  bool ReadByte(int8_t* value) {
    if (!ok_ || pos_ >= len_) return Fail("truncated thrift i8");
    *value = static_cast<int8_t>(data_[pos_++]);
    return true;
  }

  // Thrift compact doubles are written big-endian by the reference
  // implementations (Java/C++); Parquet page/metadata structs we consume do not
  // use doubles, so this is a completeness hook.
  bool ReadDouble(double* value) {
    if (!Skip(8)) return false;
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i) bits = (bits << 8) | data_[pos_ - 8 + i];
    std::memcpy(value, &bits, 8);
    return true;
  }

  bool ReadBinary(std::string* out) {
    int64_t n = 0;
    if (!ReadVarint(&n)) return false;
    if (n < 0) return Fail("negative thrift binary length");
    if (!ok_ || pos_ + static_cast<size_t>(n) > len_) return Fail("truncated thrift binary");
    out->assign(reinterpret_cast<const char*>(data_ + pos_), static_cast<size_t>(n));
    pos_ += static_cast<size_t>(n);
    return true;
  }

  bool ReadListHeader(int32_t* elem_type, int64_t* size) {
    if (!ok_ || pos_ >= len_) return Fail("truncated thrift list header");
    uint8_t b = data_[pos_++];
    int32_t et = b & 0x0f;
    int64_t n = (b >> 4) & 0x0f;
    if (n == 15) {
      if (!ReadVarint(&n)) return false;
    }
    if (n < 0) return Fail("negative thrift list size");
    *elem_type = et;
    *size = n;
    return true;
  }

  bool ReadMapHeader(int32_t* key_type, int32_t* value_type, int64_t* size) {
    int64_t n = 0;
    if (!ReadVarint(&n)) return false;
    if (n < 0) return Fail("negative thrift map size");
    *size = n;
    if (n == 0) {
      *key_type = TT_STOP;
      *value_type = TT_STOP;
      return true;
    }
    if (!ok_ || pos_ >= len_) return Fail("truncated thrift map header");
    uint8_t b = data_[pos_++];
    *key_type = (b >> 4) & 0x0f;
    *value_type = b & 0x0f;
    return true;
  }

  // Skips a value of the given type (structs/lists/maps recursively).
  bool SkipField(int32_t type) {
    switch (type) {
      case TT_BOOL_TRUE:
      case TT_BOOL_FALSE: return true;
      case TT_BYTE: return Skip(1);
      case TT_I16:
      case TT_I32:
      case TT_I64: {
        int64_t v = 0;
        return ReadVarint(&v);
      }
      case TT_DOUBLE: return Skip(8);
      case TT_BINARY: {
        std::string tmp;
        return ReadBinary(&tmp);
      }
      case TT_LIST:
      case TT_SET: {
        int32_t et = 0;
        int64_t n = 0;
        if (!ReadListHeader(&et, &n)) return false;
        // List elements are always standalone values: bool elements occupy one
        // byte each (1 = true, 2 = false) and carry no field-header bit.
        const bool bool_elements = (et == TT_BOOL_TRUE || et == TT_BOOL_FALSE);
        for (int64_t i = 0; i < n; ++i) {
          if (bool_elements) {
            if (!Skip(1)) return false;
          } else if (!SkipField(et)) {
            return false;
          }
        }
        return true;
      }
      case TT_MAP: {
        int32_t kt = 0, vt = 0;
        int64_t n = 0;
        if (!ReadMapHeader(&kt, &vt, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          if (!SkipField(kt)) return false;
          if (!SkipField(vt)) return false;
        }
        return true;
      }
      case TT_STRUCT: {
        StructScope scope(this);
        for (;;) {
          int32_t fid = 0, ft = 0;
          bool stop = false;
          if (!ReadFieldHeader(&fid, &ft, &stop)) return false;
          if (stop) return true;
          if (!SkipField(ft)) return false;
        }
      }
      default: return Fail("unknown thrift type in skip");
    }
  }

 private:
  bool Fail(const char* msg) {
    if (ok_) {
      ok_ = false;
      error_ = msg;
    }
    return false;
  }

  // Unsigned LEB128.
  bool ReadVarint(int64_t* value) {
    uint64_t r = 0;
    int shift = 0;
    for (;;) {
      if (!ok_ || pos_ >= len_) return Fail("truncated thrift varint");
      uint8_t b = data_[pos_++];
      r |= static_cast<uint64_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) break;
      shift += 7;
      if (shift > 63) return Fail("thrift varint overflow");
    }
    *value = static_cast<int64_t>(r);
    return true;
  }

  bool ReadZigZag(int64_t* value) {
    int64_t raw = 0;
    if (!ReadVarint(&raw)) return false;
    uint64_t u = static_cast<uint64_t>(raw);
    *value = static_cast<int64_t>((u >> 1) ^ (~(u & 1) + 1));
    return true;
  }

  const uint8_t* data_;
  size_t len_;
  size_t pos_;
  bool ok_;
  const char* error_ = "ok";
  int32_t last_field_id_;
};

// ---- struct parsers -------------------------------------------------------

bool ParseKeyValue(CompactReader* r, KeyValue* kv) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1: if (!r->ReadBinary(&kv->key)) return false; break;
      case 2:
        if (!r->ReadBinary(&kv->value)) return false;
        kv->has_value = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseStatistics(CompactReader* r, StatisticsInfo* st) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1:
        if (!r->ReadBinary(&st->max)) return false;
        st->has_max = true;
        break;
      case 2:
        if (!r->ReadBinary(&st->min)) return false;
        st->has_min = true;
        break;
      case 3:
        if (!r->ReadI64(&st->null_count)) return false;
        st->has_null_count = true;
        break;
      case 4:
        if (!r->ReadI64(&st->distinct_count)) return false;
        st->has_distinct_count = true;
        break;
      case 5:
        if (!r->ReadBinary(&st->max_value)) return false;
        st->has_max_value = true;
        break;
      case 6:
        if (!r->ReadBinary(&st->min_value)) return false;
        st->has_min_value = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseSchemaElement(CompactReader* r, SchemaElement* se) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1:
        if (!r->ReadI32(&se->type)) return false;
        se->has_type = true;
        break;
      case 2:
        if (!r->ReadI32(&se->type_length)) return false;
        se->has_type_length = true;
        break;
      case 3:
        if (!r->ReadI32(&se->repetition_type)) return false;
        se->has_repetition_type = true;
        break;
      case 4: if (!r->ReadBinary(&se->name)) return false; break;
      case 5:
        if (!r->ReadI32(&se->num_children)) return false;
        se->has_num_children = true;
        break;
      case 6:
        if (!r->ReadI32(&se->converted_type)) return false;
        se->has_converted_type = true;
        break;
      case 9:
        if (!r->ReadI32(&se->field_id)) return false;
        se->has_field_id = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseColumnMetaData(CompactReader* r, ColumnMetaData* cm) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1:
        if (!r->ReadI32(&cm->type)) return false;
        cm->has_type = true;
        break;
      case 2: {
        int32_t et = 0;
        int64_t n = 0;
        if (!r->ReadListHeader(&et, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          int32_t enc = 0;
          if (et == TT_I32 || et == TT_I16) {
            if (!r->ReadI32(&enc)) return false;
          } else if (et == TT_I64) {
            int64_t v = 0;
            if (!r->ReadI64(&v)) return false;
            enc = static_cast<int32_t>(v);
          } else {
            return false;
          }
          cm->encodings.push_back(enc);
        }
        break;
      }
      case 3: {
        int32_t et = 0;
        int64_t n = 0;
        if (!r->ReadListHeader(&et, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          std::string s;
          if (!r->ReadBinary(&s)) return false;
          cm->path_in_schema.push_back(s);
        }
        break;
      }
      case 4:
        if (!r->ReadI32(&cm->codec)) return false;
        cm->has_codec = true;
        break;
      case 5: if (!r->ReadI64(&cm->num_values)) return false; break;
      case 6: if (!r->ReadI64(&cm->total_uncompressed_size)) return false; break;
      case 7: if (!r->ReadI64(&cm->total_compressed_size)) return false; break;
      case 8: {
        int32_t et = 0;
        int64_t n = 0;
        if (!r->ReadListHeader(&et, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          KeyValue kv;
          if (!ParseKeyValue(r, &kv)) return false;
          cm->key_value_metadata.push_back(kv);
        }
        break;
      }
      case 9:
        if (!r->ReadI64(&cm->data_page_offset)) return false;
        cm->has_data_page_offset = true;
        break;
      case 10:
        if (!r->ReadI64(&cm->index_page_offset)) return false;
        cm->has_index_page_offset = true;
        break;
      case 11:
        if (!r->ReadI64(&cm->dictionary_page_offset)) return false;
        cm->has_dictionary_page_offset = true;
        break;
      case 12:
        if (!ParseStatistics(r, &cm->statistics)) return false;
        cm->has_statistics = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseColumnChunk(CompactReader* r, ColumnChunk* cc) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1:
        if (!r->ReadBinary(&cc->file_path)) return false;
        cc->has_file_path = true;
        break;
      case 2: if (!r->ReadI64(&cc->file_offset)) return false; break;
      case 3:
        if (!ParseColumnMetaData(r, &cc->meta_data)) return false;
        cc->has_meta_data = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseRowGroup(CompactReader* r, RowGroup* rg) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1: {
        int32_t et = 0;
        int64_t n = 0;
        if (!r->ReadListHeader(&et, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          ColumnChunk cc;
          if (!ParseColumnChunk(r, &cc)) return false;
          rg->columns.push_back(cc);
        }
        break;
      }
      case 2: if (!r->ReadI64(&rg->total_byte_size)) return false; break;
      case 3: if (!r->ReadI64(&rg->num_rows)) return false; break;
      case 5:
        if (!r->ReadI64(&rg->file_offset)) return false;
        rg->has_file_offset = true;
        break;
      case 6:
        if (!r->ReadI64(&rg->total_compressed_size)) return false;
        rg->has_total_compressed_size = true;
        break;
      case 7:
        if (!r->ReadI16(&rg->ordinal)) return false;
        rg->has_ordinal = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseFileMetaData(CompactReader* r, FileMetaData* fm) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1: if (!r->ReadI32(&fm->version)) return false; break;
      case 2: {
        int32_t et = 0;
        int64_t n = 0;
        if (!r->ReadListHeader(&et, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          SchemaElement se;
          if (!ParseSchemaElement(r, &se)) return false;
          fm->schema.push_back(se);
        }
        break;
      }
      case 3: if (!r->ReadI64(&fm->num_rows)) return false; break;
      case 4: {
        int32_t et = 0;
        int64_t n = 0;
        if (!r->ReadListHeader(&et, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          RowGroup rg;
          if (!ParseRowGroup(r, &rg)) return false;
          fm->row_groups.push_back(rg);
        }
        break;
      }
      case 5: {
        int32_t et = 0;
        int64_t n = 0;
        if (!r->ReadListHeader(&et, &n)) return false;
        for (int64_t i = 0; i < n; ++i) {
          KeyValue kv;
          if (!ParseKeyValue(r, &kv)) return false;
          fm->key_value_metadata.push_back(kv);
        }
        break;
      }
      case 6: if (!r->ReadBinary(&fm->created_by)) return false; break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseDataPageHeader(CompactReader* r, DataPageHeader* h) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1: if (!r->ReadI32(&h->num_values)) return false; break;
      case 2: if (!r->ReadI32(&h->encoding)) return false; break;
      case 3: if (!r->ReadI32(&h->definition_level_encoding)) return false; break;
      case 4: if (!r->ReadI32(&h->repetition_level_encoding)) return false; break;
      case 5:
        if (!ParseStatistics(r, &h->statistics)) return false;
        h->has_statistics = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseDictionaryPageHeader(CompactReader* r, DictionaryPageHeader* h) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1: if (!r->ReadI32(&h->num_values)) return false; break;
      case 2: if (!r->ReadI32(&h->encoding)) return false; break;
      case 3: if (!r->ReadBool(ft, &h->is_sorted)) return false; break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParseDataPageHeaderV2(CompactReader* r, DataPageHeaderV2* h) {
  CompactReader::StructScope scope(r);
  for (;;) {
    int32_t fid = 0, ft = 0;
    bool stop = false;
    if (!r->ReadFieldHeader(&fid, &ft, &stop)) return false;
    if (stop) return true;
    switch (fid) {
      case 1: if (!r->ReadI32(&h->num_values)) return false; break;
      case 2: if (!r->ReadI32(&h->num_nulls)) return false; break;
      case 3: if (!r->ReadI32(&h->num_rows)) return false; break;
      case 4: if (!r->ReadI32(&h->encoding)) return false; break;
      case 5: if (!r->ReadI32(&h->definition_levels_byte_length)) return false; break;
      case 6: if (!r->ReadI32(&h->repetition_levels_byte_length)) return false; break;
      case 7: if (!r->ReadBool(ft, &h->is_compressed)) return false; break;
      case 8:
        if (!ParseStatistics(r, &h->statistics)) return false;
        h->has_statistics = true;
        break;
      default: if (!r->SkipField(ft)) return false; break;
    }
  }
}

bool ParsePageHeader(const uint8_t* data, size_t len, PageHeader* ph) {
  CompactReader r(data, len);
  {
    CompactReader::StructScope scope(&r);
    for (;;) {
      int32_t fid = 0, ft = 0;
      bool stop = false;
      if (!r.ReadFieldHeader(&fid, &ft, &stop)) return false;
      if (stop) break;
      switch (fid) {
        case 1: if (!r.ReadI32(&ph->type)) return false; break;
        case 2: if (!r.ReadI32(&ph->uncompressed_page_size)) return false; break;
        case 3: if (!r.ReadI32(&ph->compressed_page_size)) return false; break;
        case 4:
          if (!r.ReadI32(&ph->crc)) return false;
          ph->has_crc = true;
          break;
        case 5:
          if (!ParseDataPageHeader(&r, &ph->data_page_header)) return false;
          ph->has_data_page_header = true;
          break;
        case 7:
          if (!ParseDictionaryPageHeader(&r, &ph->dictionary_page_header)) return false;
          ph->has_dictionary_page_header = true;
          break;
        case 8:
          if (!ParseDataPageHeaderV2(&r, &ph->data_page_header_v2)) return false;
          ph->has_data_page_header_v2 = true;
          break;
        default: if (!r.SkipField(ft)) return false; break;
      }
    }
  }
  ph->header_size = r.pos();
  return true;
}

// ===========================================================================
// Snappy raw-block decompressor
//
// Format: varint uncompressed length, then a stream of tags:
//   tag&3 == 0 literal : len-1 = tag>>2 (if tag>>2 < 60) else
//                        (tag>>2)-59 little-endian bytes hold len-1
//   tag&3 == 1 copy-1B : len = ((tag>>2)&7)+4, offset = ((tag>>5)<<8)|next
//   tag&3 == 2 copy-2B : len = (tag>>2)+1, offset = next 2 bytes LE
//   tag&3 == 3 copy-4B : len = (tag>>2)+1, offset = next 4 bytes LE
// ===========================================================================

bool SnappyDecompress(const uint8_t* in, size_t in_len, std::string* out,
                      std::string* err) {
  size_t p = 0;
  uint64_t ulen = 0;
  int shift = 0;
  for (;;) {
    if (p >= in_len) {
      *err = "snappy: truncated uncompressed-length varint";
      return false;
    }
    uint8_t b = in[p++];
    ulen |= static_cast<uint64_t>(b & 0x7f) << shift;
    if ((b & 0x80) == 0) break;
    shift += 7;
    if (shift > 63) {
      *err = "snappy: uncompressed-length varint overflow";
      return false;
    }
  }
  if (ulen > (1u << 28)) {
    *err = "snappy: implausible uncompressed length";
    return false;
  }
  out->clear();
  out->reserve(static_cast<size_t>(ulen));
  while (p < in_len) {
    uint8_t tag = in[p++];
    uint32_t kind = tag & 0x03;
    if (kind == 0) {
      uint64_t len = tag >> 2;
      if (len < 60) {
        len += 1;
      } else {
        uint32_t nbytes = static_cast<uint32_t>(len - 59);  // 1..4
        if (p + nbytes > in_len) {
          *err = "snappy: truncated literal length";
          return false;
        }
        uint64_t v = 0;
        for (uint32_t i = 0; i < nbytes; ++i) v |= static_cast<uint64_t>(in[p + i]) << (8 * i);
        p += nbytes;
        len = v + 1;
      }
      if (p + len > in_len) {
        *err = "snappy: truncated literal body";
        return false;
      }
      out->append(reinterpret_cast<const char*>(in + p), static_cast<size_t>(len));
      p += static_cast<size_t>(len);
    } else {
      uint64_t len = 0;
      uint64_t offset = 0;
      if (kind == 1) {
        len = ((tag >> 2) & 0x07) + 4;
        if (p + 1 > in_len) {
          *err = "snappy: truncated copy-1 offset";
          return false;
        }
        offset = (static_cast<uint64_t>(tag >> 5) << 8) | in[p];
        p += 1;
      } else if (kind == 2) {
        len = (tag >> 2) + 1;
        if (p + 2 > in_len) {
          *err = "snappy: truncated copy-2 offset";
          return false;
        }
        offset = static_cast<uint64_t>(in[p]) | (static_cast<uint64_t>(in[p + 1]) << 8);
        p += 2;
      } else {
        len = (tag >> 2) + 1;
        if (p + 4 > in_len) {
          *err = "snappy: truncated copy-4 offset";
          return false;
        }
        offset = static_cast<uint64_t>(in[p]) | (static_cast<uint64_t>(in[p + 1]) << 8) |
                 (static_cast<uint64_t>(in[p + 2]) << 16) |
                 (static_cast<uint64_t>(in[p + 3]) << 24);
        p += 4;
      }
      if (offset == 0 || offset > out->size()) {
        *err = "snappy: copy offset out of range";
        return false;
      }
      // Byte-by-byte forward copy: source and destination may overlap.
      size_t src = out->size() - static_cast<size_t>(offset);
      for (uint64_t i = 0; i < len; ++i) out->push_back((*out)[src + static_cast<size_t>(i)]);
    }
  }
  if (out->size() != static_cast<size_t>(ulen)) {
    *err = "snappy: decompressed size mismatch";
    return false;
  }
  return true;
}

// ===========================================================================
// RLE / bit-packed hybrid + plain bit-packed decoders
// ===========================================================================

uint64_t LowMask(int bit_width) {
  if (bit_width <= 0) return 0;
  if (bit_width >= 64) return ~static_cast<uint64_t>(0);
  return (static_cast<uint64_t>(1) << bit_width) - 1;
}

// Decodes `count` values of `bit_width` bits from an RLE/bit-packed hybrid
// stream that has no length prefix (length is implicit: we stop at `count`).
//
// Layout of a run header: varint h.
//   h & 1  -> bit-packed run of (h>>1) groups of 8 values
//   else   -> RLE run of (h>>1) repetitions of one (bit_width+7)/8-byte value
bool DecodeRleBitPacked(const uint8_t* data, size_t len, int bit_width, int64_t count,
                        std::vector<int64_t>* out, std::string* err) {
  if (bit_width < 0 || bit_width > 64) {
    *err = "RLE: invalid bit width";
    return false;
  }
  const int byte_width = (bit_width + 7) / 8;
  size_t pos = 0;
  int64_t produced = 0;
  while (produced < count) {
    size_t pos_before = pos;
    int64_t produced_before = produced;
    uint64_t header = 0;
    int shift = 0;
    for (;;) {
      if (pos >= len) {
        *err = "RLE: truncated run header";
        return false;
      }
      uint8_t b = data[pos++];
      header |= static_cast<uint64_t>(b & 0x7f) << shift;
      if ((b & 0x80) == 0) break;
      shift += 7;
      if (shift > 63) {
        *err = "RLE: run header varint overflow";
        return false;
      }
    }
    if ((header & 1) != 0) {
      // Bit-packed run.
      uint64_t groups = header >> 1;
      if (groups == 0) {
        *err = "RLE: zero-length bit-packed run";
        return false;
      }
      size_t nbytes = static_cast<size_t>(groups) * static_cast<size_t>(bit_width);
      if (pos + nbytes > len) {
        *err = "RLE: truncated bit-packed run";
        return false;
      }
      const uint8_t* p = data + pos;
      uint64_t nvals = groups * 8;
      const uint64_t mask = LowMask(bit_width);
      for (uint64_t i = 0; i < nvals && produced < count; ++i) {
        uint64_t v = 0;
        if (bit_width > 0) {
          uint64_t bitoff = i * static_cast<uint64_t>(bit_width);
          size_t byteoff = static_cast<size_t>(bitoff >> 3);
          size_t avail = nbytes - byteoff;
          size_t take = avail < 8 ? avail : 8;
          uint64_t window = 0;
          for (size_t k = 0; k < take; ++k) {
            window |= static_cast<uint64_t>(p[byteoff + k]) << (8 * k);
          }
          v = (window >> (bitoff & 7)) & mask;
        }
        out->push_back(static_cast<int64_t>(v));
        ++produced;
      }
      pos += nbytes;
    } else {
      // RLE run: one value repeated.
      uint64_t run = header >> 1;
      uint64_t v = 0;
      if (pos + static_cast<size_t>(byte_width) > len) {
        *err = "RLE: truncated repeated value";
        return false;
      }
      for (int k = 0; k < byte_width; ++k) v |= static_cast<uint64_t>(data[pos + k]) << (8 * k);
      pos += static_cast<size_t>(byte_width);
      for (uint64_t i = 0; i < run && produced < count; ++i) {
        out->push_back(static_cast<int64_t>(v));
        ++produced;
      }
    }
    if (produced == produced_before && pos == pos_before) {
      *err = "RLE: no progress decoding run (corrupt stream)";
      return false;
    }
  }
  return true;
}

// Deprecated BIT_PACKED level encoding: values are bit-packed LSB-first with no
// run headers and no length prefix.
bool DecodeBitPackedRaw(const uint8_t* data, size_t len, int bit_width, int64_t count,
                        std::vector<int64_t>* out, std::string* err) {
  uint64_t need_bits = static_cast<uint64_t>(count) * static_cast<uint64_t>(bit_width);
  uint64_t need_bytes = (need_bits + 7) / 8;
  if (need_bytes > len) {
    *err = "BIT_PACKED: truncated level data";
    return false;
  }
  const uint64_t mask = LowMask(bit_width);
  for (int64_t i = 0; i < count; ++i) {
    uint64_t v = 0;
    if (bit_width > 0) {
      uint64_t bitoff = static_cast<uint64_t>(i) * static_cast<uint64_t>(bit_width);
      size_t byteoff = static_cast<size_t>(bitoff >> 3);
      size_t avail = static_cast<size_t>(need_bytes) - byteoff;
      size_t take = avail < 8 ? avail : 8;
      uint64_t window = 0;
      for (size_t k = 0; k < take; ++k) window |= static_cast<uint64_t>(data[byteoff + k]) << (8 * k);
      v = (window >> (bitoff & 7)) & mask;
    }
    out->push_back(static_cast<int64_t>(v));
  }
  return true;
}

int BitWidthForMaxLevel(int max_level) {
  int w = 0;
  while ((1 << w) <= max_level) ++w;
  return w;
}

// ===========================================================================
// PLAIN decoders
// ===========================================================================

uint32_t ReadLE32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t ReadLE64(const uint8_t* p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
  return v;
}

// Appends `count` PLAIN-encoded values. `consumed` receives the bytes used.
bool DecodePlain(const uint8_t* data, size_t len, int32_t physical_type,
                 int32_t type_length, int64_t count, ColumnData* out,
                 size_t* consumed, std::string* err) {
  size_t pos = 0;
  switch (physical_type) {
    case PT_BOOLEAN: {
      size_t nbytes = static_cast<size_t>((count + 7) / 8);
      if (nbytes > len) {
        *err = "PLAIN(BOOLEAN): truncated data";
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        uint8_t bit = (data[i >> 3] >> (i & 7)) & 1;
        out->values.push_back(bit ? 1 : 0);
      }
      pos = nbytes;
      break;
    }
    case PT_INT32: {
      size_t need = static_cast<size_t>(count) * 4;
      if (need > len) {
        *err = "PLAIN(INT32): truncated data";
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        int32_t v = static_cast<int32_t>(ReadLE32(data + pos));
        out->values.push_back(v);
        pos += 4;
      }
      break;
    }
    case PT_INT64: {
      size_t need = static_cast<size_t>(count) * 8;
      if (need > len) {
        *err = "PLAIN(INT64): truncated data";
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        int64_t v = static_cast<int64_t>(ReadLE64(data + pos));
        out->values.push_back(v);
        pos += 8;
      }
      break;
    }
    case PT_FLOAT: {
      size_t need = static_cast<size_t>(count) * 4;
      if (need > len) {
        *err = "PLAIN(FLOAT): truncated data";
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        uint32_t bits = ReadLE32(data + pos);
        float f;
        std::memcpy(&f, &bits, 4);
        out->doubles.push_back(static_cast<double>(f));
        pos += 4;
      }
      break;
    }
    case PT_DOUBLE: {
      size_t need = static_cast<size_t>(count) * 8;
      if (need > len) {
        *err = "PLAIN(DOUBLE): truncated data";
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        uint64_t bits = ReadLE64(data + pos);
        double d;
        std::memcpy(&d, &bits, 8);
        out->doubles.push_back(d);
        pos += 8;
      }
      break;
    }
    case PT_INT96: {
      size_t need = static_cast<size_t>(count) * 12;
      if (need > len) {
        *err = "PLAIN(INT96): truncated data";
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        out->bytes.push_back(std::string(reinterpret_cast<const char*>(data + pos), 12));
        pos += 12;
      }
      break;
    }
    case PT_BYTE_ARRAY: {
      for (int64_t i = 0; i < count; ++i) {
        if (pos + 4 > len) {
          *err = "PLAIN(BYTE_ARRAY): truncated length";
          return false;
        }
        uint32_t n = ReadLE32(data + pos);
        pos += 4;
        if (pos + n > len) {
          *err = "PLAIN(BYTE_ARRAY): truncated value";
          return false;
        }
        out->bytes.push_back(std::string(reinterpret_cast<const char*>(data + pos), n));
        pos += n;
      }
      break;
    }
    case PT_FIXED_LEN_BYTE_ARRAY: {
      if (type_length <= 0) {
        *err = "PLAIN(FIXED_LEN_BYTE_ARRAY): missing type_length";
        return false;
      }
      size_t flen = static_cast<size_t>(type_length);
      if (static_cast<size_t>(count) * flen > len) {
        *err = "PLAIN(FIXED_LEN_BYTE_ARRAY): truncated data";
        return false;
      }
      for (int64_t i = 0; i < count; ++i) {
        out->bytes.push_back(std::string(reinterpret_cast<const char*>(data + pos), flen));
        pos += flen;
      }
      break;
    }
    default:
      *err = std::string("PLAIN: unsupported physical type ") + PhysicalTypeName(physical_type);
      return false;
  }
  if (consumed != NULL) *consumed = pos;
  return true;
}

// Appends one dictionary entry to `out`.
bool AppendDictionaryEntry(const ColumnData& dict, int64_t index, ColumnData* out,
                           std::string* err) {
  switch (dict.physical_type) {
    case PT_BOOLEAN:
    case PT_INT32:
    case PT_INT64:
      if (index < 0 || index >= static_cast<int64_t>(dict.values.size())) {
        *err = "dictionary index out of range";
        return false;
      }
      out->values.push_back(dict.values[static_cast<size_t>(index)]);
      return true;
    case PT_FLOAT:
    case PT_DOUBLE:
      if (index < 0 || index >= static_cast<int64_t>(dict.doubles.size())) {
        *err = "dictionary index out of range";
        return false;
      }
      out->doubles.push_back(dict.doubles[static_cast<size_t>(index)]);
      return true;
    case PT_INT96:
    case PT_BYTE_ARRAY:
    case PT_FIXED_LEN_BYTE_ARRAY:
      if (index < 0 || index >= static_cast<int64_t>(dict.bytes.size())) {
        *err = "dictionary index out of range";
        return false;
      }
      out->bytes.push_back(dict.bytes[static_cast<size_t>(index)]);
      return true;
    default:
      *err = "dictionary: unsupported physical type";
      return false;
  }
}

std::string CodecUnsupportedMessage(int32_t codec) {
  std::ostringstream os;
  os << "unsupported compression codec " << CodecName(codec) << " (id " << codec
     << "); this reader supports UNCOMPRESSED and SNAPPY";
  return os.str();
}

bool Decompress(int32_t codec, const uint8_t* data, size_t len, std::string* out,
                std::string* err) {
  if (codec == CODEC_UNCOMPRESSED) {
    out->assign(reinterpret_cast<const char*>(data), len);
    return true;
  }
  if (codec == CODEC_SNAPPY) {
    return SnappyDecompress(data, len, out, err);
  }
  *err = CodecUnsupportedMessage(codec);
  return false;
}

std::string HexOf(const std::string& s) {
  static const char* kHex = "0123456789abcdef";
  std::string r;
  r.reserve(s.size() * 2);
  for (size_t i = 0; i < s.size(); ++i) {
    uint8_t b = static_cast<uint8_t>(s[i]);
    r.push_back(kHex[b >> 4]);
    r.push_back(kHex[b & 0x0f]);
  }
  return r;
}

std::string JoinInts(const std::vector<int32_t>& v) {
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) os << ", ";
    os << v[i] << " (" << EncodingName(v[i]) << ")";
  }
  os << "]";
  return os.str();
}

std::string JoinStrings(const std::vector<std::string>& v) {
  std::ostringstream os;
  os << "[";
  for (size_t i = 0; i < v.size(); ++i) {
    if (i) os << ", ";
    os << "\"" << v[i] << "\"";
  }
  os << "]";
  return os.str();
}

std::string DescribeStatistics(const StatisticsInfo& st) {
  std::ostringstream os;
  os << "{";
  bool first = true;
  if (st.has_null_count) {
    os << "null_count=" << st.null_count;
    first = false;
  }
  if (st.has_distinct_count) {
    if (!first) os << ", ";
    os << "distinct_count=" << st.distinct_count;
    first = false;
  }
  if (st.has_max_value) {
    if (!first) os << ", ";
    os << "max_value=" << HexOf(st.max_value);
    first = false;
  }
  if (st.has_min_value) {
    if (!first) os << ", ";
    os << "min_value=" << HexOf(st.min_value);
    first = false;
  }
  if (!first) os << ", ";
  os << "min_set=" << (st.has_min ? "1" : "0") << " max_set=" << (st.has_max ? "1" : "0");
  os << "}";
  return os.str();
}

}  // namespace

// ===========================================================================
// Reader
// ===========================================================================

Reader::Reader() : file_(NULL), file_size_(0) {}

Reader::~Reader() { Close(); }

void Reader::Close() {
  if (file_ != NULL) {
    std::fclose(file_);
    file_ = NULL;
  }
  path_.clear();
  file_size_ = 0;
  meta_ = FileMetaData();
  columns_.clear();
}

bool Reader::ReadAt(int64_t offset, size_t len, std::string* buf, std::string* err) const {
  if (file_ == NULL) {
    *err = "file not open";
    return false;
  }
  if (offset < 0 || static_cast<int64_t>(len) < 0 || offset + static_cast<int64_t>(len) > file_size_) {
    std::ostringstream os;
    os << "read out of range: offset=" << offset << " len=" << len << " file_size=" << file_size_;
    *err = os.str();
    return false;
  }
  if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
    *err = "fseek failed";
    return false;
  }
  buf->resize(len);
  if (len == 0) return true;
  if (std::fread(&(*buf)[0], 1, len, file_) != len) {
    *err = "short read";
    return false;
  }
  return true;
}

bool Reader::Open(const std::string& path, std::string* err) {
  Close();
  file_ = std::fopen(path.c_str(), "rb");
  if (file_ == NULL) {
    *err = "cannot open file: " + path;
    return false;
  }
  path_ = path;

  if (std::fseek(file_, 0, SEEK_END) != 0) {
    *err = "fseek(END) failed";
    Close();
    return false;
  }
  long sz = std::ftell(file_);
  if (sz < 12) {
    *err = "file too small to be a Parquet file";
    Close();
    return false;
  }
  file_size_ = sz;

  // Magic "PAR1" at start and end.
  std::string magic;
  if (!ReadAt(0, 4, &magic, err)) {
    Close();
    return false;
  }
  if (magic != "PAR1") {
    *err = "missing leading PAR1 magic";
    Close();
    return false;
  }
  if (!ReadAt(file_size_ - 4, 4, &magic, err)) {
    Close();
    return false;
  }
  if (magic != "PAR1") {
    *err = "missing trailing PAR1 magic";
    Close();
    return false;
  }

  // Footer: uint32 LE length at end-8, thrift FileMetaData before it.
  std::string lenbuf;
  if (!ReadAt(file_size_ - 8, 4, &lenbuf, err)) {
    Close();
    return false;
  }
  const uint8_t* lp = reinterpret_cast<const uint8_t*>(lenbuf.data());
  uint32_t footer_len = ReadLE32(lp);
  int64_t footer_start = file_size_ - 8 - static_cast<int64_t>(footer_len);
  if (footer_start < 4) {
    std::ostringstream os;
    os << "invalid footer length " << footer_len << " for file size " << file_size_;
    *err = os.str();
    Close();
    return false;
  }

  std::string footer;
  if (!ReadAt(footer_start, footer_len, &footer, err)) {
    Close();
    return false;
  }
  CompactReader r(reinterpret_cast<const uint8_t*>(footer.data()), footer.size());
  if (!ParseFileMetaData(&r, &meta_)) {
    *err = std::string("failed to parse FileMetaData: ") + r.error();
    Close();
    return false;
  }
  if (!BuildColumns(err)) {
    Close();
    return false;
  }
  return true;
}

// Walks the flat depth-first schema list, computing definition/repetition
// levels for every leaf.
bool Reader::BuildColumns(std::string* err) {
  columns_.clear();
  if (meta_.schema.empty()) {
    *err = "empty schema";
    return false;
  }
  // Recursive walk over the pre-order schema array.
  struct Walker {
    const std::vector<SchemaElement>* schema;
    int* pos;
    int max_def;
    int max_rep;
    std::vector<std::string>* path;
    std::vector<ColumnInfo>* out;
    std::string err;

    bool Visit() {
      int idx = *pos;
      if (idx >= static_cast<int>(schema->size())) {
        err = "schema tree truncated";
        return false;
      }
      const SchemaElement& se = (*schema)[static_cast<size_t>(idx)];
      (*pos)++;
      int def = max_def;
      int rep = max_rep;
      int r = se.has_repetition_type ? se.repetition_type : REP_REQUIRED;
      if (r == REP_OPTIONAL) ++def;
      if (r == REP_REPEATED) {
        ++def;
        ++rep;
      }
      bool added = false;
      if (idx != 0) {
        path->push_back(se.name);
        added = true;
      }
      int children = se.has_num_children ? se.num_children : 0;
      if (children <= 0) {
        if (idx != 0) {
          ColumnInfo ci;
          ci.name = se.name;
          ci.path = *path;
          ci.physical_type = se.type;
          ci.type_length = se.type_length;
          ci.repetition_type = r;
          ci.converted_type = se.converted_type;
          ci.max_definition_level = def;
          ci.max_repetition_level = rep;
          ci.schema_index = idx;
          ci.column_index = static_cast<int>(out->size());
          out->push_back(ci);
        }
      } else {
        for (int i = 0; i < children; ++i) {
          if (!Visit()) return false;
        }
      }
      if (added) path->pop_back();
      return true;
    }
  };

  int pos = 0;
  Walker w;
  w.schema = &meta_.schema;
  w.pos = &pos;
  w.max_def = 0;
  w.max_rep = 0;
  std::vector<std::string> path;
  w.path = &path;
  w.out = &columns_;
  if (!w.Visit()) {
    *err = "schema walk failed: " + w.err;
    return false;
  }
  if (columns_.empty()) {
    *err = "schema has no leaf columns";
    return false;
  }
  // Validate every row group exposes the same number of column chunks.
  for (size_t rg = 0; rg < meta_.row_groups.size(); ++rg) {
    if (meta_.row_groups[rg].columns.size() != columns_.size()) {
      std::ostringstream os;
      os << "row group " << rg << " has " << meta_.row_groups[rg].columns.size()
         << " column chunks but schema has " << columns_.size() << " leaves";
      *err = os.str();
      return false;
    }
  }
  return true;
}

int Reader::FindColumn(const std::string& name) const {
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (columns_[i].name == name) return static_cast<int>(i);
  }
  for (size_t i = 0; i < columns_.size(); ++i) {
    if (!columns_[i].path.empty() && columns_[i].path.back() == name) return static_cast<int>(i);
  }
  return -1;
}

bool Reader::ReadColumn(const std::string& name, ColumnData* out, std::string* err) {
  int idx = FindColumn(name);
  if (idx < 0) {
    *err = "no such column: " + name;
    return false;
  }
  return ReadColumn(static_cast<size_t>(idx), out, err);
}

bool Reader::ReadInt64Column(const std::string& name, std::vector<int64_t>* values,
                             std::vector<uint8_t>* is_null, std::string* err) {
  ColumnData data;
  if (!ReadColumn(name, &data, err)) return false;
  if (data.physical_type != PT_INT32 && data.physical_type != PT_INT64 &&
      data.physical_type != PT_BOOLEAN) {
    *err = std::string("column ") + name + " is " + PhysicalTypeName(data.physical_type) +
           ", not an integer type";
    return false;
  }
  *values = data.values;
  *is_null = data.is_null;
  return true;
}

bool Reader::ReadColumn(size_t index, ColumnData* out, std::string* err) {
  if (file_ == NULL) {
    *err = "file not open";
    return false;
  }
  if (index >= columns_.size()) {
    *err = "column index out of range";
    return false;
  }
  const ColumnInfo& info = columns_[index];
  out->Clear();
  out->physical_type = info.physical_type;

  for (size_t rg = 0; rg < meta_.row_groups.size(); ++rg) {
    const RowGroup& row_group = meta_.row_groups[rg];
    const ColumnChunk& chunk = row_group.columns[index];
    if (!chunk.has_meta_data) {
      std::ostringstream os;
      os << "row group " << rg << " column " << info.name << " has no ColumnMetaData";
      *err = os.str();
      return false;
    }
    if (!ReadChunk(chunk.meta_data, info, out, err)) return false;
    int64_t expected = 0;
    for (size_t k = 0; k <= rg; ++k) expected += meta_.row_groups[k].num_rows;
    if (out->NumRows() != expected) {
      std::ostringstream os;
      os << "row group " << rg << " of column " << info.name << " yielded " << out->NumRows()
         << " rows, expected " << expected << " (footer num_rows per row group)";
      *err = os.str();
      return false;
    }
  }
  if (out->NumRows() != meta_.num_rows) {
    std::ostringstream os;
    os << "row count mismatch for column " << info.name << ": decoded " << out->NumRows()
       << " rows, footer says " << meta_.num_rows;
    *err = os.str();
    return false;
  }
  return true;
}

bool Reader::DecodeDictionaryPage(const PageHeader& ph, const uint8_t* body, size_t body_len,
                                  const ColumnMetaData& cm, int32_t type_length, ColumnData* dict,
                                  std::string* err) const {
  if (!ph.has_dictionary_page_header) {
    *err = "DICTIONARY_PAGE without dictionary_page_header";
    return false;
  }
  const DictionaryPageHeader& h = ph.dictionary_page_header;
  if (h.encoding != ENC_PLAIN && h.encoding != ENC_PLAIN_DICTIONARY) {
    *err = std::string("dictionary page uses unsupported encoding ") + EncodingName(h.encoding);
    return false;
  }
  std::string raw;
  if (cm.codec == CODEC_UNCOMPRESSED) {
    raw.assign(reinterpret_cast<const char*>(body), body_len);
  } else {
    if (!Decompress(cm.codec, body, body_len, &raw, err)) return false;
    if (static_cast<int64_t>(raw.size()) < ph.uncompressed_page_size) {
      *err = "dictionary page decompressed shorter than uncompressed_page_size";
      return false;
    }
  }
  // Dictionary values are PLAIN encoded.
  dict->Clear();
  dict->physical_type = cm.type;
  size_t consumed = 0;
  if (!DecodePlain(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), cm.type,
                   type_length, h.num_values, dict, &consumed, err)) {
    *err = std::string("dictionary page: ") + *err;
    return false;
  }
  return true;
}

bool Reader::DecodeDataPage(const PageHeader& ph, const uint8_t* body, size_t body_len,
                            const ColumnInfo& info, const ColumnMetaData& cm,
                            const ColumnData& dict, bool has_dict, ColumnData* out,
                            int64_t* values_seen, std::string* err) const {
  const bool is_v2 = (ph.type == PAGE_DATA_V2);
  int64_t num_values = 0;
  int32_t encoding = -1;
  int32_t def_encoding = ENC_RLE;
  int32_t rep_encoding = ENC_RLE;
  int32_t def_len = 0;
  int32_t rep_len = 0;

  if (is_v2) {
    if (!ph.has_data_page_header_v2) {
      *err = "DATA_PAGE_V2 without data_page_header_v2";
      return false;
    }
    const DataPageHeaderV2& h = ph.data_page_header_v2;
    num_values = h.num_values;
    encoding = h.encoding;
    def_len = h.definition_levels_byte_length;
    rep_len = h.repetition_levels_byte_length;
  } else {
    if (!ph.has_data_page_header) {
      *err = "DATA_PAGE without data_page_header";
      return false;
    }
    const DataPageHeader& h = ph.data_page_header;
    num_values = h.num_values;
    encoding = h.encoding;
    def_encoding = h.definition_level_encoding;
    rep_encoding = h.repetition_level_encoding;
  }
  if (num_values < 0 || static_cast<int64_t>(def_len) < 0 || static_cast<int64_t>(rep_len) < 0) {
    *err = "negative page value/level count";
    return false;
  }

  // ---- obtain the raw (decompressed) page payload -------------------------
  const uint8_t* payload = body;
  size_t payload_len = body_len;
  std::string decompressed;
  if (is_v2) {
    // Levels are never compressed; only the values are, and only when
    // is_compressed is true.
    size_t levels_len = static_cast<size_t>(def_len) + static_cast<size_t>(rep_len);
    if (levels_len > body_len) {
      *err = "DATA_PAGE_V2 levels longer than page body";
      return false;
    }
    if (ph.data_page_header_v2.is_compressed && cm.codec != CODEC_UNCOMPRESSED) {
      if (!Decompress(cm.codec, body + levels_len, body_len - levels_len, &decompressed, err)) {
        return false;
      }
      std::string joined(reinterpret_cast<const char*>(body), levels_len);
      joined += decompressed;
      decompressed.swap(joined);
      payload = reinterpret_cast<const uint8_t*>(decompressed.data());
      payload_len = decompressed.size();
    }
  } else {
    if (cm.codec != CODEC_UNCOMPRESSED) {
      if (!Decompress(cm.codec, body, body_len, &decompressed, err)) return false;
      if (static_cast<int64_t>(decompressed.size()) < ph.uncompressed_page_size) {
        *err = "data page decompressed shorter than uncompressed_page_size";
        return false;
      }
      payload = reinterpret_cast<const uint8_t*>(decompressed.data());
      payload_len = decompressed.size();
    }
  }

  size_t pos = 0;

  // ---- repetition levels (only for repeated/nested columns) --------------
  if (info.max_repetition_level > 0) {
    std::vector<int64_t> rep_levels;
    const int bw = BitWidthForMaxLevel(info.max_repetition_level);
    if (is_v2) {
      if (rep_len > 0) {
        if (!DecodeRleBitPacked(payload + pos, static_cast<size_t>(rep_len), bw, num_values,
                                &rep_levels, err)) {
          *err = std::string("repetition levels: ") + *err;
          return false;
        }
        pos += static_cast<size_t>(rep_len);
      }
    } else {
      if (rep_encoding == ENC_RLE) {
        if (pos + 4 > payload_len) {
          *err = "truncated repetition-level length";
          return false;
        }
        uint32_t n = ReadLE32(payload + pos);
        pos += 4;
        if (pos + n > payload_len) {
          *err = "truncated repetition-level data";
          return false;
        }
        if (!DecodeRleBitPacked(payload + pos, n, bw, num_values, &rep_levels, err)) {
          *err = std::string("repetition levels: ") + *err;
          return false;
        }
        pos += n;
      } else if (rep_encoding == ENC_BIT_PACKED) {
        size_t need = static_cast<size_t>((static_cast<int64_t>(num_values) * bw + 7) / 8);
        if (pos + need > payload_len) {
          *err = "truncated repetition-level data";
          return false;
        }
        if (!DecodeBitPackedRaw(payload + pos, need, bw, num_values, &rep_levels, err)) return false;
        pos += need;
      } else {
        *err = std::string("unsupported repetition level encoding ") + EncodingName(rep_encoding);
        return false;
      }
    }
  }

  // ---- definition levels --------------------------------------------------
  std::vector<int64_t> def_levels;
  const int def_bw = BitWidthForMaxLevel(info.max_definition_level);
  if (info.max_definition_level > 0) {
    if (is_v2) {
      if (def_len > 0) {
        if (pos + static_cast<size_t>(def_len) > payload_len) {
          *err = "truncated definition levels";
          return false;
        }
        if (!DecodeRleBitPacked(payload + pos, static_cast<size_t>(def_len), def_bw, num_values,
                                &def_levels, err)) {
          *err = std::string("definition levels: ") + *err;
          return false;
        }
      }
      pos += static_cast<size_t>(def_len);
    } else {
      if (def_encoding == ENC_RLE) {
        if (pos + 4 > payload_len) {
          *err = "truncated definition-level length";
          return false;
        }
        uint32_t n = ReadLE32(payload + pos);
        pos += 4;
        if (pos + n > payload_len) {
          *err = "truncated definition-level data";
          return false;
        }
        if (!DecodeRleBitPacked(payload + pos, n, def_bw, num_values, &def_levels, err)) {
          *err = std::string("definition levels: ") + *err;
          return false;
        }
        pos += n;
      } else if (def_encoding == ENC_BIT_PACKED) {
        size_t need = static_cast<size_t>((num_values * def_bw + 7) / 8);
        if (pos + need > payload_len) {
          *err = "truncated definition-level data";
          return false;
        }
        if (!DecodeBitPackedRaw(payload + pos, need, def_bw, num_values, &def_levels, err)) {
          return false;
        }
        pos += need;
      } else {
        *err = std::string("unsupported definition level encoding ") + EncodingName(def_encoding);
        return false;
      }
    }
    if (static_cast<int64_t>(def_levels.size()) != num_values) {
      std::ostringstream os;
      os << "definition levels decoded " << def_levels.size() << " of " << num_values;
      *err = os.str();
      return false;
    }
  }

  // ---- count of present values (rows whose definition level is max) ------
  int64_t present = 0;
  if (info.max_definition_level == 0) {
    present = num_values;
  } else {
    for (int64_t i = 0; i < num_values; ++i) {
      if (def_levels[static_cast<size_t>(i)] == info.max_definition_level) ++present;
    }
  }

  // ---- values -------------------------------------------------------------
  // Values are decoded densely (present values only) and then spliced into the
  // row-aligned output below, so that values/doubles/bytes always have one
  // entry per row and can be indexed by row number.
  const uint8_t* vdata = payload + pos;
  size_t vlen = payload_len - pos;

  ColumnData page_vals;
  page_vals.physical_type = cm.type;

  if (encoding == ENC_PLAIN) {
    size_t consumed = 0;
    if (!DecodePlain(vdata, vlen, cm.type, info.type_length, present, &page_vals, &consumed, err)) {
      return false;
    }
  } else if (encoding == ENC_PLAIN_DICTIONARY || encoding == ENC_RLE_DICTIONARY) {
    if (!has_dict) {
      *err = "dictionary-encoded data page but no dictionary page was seen";
      return false;
    }
    if (present > 0) {
      if (vlen < 1) {
        *err = "truncated dictionary index bit width";
        return false;
      }
      int bit_width = vdata[0];
      if (bit_width < 0 || bit_width > 32) {
        *err = "invalid dictionary index bit width";
        return false;
      }
      std::vector<int64_t> indices;
      indices.reserve(static_cast<size_t>(present));
      if (!DecodeRleBitPacked(vdata + 1, vlen - 1, bit_width, present, &indices, err)) {
        *err = std::string("dictionary indices: ") + *err;
        return false;
      }
      for (size_t i = 0; i < indices.size(); ++i) {
        if (!AppendDictionaryEntry(dict, indices[i], &page_vals, err)) return false;
      }
    }
  } else {
    std::ostringstream os;
    os << "unsupported value encoding " << EncodingName(encoding) << " (id " << encoding
       << ") in " << PageTypeName(ph.type);
    *err = os.str();
    return false;
  }

  // Integrity: the decoded value count must match the definition levels.
  size_t decoded = page_vals.values.size() + page_vals.doubles.size() + page_vals.bytes.size();
  if (decoded != static_cast<size_t>(present)) {
    std::ostringstream os;
    os << "page holds " << decoded << " values but definition levels say " << present
       << " are present";
    *err = os.str();
    return false;
  }

  // ---- splice into the row-aligned output --------------------------------
  const bool int_family = (cm.type == PT_BOOLEAN || cm.type == PT_INT32 || cm.type == PT_INT64);
  const bool dbl_family = (cm.type == PT_FLOAT || cm.type == PT_DOUBLE);
  size_t k = 0;
  for (int64_t i = 0; i < num_values; ++i) {
    const bool row_is_null =
        (info.max_definition_level > 0 &&
         def_levels[static_cast<size_t>(i)] != info.max_definition_level);
    out->is_null.push_back(row_is_null ? 1 : 0);
    if (int_family) {
      out->values.push_back(row_is_null ? 0 : page_vals.values[k]);
    } else if (dbl_family) {
      out->doubles.push_back(row_is_null ? 0.0 : page_vals.doubles[k]);
    } else {
      out->bytes.push_back(row_is_null ? std::string() : page_vals.bytes[k]);
    }
    if (!row_is_null) ++k;
  }

  *values_seen += num_values;
  return true;
}

bool Reader::ReadChunk(const ColumnMetaData& cm, const ColumnInfo& info, ColumnData* out,
                       std::string* err) const {
  if (info.max_repetition_level > 0) {
    std::ostringstream os;
    os << "column '" << info.name << "' has max_repetition_level=" << info.max_repetition_level
       << " (REPEATED/nested); this reader only flattens non-repeated columns";
    *err = os.str();
    return false;
  }
  if (!cm.has_type || !cm.has_codec) {
    *err = "ColumnMetaData missing type or codec";
    return false;
  }
  if (cm.codec != CODEC_UNCOMPRESSED && cm.codec != CODEC_SNAPPY) {
    *err = CodecUnsupportedMessage(cm.codec);
    return false;
  }
  if (!cm.has_data_page_offset || cm.data_page_offset <= 0) {
    *err = "ColumnMetaData missing data_page_offset";
    return false;
  }
  if (cm.total_compressed_size <= 0) {
    *err = "ColumnMetaData has non-positive total_compressed_size";
    return false;
  }

  // Column projection: start at the dictionary page when it precedes the data
  // pages, and read only this chunk's byte range.
  int64_t start = cm.data_page_offset;
  if (cm.has_dictionary_page_offset && cm.dictionary_page_offset > 0 &&
      cm.dictionary_page_offset < start) {
    start = cm.dictionary_page_offset;
  }
  size_t chunk_len = static_cast<size_t>(cm.total_compressed_size);
  if (start + static_cast<int64_t>(chunk_len) > file_size_) {
    // Some writers report total_compressed_size that excludes the dictionary
    // page offset base; clamp defensively to file end and let page parsing stop.
    chunk_len = static_cast<size_t>(file_size_ - start);
  }
  std::string buf;
  if (!ReadAt(start, chunk_len, &buf, err)) return false;

  const uint8_t* data = reinterpret_cast<const uint8_t*>(buf.data());
  size_t pos = 0;
  int64_t values_seen = 0;
  bool saw_dictionary = false;
  ColumnData dict;
  dict.physical_type = cm.type;

  while (values_seen < cm.num_values) {
    if (pos >= buf.size()) {
      std::ostringstream os;
      os << "column chunk exhausted after " << values_seen << " of " << cm.num_values
         << " values (column " << info.name << ")";
      *err = os.str();
      return false;
    }
    PageHeader ph;
    if (!ParsePageHeader(data + pos, buf.size() - pos, &ph)) {
      *err = "failed to parse PageHeader";
      return false;
    }
    if (ph.header_size == 0 || ph.compressed_page_size < 0) {
      *err = "invalid PageHeader sizes";
      return false;
    }
    size_t header_and_body = ph.header_size + static_cast<size_t>(ph.compressed_page_size);
    if (pos + header_and_body > buf.size()) {
      std::ostringstream os;
      os << "page at chunk offset " << pos << " overruns column chunk ("
         << header_and_body << " > " << (buf.size() - pos) << ")";
      *err = os.str();
      return false;
    }
    const uint8_t* body = data + pos + ph.header_size;
    bool ok = true;
    switch (ph.type) {
      case PAGE_DICTIONARY:
        ok = DecodeDictionaryPage(ph, body, static_cast<size_t>(ph.compressed_page_size), cm,
                                  info.type_length, &dict, err);
        saw_dictionary = true;
        break;
      case PAGE_DATA:
      case PAGE_DATA_V2:
        ok = DecodeDataPage(ph, body, static_cast<size_t>(ph.compressed_page_size), info, cm, dict,
                            saw_dictionary, out, &values_seen, err);
        break;
      case PAGE_INDEX:
        // Index pages carry no values.
        break;
      default: {
        std::ostringstream os;
        os << "unsupported page type " << ph.type;
        *err = os.str();
        ok = false;
        break;
      }
    }
    if (!ok) return false;
    pos += header_and_body;
  }
  return true;
}

// ===========================================================================
// Diagnostics
// ===========================================================================

std::string Reader::DescribeMetadata() const {
  std::ostringstream os;
  os << "FileMetaData:\n";
  os << "  version   = " << meta_.version << "\n";
  os << "  num_rows  = " << meta_.num_rows << "\n";
  os << "  created_by= \"" << meta_.created_by << "\"\n";
  os << "  schema (" << meta_.schema.size() << " elements, pre-order):\n";
  for (size_t i = 0; i < meta_.schema.size(); ++i) {
    const SchemaElement& se = meta_.schema[i];
    os << "    [" << i << "] name=\"" << se.name << "\"";
    os << " type=" << (se.has_type ? PhysicalTypeName(se.type) : "-");
    os << " repetition=" << (se.has_repetition_type ? RepetitionName(se.repetition_type) : "-");
    if (se.has_num_children) os << " num_children=" << se.num_children;
    if (se.has_type_length) os << " type_length=" << se.type_length;
    if (se.has_converted_type) os << " converted_type=" << se.converted_type;
    if (se.has_field_id) os << " field_id=" << se.field_id;
    os << "\n";
  }
  os << "  leaf columns:\n";
  for (size_t i = 0; i < columns_.size(); ++i) {
    const ColumnInfo& c = columns_[i];
    os << "    [" << i << "] \"" << c.name << "\" path=" << JoinStrings(c.path)
       << " physical=" << PhysicalTypeName(c.physical_type)
       << " max_def=" << c.max_definition_level << " max_rep=" << c.max_repetition_level << "\n";
  }
  os << "  row_groups (" << meta_.row_groups.size() << "):\n";
  for (size_t rg = 0; rg < meta_.row_groups.size(); ++rg) {
    const RowGroup& g = meta_.row_groups[rg];
    os << "    [" << rg << "] num_rows=" << g.num_rows << " total_byte_size=" << g.total_byte_size
       << " columns=" << g.columns.size();
    if (g.has_total_compressed_size) os << " total_compressed_size=" << g.total_compressed_size;
    os << "\n";
    for (size_t ci = 0; ci < g.columns.size(); ++ci) {
      const ColumnChunk& cc = g.columns[ci];
      const ColumnMetaData& cm = cc.meta_data;
      os << "      chunk[" << ci << "] file_offset=" << cc.file_offset;
      if (cc.has_file_path && !cc.file_path.empty()) os << " file_path=\"" << cc.file_path << "\"";
      os << "\n";
      if (!cc.has_meta_data) {
        os << "        <no meta_data>\n";
        continue;
      }
      os << "        ColumnMetaData:\n";
      os << "          type                    = "
         << (cm.has_type ? PhysicalTypeName(cm.type) : "-") << "\n";
      os << "          encodings               = " << JoinInts(cm.encodings) << "\n";
      os << "          path_in_schema          = " << JoinStrings(cm.path_in_schema) << "\n";
      os << "          codec                   = "
         << (cm.has_codec ? CodecName(cm.codec) : "-") << "\n";
      os << "          num_values              = " << cm.num_values << "\n";
      os << "          total_uncompressed_size = " << cm.total_uncompressed_size << "\n";
      os << "          total_compressed_size   = " << cm.total_compressed_size << "\n";
      os << "          data_page_offset        = " << cm.data_page_offset << "\n";
      os << "          index_page_offset       = "
         << (cm.has_index_page_offset ? "set" : "<unset>");
      if (cm.has_index_page_offset) os << " (" << cm.index_page_offset << ")";
      os << "\n          dictionary_page_offset  = "
         << (cm.has_dictionary_page_offset ? "set" : "<unset>");
      if (cm.has_dictionary_page_offset) os << " (" << cm.dictionary_page_offset << ")";
      os << "\n";
      if (cm.has_statistics) os << "          statistics              = " << DescribeStatistics(cm.statistics) << "\n";
    }
  }
  os << "  key_value_metadata (" << meta_.key_value_metadata.size() << "):\n";
  for (size_t i = 0; i < meta_.key_value_metadata.size(); ++i) {
    const KeyValue& kv = meta_.key_value_metadata[i];
    os << "    [" << i << "] \"" << kv.key << "\" = ";
    if (!kv.has_value) {
      os << "<none>";
    } else if (kv.value.size() <= 120) {
      os << "\"" << kv.value << "\"";
    } else {
      os << "\"" << kv.value.substr(0, 116) << "...\" ("
         << kv.value.size() << " bytes)";
    }
    os << "\n";
  }
  return os.str();
}

}  // namespace obparquet

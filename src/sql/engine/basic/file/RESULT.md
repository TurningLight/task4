# Parquet reader — implementation notes and proof

Standalone, dependency-free Parquet reader for the OceanBase SQL engine file layer.

* Sources: `ob_parquet_reader.h`, `ob_parquet_reader.cpp`, `test_parquet.cpp`
* Build area: `/home/sheng/parquet_dev/` (`g++ -std=c++17 -O2`)
* C++11-compatible (also compiles with `-std=c++11`), no external libraries, no third-party deps.
* No git commands were run; nothing under `build_release/` was touched; no `make` in the repo.

Fixture: `/home/sheng/work/seekdb/tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64`
(the `.b64` is 617 bytes of base64 → a **461-byte** Parquet file), decoded once to `/tmp/simple.parquet`.

---

## 1. Answer first: what this fixture actually uses

| Aspect | Value found in this file |
| --- | --- |
| File format | Parquet v2 metadata (`FileMetaData.version = 2`) |
| Pages | **one `DATA_PAGE_V2`** — no dictionary page, no V1 pages, no index page |
| Value encoding | **`PLAIN`** (raw little-endian INT32) |
| Definition-level encoding | **`RLE` / bit-packed hybrid** (bit width 1; V2 puts it in the page body with no length prefix, length comes from `definition_levels_byte_length`) |
| Repetition levels | none (`repetition_levels_byte_length = 0`, `max_repetition_level = 0`) |
| **Compression codec** | **`SNAPPY`** ← this is the one that matters: the file is *not* UNCOMPRESSED |
| Physical type | `INT32`, column `c1`, `OPTIONAL` (max_def_level = 1) |
| Rows / values | 7 rows, 7 values, 0 NULLs |
| Footer | 334 bytes at offset 0x77; footer length 334 at 0x1C5 |
| Writer | `created_by = "Arrow2 - Native Rust implementation of Arrow"`, plus `ARROW:schema` (188-byte base64 Flatbuffer) in `key_value_metadata` |

So the fixture needs: Thrift-compact footer parse → **SNAPPY** decompression → **RLE/bit-packed** definition levels → **PLAIN** INT32. A reader that only implements UNCOMPRESSED would fail on this file. A ~150-line Snappy raw-block decompressor is included, and it is exercised for real here.

---

## 2. Byte-level layout of `/tmp/simple.parquet` (461 bytes)

Verified byte by byte, not inferred:

| Range | Len | Contents |
| --- | --- | --- |
| `0x000–0x003` | 4 | `PAR1` magic |
| `0x004–0x029` | 38 | `PageHeader` (thrift compact) |
| `0x02A–0x049` | 32 | page body = definition levels (2 bytes) + snappy block (30 bytes) |
| `0x04A–0x076` | 45 | **not data**: byte-identical to the `ColumnChunk` struct that also appears inside `FileMetaData` at `0x093–0x0BD`, plus two `0x00` STOP bytes. `ColumnChunk.file_offset = 74 = 0x4A` points exactly here |
| `0x077–0x1C4` | 334 | `FileMetaData` (thrift compact) |
| `0x1C5–0x1C8` | 4 | footer length, LE uint32 = **334** |
| `0x1C9–0x1CC` | 4 | `PAR1` magic |

The 45 dead bytes are harmless here because page iteration is driven by `data_page_offset` + `total_compressed_size`, never by "read until the footer". `4 + 70 = 74 = 0x4A` accounts for the whole chunk exactly, so the reader stops precisely at the start of the dead region.

### 2.1 Decoded `PageHeader` (offset 0x004, 38 bytes)

| Field | Value |
| --- | --- |
| `type` | `3` = `DATA_PAGE_V2` |
| `uncompressed_page_size` | `30` |
| `compressed_page_size` | `32` |
| `crc` | unset |
| `data_page_header_v2.num_values` | `7` |
| `data_page_header_v2.num_nulls` | `0` |
| `data_page_header_v2.num_rows` | `7` |
| `data_page_header_v2.encoding` | `0` = `PLAIN` |
| `data_page_header_v2.definition_levels_byte_length` | `2` |
| `data_page_header_v2.repetition_levels_byte_length` | `0` |
| `data_page_header_v2.is_compressed` | `true` |
| `data_page_header_v2.statistics` | `null_count = 0`, `max_value = 06 00 00 00` (6), `min_value = 00 00 00 00` (0) |

Consistency check that the reader also validates: `total_compressed_size = 70 = 38 (header) + 32 (body)` and `total_uncompressed_size = 68 = 38 + 30`. Both match.

### 2.2 Decoded page body (offset 0x02A, 32 bytes)

```
03 7f                                  definition levels (2 bytes, uncompressed in V2)
1c 6c 00 00 00 00 01 00 00 00 02 00    snappy block (30 bytes)
00 00 03 00 00 00 04 00 00 00 05 00
00 00 06 00 00 00
```

* Definition levels `03 7F`: RLE/bit-packed hybrid, bit width 1.
  `0x03` = header with LSB set → bit-packed run of 1 group = 8 values; `0x7F` = `0111 1111b` →
  levels `[1,1,1,1,1,1,1,0]`. The first 7 are `1 == max_def_level` → 7 non-NULL rows, 8th slot is padding.
  This is why the file needs no NULLs but still carries definition levels.
* Snappy block: `0x1C` = uncompressed length **28**; `0x6C` = tag, `0x6C & 3 == 0` → literal,
  length `(0x6C >> 2) + 1 = 28` → the following 28 bytes are the raw payload.
* 28 bytes = 7 × little-endian `int32` = `0, 1, 2, 3, 4, 5, 6`.

Interesting consequence: this particular SNAPPY payload is *larger* than uncompressed (30 vs 28 bytes) because the writer chose a single literal — that is why `compressed_page_size (32) > uncompressed_page_size (30)`. A reader that "optimises" on the assumption compressed < uncompressed breaks here.

### 2.3 Decoded `FileMetaData` (verbatim from `./test_parquet --meta /tmp/simple.parquet`)

```
FileMetaData:
  version   = 2
  num_rows  = 7
  created_by= "Arrow2 - Native Rust implementation of Arrow"
  schema (2 elements, pre-order):
    [0] name="root" type=- repetition=- num_children=1
    [1] name="c1" type=INT32 repetition=OPTIONAL
  leaf columns:
    [0] "c1" path=["c1"] physical=INT32 max_def=1 max_rep=0
  row_groups (1):
    [0] num_rows=7 total_byte_size=70 columns=1
      chunk[0] file_offset=74
        ColumnMetaData:
          type                    = INT32
          encodings               = [3 (RLE), 0 (PLAIN)]
          path_in_schema          = ["c1"]
          codec                   = SNAPPY
          num_values              = 7
          total_uncompressed_size = 68
          total_compressed_size   = 70
          data_page_offset        = 4
          index_page_offset       = <unset>
          dictionary_page_offset  = <unset>
          statistics              = {null_count=0, max_value=06000000, min_value=00000000, min_set=0 max_set=0}
  key_value_metadata (1):
    [0] "ARROW:schema" = "/////4QAAAAQAAAAAAAKAA4ADAALAAQACgAAABQAAAAAAAABBAAKAAwAAAAIAAQACgAAAAgAAAAIAAAAAAAAAAEAAAAUAAAAEAAUABAADgAPAAQAAAAI..." (188 bytes)
```

Raw thrift compact field ids observed (useful when comparing against `parquet.thrift`):

| Struct | Fields actually present |
| --- | --- |
| `FileMetaData` | `1` version(i32)=2, `2` schema(list<SchemaElement>,2), `3` num_rows(i64)=7, `4` row_groups(list,1), `5` key_value_metadata(list,1), `6` created_by(binary) |
| `SchemaElement[0]` | `4` name="root", `5` num_children=1 |
| `SchemaElement[1]` | `1` type=1(INT32), `3` repetition_type=1(OPTIONAL), `4` name="c1" |
| `RowGroup[0]` | `1` columns(list,1), `2` total_byte_size=70, `3` num_rows=7 |
| `ColumnChunk[0]` | `2` file_offset=74, `3` meta_data |
| `ColumnMetaData` | `1` type=1, `2` encodings=[3,0], `3` path_in_schema=["c1"], `4` codec=1(SNAPPY), `5` num_values=7, `6` total_uncompressed=68, `7` total_compressed=70, `9` data_page_offset=4, `12` statistics |
| `Statistics` | `3` null_count=0, `5` max_value, `6` min_value (deprecated fields `1`/`2` absent) |
| `PageHeader` | `1` type=3, `2` uncompressed_page_size=30, `3` compressed_page_size=32, `8` data_page_header_v2 |
| `DataPageHeaderV2` | `1` 7, `2` 0, `3` 7, `4` 0(PLAIN), `5` 2, `6` 0, `7` true, `8` statistics |

---

## 3. What the reader supports

Implemented and exercised:

| Area | Support |
| --- | --- |
| File structure | `PAR1` at head and tail, LE uint32 footer length at `end-8`, `FileMetaData` |
| Thrift | full compact-protocol decoder (varint/zigzag, field-id deltas, per-struct field-id scoping, bool-in-header, list/set/map, recursive struct skip for unknown fields) |
| Metadata | `SchemaElement` (incl. §logicalType skipping), `RowGroup`, `ColumnChunk`, `ColumnMetaData` (type, encodings, path_in_schema, codec, num_values, sizes, data/index/dictionary page offsets, statistics, KV metadata), `created_by` |
| Schema model | flat pre-order schema walk → leaf columns with computed `max_definition_level` / `max_repetition_level` |
| Page types | `DATA_PAGE` (v1), `DATA_PAGE_V2`, `DICTIONARY_PAGE`, `INDEX_PAGE` (skipped, carries no values) |
| Physical types | `BOOLEAN`, `INT32`, `INT64`, `FLOAT`, `DOUBLE`, `BYTE_ARRAY`, `FIXED_LEN_BYTE_ARRAY`, `INT96` (surfaced as 12 raw bytes) |
| Value encodings | `PLAIN`, `PLAIN_DICTIONARY`, `RLE_DICTIONARY` |
| Level encodings | `RLE`/bit-packed hybrid — V1 with the 4-byte LE length prefix, V2 with the length from the header — plus the deprecated `BIT_PACKED` |
| Codecs | `UNCOMPRESSED`, `SNAPPY` (own raw-block decoder: varint length, literal, copy-1/2/4 tags, overlapping copies) |
| Access | column list with names/types/levels, per-column read, per-row-group streaming, **column projection** |
| Safety | row-aligned output + NULL bitmap, decoded-value-count vs definition-level cross-check, per-row-group row-count cross-check, footer/`num_rows` cross-check |

Deliberately **not** supported — and reported as a clear error naming the culprit, never silently wrong data:

* codecs `GZIP`, `BROTLI`, `ZSTD`, `LZ4`, `LZ4_RAW`, `LZO` → `unsupported compression codec ZSTD (id 6); this reader supports UNCOMPRESSED and SNAPPY`
* encodings `DELTA_BINARY_PACKED`, `DELTA_LENGTH_BYTE_ARRAY`, `DELTA_BYTE_ARRAY`, `BYTE_STREAM_SPLIT` → `unsupported value encoding DELTA_BINARY_PACKED (id 5) in DATA_PAGE_V2`
* `REPEATED` / nested columns (`max_repetition_level > 0`) → explicit error; only flat columns are flattened into rows
* encryption, page index / bloom filter reading, `INT96` timestamp semantics

### API

```cpp
obparquet::Reader reader;
std::string err;
if (!reader.Open("/tmp/simple.parquet", &err)) { /* err has the reason */ }

reader.NumColumns();              // 1
reader.Column(0).name;            // "c1"
reader.Column(0).physical_type;   // obparquet::PT_INT32
reader.NumRows();                 // 7

std::vector<int64_t> values;
std::vector<uint8_t> is_null;     // 1 = NULL
if (!reader.ReadInt64Column("c1", &values, &is_null, &err)) { /* ... */ }

// generic form, any physical type
obparquet::ColumnData data;
reader.ReadColumn("c1", &data, &err);   // values/doubles/bytes + is_null, all row aligned
```

`ColumnData` is row aligned: `values` (or `doubles`, or `bytes`) and `is_null` all have exactly
`NumRows()` entries, so `values[row]` is valid for every row and `is_null[row] == 1` marks a NULL.
A NULL row's value slot holds a default (`0` / `0.0` / `""`) and must not be read as data.

Only the footer stays resident; a column read touches just that column's chunk bytes, one row group
at a time.

---

## 4. Proof: acceptance test passes

```console
$ cd /home/sheng/parquet_dev
$ g++ -std=c++17 -O2 -Wall -Wextra -o test_parquet test_parquet.cpp ob_parquet_reader.cpp
$ echo $?
0

$ ./test_parquet /tmp/simple.parquet
=== Parquet reader test ===
file        : /tmp/simple.parquet
columns     : 1
row_count   : 7
row_groups  : 1
created_by  : "Arrow2 - Native Rust implementation of Arrow"

column list:
  [0] name=c1 physical_type=INT32 repetition=OPTIONAL codec=SNAPPY max_def=1 max_rep=0

--- column 1/1: "c1" (INT32) ---
  rows: 7
  nulls: 0
  values (7): 0 1 2 3 4 5 6

RESULT: PASS
$ echo $?
0
```

Acceptance criteria: exactly the column `c1` ✓, 7 rows ✓, values `0 1 2 3 4 5 6` ✓.

Also compiles as C++11 (the language level the header targets):

```console
$ g++ -std=c++11 -O2 -Wall -Wextra -c ob_parquet_reader.cpp -o /tmp/ob_cxx11.o
$ echo $?
0
```

`-Wall -Wextra` is clean under both standards.

---

## 5. Extra verification (paths the fixture does not cover)

Because the fixture only contains one V2/PLAIN page, the remaining decoder paths were proven with
independently generated Parquet files (`gen_synth.py`, 292 lines, writes the wire format from the
spec — it shares no code with the C++ reader):

* `/tmp/synth2.parquet` — 2 columns, **2 row groups**, 16 rows:
  * `d1` `INT32 OPTIONAL`; RG0 = `DICTIONARY_PAGE` (PLAIN 10,20,30) + `DATA_PAGE_V2` with
    `RLE_DICTIONARY` indices and **2 NULLs**; RG1 = `DATA_PAGE` v1 with RLE definition levels
    carrying the 4-byte length prefix, whole page SNAPPY-compressed using **literal + copy-1 + copy-2**
    tags (overlapping copies).
  * `s1` `BYTE_ARRAY REQUIRED` (`max_def = 0`, so no level data at all), PLAIN.

```console
$ ./test_parquet /tmp/synth2.parquet
columns     : 2
row_count   : 16
row_groups  : 2
  [0] name=d1 physical_type=INT32 repetition=OPTIONAL codec=UNCOMPRESSED max_def=1 max_rep=0
  [1] name=s1 physical_type=BYTE_ARRAY repetition=REQUIRED codec=UNCOMPRESSED max_def=0 max_rep=0
--- column 1/2: "d1" (INT32) ---
  rows: 16
  nulls: 2
  values (16): 10 20 NULL 10 30 NULL 20 10 7 7 7 7 7 7 7 7
--- column 2/2: "s1" (BYTE_ARRAY) ---
  rows: 16
  nulls: 0
  values (16): "a" "bb" "ccc" "dddd" "e" "ff" "ggg" "hhhh" "zz" "zz" "zz" "zz" "zz" "zz" "zz" "zz"
RESULT: PASS
```

Both expected sequences match exactly, including NULL positions.

### Column projection is real, not claimed

`/tmp/synth2_corrupt.parquet` is the same file with `s1`'s first page **body** overwritten with
`0xFF` (`d1`'s chunks untouched). A full scan must now fail on `s1`, while reading only `d1` must
still succeed — which is only possible if `d1`'s decode never reads `s1`'s bytes:

```console
$ ./test_parquet /tmp/synth2_corrupt.parquet
--- column 1/2: "d1" (INT32) ---
  values (16): 10 20 NULL 10 30 NULL 20 10 7 7 7 7 7 7 7 7
--- column 2/2: "s1" (BYTE_ARRAY) ---
  READ FAILED: PLAIN(BYTE_ARRAY): truncated value
RESULT: FAIL

$ ./test_parquet --column d1 /tmp/synth2_corrupt.parquet
reading only column "d1" (column projection; other chunks stay untouched)
--- column 1/2: "d1" (INT32) ---
  values (16): 10 20 NULL 10 30 NULL 20 10 7 7 7 7 7 7 7 7
RESULT: PASS
```

### Unsupported input is reported, never faked

Encoding patched in place (`PageHeader.data_page_header_v2.encoding` at file offset `0x12`):

```console
$ ./test_parquet /tmp/simple_delta.parquet
--- column 1/1: "c1" (INT32) ---
  READ FAILED: unsupported value encoding DELTA_BINARY_PACKED (id 5) in DATA_PAGE_V2
RESULT: FAIL    (exit 1)
```

Codec patched in place (`ColumnMetaData.codec` at file offset `0xA3`):

```console
$ ./test_parquet /tmp/simple_zstd.parquet
--- column 1/1: "c1" (INT32) ---
  READ FAILED: unsupported compression codec ZSTD (id 6); this reader supports UNCOMPRESSED and SNAPPY
RESULT: FAIL    (exit 1)
```

File-level errors:

```console
$ ./test_parquet /tmp/simple_badmagic.parquet
FAIL open /tmp/simple_badmagic.parquet: missing leading PAR1 magic          (exit 1)

$ ./test_parquet /tmp/does_not_exist.parquet
FAIL open /tmp/does_not_exist.parquet: cannot open file: /tmp/does_not_exist.parquet   (exit 1)
```

---

## 6. Reproduce from scratch

```bash
base64 -d /home/sheng/work/seekdb/tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64 > /tmp/simple.parquet
cd /home/sheng/parquet_dev
g++ -std=c++17 -O2 -Wall -Wextra -o test_parquet test_parquet.cpp ob_parquet_reader.cpp
./test_parquet /tmp/simple.parquet          # -> c1, 7 rows, 0 1 2 3 4 5 6, RESULT: PASS
./test_parquet --meta /tmp/simple.parquet   # -> decoded footer dump (§2.3)
python3 gen_synth.py /tmp/synth2.parquet    # -> dictionary/NULL/multi-row-group/snappy-copy proof
./test_parquet /tmp/synth2.parquet
./test_parquet --column d1 /tmp/synth2_corrupt.parquet   # -> column-projection proof
```

---

## 7. Files

| Path | Role |
| --- | --- |
| `/home/sheng/parquet_dev/ob_parquet_reader.h` | reader API + metadata structs |
| `/home/sheng/parquet_dev/ob_parquet_reader.cpp` | implementation (thrift, snappy, RLE, PLAIN, pages) |
| `/home/sheng/parquet_dev/test_parquet.cpp` | standalone test |
| `/home/sheng/parquet_dev/gen_synth.py` | independent fixture generator (verification only) |
| `/home/sheng/parquet_dev/test_parquet` | built binary |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/ob_parquet_reader.h` | same file, deliverable location |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/ob_parquet_reader.cpp` | same file, deliverable location |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/test_parquet.cpp` | same file, deliverable location |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/RESULT.md` | this document |

Nothing was created or modified outside `/home/sheng/parquet_dev/` and
`/home/sheng/work/seekdb/src/sql/engine/basic/file/`.

The target directory already contained unrelated work in progress (`ob_csv_scanner.h/.cpp`,
`ob_jsonl_scanner.h/.cpp`, `test_scan.cpp`); those files were left untouched, and no git operation of
any kind was performed.

## 8. Notes / limitations for integration

* `fseek`/`ftell` are used with `long`; on Linux x86-64 `long` is 64-bit so files > 2 GiB are fine,
  but this would need `fseeko`/`_fseeki64` if compiled for 32-bit Windows.
* Only flat (non-`REPEATED`) columns are supported today. Extending to nested data means keeping
  repetition levels instead of discarding them (the level decoder already returns them; the row
  model around it is what is missing).
* `INT96` is exposed as 12 raw bytes rather than a timestamp, and `BYTE_ARRAY` values are returned
  as `std::string` (raw bytes, no UTF-8 validation).
* SNAPPY is the only compressed codec implemented; `ZSTD` is the most likely next one needed by
  real OceanBase data and currently fails fast with a named error.
* `total_compressed_size` is clamped to EOF defensively if a writer reports a range that overruns the
  file, and every page read is bounds-checked against the chunk buffer.

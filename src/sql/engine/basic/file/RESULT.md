# CSV + JSONL scanners with DuckDB-style schema inference — RESULT

Verdict: **both pinned acceptance checks pass verbatim.** Every schema line and every data
row below is byte-identical to the expected output in the task, and `./test_scan` exits 0.

- CSV schema — PASS (8/8 lines)
- CSV rows — PASS (3/3 lines)
- JSONL schema — PASS (6/6 lines, name + type form)
- JSONL rows — PASS (3/3 lines)

Fixtures were treated as read-only (see §9 for the hashes/mtimes proving they are untouched).

---

## 1. Files created

| Path | Role |
| --- | --- |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/ob_csv_scanner.h` | CSV scanner API (`ObCsvType`, `ObCsvColumnSchema`, `ObCsvValue`, `ObCsvScanner`) |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/ob_csv_scanner.cpp` | Buffered streaming record reader + inference + typed row iteration |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/ob_jsonl_scanner.h` | JSONL scanner API (`ObJsonlType`, `ObJsonlColumnSchema`, `ObJsonlValue`, `ObJsonlScanner`) |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/ob_jsonl_scanner.cpp` | Self-contained minimal JSON parser + union-schema inference + row iteration |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/test_scan.cpp` | Prints schema + rows for both fixtures and diffs them against the pinned expectations |
| `/home/sheng/work/seekdb/src/sql/engine/basic/file/RESULT.md` | This report |
| `/home/sheng/csv_dev/{ob_*.h,ob_*.cpp,test_scan.cpp}` | Build mirror (identical copies), plus `test_scan`, `edge_cases`, `edge_cases.cpp` |
| `C:\Users\sheng\Desktop\OceanBase\_preflight\*.sh` | The scripts used to write/build/run everything |

Both scanner translation units are self-contained (`g++ -c` succeeds with no other TU) and
compile clean under `-std=c++11`, `-std=c++17`, `-Wall -Wextra`.

## 2. Exact commands

```bash
# build (in the allowed scratch dir, no repo build, no make)
cd /home/sheng/csv_dev
cp /home/sheng/work/seekdb/src/sql/engine/basic/file/ob_csv_scanner.h \
   /home/sheng/work/seekdb/src/sql/engine/basic/file/ob_csv_scanner.cpp \
   /home/sheng/work/seekdb/src/sql/engine/basic/file/ob_jsonl_scanner.h \
   /home/sheng/work/seekdb/src/sql/engine/basic/file/ob_jsonl_scanner.cpp \
   /home/sheng/work/seekdb/src/sql/engine/basic/file/test_scan.cpp .
g++ -std=c++17 -O2 -Wall -Wextra -o test_scan \
    ob_csv_scanner.cpp ob_jsonl_scanner.cpp test_scan.cpp

# the pinned acceptance run
./test_scan

# C++11 claim + self-containment
g++ -std=c++11 -O2 -Wall -Wextra -fsyntax-only ob_csv_scanner.cpp ob_jsonl_scanner.cpp test_scan.cpp
g++ -std=c++17 -O2 -Wall -Wextra -c ob_csv_scanner.cpp   -o ob_csv_scanner.o
g++ -std=c++17 -O2 -Wall -Wextra -c ob_jsonl_scanner.cpp -o ob_jsonl_scanner.o

# optional probes
./test_scan --sample 3        # bounded sample (covers every field)
./test_scan --sample 2        # bounded sample that misses a field: loud error
./test_scan --stream-proof    # constant-memory proof on a generated 79 MB CSV

# the scratch edge-case harness (csv_dev only, not part of the deliverable API)
g++ -std=c++17 -O2 -Wall -Wextra -o edge_cases edge_cases.cpp ob_csv_scanner.cpp ob_jsonl_scanner.cpp
./edge_cases
```

Everything above was executed through the mandated wrapper, e.g.
`wsl -d Ubuntu-24.04 -u root -- bash /mnt/c/Users/sheng/Desktop/OceanBase/_preflight/16_build_run.sh`.

## 3. Full output of `./test_scan` (pinned run)

```text
===== CSV: /home/sheng/work/seekdb/tools/deploy/mysql_test/test_suite/file_sql/data/basic.csv =====
header_mode : PRESENT (pinned)
header sniff: true (row 1 all-VARCHAR && row 2 not)
sample_size : -1 (all rows)
rows        : 3 inferred, 3 iterated

-- schema (name type nullable) --
id BIGINT NO
name VARCHAR NO
score DOUBLE NO
active BOOLEAN NO
day DATE YES
created DATETIME YES
note VARCHAR NO
id__2 BIGINT NO

-- rows (NULL for nulls, \n as <NL>) --
1 Alice 98.5 true 2026-08-01 2026-08-01 10:11:12 hello,<NL>world 101
2 Bob 87 false NULL NULL plain 102
3 Chen 90.25 true 2026-08-03 2026-08-03 12:13:14 say "hi" 103

===== JSONL: /home/sheng/work/seekdb/tools/deploy/mysql_test/test_suite/file_sql/data/basic.jsonl =====
sample_size : -1 (all records)
records     : 3 inferred, 3 iterated

-- schema (first-seen field order; name type nullable) --
id BIGINT NO
name VARCHAR NO
score DOUBLE YES
active BOOLEAN NO
day VARCHAR YES
note VARCHAR YES

-- rows (NULL for nulls, \n as <NL>) --
1 Alice 98.5 true NULL NULL
2 Bob NULL false 2026-08-02 NULL
3 Chen 90 true NULL ok

===== ACCEPTANCE (pinned, expects sample_size = -1) =====
CSV schema     PASS   8/8  lines byte-identical
CSV rows       PASS   3/3  lines byte-identical
JSONL schema   PASS   6/6  lines byte-identical
JSONL rows     PASS   3/3  lines byte-identical
OVERALL         PASS
```

The JSONL schema dump shows nullability as a third token (`score YES`, `day YES`, `note YES`)
because the deliverable asks for name + type + nullable; the acceptance comparison itself uses
the pinned two-token form (name + type), which is what the `JSONL schema PASS 6/6` line checks.
The pinned list simply omits nullability; the values shown are the correct ones (a field is
nullable exactly when some record omits it or sets it to `null`: `score` is missing on record 2,
`day` on records 1 and 3, `note` on records 1 and 2).

## 4. API shape (same for both scanners)

```cpp
oceanbase::sql::ObCsvScanner scanner;
scanner.set_header_mode(ObCsvScanner::OB_CSV_HEADER_PRESENT);  // or ABSENT / AUTO
scanner.set_sample_size(-1);                                   // -1 = all rows, N = first N
scanner.open(path, &err);                    // validates readability
scanner.detect_header(&has_header, &err);    // sniffing heuristic, also used by AUTO
scanner.infer_schema(&err);                  // pass 1: names/types/nullability, then rewinds
scanner.schema();                            // vector<ObCsvColumnSchema>{name, type, nullable}
scanner.next_row(&row, &err);                // pass 2: vector<ObCsvValue> with NULL flags
scanner.eof(); scanner.inferred_row_count(); scanner.close();
```

`ObJsonlScanner` exposes the same surface minus header handling (JSONL has no header), with
`ObJsonlType` deliberately lacking DATE/DATETIME.

## 5. Bounded-sample behaviour (`--sample 3`, then `--sample 2`)

`./test_scan --sample 3` produces exactly the same schema and rows as the full-file run for both
fixtures, because 3 records cover every field, and the acceptance block reports
`SKIPPED: pinned acceptance requires the default full-file inference` (exit 0).

`./test_scan --sample 2` shows why `sample_size = -1` is the pinned mode for schema inference:

- CSV: the schema is still correct (the header fixes the columns) and all 3 rows decode, because
  rows 1-2 already exercise every type.
- JSONL: `note` first appears on record 3, so it is absent from the 2-record union. Record 3 then
  fails loudly rather than being silently dropped:
  `ERROR: next_row: line 3: field "note" is not in the inferred schema (a bounded sample_size may be unrepresentative)` (exit 1).

That is intentional: a sampled schema can be unrepresentative, and the scanner reports it instead
of truncating data. `sample_size = 0` is rejected with a clear message.

## 6. Streaming proof (no full materialization)

Generated scratch CSV (not the fixtures, deleted afterwards): a header plus N data rows, 8 columns,
one quoted field with an embedded comma. Memory is measured with `VmHWM` from `/proc/self/status`;
rows are counted and never retained, so the figure measures the scanner.

```text
===== STREAMING PROOF (generated scratch data, not the fixture) =====
Rows are counted, never retained, so peak RSS measures the scanner.
rows       file_bytes     peak_rss_kb  rows_read  first/last row
10000      717711         3960         10000      0 name0 ... note, 0 1000 | 9999 name9999 ... 10999
1000000    79559711       4024         1000000    0 name0 ... note, 0 1000 | 999999 name999999 ... 1000999
Peak RSS stays flat across a 100x larger input => the file is
streamed, not materialized; the row count and the first/last
row prove the whole file was read twice (infer + iterate).
```

79.5 MB input, 1,000,000 rows read twice (inference pass + iteration pass) in ~4 MB of peak RSS.
Mechanism: a fixed 64 KiB buffered byte reader assembles one record at a time (quoted `,`,
embedded newlines and `""` handled); the CSV scanner re-opens the file for pass 2 instead of
caching rows. JSONL reads one line at a time with `std::getline`.

## 7. Edge-case verification (71 checks, 0 failures)

A scratch harness in `/home/sheng/csv_dev/edge_cases.cpp` covers what the fixtures cannot:

```text
[CSV] headerless file
[CSV] CRLF, embedded comma, escaped quotes, quoted newline
[CSV] quoted empty vs unquoted empty
[CSV] all-NULL column, BOM, no trailing newline
[CSV] DATE + DATETIME widening, T separator, overflow
[CSV] header auto-detection
[CSV] error paths
[CSV] bounded sample diverges from full-file inference

[JSONL] escapes: \uXXXX, surrogates, quote/backslash/control
[JSONL] flattening rules
[JSONL] nesting / syntax rejections
[JSONL] blank lines, empty file, all-null, bounded sample

EDGE CASES: 71 checks, 0 failures -> PASS
```

Concretely verified: headerless files get `column0..columnN`; header sniffing returns true for
`id,name` + data and false for an all-data file; CRLF records; `"x,1"`, `"say ""hi"""`, quoted
newlines; quoted-empty (`""` → VARCHAR, non-NULL) vs unquoted-empty (→ NULL); a column that is
empty in every row becomes type `NULL`, nullable; UTF-8 BOM stripped; missing trailing newline;
`2026-08-03T12:13:14` normalizes to `2026-08-03 12:13:14`; DATE ∪ DATETIME → DATETIME with a
date-only value rendered `... 00:00:00`; `9223372036854775808` (2^63) widens to DOUBLE; ragged
row / empty file / unterminated quote all fail with precise line numbers; JSON `\" \\ \/ \b \f \n
\r \t`, `\u00e9`, `\u4e16\u754c` and the surrogate pair `\ud83d\ude00` decode to the right UTF-8;
nested object, nested array, array at top level, trailing garbage and `01` are all rejected with
specific messages; duplicate keys are last-wins with first-seen position; `-0` → BIGINT, `1e3` →
DOUBLE, 20-digit integer → DOUBLE; JSON date strings stay VARCHAR; blank lines skipped; empty
JSONL file yields an empty schema.

## 8. Semantics I had to decide (the task pinned the rest)

1. **Quoted cells are strings.** A quoted value never infers as BIGINT/DOUBLE/DATE/DATETIME, so
   `"2026-08-01"` stays VARCHAR. Rationale: quoting is the writer's explicit "this is text"
   signal; the pinned fixture is unaffected (`note` is quoted text, numbers are unquoted).
2. **Empty quoted field `""` is the empty string, not NULL** — only an empty *unquoted* field is
   NULL. This is the distinction the CSV spec of the task states, and it is what makes `note`
   `VARCHAR NO` in the acceptance output.
3. **DATE ∪ DATETIME widens to DATETIME**, and a date-only value inside a DATETIME column renders
   as `YYYY-MM-DD 00:00:00`. Any other type mismatch widens to VARCHAR; BIGINT → DOUBLE → VARCHAR
   is exactly as pinned. DATE/DATETIME never unify with numbers.
4. **All-NULL column** (every value empty) → type `NULL`, nullable `YES` (DuckDB's NULL type).
5. **All-null headerless/dateless columns and empty header names**: an empty header cell becomes
   `column<i>` (0-based); duplicate names get `__2`, `__3`, … in order of appearance, skipping
   names already taken.
6. **BOOLEAN is only `true`/`false`, case-insensitive.** `t`/`1`/`yes` stay VARCHAR/BIGINT. The
   task pinned exactly `true`/`false`; accepting more would silently change the type of a
   `0`/`1` column.
7. **Numeric syntax is strict**: `[+-]?(digits[.digits?] | .digits)([eE][+-]?digits)?`. `nan`,
   `inf` and hex are not numbers here, so a stray word can never become DOUBLE.
8. **DATE/DATETIME are calendar-validated** (month 1-12, day checked against month length and
   leap years), and DATETIME accepts both ` ` and `T` separators plus an optional `.fraction`.
9. **JSONL: numeric integrality is decided by the token, not the value.** `1e3` and `90.0` are
   DOUBLE even though they are whole numbers; `90` is BIGINT. An integral literal too large for
   int64 becomes DOUBLE. JSON `true/false` → BOOLEAN, JSON strings → VARCHAR always (no date
   inference), JSON `null` → NULL.
10. **JSONL union schema is first-seen order across records**, duplicate keys within one object are
    last-wins (keeping the key's first position), and a column is nullable iff some record omits
    the field or sets it to `null`. Fields first seen on a later record are marked nullable.
11. **JSONL strictness**: only flat objects; nested object/array as a top-level field is an error
    naming the field and line; non-object top level, trailing data, leading zeros, raw control
    characters in strings and unpaired `\u` surrogates are errors. Blank lines are skipped.
12. **Bounded sample caveat**: rows/fields not covered by the sample raise a conversion error that
    names the line/field and explains that a bounded `sample_size` may be unrepresentative. No
    silent truncation or coercion. `sample_size = 0` is rejected.
13. **`sample_size = -1` means "infer from every row"** (two streaming passes), which is the mode
    the pinned acceptance uses.
14. **DOUBLE rendering** is shortest-round-trip, preferring plain decimal for everyday magnitudes:
    `87.0` → `87`, `90.25` → `90.25`, `90.0` → `90`, `1e+20` → `1e+20`. This is what makes the
    pinned rows show `87` and `90` rather than `87.0`/`90.0`.

## 9. Constraints respected

- Files were created **only** under `/home/sheng/work/seekdb/src/sql/engine/basic/file/` (the five
  deliverables) and `/home/sheng/csv_dev/` (build mirror, binaries, scratch edge harness, temp
  generated data — all removed except the mirror and binaries). The mandated `_preflight` scripts
  live under `C:\Users\sheng\Desktop\OceanBase\_preflight\`.
- **No git command was run.** No build in the repo: nothing was written under
  `/home/sheng/work/seekdb/build_release/`, and `make` was never invoked.
- Compilation happened under `/home/sheng/csv_dev/` with plain `g++` (`-std=c++17 -O2`, plus a
  `-std=c++11` syntax check and `-Wall -Wextra` clean on both).
- The fixtures were never modified — same sizes and the original mtimes:

```text
a7ec34e2f8af792f4eab88e021af5832cfcd5bfcc4daa42c976289853c5b9789  .../file_sql/data/basic.csv
caacf710d399ad1f397507d437ecfefb99ab97d21d70d4cb4f3615b402777ff8  .../file_sql/data/basic.jsonl
-rw-r--r-- 1 sheng sheng 202 2026-10-26 23:32:04.038698147 +0800 .../basic.csv
-rw-r--r-- 1 sheng sheng 167 2026-10-26 23:32:04.038717604 +0800 .../basic.jsonl
```

## 10. One bug found and fixed during verification (in the test harness, not the scanners)

The first version of `--stream-proof` called the same helper used by the acceptance path, which
retains every rendered row for the diff. Peak RSS therefore tracked the file (175 MB on the 1M-row
probe) — measuring the harness, not the scanner, and making a "constant memory" claim false. The
proof now uses a row counter that keeps only the first/last rendered row, and the numbers above
(3.9 MB vs 4.0 MB) are the corrected measurement. The scanners themselves never changed for this.
Two earlier edge-harness expectations were also wrong (shortest round-trip of 2^63 needs 16
significant digits; a column present on every line is not nullable) — the scanner output was
correct in both cases and the expectations were fixed.

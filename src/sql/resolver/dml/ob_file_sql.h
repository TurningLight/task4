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

#ifndef OCEANBASE_SQL_RESOLVER_DML_OB_FILE_SQL_H_
#define OCEANBASE_SQL_RESOLVER_DML_OB_FILE_SQL_H_

#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"
#include "lib/string/ob_string.h"
#include "common/object/ob_obj_type.h"

namespace oceanbase
{
namespace sql
{

// Local file table functions: read_csv(), read_json(), read_parquet() and glob().
enum ObFileSqlType
{
  FILE_SQL_CSV = 0,
  FILE_SQL_JSON = 1,
  FILE_SQL_PARQUET = 2,
  FILE_SQL_GLOB = 3,
  FILE_SQL_MAX_TYPE
};

struct ObFileSqlColumn
{
  ObFileSqlColumn()
    : name_(), doc_key_(), type_(ObMaxType), length_(0), nullable_(true) {}
  common::ObString name_;
  // key of the column in the generated JSON document, empty means "c<ordinal>"
  common::ObString doc_key_;
  ObObjType type_;
  // declared char length, only used by string types
  int64_t length_;
  bool nullable_;
  TO_STRING_KV(K_(name), K_(doc_key), K_(type), K_(length), K_(nullable));
};

struct ObFileSqlTable
{
  ObFileSqlTable() : columns_(), json_doc_() {}
  // one entry per output column, in file order
  common::ObSEArray<ObFileSqlColumn, 8> columns_;
  // JSON array text with one object per row and one sub-object key per column
  common::ObString json_doc_;
};

struct ObFileSqlOptions
{
  ObFileSqlOptions()
    : header_(false), header_set_(false), sample_all_(true), format_() {}
  // whether the first line of a CSV file contains column names
  bool header_;
  // whether the caller specified the header option explicitly
  bool header_set_;
  // scan the whole file for schema inference
  bool sample_all_;
  // value of the format option, e.g. 'newline_delimited' for read_json
  common::ObString format_;
};

class ObFileSqlReader
{
public:
  // Map a table function name to its file type. Returns false for any other name.
  static bool is_file_table_function(const common::ObString &fn_name, ObFileSqlType &type);
  // Read a local file (or a directory when type is FILE_SQL_GLOB) and build a
  // JSON document that can be consumed by JSON_TABLE, together with its schema.
  static int read_table(common::ObIAllocator &alloc,
                        const common::ObString &path,
                        const ObFileSqlType type,
                        const ObFileSqlOptions &options,
                        ObFileSqlTable &table);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_RESOLVER_DML_OB_FILE_SQL_H_

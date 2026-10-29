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

#pragma once

#include "sql/engine/ob_operator.h"
#include "sql/engine/basic/ob_file_scan_utils.h"

namespace oceanbase
{
namespace sql
{

// Physical spec of a file table function. The column layout is fixed at plan
// time from the inferred file schema, so the scan simply fills those columns.
class ObFileTableSpec : public ObOpSpec
{
  OB_UNIS_VERSION_V(1);
public:
  ObFileTableSpec(common::ObIAllocator &alloc, const ObPhyOperatorType type)
    : ObOpSpec(alloc, type), format_(), path_(), secure_file_priv_(),
      delimiter_(','), quote_('"'), escape_('\\'), has_header_(true),
      null_str_(), is_glob_(false) {}
  DECLARE_VIRTUAL_TO_STRING;

  // Resolved at plan time: the argument expression is evaluated once before the
  // scan starts so the path is known when the operator opens.
  ObString format_;
  ObString path_;
  ObString secure_file_priv_;
  char delimiter_;
  char quote_;
  char escape_;
  bool has_header_;
  ObString null_str_;
  bool is_glob_;
};

// Scan operator over one local file (or the file list produced by glob()).
class ObFileTableOp : public ObOperator
{
public:
  ObFileTableOp(ObExecContext &exec_ctx, const ObOpSpec &spec, ObOpInput *input)
    : ObOperator(exec_ctx, spec, input), scanner_(nullptr), glob_paths_(), glob_index_(0),
      files_done_(false), initialized_(false) {}
  virtual ~ObFileTableOp() { release_scanner(); }

  virtual int inner_open() override;
  virtual int inner_rescan() override;
  virtual int inner_get_next_row() override;
  virtual int inner_get_next_batch(const int64_t max_row_cnt) override;
  virtual int inner_close() override;

private:
  int open_next_file();
  int emit_row(common::ObIArray<common::ObString> &fields,
               common::ObIArray<bool> &is_null);
  int cast_cell(ObExpr *expr, const common::ObString &cell, const bool is_null,
                const ObFileColumnType type);
  void release_scanner();
  enum ScannerKind { SCAN_NONE, SCAN_CSV, SCAN_JSON, SCAN_PARQUET, SCAN_GLOB };
  ObFileCsvScanner *scanner_;
  ObFileJsonScanner *json_scanner_;
  ObFileParquetScanner *parquet_scanner_;
  ScannerKind kind_ = SCAN_NONE;
  common::ObSEArray<common::ObString, 16> glob_paths_;
  int64_t glob_index_;
  bool files_done_;
  bool initialized_;
  ObFileSchema schema_;
};

} // namespace sql
} // namespace oceanbase

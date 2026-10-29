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

#ifndef OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_UTILS_H_
#define OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_UTILS_H_

#include "lib/string/ob_string.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/container/ob_se_array.h"

namespace oceanbase
{
namespace share
{

namespace schema
{
class ObColumnSchemaV2;
class ObTableSchema;
}

// The AI column declaration is not part of the SQL grammar. `column <type> AI COLUMN (<expr>)
// [WITH (...)]` is rewritten into `column <type> COMMENT '<marker>'` before parsing, and the
// marker is parsed back by the DDL resolver. The spec therefore travels with the schema, which
// keeps it available to the DDL executor, the DML maintenance path and the background worker
// without any extra catalog lookup.
static const char *const OB_AI_COLUMN_COMMENT_PREFIX = "aic:";

enum ObAiColumnType
{
  OB_AI_COLUMN_TYPE_INVALID = 0,
  OB_AI_COLUMN_TYPE_COMPLETE = 1,
  OB_AI_COLUMN_TYPE_EMBED = 2,
};

enum ObAiColumnHiddenKind
{
  OB_AI_COLUMN_HIDDEN_STATE = 0,
  OB_AI_COLUMN_HIDDEN_REQUEST = 1,
  OB_AI_COLUMN_HIDDEN_SPEC = 2,
  OB_AI_COLUMN_HIDDEN_MAX,
};

struct ObAiColumnSpec
{public:
  ObAiColumnSpec();
  ~ObAiColumnSpec() {}
  void reset();
  bool is_valid() const { return OB_AI_COLUMN_TYPE_INVALID != ai_type_ && !model_name_.empty(); }
  bool is_embedding() const { return OB_AI_COLUMN_TYPE_EMBED == ai_type_; }
  TO_STRING_KV(K_(ai_type), K_(model_name), K_(expr_text), K_(max_retries), K_(sync_mode),
               K_(dimension), K_(dep_columns));

public:
  int64_t ai_type_;
  int64_t max_retries_;
  int64_t sync_mode_;
  int64_t dimension_;
  common::ObString model_name_;
  common::ObString expr_text_;
  common::ObSEArray<common::ObString, 4> dep_columns_;
};

class ObAiColumnUtils
{
public:
  // An AI column of a table, as declared by the column comment.
  struct ColumnDesc
  {
  public:
    ColumnDesc() : column_id_(common::OB_INVALID_ID) {}
    TO_STRING_KV(K_(column_id), K_(column_name), K_(spec));
  public:
    uint64_t column_id_;
    common::ObString column_name_;
    ObAiColumnSpec spec_;
  };
  // Parsed form of `alter table <t> retry ai column <c> [where <predicate>]`.
  struct RetryStmt
  {
  public:
    RetryStmt() : end_pos_(0), has_extra_action_(false) {}
    common::ObString db_name_;
    common::ObString table_name_;
    common::ObString column_name_;
    common::ObString predicate_;
    int64_t end_pos_;
    bool has_extra_action_;
  };
  static int parse_retry_stmt(const common::ObString &sql, RetryStmt &retry, bool &is_retry);
  // `alter table ... retry ai column c where p` -> `update t set c = null where p`.
  // The pseudo column STATUS is mapped to the result state of the AI column.
  static int build_retry_update(common::ObIAllocator &alloc,
                                const RetryStmt &retry,
                                common::ObString &new_sql);
  // `column ... AI COLUMN (...)` -> `column ... COMMENT '<marker>'`
  static int rewrite_stmt(common::ObIAllocator &alloc,
                          const common::ObString &sql,
                          common::ObString &new_sql,
                          bool &rewritten);
  static int serialize_spec(common::ObIAllocator &alloc,
                            const ObAiColumnSpec &spec,
                            common::ObString &comment);
  static int parse_spec(const common::ObString &comment,
                        common::ObIAllocator &alloc,
                        ObAiColumnSpec &spec,
                        bool &is_ai_column);
  static int build_normalized_expr(common::ObIAllocator &alloc,
                                   const ObAiColumnSpec &spec,
                                   const common::ObString &column_type_name,
                                   common::ObString &normalized_expr);
  // Short type name used to pick the prompt instruction: json, int, number, or empty.
  static int get_column_type_name(const schema::ObColumnSchemaV2 &column,
                                  common::ObString &type_name);
  // AI columns declared by the table, in column id order.
  static int collect_ai_columns(common::ObIAllocator &alloc,
                                const schema::ObTableSchema &table_schema,
                                common::ObIArray<ColumnDesc> &columns,
                                int64_t &ai_column_count);
  // Identifier words of a statement, string literals and comments excluded. Backquotes are
  // stripped. Used to find the AI columns a statement actually references.
  static int collect_identifiers(common::ObIAllocator &alloc,
                                 const common::ObString &sql,
                                 common::ObIArray<common::ObString> &identifiers);
  // Qualified names of a statement, `[db.]table` pairs with the pieces stored separately.
  // `db_names` is empty at the same index as `table_names` when the statement has no qualifier.
  static int collect_table_refs(common::ObIAllocator &alloc,
                                const common::ObString &sql,
                                common::ObIArray<common::ObString> &db_names,
                                common::ObIArray<common::ObString> &table_names);
  // Splits `update [db.]<t> set <target> = ..., ... [where ...]`. `table_pieces` holds the raw
  // table name pieces, `set_targets` the assigned column names, `set_end` the offset just after
  // the last assignment (a second action can be appended there). `is_update` is false when the
  // statement is not an UPDATE.
  static int parse_update_stmt(common::ObIAllocator &alloc,
                               const common::ObString &sql,
                               common::ObIArray<common::ObString> &table_pieces,
                               common::ObIArray<common::ObString> &set_targets,
                               int64_t &set_end,
                               bool &is_update);
  static int build_hidden_column_name(common::ObIAllocator &alloc,
                                      const common::ObString &column_name,
                                      const int64_t kind,
                                      common::ObString &hidden_name);
  // The per row maintenance column that backs `kind` for the AI column `column_name`.
  static int build_hidden_column(common::ObIAllocator &alloc,
                                 const common::ObString &column_name,
                                 const int64_t kind,
                                 const uint64_t column_id,
                                 schema::ObColumnSchemaV2 &hidden);
  // Text of the instruction appended to the model prompt so that the completion fits the
  // declared column type. Empty when the column is a plain character type.
  static int get_type_instruction(const common::ObString &column_type_name,
                                  common::ObString &instruction);

private:
  static int parse_ai_column_clause(common::ObIAllocator &alloc,
                                    const common::ObString &expr_text,
                                    const common::ObString &option_text,
                                    ObAiColumnSpec &spec);
};

} // namespace share
} // namespace oceanbase

#endif // OCEANBASE_SHARE_AI_COLUMN_OB_AI_COLUMN_UTILS_H_

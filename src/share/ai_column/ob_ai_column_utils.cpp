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

#define USING_LOG_PREFIX SHARE
#include "share/ai_column/ob_ai_column_utils.h"
#include "share/ob_errno.h"
#include "share/schema/ob_column_schema.h"
#include "share/schema/ob_table_schema.h"
#include "lib/ob_define.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/string/ob_sql_string.h"
#include "lib/utility/ob_print_utils.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace share
{

namespace
{

const char aic_FIELD_SEP = '\x1f';
const char aic_DEP_SEP = '\x1e';

inline bool aic_is_ident_char(const char c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
         || '_' == c || '$' == c;
}

inline bool aic_is_blank(const char c)
{
  return ' ' == c || '\t' == c || '\n' == c || '\r' == c || '\f' == c || '\v' == c;
}

inline char aic_to_lower(const char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool aic_word_equal(const char *ptr, const int64_t len, const char *word)
{
  bool equal = true;
  const int64_t word_len = static_cast<int64_t>(strlen(word));
  if (len != word_len) {
    equal = false;
  } else {
    for (int64_t i = 0; equal && i < len; ++i) {
      if (aic_to_lower(ptr[i]) != aic_to_lower(word[i])) {
        equal = false;
      }
    }
  }
  return equal;
}

// Reads one identifier word starting at `pos`, returns the end offset.
int64_t aic_read_word(const ObString &sql, const int64_t pos, int64_t &end)
{
  end = pos;
  while (end < sql.length() && aic_is_ident_char(sql[end])) {
    ++end;
  }
  return end;
}

int64_t aic_skip_blanks(const ObString &sql, int64_t pos)
{
  while (pos < sql.length()) {
    const char c = sql[pos];
    if (aic_is_blank(c)) {
      ++pos;
    } else if ('-' == c && pos + 1 < sql.length() && '-' == sql[pos + 1]) {
      while (pos < sql.length() && '\n' != sql[pos]) {
        ++pos;
      }
    } else if ('#' == c) {
      while (pos < sql.length() && '\n' != sql[pos]) {
        ++pos;
      }
    } else if ('/' == c && pos + 1 < sql.length() && '*' == sql[pos + 1]) {
      pos += 2;
      while (pos + 1 < sql.length() && !('*' == sql[pos] && '/' == sql[pos + 1])) {
        ++pos;
      }
      pos = (pos + 1 < sql.length()) ? pos + 2 : sql.length();
    } else {
      break;
    }
  }
  return pos;
}

// Returns the offset just after the quoted literal that starts at `pos`.
int64_t aic_skip_quoted(const ObString &sql, const int64_t pos)
{
  const char quote = sql[pos];
  int64_t out = pos + 1;
  while (out < sql.length()) {
    if ('\\' == sql[out] && '\'' != quote) {
      out += 2;
    } else if (quote == sql[out]) {
      if (out + 1 < sql.length() && quote == sql[out + 1]) {
        out += 2;
      } else {
        ++out;
        break;
      }
    } else {
      ++out;
    }
  }
  return OB_MIN(out, sql.length());
}

int aic_find_matching_paren(const ObString &sql, const int64_t open_pos, int64_t &close_pos)
{
  int ret = OB_SUCCESS;
  int64_t depth = 0;
  int64_t pos = open_pos;
  close_pos = OB_INVALID_INDEX;
  while (OB_SUCC(ret) && pos < sql.length()) {
    const char c = sql[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      pos = aic_skip_quoted(sql, pos);
    } else if ('(' == c) {
      ++depth;
      ++pos;
    } else if (')' == c) {
      --depth;
      ++pos;
      if (0 == depth) {
        close_pos = pos - 1;
        break;
      }
    } else {
      ++pos;
    }
  }
  if (OB_INVALID_INDEX == close_pos) {
    ret = OB_ERR_PARSER_SYNTAX;
  }
  return ret;
}

int aic_trim_string(const ObString &in, ObString &out)
{
  int64_t start = 0;
  int64_t end = in.length();
  while (start < end && aic_is_blank(in[start])) {
    ++start;
  }
  while (end > start && aic_is_blank(in[end - 1])) {
    --end;
  }
  out.assign_ptr(in.ptr() + start, static_cast<int32_t>(end - start));
  return OB_SUCCESS;
}

int aic_unquote_literal(const ObString &in, ObString &out)
{
  int ret = OB_SUCCESS;
  ObString text;
  aic_trim_string(in, text);
  if (text.length() < 2) {
    ret = OB_ERR_PARSER_SYNTAX;
  } else {
    const char quote = text[0];
    if (('\'' != quote && '"' != quote) || text[text.length() - 1] != quote) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      out.assign_ptr(text.ptr() + 1, text.length() - 2);
    }
  }
  return ret;
}

int aic_hex_encode(const ObString &in, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  static const char *digits = "0123456789abcdef";
  for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); ++i) {
    const uint8_t byte = static_cast<uint8_t>(in[i]);
    if (OB_FAIL(out.append_fmt("%c%c", digits[byte >> 4], digits[byte & 0x0F]))) {
    }
  }
  return ret;
}

int aic_hex_decode(const ObString &in, ObSqlString &out)
{
  int ret = OB_SUCCESS;
  if (0 != in.length() % 2) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < in.length(); i += 2) {
    int hi = 0;
    int lo = 0;
    const char a = in[i];
    const char b = in[i + 1];
    if (a >= '0' && a <= '9') {
      hi = a - '0';
    } else if (a >= 'a' && a <= 'f') {
      hi = a - 'a' + 10;
    } else if (a >= 'A' && a <= 'F') {
      hi = a - 'A' + 10;
    } else {
      ret = OB_INVALID_ARGUMENT;
    }
    if (OB_SUCC(ret)) {
      if (b >= '0' && b <= '9') {
        lo = b - '0';
      } else if (b >= 'a' && b <= 'f') {
        lo = b - 'a' + 10;
      } else if (b >= 'A' && b <= 'F') {
        lo = b - 'A' + 10;
      } else {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    if (OB_SUCC(ret)) {
      const char decoded = static_cast<char>((hi << 4) | lo);
      if (OB_FAIL(out.append(&decoded, 1))) {
      }
    }
  }
  return ret;
}

const char *const aic_NON_DEP_WORDS[] = {
  "null", "true", "false", "and", "or", "not", "case", "when", "then", "else", "end",
  "cast", "as", "interval", "is", "like", "in", "between", "collate", "charset", "binary",
  "distinct", "separator", "using", "decimal", "signed", "unsigned", "char", "date",
  "datetime", "time", "json", "year", "day", "hour", "minute", "second", "microsecond",
};

bool aic_is_non_dep_word(const char *ptr, const int64_t len)
{
  bool found = false;
  for (size_t i = 0; !found && i < sizeof(aic_NON_DEP_WORDS) / sizeof(aic_NON_DEP_WORDS[0]); ++i) {
    found = aic_word_equal(ptr, len, aic_NON_DEP_WORDS[i]);
  }
  return found;
}

// Column references of the AI expression: identifiers that are neither function names, keywords
// nor parts of a string literal.
int aic_collect_dep_columns(ObIAllocator &alloc,
                        const ObString &expr,
                        common::ObIArray<ObString> &deps)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < expr.length()) {
    const char c = expr[pos];
    if ('\'' == c || '"' == c) {
      pos = aic_skip_quoted(expr, pos);
    } else if ('`' == c) {
      const int64_t end = aic_skip_quoted(expr, pos);
      ObString quoted;
      quoted.assign_ptr(expr.ptr() + pos + 1, static_cast<int32_t>(OB_MAX(end - pos - 2, 0)));
      bool exist = false;
      for (int64_t i = 0; !exist && i < deps.count(); ++i) {
        exist = (0 == deps.at(i).compare(quoted));
      }
      if (!exist && OB_FAIL(deps.push_back(quoted))) {
      }
      pos = end;
    } else if (aic_is_ident_char(c) && (0 == pos || !aic_is_ident_char(expr[pos - 1]))) {
      int64_t end = pos;
      aic_read_word(expr, pos, end);
      const bool is_call = (aic_skip_blanks(expr, end) < expr.length()
                            && '(' == expr[aic_skip_blanks(expr, end)]);
      const bool is_digit = (c >= '0' && c <= '9');
      if (!is_call && !is_digit && !aic_is_non_dep_word(expr.ptr() + pos, end - pos)) {
        bool exist = false;
        ObString name;
        name.assign_ptr(expr.ptr() + pos, static_cast<int32_t>(end - pos));
        for (int64_t i = 0; !exist && i < deps.count(); ++i) {
          exist = (0 == deps.at(i).compare(name));
        }
        if (!exist) {
          ObString stored;
          if (OB_FAIL(ob_write_string(alloc, name, stored))) {
          } else if (OB_FAIL(deps.push_back(stored))) {
          }
        }
      }
      pos = end;
    } else {
      ++pos;
    }
  }
  return ret;
}

int aic_parse_int(const ObString &text, int64_t &value)
{
  int ret = OB_SUCCESS;
  value = 0;
  if (0 == text.length()) {
    ret = OB_INVALID_ARGUMENT;
  }
  for (int64_t i = 0; OB_SUCC(ret) && i < text.length(); ++i) {
    if (text[i] < '0' || text[i] > '9') {
      ret = OB_INVALID_ARGUMENT;
    } else {
      value = value * 10 + (text[i] - '0');
    }
  }
  return ret;
}

int aic_parse_options(const ObString &text, int64_t &max_retries, int64_t &sync_mode)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < text.length()) {
    pos = aic_skip_blanks(text, pos);
    if (pos >= text.length()) {
      break;
    }
    int64_t key_end = pos;
    aic_read_word(text, pos, key_end);
    const char *key = text.ptr() + pos;
    const int64_t key_len = key_end - pos;
    pos = aic_skip_blanks(text, key_end);
    if (pos < text.length() && '=' == text[pos]) {
      ++pos;
      pos = aic_skip_blanks(text, pos);
    }
    if (pos >= text.length()) {
      ret = OB_ERR_PARSER_SYNTAX;
    } else {
      int64_t value_end = pos;
      if ('\'' == text[pos] || '"' == text[pos]) {
        value_end = aic_skip_quoted(text, pos);
      } else {
        while (value_end < text.length() && ',' != text[value_end] && aic_is_blank(text[value_end]) == false) {
          ++value_end;
        }
      }
      const char *value = text.ptr() + pos;
      int64_t value_len = value_end - pos;
      if (value_len >= 2 && ('\'' == value[0] || '"' == value[0])) {
        ++value;
        value_len -= 2;
      }
      if (aic_word_equal(key, key_len, "max_retries")) {
        max_retries = 0;
        for (int64_t i = 0; i < value_len; ++i) {
          if (value[i] < '0' || value[i] > '9') {
            ret = OB_ERR_PARSER_SYNTAX;
            break;
          }
          max_retries = max_retries * 10 + (value[i] - '0');
        }
      } else if (aic_word_equal(key, key_len, "sync_mode")) {
        sync_mode = aic_word_equal(value, value_len, "sync") ? 0 : 1;
      } else {
        ret = OB_NOT_SUPPORTED;
      }
      pos = value_end;
      if (OB_SUCC(ret)) {
        pos = aic_skip_blanks(text, pos);
        if (pos < text.length() && ',' == text[pos]) {
          ++pos;
        }
      }
    }
  }
  return ret;
}

int aic_add_identifier(ObIAllocator &alloc,
                       const ObString &name,
                       common::ObIArray<ObString> &identifiers)
{
  int ret = OB_SUCCESS;
  ObString trimmed;
  bool exist = false;
  aic_trim_string(name, trimmed);
  for (int64_t i = 0; !exist && i < identifiers.count(); ++i) {
    exist = (0 == identifiers.at(i).case_compare(trimmed));
  }
  if (!exist && !trimmed.empty()) {
    ObString stored;
    if (OB_FAIL(ob_write_string(alloc, trimmed, stored))) {
    } else if (OB_FAIL(identifiers.push_back(stored))) {
    }
  }
  return ret;
}

} // namespace

ObAiColumnSpec::ObAiColumnSpec()
  : ai_type_(OB_AI_COLUMN_TYPE_INVALID),
    max_retries_(0),
    sync_mode_(1),
    dimension_(0)
{
}

void ObAiColumnSpec::reset()
{
  ai_type_ = OB_AI_COLUMN_TYPE_INVALID;
  max_retries_ = 0;
  sync_mode_ = 1;
  dimension_ = 0;
  model_name_.reset();
  expr_text_.reset();
  dep_columns_.reset();
}

int ObAiColumnUtils::parse_ai_column_clause(ObIAllocator &alloc,
                                            const ObString &expr_text,
                                            const ObString &option_text,
                                            ObAiColumnSpec &spec)
{
  int ret = OB_SUCCESS;
  int64_t pos = aic_skip_blanks(expr_text, 0);
  int64_t name_end = pos;
  aic_read_word(expr_text, pos, name_end);
  const char *name = expr_text.ptr() + pos;
  const int64_t name_len = name_end - pos;
  ObString expr;
  if (aic_word_equal(name, name_len, "AI_COMPLETE")) {
    spec.ai_type_ = OB_AI_COLUMN_TYPE_COMPLETE;
  } else if (aic_word_equal(name, name_len, "AI_EMBED")) {
    spec.ai_type_ = OB_AI_COLUMN_TYPE_EMBED;
  } else {
    ret = OB_ERR_PARSER_SYNTAX;
    LOG_WARN("unsupported ai column function", K(expr_text), K(ret));
  }
  int64_t open = aic_skip_blanks(expr_text, name_end);
  int64_t close = OB_INVALID_INDEX;
  if (OB_FAIL(ret)) {
  } else if (open >= expr_text.length() || '(' != expr_text[open]
             || OB_FAIL(aic_find_matching_paren(expr_text, open, close))) {
    ret = OB_ERR_PARSER_SYNTAX;
    LOG_WARN("invalid ai column expression", K(expr_text), K(ret));
  } else {
    expr.assign_ptr(expr_text.ptr() + open + 1, static_cast<int32_t>(close - open - 1));
    // arguments: model name, expression, optional dimension (AI_EMBED only)
    int64_t arg_pos = 0;
    int64_t depth = 0;
    int64_t arg_start = 0;
    int64_t arg_index = 0;
    while (arg_pos <= expr.length()) {
      const bool at_end = (arg_pos == expr.length());
      const char c = at_end ? ',' : expr[arg_pos];
      if (!at_end && ('\'' == c || '"' == c || '`' == c)) {
        arg_pos = aic_skip_quoted(expr, arg_pos);
        continue;
      }
      if (!at_end && '(' == c) {
        ++depth;
      } else if (!at_end && ')' == c) {
        --depth;
      }
      if (at_end || (',' == c && 0 == depth)) {
        ObString arg;
        arg.assign_ptr(expr.ptr() + arg_start, static_cast<int32_t>(arg_pos - arg_start));
        ObString trimmed;
        aic_trim_string(arg, trimmed);
        if (0 == arg_index) {
          ObString model;
          if (OB_FAIL(aic_unquote_literal(trimmed, model))) {
            LOG_WARN("ai column model name must be a string literal", K(trimmed), K(ret));
          } else if (OB_FAIL(ob_write_string(alloc, model, spec.model_name_))) {
          }
        } else if (1 == arg_index) {
          if (OB_FAIL(ob_write_string(alloc, trimmed, spec.expr_text_))) {
          } else if (OB_FAIL(aic_collect_dep_columns(alloc, trimmed, spec.dep_columns_))) {
          }
        } else if (OB_AI_COLUMN_TYPE_EMBED == spec.ai_type_ && 2 == arg_index) {
          spec.dimension_ = 0;
          for (int64_t i = 0; OB_SUCC(ret) && i < trimmed.length(); ++i) {
            if (trimmed[i] < '0' || trimmed[i] > '9') {
              ret = OB_ERR_PARSER_SYNTAX;
            } else {
              spec.dimension_ = spec.dimension_ * 10 + (trimmed[i] - '0');
            }
          }
        } else {
          ret = OB_NOT_SUPPORTED;
        }
        ++arg_index;
        arg_start = arg_pos + 1;
      }
      ++arg_pos;
      if (OB_FAIL(ret)) {
        break;
      }
    }
    if (OB_SUCC(ret) && OB_FAIL(aic_parse_options(option_text, spec.max_retries_, spec.sync_mode_))) {
      LOG_WARN("failed to parse ai column options", K(option_text), K(ret));
    }
    if (OB_SUCC(ret) && (spec.model_name_.empty() || spec.expr_text_.empty())) {
      ret = OB_ERR_PARSER_SYNTAX;
    }
  }
  return ret;
}

int ObAiColumnUtils::serialize_spec(ObIAllocator &alloc,
                                    const ObAiColumnSpec &spec,
                                    ObString &comment)
{
  int ret = OB_SUCCESS;
  ObSqlString raw;
  ObSqlString deps;
  ObSqlString marker;
  for (int64_t i = 0; OB_SUCC(ret) && i < spec.dep_columns_.count(); ++i) {
    if (OB_FAIL(deps.append_fmt("%s%.*s", 0 == i ? "" : &aic_DEP_SEP,
                                spec.dep_columns_.at(i).length(),
                                spec.dep_columns_.at(i).ptr()))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(raw.append_fmt("%ld%c%.*s%c%.*s%c%ld%c%ld%c%ld%c%.*s",
                                    spec.ai_type_, aic_FIELD_SEP,
                                    spec.model_name_.length(), spec.model_name_.ptr(), aic_FIELD_SEP,
                                    spec.expr_text_.length(), spec.expr_text_.ptr(), aic_FIELD_SEP,
                                    spec.max_retries_, aic_FIELD_SEP,
                                    spec.sync_mode_, aic_FIELD_SEP,
                                    spec.dimension_, aic_FIELD_SEP,
                                    static_cast<int32_t>(deps.length()), deps.ptr()))) {
  } else if (OB_FAIL(marker.append(OB_AI_COLUMN_COMMENT_PREFIX))) {
  } else if (OB_FAIL(aic_hex_encode(ObString(static_cast<int32_t>(raw.length()), raw.ptr()), marker))) {
  } else if (OB_FAIL(ob_write_string(alloc, ObString(static_cast<int32_t>(marker.length()), marker.ptr()), comment))) {
  }
  return ret;
}

int ObAiColumnUtils::parse_spec(const ObString &comment,
                                ObIAllocator &alloc,
                                ObAiColumnSpec &spec,
                                bool &is_ai_column)
{
  int ret = OB_SUCCESS;
  spec.reset();
  is_ai_column = false;
  const int64_t prefix_len = static_cast<int64_t>(strlen(OB_AI_COLUMN_COMMENT_PREFIX));
  if (comment.length() > prefix_len
      && 0 == ObString(static_cast<int32_t>(prefix_len), comment.ptr()).case_compare(
                     ObString(static_cast<int32_t>(prefix_len), OB_AI_COLUMN_COMMENT_PREFIX))) {
    is_ai_column = true;
    ObString hex;
    hex.assign_ptr(comment.ptr() + prefix_len, comment.length() - prefix_len);
    ObSqlString raw;
    ObString fields[7];
    if (OB_FAIL(aic_hex_decode(hex, raw))) {
    } else {
      ObString raw_str(static_cast<int32_t>(raw.length()), raw.ptr());
      int32_t field_count = 0;
      int32_t start = 0;
      for (int32_t i = 0; OB_SUCC(ret) && i <= raw_str.length(); ++i) {
        if (i == raw_str.length() || aic_FIELD_SEP == raw_str[i]) {
          if (field_count >= 7) {
            ret = OB_INVALID_ARGUMENT;
          } else {
            fields[field_count++].assign_ptr(raw_str.ptr() + start, i - start);
          }
          start = i + 1;
        }
      }
      if (OB_FAIL(ret)) {
      } else if (7 != field_count) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        if (OB_FAIL(aic_parse_int(fields[0], spec.ai_type_))) {
        } else if (OB_FAIL(aic_parse_int(fields[3], spec.max_retries_))) {
        } else if (OB_FAIL(aic_parse_int(fields[4], spec.sync_mode_))) {
        } else if (OB_FAIL(aic_parse_int(fields[5], spec.dimension_))) {
        } else if (OB_FAIL(ob_write_string(alloc, fields[1], spec.model_name_))) {
        } else if (OB_FAIL(ob_write_string(alloc, fields[2], spec.expr_text_))) {
        } else {
          int32_t dep_start = 0;
          for (int32_t i = 0; OB_SUCC(ret) && i <= fields[6].length(); ++i) {
            if (i == fields[6].length() || aic_DEP_SEP == fields[6][i]) {
              if (i > dep_start) {
                ObString dep;
                if (OB_FAIL(ob_write_string(alloc,
                                            ObString(i - dep_start, fields[6].ptr() + dep_start),
                                            dep))) {
                } else if (OB_FAIL(spec.dep_columns_.push_back(dep))) {
                }
              }
              dep_start = i + 1;
            }
          }
        }
      }
    }
    if (OB_FAIL(ret)) {
      LOG_WARN("failed to parse ai column comment", K(ret), K(comment));
      spec.reset();
    }
  }
  return ret;
}

int ObAiColumnUtils::get_type_instruction(const ObString &column_type_name,
                                          ObString &instruction)
{
  int ret = OB_SUCCESS;
  if (0 == column_type_name.case_compare("json")) {
    instruction = ObString::make_string("Return exactly one valid JSON value.");
  } else if (0 == column_type_name.case_compare("int")) {
    instruction = ObString::make_string("Return exactly one base-10 integer value.");
  } else if (0 == column_type_name.case_compare("number")) {
    instruction = ObString::make_string("Return exactly one decimal number value.");
  } else {
    instruction.reset();
  }
  return ret;
}

int ObAiColumnUtils::build_normalized_expr(ObIAllocator &alloc,
                                           const ObAiColumnSpec &spec,
                                           const ObString &column_type_name,
                                           ObString &normalized_expr)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString instruction;
  if (OB_FAIL(get_type_instruction(column_type_name, instruction))) {
  } else if (OB_AI_COLUMN_TYPE_EMBED == spec.ai_type_) {
    if (spec.dimension_ > 0) {
      if (OB_FAIL(sql.append_fmt("AI_EMBED('%.*s', %.*s, %ld)",
                                 spec.model_name_.length(), spec.model_name_.ptr(),
                                 spec.expr_text_.length(), spec.expr_text_.ptr(),
                                 spec.dimension_))) {
      }
    } else if (OB_FAIL(sql.append_fmt("AI_EMBED('%.*s', %.*s)",
                                      spec.model_name_.length(), spec.model_name_.ptr(),
                                      spec.expr_text_.length(), spec.expr_text_.ptr()))) {
    }
  } else if (!instruction.empty()) {
    if (OB_FAIL(sql.append_fmt("AI_COMPLETE('%.*s', %.*s, '{\"system\":\"%.*s\"}')",
                               spec.model_name_.length(), spec.model_name_.ptr(),
                               spec.expr_text_.length(), spec.expr_text_.ptr(),
                               instruction.length(), instruction.ptr()))) {
    }
  } else if (OB_FAIL(sql.append_fmt("AI_COMPLETE('%.*s', %.*s)",
                                    spec.model_name_.length(), spec.model_name_.ptr(),
                                    spec.expr_text_.length(), spec.expr_text_.ptr()))) {
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc,
                                              ObString(static_cast<int32_t>(sql.length()), sql.ptr()),
                                              normalized_expr))) {
  }
  return ret;
}

int ObAiColumnUtils::build_hidden_column_name(ObIAllocator &alloc,
                                              const ObString &column_name,
                                              const int64_t kind,
                                              ObString &hidden_name)
{
  int ret = OB_SUCCESS;
  static const char *const suffixes[OB_AI_COLUMN_HIDDEN_MAX] = {
    "_ai_state", "_ai_req", "_ai_spec",
  };
  ObSqlString name;
  if (kind < 0 || kind >= OB_AI_COLUMN_HIDDEN_MAX) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(name.append_fmt("__%.*s%s",
                                     OB_MIN(column_name.length(), 100),
                                     column_name.ptr(),
                                     suffixes[kind]))) {
  } else if (OB_FAIL(ob_write_string(alloc,
                                     ObString(static_cast<int32_t>(name.length()), name.ptr()),
                                     hidden_name))) {
  }
  return ret;
}

int ObAiColumnUtils::build_hidden_column(ObIAllocator &alloc,
                                         const ObString &column_name,
                                         const int64_t kind,
                                         const uint64_t column_id,
                                         schema::ObColumnSchemaV2 &hidden)
{
  int ret = OB_SUCCESS;
  ObString hidden_name;
  hidden.reset();
  if (OB_FAIL(build_hidden_column_name(alloc, column_name, kind, hidden_name))) {
  } else if (OB_FAIL(hidden.set_column_name(hidden_name))) {
  } else {
    hidden.set_column_id(column_id);
    hidden.set_rowkey_position(0);
    hidden.set_index_position(0);
    hidden.set_tbl_part_key_pos(0);
    hidden.set_prev_column_id(UINT64_MAX);
    hidden.set_next_column_id(UINT64_MAX);
    hidden.set_nullable(true);
    hidden.set_is_hidden(true);
    switch (kind) {
      case OB_AI_COLUMN_HIDDEN_STATE: {
        hidden.set_data_type(ObIntType);
        hidden.set_collation_type(CS_TYPE_BINARY);
        hidden.set_charset_type(CHARSET_BINARY);
        break;
      }
      case OB_AI_COLUMN_HIDDEN_REQUEST: {
        hidden.set_data_type(ObVarcharType);
        hidden.set_data_length(128);
        hidden.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        hidden.set_charset_type(CHARSET_UTF8MB4);
        break;
      }
      case OB_AI_COLUMN_HIDDEN_SPEC: {
        hidden.set_data_type(ObVarcharType);
        hidden.set_data_length(512);
        hidden.set_collation_type(CS_TYPE_UTF8MB4_GENERAL_CI);
        hidden.set_charset_type(CHARSET_UTF8MB4);
        break;
      }
      default: {
        ret = OB_INVALID_ARGUMENT;
        break;
      }
    }
  }
  return ret;
}

int ObAiColumnUtils::parse_retry_stmt(const ObString &sql, RetryStmt &retry, bool &is_retry)
{
  int ret = OB_SUCCESS;
  is_retry = false;
  int64_t pos = aic_skip_blanks(sql, 0);
  int64_t word_end = pos;
  const char *words[] = {"alter", "table"};
  bool matched = true;
  for (int64_t i = 0; matched && i < 2; ++i) {
    if (pos >= sql.length() || !aic_is_ident_char(sql[pos])) {
      matched = false;
    } else {
      aic_read_word(sql, pos, word_end);
      matched = aic_word_equal(sql.ptr() + pos, word_end - pos, words[i]);
      pos = aic_skip_blanks(sql, word_end);
    }
  }
  if (matched) {
    // table name, optionally qualified and quoted
    ObString pieces[2];
    int64_t piece_count = 0;
    bool ok = true;
    while (ok && piece_count < 2) {
      if (pos >= sql.length()) {
        ok = false;
      } else if ('`' == sql[pos]) {
        const int64_t end = aic_skip_quoted(sql, pos);
        pieces[piece_count++].assign_ptr(sql.ptr() + pos, static_cast<int32_t>(end - pos));
        pos = aic_skip_blanks(sql, end);
      } else if (aic_is_ident_char(sql[pos])) {
        int64_t end = pos;
        aic_read_word(sql, pos, end);
        pieces[piece_count++].assign_ptr(sql.ptr() + pos, static_cast<int32_t>(end - pos));
        pos = aic_skip_blanks(sql, end);
      } else {
        ok = false;
      }
      if (ok && pos < sql.length() && '.' == sql[pos]) {
        pos = aic_skip_blanks(sql, pos + 1);
      } else {
        ok = (piece_count > 0);
        break;
      }
    }
    matched = ok;
    if (matched) {
      if (2 == piece_count) {
        retry.db_name_ = pieces[0];
        retry.table_name_ = pieces[1];
      } else {
        retry.table_name_ = pieces[0];
      }
      const char *keywords[] = {"retry", "ai", "column"};
      for (int64_t i = 0; matched && i < 3; ++i) {
        if (pos >= sql.length() || !aic_is_ident_char(sql[pos])) {
          matched = false;
        } else {
          aic_read_word(sql, pos, word_end);
          matched = aic_word_equal(sql.ptr() + pos, word_end - pos, keywords[i]);
          pos = aic_skip_blanks(sql, word_end);
        }
      }
    }
    if (matched) {
      if (pos < sql.length() && '`' == sql[pos]) {
        const int64_t end = aic_skip_quoted(sql, pos);
        retry.column_name_.assign_ptr(sql.ptr() + pos, static_cast<int32_t>(end - pos));
        pos = aic_skip_blanks(sql, end);
      } else if (pos < sql.length() && aic_is_ident_char(sql[pos])) {
        int64_t end = pos;
        aic_read_word(sql, pos, end);
        retry.column_name_.assign_ptr(sql.ptr() + pos, static_cast<int32_t>(end - pos));
        pos = aic_skip_blanks(sql, end);
      } else {
        matched = false;
      }
    }
    if (matched) {
      int64_t where_end = pos;
      if (pos < sql.length() && aic_is_ident_char(sql[pos])) {
        int64_t end = pos;
        aic_read_word(sql, pos, end);
        if (aic_word_equal(sql.ptr() + pos, end - pos, "where")) {
          where_end = aic_skip_blanks(sql, end);
        } else {
          where_end = pos;
        }
      }
      // a trailing alter action means the statement mixes RETRY with something else
      ObString rest = (where_end < sql.length())
                          ? ObString(sql.length() - static_cast<int32_t>(where_end),
                                     sql.ptr() + where_end)
                          : ObString();
      int64_t depth = 0;
      int64_t scan = 0;
      while (scan < rest.length()) {
        const char c = rest[scan];
        if ('\'' == c || '"' == c || '`' == c) {
          scan = aic_skip_quoted(rest, scan);
        } else if ('(' == c) {
          ++depth;
          ++scan;
        } else if (')' == c) {
          --depth;
          ++scan;
        } else if (',' == c && 0 == depth) {
          retry.has_extra_action_ = true;
          ++scan;
        } else {
          ++scan;
        }
      }
      retry.predicate_.assign_ptr(sql.ptr() + where_end,
                                  static_cast<int32_t>(sql.length() - where_end));
      retry.end_pos_ = sql.length();
      is_retry = true;
    }
  }
  return ret;
}

int ObAiColumnUtils::build_retry_update(ObIAllocator &alloc,
                                        const RetryStmt &retry,
                                        ObString &new_sql)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString column_name;
  ObString pred = retry.predicate_;
  ObString trimmed_pred;
  if (!retry.column_name_.empty() && '`' == retry.column_name_[0]) {
    column_name.assign_ptr(retry.column_name_.ptr() + 1,
                           static_cast<int32_t>(retry.column_name_.length() - 2));
  } else {
    column_name = retry.column_name_;
  }
  if (column_name.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(sql.append("update "))) {
  } else if (!retry.db_name_.empty()) {
    if (OB_FAIL(sql.append(retry.db_name_))) {
    } else if (OB_FAIL(sql.append("."))) {
    }
  }
  if (OB_SUCC(ret)) {
    if (OB_FAIL(sql.append(retry.table_name_))) {
    } else if (OB_FAIL(sql.append(" set "))) {
    } else if (OB_FAIL(sql.append(column_name))) {
    } else if (OB_FAIL(sql.append(" = null where "))) {
    } else if (OB_FAIL(aic_trim_string(pred, trimmed_pred))) {
    } else if (trimmed_pred.empty()) {
      if (OB_FAIL(sql.append("1 = 1"))) {
      }
    } else {
      // STATUS is a retry-only pseudo column: it reflects the result state of the AI column,
      // while a backquoted `status` still addresses the physical column.
      int64_t pos = 0;
      while (OB_SUCC(ret) && pos < pred.length()) {
        const char c = pred[pos];
        if ('\'' == c || '"' == c || '`' == c) {
          const int64_t end = aic_skip_quoted(pred, pos);
          if (OB_FAIL(sql.append(pred.ptr() + pos, end - pos))) {
          }
          pos = end;
        } else if (aic_is_ident_char(c) && (0 == pos || !aic_is_ident_char(pred[pos - 1]))) {
          int64_t end = pos;
          aic_read_word(pred, pos, end);
          if (aic_word_equal(pred.ptr() + pos, end - pos, "status")) {
            if (OB_FAIL(sql.append_fmt("if(%.*s is null, 'FAILED', 'READY')",
                                       column_name.length(), column_name.ptr()))) {
            }
          } else if (OB_FAIL(sql.append(pred.ptr() + pos, end - pos))) {
          }
          pos = end;
        } else {
          if (OB_FAIL(sql.append(&c, 1))) {
          }
          ++pos;
        }
      }
    }
  }
  if (OB_SUCC(ret) && OB_FAIL(ob_write_string(alloc,
                                              ObString(static_cast<int32_t>(sql.length()), sql.ptr()),
                                              new_sql))) {
  }
  return ret;
}

int ObAiColumnUtils::rewrite_stmt(ObIAllocator &alloc,
                                  const ObString &sql,
                                  ObString &new_sql,
                                  bool &rewritten)
{
  int ret = OB_SUCCESS;
  rewritten = false;
  ObSqlString out;
  int64_t pos = 0;
  bool changed = false;
  while (OB_SUCC(ret) && pos < sql.length()) {
    const char c = sql[pos];
    if ('\'' == c || '"' == c || '`' == c) {
      const int64_t end = aic_skip_quoted(sql, pos);
      if (OB_FAIL(out.append(sql.ptr() + pos, end - pos))) {
      }
      pos = end;
    } else if ('-' == c || '#' == c || '/' == c) {
      const int64_t end = aic_skip_blanks(sql, pos);
      if (end == pos) {
        if (OB_FAIL(out.append(&c, 1))) {
        }
        ++pos;
      } else {
        if (OB_FAIL(out.append(sql.ptr() + pos, end - pos))) {
        }
        pos = end;
      }
    } else if (aic_is_ident_char(c) && (0 == pos || !aic_is_ident_char(sql[pos - 1]))) {
      int64_t word_end = pos;
      aic_read_word(sql, pos, word_end);
      bool handled = false;
      if (aic_word_equal(sql.ptr() + pos, word_end - pos, "AI")) {
        const int64_t second = aic_skip_blanks(sql, word_end);
        int64_t second_end = second;
        if (second < sql.length() && aic_is_ident_char(sql[second])) {
          aic_read_word(sql, second, second_end);
          if (aic_word_equal(sql.ptr() + second, second_end - second, "COLUMN")) {
            const int64_t open = aic_skip_blanks(sql, second_end);
            int64_t close = OB_INVALID_INDEX;
            if (open < sql.length() && '(' == sql[open]) {
              if (OB_FAIL(aic_find_matching_paren(sql, open, close))) {
                LOG_WARN("unbalanced ai column clause", K(ret), K(sql));
              } else {
                ObString expr_text;
                ObString option_text;
                int64_t after = close + 1;
                const int64_t with_pos = aic_skip_blanks(sql, after);
                int64_t with_end = with_pos;
                if (with_pos < sql.length() && aic_is_ident_char(sql[with_pos])) {
                  aic_read_word(sql, with_pos, with_end);
                  if (aic_word_equal(sql.ptr() + with_pos, with_end - with_pos, "WITH")) {
                    const int64_t opt_open = aic_skip_blanks(sql, with_end);
                    int64_t opt_close = OB_INVALID_INDEX;
                    if (opt_open < sql.length() && '(' == sql[opt_open]
                        && OB_SUCC(aic_find_matching_paren(sql, opt_open, opt_close))) {
                      option_text.assign_ptr(sql.ptr() + opt_open + 1,
                                             static_cast<int32_t>(opt_close - opt_open - 1));
                      after = opt_close + 1;
                    }
                  }
                }
                expr_text.assign_ptr(sql.ptr() + open + 1, static_cast<int32_t>(close - open - 1));
                ObAiColumnSpec spec;
                ObString comment;
                if (OB_FAIL(parse_ai_column_clause(alloc, expr_text, option_text, spec))) {
                } else if (OB_FAIL(serialize_spec(alloc, spec, comment))) {
                } else if (OB_FAIL(out.append("COMMENT '"))) {
                } else if (OB_FAIL(out.append(comment))) {
                } else if (OB_FAIL(out.append("'"))) {
                } else {
                  pos = after;
                  handled = true;
                  changed = true;
                }
              }
            }
          }
        }
      }
      if (OB_SUCC(ret) && !handled) {
        if (OB_FAIL(out.append(sql.ptr() + pos, word_end - pos))) {
        } else {
          pos = word_end;
        }
      }
    } else {
      if (OB_FAIL(out.append(&c, 1))) {
      }
      ++pos;
    }
  }
  if (OB_SUCC(ret)) {
    if (changed) {
      if (OB_FAIL(ob_write_string(alloc,
                                  ObString(static_cast<int32_t>(out.length()), out.ptr()),
                                  new_sql))) {
      } else {
        rewritten = true;
      }
    } else {
      new_sql = sql;
    }
  }
  return ret;
}

int ObAiColumnUtils::get_column_type_name(const schema::ObColumnSchemaV2 &column,
                                          ObString &type_name)
{
  int ret = OB_SUCCESS;
  const ObObjType type = column.get_data_type();
  if (ob_is_json_tc(type)) {
    type_name = ObString::make_string("json");
  } else if (ob_is_integer_type(type)) {
    type_name = ObString::make_string("int");
  } else if (ob_is_number_tc(type) || ob_is_decimal_int_tc(type)
             || ob_is_float_tc(type) || ob_is_double_tc(type)) {
    type_name = ObString::make_string("number");
  } else {
    type_name.reset();
  }
  return ret;
}

int ObAiColumnUtils::collect_ai_columns(ObIAllocator &alloc,
                                        const schema::ObTableSchema &table_schema,
                                        ObIArray<ColumnDesc> &columns,
                                        int64_t &ai_column_count)
{
  int ret = OB_SUCCESS;
  ai_column_count = 0;
  for (schema::ObTableSchema::const_column_iterator it = table_schema.column_begin();
       OB_SUCC(ret) && it != table_schema.column_end(); ++it) {
    const schema::ObColumnSchemaV2 *column = *it;
    bool is_ai_column = false;
    ObAiColumnSpec spec;
    if (OB_ISNULL(column)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("column schema is null", K(ret), K(table_schema.get_table_id()));
    } else if (OB_FAIL(parse_spec(column->get_comment_str(), alloc, spec, is_ai_column))) {
      LOG_WARN("failed to parse ai column comment, ignored", K(ret), KPC(column));
      ret = OB_SUCCESS;
    } else if (!is_ai_column) {
      // ordinary column
    } else {
      ColumnDesc desc;
      desc.column_id_ = column->get_column_id();
      if (OB_FAIL(ob_write_string(alloc, column->get_column_name_str(), desc.column_name_))) {
      } else if (FALSE_IT(desc.spec_ = spec)) {
      } else if (OB_FAIL(columns.push_back(desc))) {
      } else {
        ++ai_column_count;
      }
    }
  }
  return ret;
}

int ObAiColumnUtils::collect_identifiers(ObIAllocator &alloc,
                                         const ObString &sql,
                                         ObIArray<ObString> &identifiers)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < sql.length()) {
    const char c = sql[pos];
    if ('\'' == c || '"' == c) {
      pos = aic_skip_quoted(sql, pos);
    } else if ('`' == c) {
      const int64_t end = aic_skip_quoted(sql, pos);
      ObString name;
      name.assign_ptr(sql.ptr() + pos + 1, static_cast<int32_t>(OB_MAX(end - pos - 2, 0)));
      if (OB_FAIL(aic_add_identifier(alloc, name, identifiers))) {
      }
      pos = end;
    } else if ('-' == c || '#' == c || '/' == c) {
      const int64_t next = aic_skip_blanks(sql, pos);
      pos = (next > pos) ? next : pos + 1;
    } else if (aic_is_ident_char(c) && (0 == pos || !aic_is_ident_char(sql[pos - 1]))) {
      int64_t end = pos;
      aic_read_word(sql, pos, end);
      ObString name;
      name.assign_ptr(sql.ptr() + pos, static_cast<int32_t>(end - pos));
      if (OB_FAIL(aic_add_identifier(alloc, name, identifiers))) {
      }
      pos = end;
    } else {
      ++pos;
    }
  }
  return ret;
}

int ObAiColumnUtils::parse_update_stmt(ObIAllocator &alloc,
                                       const ObString &sql,
                                       ObIArray<ObString> &table_pieces,
                                       ObIArray<ObString> &set_targets,
                                       int64_t &set_end,
                                       bool &is_update)
{
  int ret = OB_SUCCESS;
  is_update = false;
  set_end = 0;
  int64_t pos = aic_skip_blanks(sql, 0);
  int64_t word_end = pos;
  bool matched = (pos < sql.length() && aic_is_ident_char(sql[pos]));
  if (matched) {
    aic_read_word(sql, pos, word_end);
    matched = aic_word_equal(sql.ptr() + pos, word_end - pos, "update");
    pos = aic_skip_blanks(sql, word_end);
  }
  // table name, optionally qualified and backquoted
  while (OB_SUCC(ret) && matched && table_pieces.count() < 2) {
    ObString piece;
    if (pos >= sql.length()) {
      matched = false;
    } else if ('`' == sql[pos]) {
      const int64_t end = aic_skip_quoted(sql, pos);
      piece.assign_ptr(sql.ptr() + pos + 1, static_cast<int32_t>(OB_MAX(end - pos - 2, 0)));
      pos = aic_skip_blanks(sql, end);
    } else if (aic_is_ident_char(sql[pos])) {
      int64_t end = pos;
      aic_read_word(sql, pos, end);
      piece.assign_ptr(sql.ptr() + pos, static_cast<int32_t>(end - pos));
      pos = aic_skip_blanks(sql, end);
    } else {
      matched = false;
    }
    if (OB_SUCC(ret) && matched) {
      ObString stored;
      if (OB_FAIL(ob_write_string(alloc, piece, stored))) {
      } else if (OB_FAIL(table_pieces.push_back(stored))) {
      } else if (pos < sql.length() && '.' == sql[pos]) {
        pos = aic_skip_blanks(sql, pos + 1);
      } else {
        break;
      }
    }
  }
  if (OB_SUCC(ret) && matched) {
    if (pos >= sql.length() || !aic_is_ident_char(sql[pos])) {
      matched = false;
    } else {
      aic_read_word(sql, pos, word_end);
      matched = aic_word_equal(sql.ptr() + pos, word_end - pos, "set");
      pos = aic_skip_blanks(sql, word_end);
    }
  }
  // assignments: `<target> = <expr> [, ...]`, ending at the next top level clause
  bool last = false;
  while (OB_SUCC(ret) && matched && !last) {
    const int64_t elem_start = pos;
    int64_t elem_end = OB_INVALID_INDEX;
    int64_t depth = 0;
    while (pos < sql.length()) {
      const char c = sql[pos];
      if ('\'' == c || '"' == c || '`' == c) {
        pos = aic_skip_quoted(sql, pos);
      } else if ('(' == c) {
        ++depth;
        ++pos;
      } else if (')' == c) {
        --depth;
        ++pos;
      } else if (0 == depth && ',' == c) {
        elem_end = pos;
        ++pos;
        break;
      } else if (0 == depth && aic_is_ident_char(c)
                 && (0 == pos || !aic_is_ident_char(sql[pos - 1]))) {
        int64_t end = pos;
        aic_read_word(sql, pos, end);
        if (aic_word_equal(sql.ptr() + pos, end - pos, "where")
            || aic_word_equal(sql.ptr() + pos, end - pos, "order")
            || aic_word_equal(sql.ptr() + pos, end - pos, "limit")) {
          elem_end = pos;
          break;
        }
        pos = end;
      } else {
        ++pos;
      }
    }
    if (OB_INVALID_INDEX == elem_end) {
      elem_end = sql.length();
    }
    last = (elem_end >= sql.length() || ',' != sql[elem_end]);
    // the assigned column is the last name of the qualification, before the first '='
    int64_t scan = aic_skip_blanks(sql, elem_start);
    ObString names[2];
    int64_t name_count = 0;
    bool ok = true;
    while (ok && name_count < 2) {
      ObString name;
      if (scan < elem_end && '`' == sql[scan]) {
        const int64_t end = aic_skip_quoted(sql, scan);
        name.assign_ptr(sql.ptr() + scan + 1, static_cast<int32_t>(OB_MAX(end - scan - 2, 0)));
        scan = aic_skip_blanks(sql, end);
      } else if (scan < elem_end && aic_is_ident_char(sql[scan])) {
        int64_t end = scan;
        aic_read_word(sql, scan, end);
        name.assign_ptr(sql.ptr() + scan, static_cast<int32_t>(end - scan));
        scan = aic_skip_blanks(sql, end);
      } else {
        ok = false;
      }
      if (ok) {
        names[name_count++] = name;
      }
      if (ok && scan < elem_end && '.' == sql[scan]) {
        scan = aic_skip_blanks(sql, scan + 1);
      } else {
        break;
      }
    }
    if (!ok || name_count <= 0 || scan >= elem_end || '=' != sql[scan]) {
      // not a plain `column = expr` assignment
      matched = false;
    } else {
      ObString stored;
      if (OB_FAIL(ob_write_string(alloc, names[name_count - 1], stored))) {
      } else if (OB_FAIL(set_targets.push_back(stored))) {
      } else {
        set_end = elem_end;
      }
    }
  }
  is_update = matched;
  return ret;
}

int ObAiColumnUtils::collect_table_refs(ObIAllocator &alloc,
                                        const ObString &sql,
                                        ObIArray<ObString> &db_names,
                                        ObIArray<ObString> &table_names)
{
  int ret = OB_SUCCESS;
  ObString piece;
  ObString qualifier;
  int64_t pos = 0;
  while (OB_SUCC(ret) && pos < sql.length()) {
    const char c = sql[pos];
    if ('\'' == c || '"' == c) {
      pos = aic_skip_quoted(sql, pos);
    } else if ('-' == c || '#' == c || '/' == c) {
      const int64_t next = aic_skip_blanks(sql, pos);
      pos = (next > pos) ? next : pos + 1;
    } else if ('`' == c || aic_is_ident_char(c)) {
      int64_t end = pos;
      if ('`' == c) {
        end = aic_skip_quoted(sql, pos);
        piece.assign_ptr(sql.ptr() + pos + 1, static_cast<int32_t>(OB_MAX(end - pos - 2, 0)));
      } else {
        aic_read_word(sql, pos, end);
        piece.assign_ptr(sql.ptr() + pos, static_cast<int32_t>(end - pos));
      }
      int64_t next = aic_skip_blanks(sql, end);
      if (next < sql.length() && '.' == sql[next]) {
        qualifier = piece;
        pos = aic_skip_blanks(sql, next + 1);
        continue;
      }
      ObString db_copy;
      ObString table_copy;
      if (OB_FAIL(ob_write_string(alloc, qualifier, db_copy))
          || OB_FAIL(ob_write_string(alloc, piece, table_copy))
          || OB_FAIL(db_names.push_back(db_copy))
          || OB_FAIL(table_names.push_back(table_copy))) {
        LOG_WARN("failed to record table reference", K(ret), K(piece));
      } else {
        qualifier.reset();
      }
      pos = next;
    } else {
      ++pos;
    }
  }
  return ret;
}

} // namespace share
} // namespace oceanbase

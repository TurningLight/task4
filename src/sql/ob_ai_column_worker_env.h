/**
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

#ifndef OCEANBASE_SQL_OB_AI_COLUMN_WORKER_ENV_H_
#define OCEANBASE_SQL_OB_AI_COLUMN_WORKER_ENV_H_

#include "lib/string/ob_string.h"

namespace oceanbase
{
namespace sql
{

// Thread local execution environment pinned by the AI COLUMN background worker
// while it runs the statements that implement one publish group.
//
// The worker evaluates a queued AI expression with a plain
//   select <ai_expr> from <db>.<table> where <rowkey predicate>
// statement so that the normal AI function implementation owns the request. Two
// pieces of context have to reach that statement:
//
//   * active: the worker's own write back statement must never be treated as
//     user DML by the AI COLUMN DML hook, otherwise publishing a value would
//     invalidate it again and enqueue a new task forever.
//   * system prompt: the type hint sentence rendered into the AI column's
//     normalized expression has to be sent as the "system" message of the
//     completion request, exactly like a user query would.
class ObAiColumnWorkerEnv
{
public:
  static bool is_active() { return active_ref(); }
  static void set_active(const bool active) { active_ref() = active; }
  static const common::ObString &get_system_prompt() { return prompt_ref(); }
  static void set_system_prompt(const common::ObString &prompt) { prompt_ref() = prompt; }
  static void reset_system_prompt() { prompt_ref().reset(); }
private:
  static bool &active_ref()
  {
    static thread_local bool active = false;
    return active;
  }
  static common::ObString &prompt_ref()
  {
    static thread_local common::ObString prompt;
    return prompt;
  }
};

// Marks the calling thread as the AI COLUMN worker thread for the lifetime of
// the guard and restores the previous state afterwards.
class ObAiColumnWorkerGuard
{
public:
  ObAiColumnWorkerGuard()
    : saved_active_(ObAiColumnWorkerEnv::is_active()),
      saved_prompt_(ObAiColumnWorkerEnv::get_system_prompt())
  {
    ObAiColumnWorkerEnv::set_active(true);
  }
  ~ObAiColumnWorkerGuard()
  {
    ObAiColumnWorkerEnv::set_active(saved_active_);
    ObAiColumnWorkerEnv::set_system_prompt(saved_prompt_);
  }
private:
  bool saved_active_;
  common::ObString saved_prompt_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorkerGuard);
};

} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_AI_COLUMN_WORKER_ENV_H_

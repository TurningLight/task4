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

#ifndef OCEANBASE_OBSERVER_OB_AI_COLUMN_WORKER_H_
#define OCEANBASE_OBSERVER_OB_AI_COLUMN_WORKER_H_

#include "lib/thread/threads.h"
#include "lib/container/ob_se_array.h"
#include "common/mysqlclient/ob_isql_client.h"
#include "share/ai_column/ob_ai_column_task.h"

namespace oceanbase
{
namespace common
{
class ObIAllocator;
}
namespace observer
{

// Drains __all_ai_column_task and recomputes the AI columns of the queued rows. The DML path
// only enqueues, so the user statement never waits for the model. Every AI column of one
// (table, request, row) group is written by a single statement inside one transaction, which is
// what makes a multi-column row become visible as a whole.
class ObAiColumnWorker : public lib::Threads
{
public:
  ObAiColumnWorker();
  virtual ~ObAiColumnWorker() override;
  int init(common::ObISQLClient &sql_proxy);
  int start();
  void stop();
  void wait();
  void destroy();
protected:
  void run1() override;
private:
  // Returns OB_SUCCESS unless loading the queue failed. A failed group is turned into a retry
  // or into a dropped task, so it never fails the batch.
  int do_one_batch(bool &has_work);
  int load_tasks(common::ObIAllocator &alloc,
                 common::ObIArray<share::ObAiColumnTaskRow> &rows,
                 bool &full_batch);
  int process_group(common::ObIAllocator &alloc,
                    const common::ObIArray<share::ObAiColumnTaskRow> &group);
  // Requeues the group with a later eligibility time, or drops the tasks whose retry budget is
  // exhausted so that a permanently failing row does not spin forever.
  int fail_group(common::ObIAllocator &alloc,
                 const common::ObIArray<share::ObAiColumnTaskRow> &group);
private:
  bool is_inited_;
  common::ObISQLClient *sql_proxy_;
  DISALLOW_COPY_AND_ASSIGN(ObAiColumnWorker);
};

} // namespace observer
} // namespace oceanbase

#endif // OCEANBASE_OBSERVER_OB_AI_COLUMN_WORKER_H_

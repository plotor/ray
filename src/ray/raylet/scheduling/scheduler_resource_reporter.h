// Copyright 2020-2021 The Ray Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <deque>

#include "absl/container/flat_hash_map.h"
#include "ray/common/ray_config.h"
#include "ray/common/task/task_spec.h"
#include "ray/raylet/scheduling/internal.h"
#include "ray/raylet/scheduling/local_task_manager_interface.h"

namespace ray {
namespace raylet {

/// Helper class that reports resource_load and resource_load_by_shape to gcs.
class SchedulerResourceReporter {
 public:
  SchedulerResourceReporter(
      const absl::flat_hash_map<SchedulingClass,
                                std::deque<std::shared_ptr<internal::Work>>>
          &tasks_to_schedule,
      const absl::flat_hash_map<SchedulingClass,
                                std::deque<std::shared_ptr<internal::Work>>>
          &infeasible_tasks,
      const ILocalTaskManager &local_task_manager);

  /// Populate the relevant parts of the heartbeat table. This is intended for
  /// sending resource usage of raylet to gcs. In particular, this should fill in
  /// resource_load and resource_load_by_shape.
  ///
  /// \param[out] data: Output parameter. `resource_load` and `resource_load_by_shape` are
  /// the only fields used.
  void FillResourceUsage(rpc::ResourcesData &data) const;

  /// Populate the count of pending and infeasible actor tasks, organized by shape.
  ///
  /// \param[out] data: Output parameter. `resource_load_by_shape` is the only field
  /// filled.
  void FillPendingActorCountByShape(rpc::ResourcesData &data) const;

 private:
  int64_t TotalBacklogSize(SchedulingClass scheduling_class) const;

  const int64_t max_resource_shapes_per_load_report_;
  const absl::flat_hash_map<SchedulingClass, std::deque<std::shared_ptr<internal::Work>>>
      &tasks_to_schedule_;

  const absl::flat_hash_map<SchedulingClass, std::deque<std::shared_ptr<internal::Work>>>
      &tasks_to_dispatch_;

  const absl::flat_hash_map<SchedulingClass, std::deque<std::shared_ptr<internal::Work>>>
      &infeasible_tasks_;

  const absl::flat_hash_map<SchedulingClass, absl::flat_hash_map<WorkerID, int64_t>>
      &backlog_tracker_;
};

}  // namespace raylet
}  // namespace ray

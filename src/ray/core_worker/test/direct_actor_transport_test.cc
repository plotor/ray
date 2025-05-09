// Copyright 2017 The Ray Authors.
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

#include <memory>
#include <string>
#include <vector>

// clang-format off
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "ray/common/asio/instrumented_io_context.h"
#include "ray/common/task/task_spec.h"
#include "ray/common/test_util.h"
#include "ray/core_worker/store_provider/memory_store/memory_store.h"
#include "ray/core_worker/transport/normal_task_submitter.h"
#include "ray/raylet_client/raylet_client.h"
#include "ray/rpc/worker/core_worker_client.h"
#include "mock/ray/core_worker/actor_creator.h"
#include "mock/ray/core_worker/task_manager.h"
#include "mock/ray/core_worker/reference_count.h"
// clang-format on

namespace ray {
namespace core {

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::Return;

rpc::ActorDeathCause CreateMockDeathCause() {
  ray::rpc::ActorDeathCause death_cause;
  death_cause.mutable_runtime_env_failed_context()->set_error_message("failed");
  return death_cause;
}

TaskSpecification CreateActorTaskHelper(ActorID actor_id,
                                        WorkerID caller_worker_id,
                                        int64_t counter,
                                        TaskID caller_id = TaskID::Nil()) {
  TaskSpecification task;
  task.GetMutableMessage().set_task_id(TaskID::FromRandom(actor_id.JobId()).Binary());
  task.GetMutableMessage().set_caller_id(caller_id.Binary());
  task.GetMutableMessage().set_type(TaskType::ACTOR_TASK);
  task.GetMutableMessage().mutable_caller_address()->set_worker_id(
      caller_worker_id.Binary());
  task.GetMutableMessage().mutable_actor_task_spec()->set_actor_id(actor_id.Binary());
  task.GetMutableMessage().mutable_actor_task_spec()->set_actor_counter(counter);
  task.GetMutableMessage().set_num_returns(0);
  return task;
}

rpc::PushTaskRequest CreatePushTaskRequestHelper(ActorID actor_id,
                                                 int64_t counter,
                                                 WorkerID caller_worker_id,
                                                 TaskID caller_id,
                                                 int64_t caller_timestamp) {
  auto task_spec = CreateActorTaskHelper(actor_id, caller_worker_id, counter, caller_id);

  rpc::PushTaskRequest request;
  request.mutable_task_spec()->CopyFrom(task_spec.GetMessage());
  request.set_sequence_number(request.task_spec().actor_task_spec().actor_counter());
  request.set_client_processed_up_to(-1);
  return request;
}

class MockWorkerClient : public rpc::CoreWorkerClientInterface {
 public:
  const rpc::Address &Addr() const override { return addr; }

  void PushActorTask(std::unique_ptr<rpc::PushTaskRequest> request,
                     bool skip_queue,
                     rpc::ClientCallback<rpc::PushTaskReply> &&callback) override {
    received_seq_nos.push_back(request->sequence_number());
    callbacks.push_back(callback);
  }

  bool ReplyPushTask(Status status = Status::OK(), size_t index = 0) {
    if (callbacks.size() == 0) {
      return false;
    }
    auto callback = callbacks.at(index);
    callback(status, rpc::PushTaskReply());
    callbacks.erase(callbacks.begin() + index);
    return true;
  }

  rpc::Address addr;
  std::vector<rpc::ClientCallback<rpc::PushTaskReply>> callbacks;
  std::vector<uint64_t> received_seq_nos;
  int64_t acked_seqno = 0;
};

class ActorTaskSubmitterTest : public ::testing::TestWithParam<bool> {
 public:
  ActorTaskSubmitterTest()
      : client_pool_(
            std::make_shared<rpc::CoreWorkerClientPool>([&](const rpc::Address &addr) {
              num_clients_connected_++;
              return worker_client_;
            })),
        worker_client_(std::make_shared<MockWorkerClient>()),
        store_(std::make_shared<CoreWorkerMemoryStore>(io_context)),
        task_finisher_(std::make_shared<MockTaskFinisherInterface>()),
        io_work(io_context.get_executor()),
        reference_counter_(std::make_shared<MockReferenceCounter>()),
        submitter_(
            *client_pool_,
            *store_,
            *task_finisher_,
            actor_creator_,
            [this](const ActorID &actor_id, int64_t num_queued) {
              last_queue_warning_ = num_queued;
            },
            io_context,
            reference_counter_) {}

  void TearDown() override { io_context.stop(); }

  int num_clients_connected_ = 0;
  int64_t last_queue_warning_ = 0;
  MockActorCreatorInterface actor_creator_;
  std::shared_ptr<rpc::CoreWorkerClientPool> client_pool_;
  std::shared_ptr<MockWorkerClient> worker_client_;
  std::shared_ptr<CoreWorkerMemoryStore> store_;
  std::shared_ptr<MockTaskFinisherInterface> task_finisher_;
  instrumented_io_context io_context;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> io_work;
  std::shared_ptr<MockReferenceCounter> reference_counter_;
  ActorTaskSubmitter submitter_;

 protected:
  bool CheckSubmitTask(TaskSpecification task) {
    EXPECT_TRUE(submitter_.SubmitTask(task).ok());
    return 1 == io_context.poll_one();
  }
};

TEST_P(ActorTaskSubmitterTest, TestSubmitTask) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);

  auto task = CreateActorTaskHelper(actor_id, worker_id, 0);
  ASSERT_TRUE(CheckSubmitTask(task));
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  task = CreateActorTaskHelper(actor_id, worker_id, 1);
  ASSERT_TRUE(CheckSubmitTask(task));
  ASSERT_EQ(worker_client_->callbacks.size(), 2);

  EXPECT_CALL(*task_finisher_, CompletePendingTask(_, _, _, _))
      .Times(worker_client_->callbacks.size());
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(_, _, _, _, _, _)).Times(0);
  while (!worker_client_->callbacks.empty()) {
    ASSERT_TRUE(worker_client_->ReplyPushTask());
  }
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));

  // Connect to the actor again.
  // Because the IP and port of address are not modified, it will skip directly and will
  // not reset `received_seq_nos`.
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));
}

TEST_P(ActorTaskSubmitterTest, TestQueueingWarning) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);

  for (int i = 0; i < 7500; i++) {
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    ASSERT_TRUE(CheckSubmitTask(task));
    ASSERT_TRUE(worker_client_->ReplyPushTask());
  }
  ASSERT_EQ(last_queue_warning_, 0);

  for (int i = 7500; i < 15000; i++) {
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    ASSERT_TRUE(CheckSubmitTask(task));
    /* no ack */
  }
  ASSERT_EQ(last_queue_warning_, 5000);

  for (int i = 15000; i < 35000; i++) {
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    ASSERT_TRUE(CheckSubmitTask(task));
    /* no ack */
  }
  ASSERT_EQ(last_queue_warning_, 20000);
}

TEST_P(ActorTaskSubmitterTest, TestDependencies) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create two tasks for the actor with different arguments.
  ObjectID obj1 = ObjectID::FromRandom();
  ObjectID obj2 = ObjectID::FromRandom();
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  task1.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj1.Binary());
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  task2.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj2.Binary());

  // Neither task can be submitted yet because they are still waiting on
  // dependencies.
  ASSERT_TRUE(CheckSubmitTask(task1));
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Put the dependencies in the store in the same order as task submission.
  auto data = GenerateRandomObject();

  // Each Put schedules a callback onto io_context, and let's run it.
  ASSERT_TRUE(store_->Put(*data, obj1));
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  ASSERT_TRUE(store_->Put(*data, obj2));
  ASSERT_EQ(io_context.poll_one(), 1);
  ASSERT_EQ(worker_client_->callbacks.size(), 2);

  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));
}

TEST_P(ActorTaskSubmitterTest, TestOutOfOrderDependencies) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create two tasks for the actor with different arguments.
  ObjectID obj1 = ObjectID::FromRandom();
  ObjectID obj2 = ObjectID::FromRandom();
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  task1.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj1.Binary());
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  task2.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(
      obj2.Binary());

  // Neither task can be submitted yet because they are still waiting on
  // dependencies.
  ASSERT_TRUE(CheckSubmitTask(task1));
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  if (execute_out_of_order) {
    // Put the dependencies in the store in the opposite order of task
    // submission.
    auto data = GenerateRandomObject();
    // task2 is submitted first as we allow out of order execution.
    ASSERT_TRUE(store_->Put(*data, obj2));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 1);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(1));
    // then task1 is submitted
    ASSERT_TRUE(store_->Put(*data, obj1));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 2);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(1, 0));
  } else {
    // Put the dependencies in the store in the opposite order of task
    // submission.
    auto data = GenerateRandomObject();
    ASSERT_TRUE(store_->Put(*data, obj2));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 0);
    ASSERT_TRUE(store_->Put(*data, obj1));
    ASSERT_EQ(io_context.poll_one(), 1);
    ASSERT_EQ(worker_client_->callbacks.size(), 2);
    ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1));
  }
}

TEST_P(ActorTaskSubmitterTest, TestActorDead) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create two tasks for the actor. One depends on an object that is not yet available.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  ObjectID obj = ObjectID::FromRandom();
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  task2.GetMutableMessage().add_args()->mutable_object_ref()->set_object_id(obj.Binary());
  ASSERT_TRUE(CheckSubmitTask(task1));
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  // Simulate the actor dying. All in-flight tasks should get failed.
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task1.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_finisher_, CompletePendingTask(_, _, _, _)).Times(0);
  while (!worker_client_->callbacks.empty()) {
    ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError("")));
  }

  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(_, _, _, _, _, _)).Times(0);
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Actor marked as dead. All queued tasks should get failed.
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1);
  submitter_.DisconnectActor(
      actor_id, 2, /*dead=*/true, death_cause, /*is_restartable=*/false);
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartNoRetry) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create four tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 2);
  auto task4 = CreateActorTaskHelper(actor_id, worker_id, 3);
  // Submit three tasks.
  ASSERT_TRUE(CheckSubmitTask(task1));
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_TRUE(CheckSubmitTask(task3));

  EXPECT_CALL(*task_finisher_, CompletePendingTask(task1.TaskId(), _, _, _)).Times(1);
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task3.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task4.TaskId(), _, _, _)).Times(1);
  // First task finishes. Second task fails.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError("")));

  // Simulate the actor failing.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, /*num_restarts=*/1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Third task fails after the actor is disconnected. It should not get
  // retried.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError("")));

  // Actor gets restarted.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);
  ASSERT_TRUE(CheckSubmitTask(task4));
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));
  ASSERT_TRUE(worker_client_->callbacks.empty());
  // task1, task2 failed, task3 failed, task4
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1, 2, 3));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartRetry) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create four tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 2);
  auto task4 = CreateActorTaskHelper(actor_id, worker_id, 3);
  // Submit three tasks.
  ASSERT_TRUE(CheckSubmitTask(task1));
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_TRUE(CheckSubmitTask(task3));

  // All tasks will eventually finish.
  EXPECT_CALL(*task_finisher_, CompletePendingTask(_, _, _, _)).Times(4);
  // Tasks 2 and 3 will be retried.
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task3.TaskId(), _, _, _, _, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  // First task finishes. Second task fails.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError("")));

  // Simulate the actor failing.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, /*num_restarts=*/1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  // Third task fails after the actor is disconnected.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError("")));

  // Actor gets restarted.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);
  // A new task is submitted.
  ASSERT_TRUE(CheckSubmitTask(task4));
  // Tasks 2 and 3 get retried. In the real world, the seq_no of these two tasks should be
  // updated to 4 and 5 by `CoreWorker::InternalHeartbeat`.
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_TRUE(CheckSubmitTask(task3));
  while (!worker_client_->callbacks.empty()) {
    ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));
  }
  // task1, task2 failed, task3 failed, task4, task2 retry, task3 retry
  ASSERT_THAT(worker_client_->received_seq_nos, ElementsAre(0, 1, 2, 3, 1, 2));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartOutOfOrderRetry) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);

  // Create four tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 2);
  // Submit three tasks.
  ASSERT_TRUE(CheckSubmitTask(task1));
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_TRUE(CheckSubmitTask(task3));
  // All tasks will eventually finish.
  EXPECT_CALL(*task_finisher_, CompletePendingTask(_, _, _, _)).Times(3);

  // Tasks 2 will be retried
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1)
      .WillRepeatedly(Return(true));
  // First task finishes. Second task hang. Third task finishes.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK(), /*index=*/0));
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK(), /*index=*/1));
  // Simulate the actor failing.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError(""), /*index=*/0));
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);

  // Actor gets restarted.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);

  // Upon re-connect, task 2 (failed) should be retried.
  // Retry task 2 manually (simulating task_finisher and SendPendingTask's behavior)
  ASSERT_TRUE(CheckSubmitTask(task2));

  // Only task2 should be submitted. task 3 (completed) should not be retried.
  ASSERT_EQ(worker_client_->callbacks.size(), 1);

  // Finishes all task
  while (!worker_client_->callbacks.empty()) {
    ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));
  }
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartOutOfOrderGcs) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);
  ASSERT_EQ(num_clients_connected_, 1);

  // Create four tasks for the actor.
  auto task = CreateActorTaskHelper(actor_id, worker_id, 0);
  // Submit a task.
  ASSERT_TRUE(CheckSubmitTask(task));
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));

  // Actor restarts, but we don't receive the disconnect message until later.
  addr.set_port(1);
  submitter_.ConnectActor(actor_id, addr, 1);
  ASSERT_EQ(num_clients_connected_, 2);
  // Submit a task.
  task = CreateActorTaskHelper(actor_id, worker_id, 1);
  ASSERT_TRUE(CheckSubmitTask(task));
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));

  // We receive the RESTART message late. Nothing happens.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);
  ASSERT_EQ(num_clients_connected_, 2);
  // Submit a task.
  task = CreateActorTaskHelper(actor_id, worker_id, 2);
  ASSERT_TRUE(CheckSubmitTask(task));
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));

  // The actor dies twice. We receive the last RESTART message first.
  submitter_.DisconnectActor(
      actor_id, 3, /*dead=*/false, death_cause, /*is_restartable=*/true);
  ASSERT_EQ(num_clients_connected_, 2);
  // Submit a task.
  task = CreateActorTaskHelper(actor_id, worker_id, 3);
  ASSERT_TRUE(CheckSubmitTask(task));
  // Tasks submitted when the actor is in RESTARTING state will fail immediately.
  // This happens in an io_service.post. Search `SendPendingTasks_ForceFail` to locate
  // the code.
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task.TaskId(), _, _, _, _, _))
      .Times(1);
  ASSERT_EQ(io_context.poll_one(), 1);

  // We receive the late messages. Nothing happens.
  addr.set_port(2);
  submitter_.ConnectActor(actor_id, addr, 2);
  submitter_.DisconnectActor(
      actor_id, 2, /*dead=*/false, death_cause, /*is_restartable=*/true);
  ASSERT_EQ(num_clients_connected_, 2);

  // The actor dies permanently.
  submitter_.DisconnectActor(
      actor_id, 3, /*dead=*/true, death_cause, /*is_restartable=*/false);
  ASSERT_EQ(num_clients_connected_, 2);

  // We receive more late messages. Nothing happens because the actor is dead.
  submitter_.DisconnectActor(
      actor_id, 4, /*dead=*/false, death_cause, /*is_restartable=*/true);
  addr.set_port(3);
  submitter_.ConnectActor(actor_id, addr, 4);
  ASSERT_EQ(num_clients_connected_, 2);
  // Submit a task.
  task = CreateActorTaskHelper(actor_id, worker_id, 4);
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task.TaskId(), _, _, _, _, _))
      .Times(1);
  ASSERT_FALSE(CheckSubmitTask(task));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartFailInflightTasks) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);
  ASSERT_EQ(num_clients_connected_, 1);

  // Create 3 tasks for the actor.
  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  auto task3 = CreateActorTaskHelper(actor_id, worker_id, 1);
  // Submit a task.
  ASSERT_TRUE(CheckSubmitTask(task1));
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task1.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));

  // Submit 2 tasks.
  ASSERT_TRUE(CheckSubmitTask(task2));
  ASSERT_TRUE(CheckSubmitTask(task3));
  // Actor failed, but the task replies are delayed (or in some scenarios, lost).
  // We should still be able to fail the inflight tasks.
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1);
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task3.TaskId(), _, _, _, _, _))
      .Times(1);
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);

  // The task replies are now received. Since the tasks are already failed, they will not
  // be marked as failed or finished again.
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task2.TaskId(), _, _, _)).Times(0);
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(0);
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task3.TaskId(), _, _, _)).Times(0);
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task3.TaskId(), _, _, _, _, _))
      .Times(0);
  // Task 2 replied with OK.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));
  // Task 3 replied with error.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::IOError("")));
}

TEST_P(ActorTaskSubmitterTest, TestActorRestartFastFail) {
  auto execute_out_of_order = GetParam();
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      -1,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 0);
  ASSERT_EQ(num_clients_connected_, 1);

  auto task1 = CreateActorTaskHelper(actor_id, worker_id, 0);
  // Submit a task.
  ASSERT_TRUE(CheckSubmitTask(task1));
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task1.TaskId(), _, _, _)).Times(1);
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK()));

  // Actor failed and is now restarting.
  const auto death_cause = CreateMockDeathCause();
  submitter_.DisconnectActor(
      actor_id, 1, /*dead=*/false, death_cause, /*is_restartable=*/true);

  // Submit a new task. This task should fail immediately because "max_task_retries" is 0.
  auto task2 = CreateActorTaskHelper(actor_id, worker_id, 1);
  ASSERT_TRUE(CheckSubmitTask(task2));
  EXPECT_CALL(*task_finisher_, CompletePendingTask(task2.TaskId(), _, _, _)).Times(0);
  EXPECT_CALL(*task_finisher_, FailOrRetryPendingTask(task2.TaskId(), _, _, _, _, _))
      .Times(1);
  ASSERT_EQ(io_context.poll_one(), 1);
}

TEST_P(ActorTaskSubmitterTest, TestPendingTasks) {
  auto execute_out_of_order = GetParam();
  int32_t max_pending_calls = 10;
  rpc::Address addr;
  auto worker_id = WorkerID::FromRandom();
  addr.set_worker_id(worker_id.Binary());
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  submitter_.AddActorQueueIfNotExists(actor_id,
                                      max_pending_calls,
                                      execute_out_of_order,
                                      /*fail_if_actor_unreachable*/ true,
                                      /*owned*/ false);
  addr.set_port(0);

  // Submit number of `max_pending_calls` tasks would be OK.
  for (int32_t i = 0; i < max_pending_calls; i++) {
    ASSERT_FALSE(submitter_.PendingTasksFull(actor_id));
    auto task = CreateActorTaskHelper(actor_id, worker_id, i);
    ASSERT_TRUE(CheckSubmitTask(task));
  }

  // Then the queue should be full.
  ASSERT_TRUE(submitter_.PendingTasksFull(actor_id));

  ASSERT_EQ(worker_client_->callbacks.size(), 0);
  submitter_.ConnectActor(actor_id, addr, 0);
  ASSERT_EQ(worker_client_->callbacks.size(), 10);

  // After task 0 reply comes, the queue turn to not full.
  ASSERT_TRUE(worker_client_->ReplyPushTask(Status::OK(), 0));
  ASSERT_FALSE(submitter_.PendingTasksFull(actor_id));

  // We can submit task 10, but after that the queue is full.
  auto task = CreateActorTaskHelper(actor_id, worker_id, 10);
  ASSERT_TRUE(CheckSubmitTask(task));
  ASSERT_TRUE(submitter_.PendingTasksFull(actor_id));

  // All the replies comes, the queue shouble be empty.
  while (!worker_client_->callbacks.empty()) {
    ASSERT_TRUE(worker_client_->ReplyPushTask());
  }
  ASSERT_FALSE(submitter_.PendingTasksFull(actor_id));
}

INSTANTIATE_TEST_SUITE_P(ExecuteOutOfOrder,
                         ActorTaskSubmitterTest,
                         ::testing::Values(true, false));

class MockDependencyWaiter : public DependencyWaiter {
 public:
  MOCK_METHOD2(Wait,
               void(const std::vector<rpc::ObjectReference> &dependencies,
                    std::function<void()> on_dependencies_available));

  virtual ~MockDependencyWaiter() {}
};

class MockTaskEventBuffer : public worker::TaskEventBuffer {
 public:
  void AddTaskEvent(std::unique_ptr<worker::TaskEvent> task_event) override {}

  void FlushEvents(bool forced) override {}

  Status Start(bool auto_flush = true) override { return Status::OK(); }

  void Stop() override {}

  bool Enabled() const override { return true; }

  std::string DebugString() override { return ""; }
};

class MockTaskReceiver : public TaskReceiver {
 public:
  MockTaskReceiver(instrumented_io_context &task_execution_service,
                   worker::TaskEventBuffer &task_event_buffer,
                   const TaskHandler &task_handler,
                   std::function<std::function<void()>()> initialize_thread_callback,
                   const OnActorCreationTaskDone &actor_creation_task_done_)
      : TaskReceiver(task_execution_service,
                     task_event_buffer,
                     task_handler,
                     initialize_thread_callback,
                     actor_creation_task_done_) {}

  void UpdateConcurrencyGroupsCache(const ActorID &actor_id,
                                    const std::vector<ConcurrencyGroup> &cgs) {
    concurrency_groups_cache_[actor_id] = cgs;
  }
};

class TaskReceiverTest : public ::testing::Test {
 public:
  TaskReceiverTest()
      : worker_client_(std::make_shared<MockWorkerClient>()),
        dependency_waiter_(std::make_unique<MockDependencyWaiter>()) {
    auto execute_task = std::bind(&TaskReceiverTest::MockExecuteTask,
                                  this,
                                  std::placeholders::_1,
                                  std::placeholders::_2,
                                  std::placeholders::_3,
                                  std::placeholders::_4,
                                  std::placeholders::_5,
                                  std::placeholders::_6);
    receiver_ = std::make_unique<MockTaskReceiver>(
        task_execution_service_,
        task_event_buffer_,
        execute_task,
        /* intiialize_thread_callback= */ []() { return []() { return; }; },
        /* actor_creation_task_done= */ []() { return Status::OK(); });
    receiver_->Init(std::make_shared<rpc::CoreWorkerClientPool>(
                        [&](const rpc::Address &addr) { return worker_client_; }),
                    rpc_address_,
                    dependency_waiter_.get());
  }

  Status MockExecuteTask(
      const TaskSpecification &task_spec,
      std::optional<ResourceMappingType> resource_ids,
      std::vector<std::pair<ObjectID, std::shared_ptr<RayObject>>> *return_objects,
      std::vector<std::pair<ObjectID, std::shared_ptr<RayObject>>>
          *dynamic_return_objects,
      std::vector<std::pair<ObjectID, bool>> *streaming_generator_returns,
      ReferenceCounter::ReferenceTableProto *borrowed_refs) {
    return Status::OK();
  }

  void StartIOService() { task_execution_service_.run(); }

  void StopIOService() {
    // We must delete the receiver before stopping the IO service, since it
    // contains timers referencing the service.
    receiver_.reset();
    task_execution_service_.stop();
  }

  std::unique_ptr<MockTaskReceiver> receiver_;

 private:
  rpc::Address rpc_address_;
  instrumented_io_context task_execution_service_;
  MockTaskEventBuffer task_event_buffer_;
  std::shared_ptr<MockWorkerClient> worker_client_;
  std::unique_ptr<DependencyWaiter> dependency_waiter_;
};

TEST_F(TaskReceiverTest, TestNewTaskFromDifferentWorker) {
  TaskID current_task_id = TaskID::Nil();
  ActorID actor_id = ActorID::Of(JobID::FromInt(0), TaskID::Nil(), 0);
  WorkerID worker_id = WorkerID::FromRandom();
  TaskID caller_id =
      TaskID::ForActorTask(JobID::FromInt(0), current_task_id, 0, actor_id);

  int64_t curr_timestamp = current_sys_time_ms();
  int64_t old_timestamp = curr_timestamp - 1000;
  int64_t new_timestamp = curr_timestamp + 1000;

  int callback_count = 0;

  // Push a task request with actor counter 0. This should scucceed
  // on the receiver.
  {
    auto request =
        CreatePushTaskRequestHelper(actor_id, 0, worker_id, caller_id, curr_timestamp);
    rpc::PushTaskReply reply;
    auto reply_callback = [&callback_count](Status status,
                                            std::function<void()> success,
                                            std::function<void()> failure) {
      ++callback_count;
      ASSERT_TRUE(status.ok());
    };
    receiver_->UpdateConcurrencyGroupsCache(actor_id, {});
    receiver_->HandleTask(request, &reply, reply_callback);
  }

  // Push a task request with actor counter 1. This should scucceed
  // on the receiver.
  {
    auto request =
        CreatePushTaskRequestHelper(actor_id, 1, worker_id, caller_id, curr_timestamp);
    rpc::PushTaskReply reply;
    auto reply_callback = [&callback_count](Status status,
                                            std::function<void()> success,
                                            std::function<void()> failure) {
      ++callback_count;
      ASSERT_TRUE(status.ok());
    };
    receiver_->HandleTask(request, &reply, reply_callback);
  }

  // Create another request with the same caller id, but a different worker id,
  // and a newer timestamp. This simulates caller reconstruction.
  // Note that here the task request still has counter 0, which should be
  // ignored normally, but here it's from a different worker and with a newer
  // timestamp, in this case it should succeed.
  {
    worker_id = WorkerID::FromRandom();
    auto request =
        CreatePushTaskRequestHelper(actor_id, 0, worker_id, caller_id, new_timestamp);
    rpc::PushTaskReply reply;
    auto reply_callback = [&callback_count](Status status,
                                            std::function<void()> success,
                                            std::function<void()> failure) {
      ++callback_count;
      ASSERT_TRUE(status.ok());
    };
    receiver_->HandleTask(request, &reply, reply_callback);
  }

  // Push a task request with actor counter 1, but with a different worker id,
  // and a older timestamp. In this case the request should fail.
  {
    worker_id = WorkerID::FromRandom();
    auto request =
        CreatePushTaskRequestHelper(actor_id, 1, worker_id, caller_id, old_timestamp);
    rpc::PushTaskReply reply;
    auto reply_callback = [&callback_count](Status status,
                                            std::function<void()> success,
                                            std::function<void()> failure) {
      ++callback_count;
      ASSERT_TRUE(!status.ok());
    };
    receiver_->HandleTask(request, &reply, reply_callback);
  }

  StartIOService();

  // Wait for all the callbacks to be invoked.
  auto condition_func = [&callback_count]() -> bool { return callback_count == 4; };

  ASSERT_TRUE(WaitForCondition(condition_func, 10 * 1000));

  StopIOService();
}

}  // namespace core
}  // namespace ray

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  InitShutdownRAII ray_log_shutdown_raii(
      ray::RayLog::StartRayLog,
      ray::RayLog::ShutDownRayLog,
      argv[0],
      ray::RayLogLevel::INFO,
      ray::RayLog::GetLogFilepathFromDirectory(/*log_dir=*/"", /*app_name=*/argv[0]),
      ray::RayLog::GetErrLogFilepathFromDirectory(/*log_dir=*/"", /*app_name=*/argv[0]),
      ray::RayLog::GetRayLogRotationMaxBytesOrDefault(),
      ray::RayLog::GetRayLogRotationBackupCountOrDefault());
  ray::RayLog::InstallFailureSignalHandler(argv[0]);
  return RUN_ALL_TESTS();
}

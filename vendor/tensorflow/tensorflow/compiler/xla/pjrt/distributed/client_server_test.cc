/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/synchronization/barrier.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"
#include "grpcpp/grpcpp.h"
#include "grpcpp/server.h"
#include "tensorflow/compiler/xla/pjrt/distributed/client.h"
#include "tensorflow/compiler/xla/pjrt/distributed/protocol.pb.h"
#include "tensorflow/compiler/xla/pjrt/distributed/service.h"
#include "tensorflow/compiler/xla/protobuf_util.h"
#include "tensorflow/compiler/xla/status_macros.h"
#include "tensorflow/tsl/lib/core/status_test_util.h"
#include "tensorflow/tsl/platform/errors.h"
#include "tensorflow/tsl/platform/test.h"

namespace xla {
namespace {
constexpr absl::Duration kHeartbeatInterval = absl::Milliseconds(500);
constexpr int kMaxMissingHeartbeats = 3;
constexpr absl::Duration kBarrierTimeout = absl::Milliseconds(200);

struct ServiceParams {
  std::string test_name;
  // If false, test uses distributed runtime service instead.
  bool use_coordination_service = false;
};

class ClientServerTest : public testing::TestWithParam<ServiceParams> {
 public:
  std::unique_ptr<DistributedRuntimeClient> GetClient(
      int node_id, bool use_coordination_service,
      DistributedRuntimeClient::Options client_options = {},
      std::shared_ptr<::grpc::Channel> channel = nullptr) {
    client_options.node_id = node_id;
    // Set a small heartbeat interval for quicker tests.
    client_options.heartbeat_interval = kHeartbeatInterval;
    client_options.max_missing_heartbeats = kMaxMissingHeartbeats;
    if (channel == nullptr) {
      channel = server_->InProcessChannel(::grpc::ChannelArguments());
    }
    return GetDistributedRuntimeClient(channel, client_options,
                                       use_coordination_service);
  }

  void StartService(int num_nodes, bool use_coordination_service,
                    DistributedRuntimeServiceImpl::Options service_options = {},
                    absl::string_view service_address = "") {
    ::grpc::ServerBuilder builder;
    service_options.num_nodes = num_nodes;
    // Set a small heartbeat interval for quicker tests.
    service_options.heartbeat_interval = kHeartbeatInterval;
    service_options.max_missing_heartbeats = kMaxMissingHeartbeats;

    // Add a listening port if address is specified.
    if (!service_address.empty()) {
      auto credentials = ::grpc::InsecureServerCredentials();
      builder.AddListeningPort(std::string(service_address), credentials);
    }

    // Set up and register service on the gRPC server.
    if (use_coordination_service) {
      coord_service_ =
          std::make_unique<CoordinationServiceImpl>(service_options, &builder);
      server_ = builder.BuildAndStart();
      coord_service_->StartRpcThread();

    } else {
      distributed_runtime_service_ =
          std::make_unique<DistributedRuntimeServiceImpl>(service_options);
      builder.RegisterService(distributed_runtime_service_.get());
      server_ = builder.BuildAndStart();
    }
  }

  // Shut down the server.
  void Stop() {
    // Avoid shutting down the server twice if the test has already called
    // Stop() earlier.
    if (stop_is_already_called_) {
      return;
    }
    server_->Shutdown();
    stop_is_already_called_ = true;
  }

  void TearDown() override { Stop(); }

  std::unique_ptr<::grpc::Server> server_;

 private:
  std::unique_ptr<CoordinationServiceImpl> coord_service_;
  std::unique_ptr<DistributedRuntimeServiceImpl> distributed_runtime_service_;
  bool stop_is_already_called_ = false;
};

TEST_P(ClientServerTest, ConnectAndShutdownAreBarriers) {
  int num_nodes = 3;
  StartService(num_nodes, GetParam().use_coordination_service);

  absl::Mutex mu;
  int connect_count = 0;
  int shutdown_count = 0;

  absl::Barrier barrier(num_nodes);

  auto thread_fn = [&](int node_id) -> xla::Status {
    auto client = GetClient(node_id, GetParam().use_coordination_service);

    // Allow the threads to call Connect one-by-one in order.
    auto my_connect_turn = [&]() {
      mu.AssertHeld();
      return connect_count == node_id;
    };
    {
      absl::MutexLock lock(&mu);
      mu.Await(absl::Condition(&my_connect_turn));
      ++connect_count;
    }
    TF_RETURN_IF_ERROR(client->Connect());
    // Verify that all of the threads have called Connect() by the time we get
    // here.
    {
      absl::MutexLock lock(&mu);
      TF_RET_CHECK(connect_count == num_nodes);
    }

    // Similarly for shutting down.
    auto my_shutdown_turn = [&]() {
      mu.AssertHeld();
      return shutdown_count == node_id;
    };
    {
      absl::MutexLock lock(&mu);
      mu.Await(absl::Condition(&my_shutdown_turn));
      ++shutdown_count;
    }
    TF_RETURN_IF_ERROR(client->Shutdown());
    {
      absl::MutexLock lock(&mu);
      TF_RET_CHECK(shutdown_count == num_nodes);
    }

    return OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    TF_EXPECT_OK(statuses[i]);
  }
}

TEST_P(ClientServerTest, ConnectAndEnumerateDevices) {
  StartService(/*num_nodes=*/2, GetParam().use_coordination_service);

  std::vector<LocalTopologyProto> locals(2);
  locals[0].set_node_id(0);
  locals[1].set_node_id(1);
  DeviceProto* d0 = locals[0].add_devices();
  d0->set_local_device_ordinal(0);
  DeviceProto* d1 = locals[0].add_devices();
  d1->set_local_device_ordinal(0);
  DeviceProto* d2 = locals[0].add_devices();
  d2->set_local_device_ordinal(707);
  DeviceProto* d3 = locals[1].add_devices();
  d3->set_local_device_ordinal(1);

  GlobalTopologyProto expected_topology;
  auto* node0 = expected_topology.add_nodes();
  auto* node1 = expected_topology.add_nodes();
  *node0 = locals[0];
  node0->mutable_devices(0)->set_global_device_id(0);
  node0->mutable_devices(1)->set_global_device_id(1);
  node0->mutable_devices(2)->set_global_device_id(2);
  *node1 = locals[1];
  node1->mutable_devices(0)->set_global_device_id(3);

  // Used to ensure that thread0's client connects before thread1's client to
  // set the global device ids deterministically.
  absl::Notification n;
  auto thread0_fn = [&]() -> xla::Status {
    auto client = GetClient(/*node_id=*/0, GetParam().use_coordination_service);
    GlobalTopologyProto topology;
    // Unblock the second thread.
    // Note: For distributed runtime service, client->Connect() blocks
    // until all clients have connected concurrently. Thus, we cannot notify
    // after this Connect() due to a deadlock.
    n.Notify();
    TF_RETURN_IF_ERROR(client->Connect());
    TF_RETURN_IF_ERROR(client->EnumerateDevices(locals[0], &topology));
    TF_RET_CHECK(
        xla::protobuf_util::ProtobufEquals(topology, expected_topology))
        << topology.DebugString();
    TF_RETURN_IF_ERROR(client->KeyValueSet("key1", "value1"));
    TF_ASSIGN_OR_RETURN(
        std::string value,
        client->BlockingKeyValueGet("key2", absl::InfiniteDuration()));
    TF_RET_CHECK(value == "value2");
    return OkStatus();
  };
  auto thread1_fn = [&]() -> xla::Status {
    // Wait for thread0 client to be ready for connection, to ensure global ids
    // are set in order (thread0 client, then thread1 client).
    n.WaitForNotification();
    auto client = GetClient(/*node_id=*/1, GetParam().use_coordination_service);
    GlobalTopologyProto topology;
    TF_RETURN_IF_ERROR(client->Connect());
    absl::SleepFor(absl::Seconds(1));
    TF_RETURN_IF_ERROR(client->EnumerateDevices(locals[1], &topology));
    TF_RET_CHECK(
        xla::protobuf_util::ProtobufEquals(topology, expected_topology))
        << topology.DebugString();
    TF_ASSIGN_OR_RETURN(
        std::string value,
        client->BlockingKeyValueGet("key1", absl::InfiniteDuration()));
    TF_RET_CHECK(value == "value1");
    TF_RETURN_IF_ERROR(client->KeyValueSet("key2", "value2"));
    return OkStatus();
  };

  std::vector<std::function<xla::Status()>> functions = {thread0_fn,
                                                         thread1_fn};
  std::vector<xla::Status> statuses(functions.size());
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        functions.size());
    for (int i = 0; i < functions.size(); ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = functions[i](); });
    }
  }
  TF_EXPECT_OK(statuses[0]);
  TF_EXPECT_OK(statuses[1]);
}

// Setting `init_timeout` to 0 means that the client should attempt connection
// only once, but the client should still wait a short while for other tasks.
TEST_P(ClientServerTest, ZeroInitTimeoutShouldStillWaitForOtherTasks) {
  int num_nodes = 2;
  StartService(num_nodes, GetParam().use_coordination_service);

  absl::Barrier barrier(num_nodes);

  auto thread_fn = [&](int node_id) -> xla::Status {
    DistributedRuntimeClient::Options client_options;
    client_options.init_timeout = absl::ZeroDuration();
    auto client =
        GetClient(node_id, GetParam().use_coordination_service, client_options);

    // Node 0 will connect to the service immediately, but still wait for the
    // straggling node 1.
    if (node_id == 1) {
      absl::SleepFor(absl::Seconds(5));
    }
    TF_RETURN_IF_ERROR(client->Connect());

    return OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    TF_EXPECT_OK(statuses[i]);
  }
}

TEST_P(ClientServerTest, ClientsTerminateShutdownIfAnyClientGoesAway) {
  int num_nodes = 3;
  StartService(num_nodes, GetParam().use_coordination_service);

  auto thread_fn = [&](int node_id) -> xla::Status {
    DistributedRuntimeClient::Options client_options;
    client_options.shutdown_on_destruction = node_id != 0;
    client_options.missed_heartbeat_callback =
        [&](xla::Status status, bool coordinator_initiated) {};
    auto client =
        GetClient(node_id, GetParam().use_coordination_service, client_options);

    TF_RETURN_IF_ERROR(client->Connect());

    if (node_id == 0) {
      return OkStatus();
    }

    // The call to Shutdown() should be interrupted if a worker stops issuing
    // heartbeats.
    TF_RETURN_IF_ERROR(client->Shutdown());
    return OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  TF_EXPECT_OK(statuses[0]);
  for (int i = 1; i < num_nodes; ++i) {
    if (GetParam().use_coordination_service) {
      // Other nodes will be placed into ERROR state when the service informs
      // them of node 0's missing heartbeat failure.
      // agent->Shutdown() may lead into two different error codes depending on
      // the timing of the call:
      // 1. Internal: node turns into ERROR state during the shutdown call.
      // 2. Failed Precondition: node is already in ERROR state before the
      // shutdown call (note: agent will still stop sending heartbeats).
      EXPECT_TRUE(tsl::errors::IsInternal(statuses[i]) ||
                  tsl::errors::IsFailedPrecondition(statuses[i]));
    } else {
      EXPECT_EQ(statuses[i].code(), tensorflow::error::ABORTED);
    }
  }
}

TEST_P(ClientServerTest, ClientsReceiveMissedHeartbeatIfAnyClientGoesAway) {
  int num_nodes = 3;
  StartService(num_nodes, GetParam().use_coordination_service);

  auto thread_fn = [&](int node_id) -> xla::Status {
    DistributedRuntimeClient::Options client_options;
    client_options.shutdown_on_destruction = (node_id != 0);
    absl::Notification shutdown;
    client_options.missed_heartbeat_callback = [&](xla::Status status,
                                                   bool coordinator_initiated) {
      shutdown.Notify();
    };
    auto client =
        GetClient(node_id, GetParam().use_coordination_service, client_options);

    TF_RETURN_IF_ERROR(client->Connect());

    if (node_id == 0) {
      return OkStatus();
    }
    shutdown.WaitForNotification();
    return OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    TF_EXPECT_OK(statuses[i]);
  }
}

TEST_P(ClientServerTest, ClientsTerminateIfServiceGoesAway) {
  int num_nodes = 3;
  // We use a socket connection for this test case because the in-process API
  // does not react well to the server being told to shutdown while there are
  // active clients.
  int port = tsl::testing::PickUnusedPortOrDie();
  StartService(num_nodes, GetParam().use_coordination_service,
               /*service_options=*/{}, absl::StrCat("[::]:", port));

  absl::Barrier barrier(num_nodes + 1);

  auto thread_fn = [&](int node_id) -> xla::Status {
    DistributedRuntimeClient::Options client_options;
    client_options.rpc_timeout = absl::Seconds(1);
    client_options.shutdown_timeout = absl::Seconds(10);
    absl::Notification shutdown;
    client_options.missed_heartbeat_callback = [&](xla::Status status,
                                                   bool coordinator_initiated) {
      shutdown.Notify();
    };
    std::shared_ptr<::grpc::ChannelCredentials> creds =
        ::grpc::InsecureChannelCredentials();
    std::shared_ptr<::grpc::Channel> channel =
        ::grpc::CreateChannel(absl::StrCat("dns:///localhost:", port), creds);
    auto client = GetClient(node_id, GetParam().use_coordination_service,
                            client_options, channel);

    TF_RETURN_IF_ERROR(client->Connect());

    barrier.Block();
    shutdown.WaitForNotification();

    TF_RETURN_IF_ERROR(client->Shutdown());
    return OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
    barrier.Block();
    Stop();
  }
  for (int i = 0; i < num_nodes; ++i) {
    if (GetParam().use_coordination_service) {
      EXPECT_EQ(statuses[i].code(), tensorflow::error::FAILED_PRECONDITION);
    } else {
      EXPECT_EQ(statuses[i].code(), tensorflow::error::DEADLINE_EXCEEDED)
          << statuses[i];
    }
  }
}

// We should eventually connect, even if some clients are late to show up.
TEST_P(ClientServerTest, LateClientsAreOk) {
  int num_nodes = 3;
  StartService(num_nodes, GetParam().use_coordination_service);

  absl::Barrier barrier(num_nodes);

  auto thread_fn = [&](int node_id) -> xla::Status {
    DistributedRuntimeClient::Options client_options;
    client_options.init_timeout = absl::Seconds(20);
    client_options.rpc_timeout = absl::Milliseconds(200);
    auto client =
        GetClient(node_id, GetParam().use_coordination_service, client_options);

    barrier.Block();
    absl::SleepFor(absl::Milliseconds(200) * node_id);
    TF_RETURN_IF_ERROR(client->Connect());
    TF_RETURN_IF_ERROR(client->Shutdown());
    return OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    TF_EXPECT_OK(statuses[i]);
  }
}

// We should eventually time out if a client does not show up.
TEST_P(ClientServerTest, ConnectEventuallyTimesOutIfAClientDoesNotShowUp) {
  int num_nodes = 3;
  absl::Duration timeout = absl::Milliseconds(100);
  DistributedRuntimeServiceImpl::Options service_options;
  service_options.enumerate_devices_timeout = timeout;
  service_options.shutdown_timeout = timeout;
  StartService(num_nodes, GetParam().use_coordination_service, service_options);

  auto thread_fn = [&](int node_id) -> xla::Status {
    DistributedRuntimeClient::Options client_options;
    client_options.init_timeout = timeout;
    client_options.rpc_timeout = timeout;
    // Overwrite the default error callback which invokes LOG(QFATAL).
    client_options.missed_heartbeat_callback =
        [](xla::Status status, bool coordinator_reported_failure) {
          LOG(ERROR) << "Distributed client has missing heartbeats: " << status;
        };
    auto client =
        GetClient(node_id, GetParam().use_coordination_service, client_options);

    TF_RETURN_IF_ERROR(client->Connect());
    TF_RETURN_IF_ERROR(client->Shutdown());
    return OkStatus();
  };

  // Note: one fewer thread than 'num_nodes'.
  std::vector<xla::Status> statuses(num_nodes - 1);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes - 1; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes - 1; ++i) {
    EXPECT_EQ(statuses[i].code(), tensorflow::error::DEADLINE_EXCEEDED);
  }
}

TEST_P(ClientServerTest, WaitAtBarrier_Succeed) {
  int num_nodes = 2;
  StartService(num_nodes, GetParam().use_coordination_service);

  auto thread_fn = [&](int node_id) -> xla::Status {
    auto client = GetClient(node_id, GetParam().use_coordination_service);
    TF_RETURN_IF_ERROR(client->Connect());

    TF_RETURN_IF_ERROR(client->WaitAtBarrier("barrier_1", kBarrierTimeout));
    TF_RETURN_IF_ERROR(client->WaitAtBarrier("barrier_2", kBarrierTimeout));

    TF_RETURN_IF_ERROR(client->Shutdown());
    return xla::OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    TF_EXPECT_OK(statuses[i]);
  }
}

TEST_P(ClientServerTest, WaitAtBarrier_Timeout) {
  int num_nodes = 2;
  StartService(num_nodes, GetParam().use_coordination_service);
  absl::Notification n;

  auto thread_fn = [&](int node_id) -> xla::Status {
    auto client = GetClient(node_id, GetParam().use_coordination_service);
    TF_RETURN_IF_ERROR(client->Connect());

    // Node 1 waits for barrier to time out before proceeding.
    if (node_id == 1) {
      n.WaitForNotification();
    }
    Status barrier_status = client->WaitAtBarrier("barrier_1", kBarrierTimeout);
    // Node 0 notifies that barrier has already timed out.
    if (node_id == 0) {
      n.Notify();
    }
    TF_RETURN_IF_ERROR(barrier_status);

    TF_RETURN_IF_ERROR(client->Shutdown());
    return xla::OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    if (GetParam().use_coordination_service) {
      // Co-ordination service returns the status of the previous barrier
      // failure without waiting for the thread to time out.
      EXPECT_EQ(statuses[i].code(), tensorflow::error::DEADLINE_EXCEEDED)
          << " node id: " << i;
    } else {
      if (i == 0) {
        EXPECT_EQ(statuses[i].code(), tensorflow::error::DEADLINE_EXCEEDED)
            << " node id: " << i;
      }
      if (i == 1) {
        EXPECT_EQ(statuses[i].code(), tensorflow::error::FAILED_PRECONDITION)
            << " node id: " << i;
      }
    }
  }
}

TEST_P(ClientServerTest, WaitAtBarrier_TimeoutWithDifferentBarrierId) {
  int num_nodes = 2;
  StartService(num_nodes, GetParam().use_coordination_service);

  auto thread_fn = [&](int node_id) -> xla::Status {
    auto client = GetClient(node_id, GetParam().use_coordination_service);
    TF_RETURN_IF_ERROR(client->Connect());

    std::string barrier_id;
    if (node_id == 0) {
      barrier_id = "barrier_0";
    } else if (node_id == 1) {
      barrier_id = "barrier_1";
    }
    TF_RETURN_IF_ERROR(client->WaitAtBarrier(barrier_id, kBarrierTimeout));

    TF_RETURN_IF_ERROR(client->Shutdown());
    return xla::OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    EXPECT_EQ(statuses[i].code(), tensorflow::error::DEADLINE_EXCEEDED)
        << " node id: " << i;
  }
}

TEST_P(ClientServerTest, WaitAtBarrier_FailWithSameBarrierId) {
  int num_nodes = 2;
  StartService(num_nodes, GetParam().use_coordination_service);

  auto thread_fn = [&](int node_id) -> xla::Status {
    auto client = GetClient(node_id, GetParam().use_coordination_service);
    TF_RETURN_IF_ERROR(client->Connect());

    TF_RETURN_IF_ERROR(client->WaitAtBarrier("barrier_1", kBarrierTimeout));
    TF_RETURN_IF_ERROR(client->WaitAtBarrier("barrier_1", kBarrierTimeout));

    TF_RETURN_IF_ERROR(client->Shutdown());
    return xla::OkStatus();
  };

  std::vector<xla::Status> statuses(num_nodes);
  {
    tsl::thread::ThreadPool thread_pool(tsl::Env::Default(), "test_threads",
                                        num_nodes);
    for (int i = 0; i < num_nodes; ++i) {
      thread_pool.Schedule([&, i]() { statuses[i] = thread_fn(i); });
    }
  }
  for (int i = 0; i < num_nodes; ++i) {
    EXPECT_EQ(statuses[i].code(), tensorflow::error::FAILED_PRECONDITION)
        << " node id: " << i;
  }
}

INSTANTIATE_TEST_SUITE_P(
    ClientServerTests, ClientServerTest,
    ::testing::ValuesIn<ServiceParams>({
        {"CoordinationService", true},
        {"DistributedRuntimeService", false},
    }),
    [](const ::testing::TestParamInfo<ClientServerTest::ParamType>& info) {
      return info.param.test_name;
    });
}  // namespace
}  // namespace xla

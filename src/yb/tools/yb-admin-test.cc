// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// The following only applies to changes made to this file as part of YugaByte development.
//
// Portions Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.
//
// Tests for the yb-admin command-line tool.

#include <regex>

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/time_parsers.hpp>

#include <gtest/gtest.h>

#include "yb/client/client.h"
#include "yb/client/table_creator.h"

#include "yb/common/json_util.h"

#include "yb/gutil/map-util.h"
#include "yb/gutil/strings/join.h"
#include "yb/gutil/strings/substitute.h"

#include "yb/integration-tests/cluster_verifier.h"
#include "yb/integration-tests/external_mini_cluster.h"
#include "yb/integration-tests/test_workload.h"
#include "yb/integration-tests/ts_itest-base.h"

#include "yb/master/master_defaults.h"
#include "yb/master/master_backup.pb.h"

#include "yb/util/date_time.h"
#include "yb/util/jsonreader.h"
#include "yb/util/net/net_util.h"
#include "yb/util/port_picker.h"
#include "yb/util/random_util.h"
#include "yb/util/stol_utils.h"
#include "yb/util/string_trim.h"
#include "yb/util/string_util.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"

#include "yb/yql/pgwrapper/libpq_utils.h"

using namespace std::literals;

namespace yb {
namespace tools {

using client::YBClient;
using client::YBClientBuilder;
using client::YBTableName;
using client::YBSchema;
using client::YBSchemaBuilder;
using client::YBTableCreator;
using client::YBTableType;
using std::shared_ptr;
using std::vector;
using itest::TabletServerMap;
using itest::TServerDetails;
using pgwrapper::PGConn;
using strings::Substitute;

namespace {

Result<const rapidjson::Value&> Get(const rapidjson::Value& value, const char* name) {
  auto it = value.FindMember(name);
  if (it == value.MemberEnd()) {
    return STATUS_FORMAT(InvalidArgument, "Missing $0 field", name);
  }
  return it->value;
}

Result<rapidjson::Value&> Get(rapidjson::Value* value, const char* name) {
  auto it = value->FindMember(name);
  if (it == value->MemberEnd()) {
    return STATUS_FORMAT(InvalidArgument, "Missing $0 field", name);
  }
  return it->value;
}

static const char* const kAdminToolName = "yb-admin";

//  Helper to check hosts list by requesting cluster config via yb-admin and parse its output:
//
//  Config:
//  version: 1
//  server_blacklist {
//    hosts {
//      host: "node1"
//      port: 9100
//    }
//    hosts {
//      host: "node2"
//      port: 9100
//    }
//    initial_replica_load: 0
//  }
//
class BlacklistChecker {
 public:
  BlacklistChecker(const string& yb_admin_exe, const string& master_address) :
      args_{yb_admin_exe, "-master_addresses", master_address, "get_universe_config"} {
  }

  CHECKED_STATUS operator()(const vector<HostPort>& servers) const {
    string out;
    RETURN_NOT_OK(Subprocess::Call(args_, &out));
    boost::erase_all(out, "\n");
    JsonReader reader(out);

    vector<const rapidjson::Value *> blacklistEntries;
    const rapidjson::Value *blacklistRoot;
    RETURN_NOT_OK(reader.Init());
    RETURN_NOT_OK(
        reader.ExtractObject(reader.root(), "serverBlacklist", &blacklistRoot));
    RETURN_NOT_OK(
        reader.ExtractObjectArray(blacklistRoot, "hosts", &blacklistEntries));

    for (const rapidjson::Value *entry : blacklistEntries) {
      std::string host;
      int32_t port;
      RETURN_NOT_OK(reader.ExtractString(entry, "host", &host));
      RETURN_NOT_OK(reader.ExtractInt32(entry, "port", &port));
      HostPort blacklistServer(host, port);
      if (std::find(servers.begin(), servers.end(), blacklistServer) ==
          servers.end()) {
        return STATUS_FORMAT(NotFound,
                             "Item $0 not found in list of expected hosts $1",
                             blacklistServer, servers);
      }
    }

    if (blacklistEntries.size() != servers.size()) {
      return STATUS_FORMAT(NotFound, "$0 items expected but $1 found",
                           servers.size(), blacklistEntries.size());
    }

    return Status::OK();
  }

 private:
  vector<string> args_;
};

} // namespace

class AdminCliTest : public tserver::TabletServerIntegrationTestBase {
 protected:
  // Figure out where the admin tool is.
  std::string GetAdminToolPath() const;

  template <class... Args>
  Result<std::string> CallAdmin(Args&&... args) {
    std::string result;
    RETURN_NOT_OK(Subprocess::Call(
        ToStringVector(
            GetAdminToolPath(), "-master_addresses", cluster_->master()->bound_rpc_addr(),
            std::forward<Args>(args)...),
        &result));
    return result;
  }

  template <class... Args>
  Result<rapidjson::Document> CallJsonAdmin(Args&&... args) {
    auto raw = VERIFY_RESULT(CallAdmin(std::forward<Args>(args)...));
    rapidjson::Document result;
    if (result.Parse(raw.c_str(), raw.length()).HasParseError()) {
      return STATUS_FORMAT(
          InvalidArgument, "Failed to parse json output $0: $1", result.GetParseError(), raw);
    }
    return result;
  }

  Result<rapidjson::Document> GetSnapshotSchedule(const std::string& id = std::string()) {
    auto out = VERIFY_RESULT(id.empty() ? CallJsonAdmin("list_snapshot_schedules")
                                        : CallJsonAdmin("list_snapshot_schedules", id));
    auto schedules = VERIFY_RESULT(Get(&out, "schedules")).get().GetArray();
    SCHECK_EQ(schedules.Size(), 1, IllegalState, "Wrong schedules number");
    rapidjson::Document result;
    result.CopyFrom(schedules[0], result.GetAllocator());
    return result;
  }

  Result<rapidjson::Document> WaitScheduleSnapshot(
      MonoDelta duration, const std::string& id = std::string(), int num_snashots = 1) {
    rapidjson::Document result;
    RETURN_NOT_OK(WaitFor([this, id, num_snashots, &result]() -> Result<bool> {
      auto schedule = VERIFY_RESULT(GetSnapshotSchedule(id));
      auto snapshots = VERIFY_RESULT(Get(&schedule, "snapshots")).get().GetArray();
      if (snapshots.Size() < num_snashots) {
        return false;
      }
      result.CopyFrom(snapshots[snapshots.Size() - 1], result.GetAllocator());
      return true;
    }, duration, "Wait schedule snapshot"));
    return result;
  }

  CHECKED_STATUS RestoreSnapshotSchedule(const std::string& schedule_id, Timestamp restore_at) {
    auto out = VERIFY_RESULT(CallJsonAdmin(
        "restore_snapshot_schedule", schedule_id, restore_at.ToFormattedString()));
    std::string restoration_id = VERIFY_RESULT(Get(out, "restoration_id")).get().GetString();
    LOG(INFO) << "Restoration id: " << restoration_id;

    return WaitRestorationDone(restoration_id, 20s);
  }

  CHECKED_STATUS WaitRestorationDone(const std::string& restoration_id, MonoDelta timeout) {
    return WaitFor([this, restoration_id]() -> Result<bool> {
      auto out = VERIFY_RESULT(CallJsonAdmin("list_snapshot_restorations", restoration_id));
      const auto& restorations = VERIFY_RESULT(Get(out, "restorations")).get().GetArray();
      SCHECK_EQ(restorations.Size(), 1, IllegalState, "Wrong restorations number");
      auto id = VERIFY_RESULT(Get(restorations[0], "id")).get().GetString();
      SCHECK_EQ(id, restoration_id, IllegalState, "Wrong restoration id");
      std::string state_str = VERIFY_RESULT(Get(restorations[0], "state")).get().GetString();
      master::SysSnapshotEntryPB::State state;
      if (!master::SysSnapshotEntryPB_State_Parse(state_str, &state)) {
        return STATUS_FORMAT(IllegalState, "Failed to parse restoration state: $0", state_str);
      }
      if (state == master::SysSnapshotEntryPB::RESTORING) {
        return false;
      }
      if (state == master::SysSnapshotEntryPB::RESTORED) {
        return true;
      }
      return STATUS_FORMAT(IllegalState, "Unexpected restoration state: $0",
                           master::SysSnapshotEntryPB_State_Name(state));
    }, timeout, "Wait restoration complete");
  }

  Result<PGConn> PgConnect(const std::string& db_name = std::string()) {
    auto* ts = cluster_->tablet_server(RandomUniformInt(0, cluster_->num_tablet_servers() - 1));
    return PGConn::Connect(HostPort(ts->bind_host(), ts->pgsql_rpc_port()), db_name);
  }
};

string AdminCliTest::GetAdminToolPath() const {
  return GetToolPath(kAdminToolName);
}

// Test yb-admin config change while running a workload.
// 1. Instantiate external mini cluster with 3 TS.
// 2. Create table with 2 replicas.
// 3. Invoke yb-admin CLI to invoke a config change.
// 4. Wait until the new server bootstraps.
// 5. Profit!
TEST_F(AdminCliTest, TestChangeConfig) {
  FLAGS_num_tablet_servers = 3;
  FLAGS_num_replicas = 2;

  std::vector<std::string> master_flags = {
    "--catalog_manager_wait_for_new_tablets_to_elect_leader=false"s,
    "--replication_factor=2"s,
    "--use_create_table_leader_hint=false"s,
  };
  std::vector<std::string> ts_flags = {
    "--enable_leader_failure_detection=false"s,
  };
  BuildAndStart(ts_flags, master_flags);

  vector<TServerDetails*> tservers = TServerDetailsVector(tablet_servers_);
  ASSERT_EQ(FLAGS_num_tablet_servers, tservers.size());

  itest::TabletServerMapUnowned active_tablet_servers;
  auto iter = tablet_replicas_.find(tablet_id_);
  TServerDetails* leader = iter->second;
  TServerDetails* follower = (++iter)->second;
  InsertOrDie(&active_tablet_servers, leader->uuid(), leader);
  InsertOrDie(&active_tablet_servers, follower->uuid(), follower);

  TServerDetails* new_node = nullptr;
  for (TServerDetails* ts : tservers) {
    if (!ContainsKey(active_tablet_servers, ts->uuid())) {
      new_node = ts;
      break;
    }
  }
  ASSERT_TRUE(new_node != nullptr);

  int cur_log_index = 0;
  // Elect the leader (still only a consensus config size of 2).
  ASSERT_OK(StartElection(leader, tablet_id_, MonoDelta::FromSeconds(10)));
  ASSERT_OK(WaitUntilCommittedOpIdIndexIs(++cur_log_index, leader, tablet_id_,
                                          MonoDelta::FromSeconds(30)));
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(30), active_tablet_servers,
                                  tablet_id_, 1));

  TestWorkload workload(cluster_.get());
  workload.set_table_name(kTableName);
  workload.set_timeout_allowed(true);
  workload.set_write_timeout_millis(10000);
  workload.set_num_write_threads(1);
  workload.set_write_batch_size(1);
  workload.Setup();
  workload.Start();

  // Wait until the Master knows about the leader tserver.
  TServerDetails* master_observed_leader;
  ASSERT_OK(GetLeaderReplicaWithRetries(tablet_id_, &master_observed_leader));
  ASSERT_EQ(leader->uuid(), master_observed_leader->uuid());

  LOG(INFO) << "Adding tserver with uuid " << new_node->uuid() << " as PRE_VOTER ...";
  string exe_path = GetAdminToolPath();
  ASSERT_OK(CallAdmin("change_config", tablet_id_, "ADD_SERVER", new_node->uuid(), "PRE_VOTER"));

  InsertOrDie(&active_tablet_servers, new_node->uuid(), new_node);
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
                                                leader, tablet_id_,
                                                MonoDelta::FromSeconds(10)));

  workload.StopAndJoin();
  int num_batches = workload.batches_completed();

  LOG(INFO) << "Waiting for replicas to agree...";
  // Wait for all servers to replicate everything up through the last write op.
  // Since we don't batch, there should be at least # rows inserted log entries,
  // plus the initial leader's no-op, plus 1 for
  // the added replica for a total == #rows + 2.
  int min_log_index = num_batches + 2;
  ASSERT_OK(WaitForServersToAgree(MonoDelta::FromSeconds(30),
                                  active_tablet_servers, tablet_id_,
                                  min_log_index));

  int rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;

  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
      kTableName, ClusterVerifier::AT_LEAST, rows_inserted));

  // Now remove the server once again.
  LOG(INFO) << "Removing tserver with uuid " << new_node->uuid() << " from the config...";
  ASSERT_OK(CallAdmin("change_config", tablet_id_, "REMOVE_SERVER", new_node->uuid()));

  ASSERT_EQ(1, active_tablet_servers.erase(new_node->uuid()));
  ASSERT_OK(WaitUntilCommittedConfigNumVotersIs(active_tablet_servers.size(),
                                                leader, tablet_id_,
                                                MonoDelta::FromSeconds(10)));
}

TEST_F(AdminCliTest, TestDeleteTable) {
  FLAGS_num_tablet_servers = 1;
  FLAGS_num_replicas = 1;

  vector<string> ts_flags, master_flags;
  master_flags.push_back("--replication_factor=1");
  BuildAndStart(ts_flags, master_flags);
  string master_address = ToString(cluster_->master()->bound_rpc_addr());

  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Default table that gets created;
  string table_name = kTableName.table_name();
  string keyspace = kTableName.namespace_name();

  string exe_path = GetAdminToolPath();
  ASSERT_OK(CallAdmin("delete_table", keyspace, table_name));

  const auto tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(master::kNumSystemTables, tables.size());
}

TEST_F(AdminCliTest, TestDeleteIndex) {
  FLAGS_num_tablet_servers = 1;
  FLAGS_num_replicas = 1;

  vector<string> ts_flags, master_flags;
  master_flags.push_back("--replication_factor=1");
  ts_flags.push_back("--index_backfill_upperbound_for_user_enforced_txn_duration_ms=12000");
  BuildAndStart(ts_flags, master_flags);
  string master_address = ToString(cluster_->master()->bound_rpc_addr());

  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Default table that gets created;
  string table_name = kTableName.table_name();
  string keyspace = kTableName.namespace_name();
  string index_name = table_name + "-index";

  auto tables = ASSERT_RESULT(client->ListTables(/* filter */ table_name));
  ASSERT_EQ(1, tables.size());
  const auto table_id = tables.front().table_id();

  YBSchema index_schema;
  YBSchemaBuilder b;
  b.AddColumn("C$_key")->Type(INT32)->NotNull()->HashPrimaryKey();
  ASSERT_OK(b.Build(&index_schema));

  // Create index.
  shared_ptr<YBTableCreator> table_creator(client->NewTableCreator());

  IndexInfoPB *index_info = table_creator->mutable_index_info();
  index_info->set_indexed_table_id(table_id);
  index_info->set_is_local(false);
  index_info->set_is_unique(false);
  index_info->set_hash_column_count(1);
  index_info->set_range_column_count(0);
  index_info->set_use_mangled_column_name(true);
  index_info->add_indexed_hash_column_ids(10);

  auto *col = index_info->add_columns();
  col->set_column_name("C$_key");
  col->set_indexed_column_id(10);

  Status s = table_creator->table_name(YBTableName(YQL_DATABASE_CQL, keyspace, index_name))
                 .table_type(YBTableType::YQL_TABLE_TYPE)
                 .schema(&index_schema)
                 .indexed_table_id(table_id)
                 .is_local_index(false)
                 .is_unique_index(false)
                 .timeout(MonoDelta::FromSeconds(60))
                 .Create();
  ASSERT_OK(s);

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(2 + master::kNumSystemTables, tables.size());

  // Delete index.
  string exe_path = GetAdminToolPath();
  LOG(INFO) << "Delete index via yb-admin: " << keyspace << "." << index_name;
  ASSERT_OK(CallAdmin("delete_index", keyspace, index_name));

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(1 + master::kNumSystemTables, tables.size());

  // Delete table.
  LOG(INFO) << "Delete table via yb-admin: " << keyspace << "." << table_name;
  ASSERT_OK(CallAdmin("delete_table", keyspace, table_name));

  tables = ASSERT_RESULT(client->ListTables(/* filter */ "", /* exclude_ysql */ true));
  ASSERT_EQ(master::kNumSystemTables, tables.size());
}

TEST_F(AdminCliTest, BlackList) {
  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());
  const auto exe_path = GetAdminToolPath();
  const auto default_port = 9100;
  vector<HostPort> hosts{{"node1", default_port}, {"node2", default_port}, {"node3", default_port}};
  ASSERT_OK(CallAdmin("change_blacklist", "ADD", unpack(hosts)));
  const BlacklistChecker checker(exe_path, master_address);
  ASSERT_OK(checker(hosts));
  ASSERT_OK(CallAdmin("change_blacklist", "REMOVE", hosts.back()));
  hosts.pop_back();
  ASSERT_OK(checker(hosts));
}

TEST_F(AdminCliTest, InvalidMasterAddresses) {
  int port = AllocateFreePort();
  string unreachable_host = Substitute("127.0.0.1:$0", port);
  std::string error_string;
  ASSERT_NOK(Subprocess::Call(ToStringVector(
      GetAdminToolPath(), "-master_addresses", unreachable_host,
      "-timeout_ms", "1000", "list_tables"), &error_string, true /*read_stderr*/));
  ASSERT_STR_CONTAINS(error_string, "verify the addresses");
}

TEST_F(AdminCliTest, CheckTableIdUsage) {
  BuildAndStart();
  const auto master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder().add_master_server_addr(master_address).Build());
  const auto tables = ASSERT_RESULT(client->ListTables(kTableName.table_name(),
                                                       /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  const auto exe_path = GetAdminToolPath();
  const auto table_id = tables.front().table_id();
  const auto table_id_arg = Format("tableid.$0", table_id);
  auto args = ToStringVector(
      exe_path, "-master_addresses", master_address, "list_tablets", table_id_arg);
  const auto args_size = args.size();
  ASSERT_OK(Subprocess::Call(args));
  // Check good optional integer argument.
  args.push_back("1");
  ASSERT_OK(Subprocess::Call(args));
  // Check bad optional integer argument.
  args.resize(args_size);
  args.push_back("bad");
  std::string output;
  ASSERT_NOK(Subprocess::Call(args, &output, /* read_stderr */ true));
  // Due to greedy algorithm all bad arguments are treated as table identifier.
  ASSERT_NE(output.find("Namespace 'bad' of type 'ycql' not found"), std::string::npos);
  // Check multiple tables when single one is expected.
  args.resize(args_size);
  args.push_back(table_id_arg);
  ASSERT_NOK(Subprocess::Call(args, &output, /* read_stderr */ true));
  ASSERT_NE(output.find("Single table expected, 2 found"), std::string::npos);
  // Check wrong table id.
  args.resize(args_size - 1);
  const auto bad_table_id = table_id + "_bad";
  args.push_back(Format("tableid.$0", bad_table_id));
  ASSERT_NOK(Subprocess::Call(args, &output, /* read_stderr */ true));
  ASSERT_NE(
      output.find(Format("Table with id '$0' not found", bad_table_id)), std::string::npos);
}

TEST_F(AdminCliTest, TestSnapshotCreation) {
  BuildAndStart();
  const auto extra_table = YBTableName(YQLDatabase::YQL_DATABASE_CQL,
                                       kTableName.namespace_name(),
                                       "extra-table");
  YBSchemaBuilder schemaBuilder;
  schemaBuilder.AddColumn("k")->HashPrimaryKey()->Type(yb::BINARY)->NotNull();
  schemaBuilder.AddColumn("v")->Type(yb::BINARY)->NotNull();
  YBSchema schema;
  ASSERT_OK(schemaBuilder.Build(&schema));
  ASSERT_OK(client_->NewTableCreator()->table_name(extra_table)
      .schema(&schema).table_type(yb::client::YBTableType::YQL_TABLE_TYPE).Create());
  const auto tables = ASSERT_RESULT(client_->ListTables(kTableName.table_name(),
      /* exclude_ysql */ true));
  ASSERT_EQ(1, tables.size());
  std::string output = ASSERT_RESULT(CallAdmin(
      "create_snapshot", Format("tableid.$0", tables.front().table_id()),
      extra_table.namespace_name(), extra_table.table_name()));
  ASSERT_NE(output.find("Started snapshot creation"), string::npos);

  output = ASSERT_RESULT(CallAdmin("list_snapshots", "SHOW_DETAILS"));
  ASSERT_NE(output.find(extra_table.table_name()), string::npos);
  ASSERT_NE(output.find(kTableName.table_name()), string::npos);
}

TEST_F(AdminCliTest, SnapshotSchedule) {
  BuildAndStart();

  auto out = ASSERT_RESULT(CallJsonAdmin(
      "create_snapshot_schedule", 0.1, 10, kTableName.namespace_name(), kTableName.table_name()));

  std::string schedule_id = ASSERT_RESULT(Get(out, "schedule_id")).get().GetString();
  LOG(INFO) << "Schedule id: " << schedule_id;
  std::this_thread::sleep_for(20s);

  Timestamp last_snapshot_time;
  ASSERT_OK(WaitFor([this, schedule_id, &last_snapshot_time]() -> Result<bool> {
    auto schedule = VERIFY_RESULT(GetSnapshotSchedule());
    auto received_schedule_id = VERIFY_RESULT(Get(schedule, "id")).get().GetString();
    SCHECK_EQ(schedule_id, received_schedule_id, IllegalState, "Wrong schedule id");
    const auto& snapshots = VERIFY_RESULT(Get(schedule, "snapshots")).get().GetArray();

    if (snapshots.Size() < 2) {
      return false;
    }
    std::string last_snapshot_time_str;
    for (const auto& snapshot : snapshots) {
      std::string snapshot_time = VERIFY_RESULT(
          Get(snapshot, "snapshot_time_utc")).get().GetString();
      if (!last_snapshot_time_str.empty()) {
        std::string previous_snapshot_time = VERIFY_RESULT(
            Get(snapshot, "previous_snapshot_time_utc")).get().GetString();
        SCHECK_EQ(previous_snapshot_time, last_snapshot_time_str, IllegalState,
                  "Wrong previous_snapshot_hybrid_time");
      }
      last_snapshot_time_str = snapshot_time;
    }
    LOG(INFO) << "Last snapshot time: " << last_snapshot_time_str;
    last_snapshot_time = VERIFY_RESULT(DateTime::TimestampFromString(last_snapshot_time_str));
    return true;
  }, 20s, "At least 2 snapshots"));

  last_snapshot_time.set_value(last_snapshot_time.value() + 1);
  LOG(INFO) << "Restore at: " << last_snapshot_time.ToFormattedString();

  ASSERT_OK(RestoreSnapshotSchedule(schedule_id, last_snapshot_time));
}

TEST_F(AdminCliTest, GetIsLoadBalancerIdle) {
  const MonoDelta kWaitTime = 20s;
  std::string output;
  std::vector<std::string> master_flags;
  std::vector<std::string> ts_flags;
  master_flags.push_back("--enable_load_balancing=true");
  BuildAndStart(ts_flags, master_flags);

  const std::string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Load balancer IsIdle() logic has been changed to the following - unless a task was explicitly
  // triggered by the load balancer (AsyncAddServerTask / AsyncRemoveServerTask / AsyncTryStepDown)
  // then the task does not count towards determining whether the load balancer is active. If no
  // pending LB tasks of the aforementioned types exist, the load balancer will report idle.

  // Delete table should not activate the load balancer.
  ASSERT_OK(client->DeleteTable(kTableName, false /* wait */));
  // This should timeout.
  Status s = WaitFor(
      [&]() -> Result<bool> {
        auto output = VERIFY_RESULT(CallAdmin("get_is_load_balancer_idle"));
        return output.compare("Idle = 0\n") == 0;
      },
      kWaitTime,
      "wait for load balancer to stay idle");

  ASSERT_FALSE(s.ok());
}

TEST_F(AdminCliTest, TestLeaderStepdown) {
  BuildAndStart();
  std::string out;
  auto regex_fetch_first = [&out](const std::string& exp) -> Result<std::string> {
    std::smatch match;
    if (!std::regex_search(out.cbegin(), out.cend(), match, std::regex(exp)) || match.size() != 2) {
      return STATUS_FORMAT(NotFound, "No pattern in '$0'", out);
    }
    return match[1];
  };

  out = ASSERT_RESULT(CallAdmin(
      "list_tablets", kTableName.namespace_name(), kTableName.table_name()));
  const auto tablet_id = ASSERT_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+)"));
  out = ASSERT_RESULT(CallAdmin("list_tablet_servers", tablet_id));
  const auto tserver_id = ASSERT_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+FOLLOWER)"));
  ASSERT_OK(CallAdmin("leader_stepdown", tablet_id, tserver_id));

  ASSERT_OK(WaitFor([&]() -> Result<bool> {
    out = VERIFY_RESULT(CallAdmin("list_tablet_servers", tablet_id));
    return tserver_id == VERIFY_RESULT(regex_fetch_first(R"(\s+([a-z0-9]{32})\s+\S+\s+LEADER)"));
  }, 5s, "Leader stepdown"));
}

TEST_F(AdminCliTest, TestGetClusterLoadBalancerState) {
  std::string output;
  std::vector<std::string> master_flags;
  std::vector<std::string> ts_flags;
  master_flags.push_back("--enable_load_balancing=true");
  BuildAndStart(ts_flags, master_flags);

  const std::string master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder()
                                  .add_master_server_addr(master_address)
                                  .Build());
  output = ASSERT_RESULT(CallAdmin("get_load_balancer_state"));

  ASSERT_NE(output.find("ENABLED"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("set_load_balancer_enabled", "0"));

  ASSERT_EQ(output.find("Unable to change load balancer state"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("get_load_balancer_state"));

  ASSERT_NE(output.find("DISABLED"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("set_load_balancer_enabled", "1"));

  ASSERT_EQ(output.find("Unable to change load balancer state"), std::string::npos);

  output = ASSERT_RESULT(CallAdmin("get_load_balancer_state"));

  ASSERT_NE(output.find("ENABLED"), std::string::npos);
}

TEST_F(AdminCliTest, TestModifyTablePlacementPolicy) {
  // Start a cluster with 3 tservers, each corresponding to a different zone.
  FLAGS_num_tablet_servers = 3;
  FLAGS_num_replicas = 2;
  std::vector<std::string> master_flags;
  master_flags.push_back("--enable_load_balancing=true");
  master_flags.push_back("--catalog_manager_wait_for_new_tablets_to_elect_leader=false");
  std::vector<std::string> ts_flags;
  ts_flags.push_back("--placement_cloud=c");
  ts_flags.push_back("--placement_region=r");
  ts_flags.push_back("--placement_zone=z${index}");
  BuildAndStart(ts_flags, master_flags);

  const std::string& master_address = ToString(cluster_->master()->bound_rpc_addr());
  auto client = ASSERT_RESULT(YBClientBuilder()
      .add_master_server_addr(master_address)
      .Build());

  // Modify the cluster placement policy to consist of 2 zones.
  ASSERT_OK(CallAdmin("modify_placement_info", "c.r.z0,c.r.z1", 2, ""));

  // Create a new table.
  const auto extra_table = YBTableName(YQLDatabase::YQL_DATABASE_CQL,
                                       kTableName.namespace_name(),
                                       "extra-table");
  // Start a workload.
  TestWorkload workload(cluster_.get());
  workload.set_table_name(extra_table);
  workload.set_timeout_allowed(true);
  workload.Setup();
  workload.Start();

  // Verify that the table has no custom placement policy set for it.
  std::shared_ptr<client::YBTable> table;
  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_FALSE(table->replication_info());

  // Use yb-admin_cli to set a custom placement policy different from that of
  // the cluster placement policy for the new table.
  ASSERT_OK(CallAdmin(
      "modify_table_placement_info", kTableName.namespace_name(), "extra-table",
      "c.r.z0,c.r.z1,c.r.z2", 3, ""));

  // Verify that changing the placement _uuid for a table fails if the
  // placement_uuid does not match the cluster live placement_uuid.
  const string& random_placement_uuid = "19dfa091-2b53-434f-b8dc-97280a5f8831";
  ASSERT_NOK(CallAdmin(
      "modify_table_placement_info", kTableName.namespace_name(), "extra-table",
      "c.r.z0,c.r.z1,c.r.z2", 3, random_placement_uuid));

  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_TRUE(table->replication_info().get().live_replicas().placement_uuid().empty());

  // Fetch the placement policy for the table and verify that it matches
  // the custom info set previously.
  ASSERT_OK(client->OpenTable(extra_table, &table));
  vector<bool> found_zones;
  found_zones.assign(3, false);
  ASSERT_EQ(table->replication_info().get().live_replicas().placement_blocks_size(), 3);
  for (int ii = 0; ii < 3; ++ii) {
    auto pb = table->replication_info().get().live_replicas().placement_blocks(ii).cloud_info();
    ASSERT_EQ(pb.placement_cloud(), "c");
    ASSERT_EQ(pb.placement_region(), "r");
    if (pb.placement_zone() == "z0") {
      found_zones[0] = true;
    } else if (pb.placement_zone() == "z1") {
      found_zones[1] = true;
    } else {
      ASSERT_EQ(pb.placement_zone(), "z2");
      found_zones[2] = true;
    }
  }
  for (const bool found : found_zones) {
    ASSERT_TRUE(found);
  }

  // Perform the same test, but use the table-id instead of table name to set the
  // custom placement policy.
  std::string table_id = "tableid." + table->id();
  ASSERT_OK(CallAdmin("modify_table_placement_info", table_id, "c.r.z1", 1, ""));

  // Verify that changing the placement _uuid for a table fails if the
  // placement_uuid does not match the cluster live placement_uuid.
  ASSERT_NOK(CallAdmin(
      "modify_table_placement_info", table_id, "c.r.z1", 1, random_placement_uuid));

  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_TRUE(table->replication_info().get().live_replicas().placement_uuid().empty());

  // Fetch the placement policy for the table and verify that it matches
  // the custom info set previously.
  ASSERT_OK(client->OpenTable(extra_table, &table));
  ASSERT_EQ(table->replication_info().get().live_replicas().placement_blocks_size(), 1);
  auto pb = table->replication_info().get().live_replicas().placement_blocks(0).cloud_info();
  ASSERT_EQ(pb.placement_cloud(), "c");
  ASSERT_EQ(pb.placement_region(), "r");
  ASSERT_EQ(pb.placement_zone(), "z1");

  // Stop the workload.
  workload.StopAndJoin();
  int rows_inserted = workload.rows_inserted();
  LOG(INFO) << "Number of rows inserted: " << rows_inserted;

  sleep(5);

  // Verify that there was no data loss.
  ClusterVerifier cluster_verifier(cluster_.get());
  ASSERT_NO_FATALS(cluster_verifier.CheckCluster());
  ASSERT_NO_FATALS(cluster_verifier.CheckRowCount(
    extra_table, ClusterVerifier::EXACTLY, rows_inserted));
}

TEST_F(AdminCliTest, TestClearPlacementPolicy) {
  // Start a cluster with 3 tservers.
  FLAGS_num_tablet_servers = 3;
  FLAGS_num_replicas = 2;
  std::vector<std::string> master_flags;
  master_flags.push_back("--enable_load_balancing=true");
  std::vector<std::string> ts_flags;
  ts_flags.push_back("--placement_cloud=c");
  ts_flags.push_back("--placement_region=r");
  ts_flags.push_back("--placement_zone=z");
  BuildAndStart(ts_flags, master_flags);

  // Create the placement config.
  ASSERT_OK(CallAdmin("modify_placement_info", "c.r.z", 3, ""));

  // Ensure that the universe config has placement information.
  auto output = ASSERT_RESULT(CallAdmin("get_universe_config"));
  ASSERT_TRUE(output.find("replicationInfo") != std::string::npos);

  // Clear the placement config.
  ASSERT_OK(CallAdmin("clear_placement_info"));

  // Ensure that the placement config is absent.
  output = ASSERT_RESULT(CallAdmin("get_universe_config"));
  ASSERT_TRUE(output.find("replicationInfo") == std::string::npos);
}

class AdminCliTestWithYsql : public AdminCliTest {
 public:
  void UpdateMiniClusterOptions(ExternalMiniClusterOptions* opts) override {
    opts->enable_ysql = true;
    opts->extra_tserver_flags.emplace_back("--ysql_num_shards_per_tserver=1");
  }
};

TEST_F_EX(AdminCliTest, YB_DISABLE_TEST_IN_TSAN(SnapshotSchedulePgsql), AdminCliTestWithYsql) {
  const std::string kDbName = "ybtest";

  CreateCluster("raft_consensus-itest-cluster");
  client_ = ASSERT_RESULT(CreateClient());

  auto conn = ASSERT_RESULT(PgConnect());
  ASSERT_OK(conn.ExecuteFormat("CREATE DATABASE $0", kDbName));

  auto out = ASSERT_RESULT(CallJsonAdmin("create_snapshot_schedule", 0.1, 10, "ysql." + kDbName));
  std::string schedule_id = ASSERT_RESULT(Get(out, "schedule_id")).get().GetString();
  ASSERT_OK(WaitScheduleSnapshot(30s, schedule_id));

  conn = ASSERT_RESULT(PgConnect(kDbName));

  ASSERT_OK(conn.Execute("CREATE TABLE test_table (key INT PRIMARY KEY, value TEXT)"));

  ASSERT_OK(conn.Execute("INSERT INTO test_table VALUES (1, 'before')"));

  Timestamp time(ASSERT_RESULT(WallClock()->Now()).time_point);

  ASSERT_OK(conn.Execute("UPDATE test_table SET value = 'after'"));

  ASSERT_OK(RestoreSnapshotSchedule(schedule_id, time));

  auto res = ASSERT_RESULT(conn.FetchValue<std::string>("SELECT value FROM test_table"));

  ASSERT_EQ(res, "before");
}

}  // namespace tools
}  // namespace yb

// Copyright (c) YugaByte, Inc.
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

#include "yb/tools/tools_test_utils.h"

#include "yb/util/jsonreader.h"
#include "yb/util/net/net_util.h"
#include "yb/util/path_util.h"
#include "yb/util/random_util.h"
#include "yb/util/status_log.h"
#include "yb/util/subprocess.h"
#include "yb/util/test_util.h"
#include "yb/util/flags.h"

using std::string;

DEFINE_UNKNOWN_bool(verbose_yb_backup, false, "Add --verbose flag to yb_backup.py.");

namespace yb {
namespace tools {

Status RunBackupCommand(
    const HostPort& pg_hp, const std::string& master_addresses,
    const std::string& tserver_http_addresses, const std::string& tmp_dir,
    const std::vector<std::string>& extra_args) {
  std::vector <std::string> args = {
      GetToolPath("../../../build-support", "run_in_build_python_venv.sh"),
      GetToolPath("../../../managed/devops/bin", "yb_backup.py"),
      "--masters", master_addresses,
      "--ts_web_hosts_ports", tserver_http_addresses,
      "--remote_yb_admin_binary", GetToolPath("yb-admin"),
      "--remote_ysql_dump_binary", GetPgToolPath("ysql_dump"),
      "--remote_ysql_shell_binary", GetPgToolPath("ysqlsh"),
      "--storage_type", "nfs",
      "--nfs_storage_path", tmp_dir,
      "--no_ssh",
      "--no_auto_name",
      "--TEST_never_fsync",
  };

  if (!pg_hp.host().empty()) {
    args.push_back("--ysql_host");
    args.push_back(pg_hp.host());
    args.push_back("--ysql_port");
    args.push_back(AsString(pg_hp.port()));
  }

#if defined(__APPLE__)
  args.push_back("--mac");
#endif // defined(__APPLE__)

  std::string backup_cmd;
  for (const auto& a : extra_args) {
    args.push_back(a);
    if (a == "create" || a == "restore") {
      backup_cmd = a;
    }
  }

  if (FLAGS_verbose_yb_backup) {
    args.push_back("--verbose");
  }

  LOG(INFO) << "Run tool: " << AsString(args);
  string output;
  RETURN_NOT_OK(Subprocess::Call(args, &output));
  LOG(INFO) << "Tool output: " << output;

  JsonReader r(output);
  RETURN_NOT_OK(r.Init());
  string error;
  Status s = r.ExtractString(r.root(), "error", &error);
  if (s.ok()) {
    LOG(ERROR) << "yb_backup.py error: " << error;
    return STATUS(RuntimeError, "yb_backup.py error", error);
  }

  if (backup_cmd == "create") {
    string url;
    RETURN_NOT_OK(r.ExtractString(r.root(), "snapshot_url", &url));
    LOG(INFO) << "Backup-create operation result - snapshot url: " << url;
  } else if (backup_cmd == "restore") {
    bool result_ok = false;
    RETURN_NOT_OK(r.ExtractBool(r.root(), "success", &result_ok));
    LOG(INFO) << "Backup-restore operation result: " << result_ok;
    if (!result_ok) {
      return STATUS(RuntimeError, "Failed backup restore operation");
    }
  } else {
    return STATUS(InvalidArgument, "Unknown backup command", ToString(args));
  }

  return Status::OK();
}

TmpDirProvider::~TmpDirProvider() {
  if (!dir_.empty()) {
    LOG(INFO) << "Deleting temporary folder: " << dir_;
    CHECK_OK(Env::Default()->DeleteRecursively(dir_));
  }
}

std::string TmpDirProvider::operator/(const std::string& subdir) {
  return JoinPathSegments(**this, subdir);
}

std::string TmpDirProvider::operator*() {
  if (dir_.empty()) {
    std::string temp;
    CHECK_OK(Env::Default()->GetTestDirectory(&temp));
    dir_ = JoinPathSegments(
        temp, std::string(CURRENT_TEST_CASE_NAME()) + '_' + RandomHumanReadableString(8));
  }
  // Create the directory if it doesn't exist.
  if (!Env::Default()->DirExists(dir_)) {
    EXPECT_OK(Env::Default()->CreateDir(dir_));
  }
  return dir_;
}

} // namespace tools
} // namespace yb

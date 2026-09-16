/*
 *Copyright (c) 2024, Alibaba Group;
 *Licensed under the Apache License, Version 2.0 (the "License");
 *you may not use this file except in compliance with the License.
 *You may obtain a copy of the License at

 *   http://www.apache.org/licenses/LICENSE-2.0

 *Unless required by applicable law or agreed to in writing, software
 *distributed under the License is distributed on an "AS IS" BASIS,
 *WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *See the License for the specific language governing permissions and
 *limitations under the License.
*/

#include "BookSimNetwork.h"
#include "BookSimSim.h"
#include "entry.h"

#include "astra-sim/system/Sys.hh"
#include "engine/booksim_engine.hpp"
#include "SimCCL/mock/MockNcclLog.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <vector>

void BookSimNetworkInstallCallbacks();

namespace {

struct UserArgs {
  std::string workload;
  std::string booksim_config;
  int         gpus;
  int         gpus_per_server;
  uint64_t    send_lat_ns;
  bool        fast_forward;
  std::string result_prefix;
  std::string gpu_type;
  UserArgs() : gpus(8), gpus_per_server(8), send_lat_ns(0),
               fast_forward(true), result_prefix("./booksim_"),
               gpu_type("A100") {}
};

void Usage(const char * prog) {
  std::cerr
    << "usage: " << prog << " -w <workload> -c <booksim config> [options]\n"
    << "  -w <file>          ASTRA-sim workload file\n"
    << "  -c <file>          BookSim config (topology, link widths, MPS)\n"
    << "  -g <n>             number of GPUs / ranks (default 8)\n"
    << "  -g_p_s <n>         GPUs per server (default 8)\n"
    << "  -r <prefix>        result path prefix (default ./booksim_)\n"
    << "  --send-lat-ns <n>  software send latency per transfer, ns (default 0)\n"
    << "                     ns-3 uses NCCL's 7.2-15.9 us tables here; that term\n"
    << "                     dominates an intra-host PCIe study, so it is off by\n"
    << "                     default and opt-in for cross-checking\n"
    << "  --no-ff            disable idle clock fast-forward (for equivalence tests)\n";
}

bool ParseArgs(int argc, char ** argv, UserArgs & a) {
  for(int i = 1; i < argc; ++i) {
    std::string s = argv[i];
    bool has_next = (i + 1 < argc);
    if(s == "-w" && has_next)            a.workload = argv[++i];
    else if(s == "-c" && has_next)       a.booksim_config = argv[++i];
    else if(s == "-g" && has_next)       a.gpus = atoi(argv[++i]);
    else if(s == "-g_p_s" && has_next)   a.gpus_per_server = atoi(argv[++i]);
    else if(s == "-r" && has_next)       a.result_prefix = argv[++i];
    else if(s == "-g_type" && has_next)  a.gpu_type = argv[++i];
    else if(s == "--send-lat-ns" && has_next) a.send_lat_ns = strtoull(argv[++i], NULL, 10);
    else if(s == "--no-ff")              a.fast_forward = false;
    else if(s == "-h" || s == "--help")  return false;
    else { std::cerr << "unknown argument: " << s << std::endl; return false; }
  }
  return !a.workload.empty() && !a.booksim_config.empty();
}

} // namespace

int main(int argc, char ** argv) {
  UserArgs args;
  if(!ParseArgs(argc, argv, args)) { Usage(argv[0]); return 1; }

  MockNcclLog::set_log_name("SimAI_booksim.log");
  MockNcclLog * NcclLog = MockNcclLog::getInstance();
  NcclLog->writeLog(NcclLogLevel::INFO, " init SimAI_booksim.log ");

  g_send_lat_ns = args.send_lat_ns;

  GPUType gpu_type = GPUType::A100;
  if(args.gpu_type == "A100")      gpu_type = GPUType::A100;
  else if(args.gpu_type == "A800") gpu_type = GPUType::A800;
  else if(args.gpu_type == "H100") gpu_type = GPUType::H100;
  else if(args.gpu_type == "H800") gpu_type = GPUType::H800;
  else if(args.gpu_type == "H20")  gpu_type = GPUType::H20;
  else {
    std::cerr << "unknown -g_type " << args.gpu_type
              << " (expected A100|A800|H100|H800|H20)" << std::endl;
    return 1;
  }

  // NVLS models an all-reduce offloaded into an NVSwitch, so the NVSwitch rank
  // becomes a real traffic hub. There is no NVSwitch in a PCIe fabric -- our
  // NVSwitch rank exists only to satisfy a SimCCL data structure and is wired
  // into the topology as an inert endpoint. Letting NVLS be selected would
  // route the whole collective through that fiction and silently produce
  // meaningless numbers, so refuse the combination that enables it
  // (MockNcclGroup.cc:2343 requires nRanks>=8, AS_NVLS_ENABLE=1, an H-series
  // gpu_type, and data_size >= AS_NVLS_MIN_BYTES).
  {
    const char * nvls = getenv("AS_NVLS_ENABLE");
    bool h_series = (gpu_type == GPUType::H100 || gpu_type == GPUType::H800 ||
                     gpu_type == GPUType::H20);
    if(nvls && strcmp(nvls, "1") == 0 && h_series) {
      std::cerr << "SimAI-BookSim: AS_NVLS_ENABLE=1 with -g_type "
                << args.gpu_type << " would select the NVLS algorithm, which "
                << "routes the collective through an NVSwitch. This backend "
                << "models a PCIe fabric that has no NVSwitch; the NVSwitch "
                << "rank is an inert placeholder. Unset AS_NVLS_ENABLE or use "
                << "-g_type A100." << std::endl;
      return 1;
    }
  }

  // Build the fabric first: Sys construction reads the clock through
  // sim_get_time(), which needs the engine to exist.
  BookSimSim::Init(args.booksim_config.c_str(), args.fast_forward);
  BookSimNetworkInstallCallbacks();

  // Node layout follows the ns3 and analytical frontends: GPUs occupy ranks
  // [0, gpu_num), then one "NVSwitch" rank per server.
  //
  // A PCIe fabric has no NVSwitch -- the switches are BookSim routers, not
  // endpoints. But SimCCL requires one anyway: MockNcclComm's constructor
  // unconditionally calls MockNcclGroup::get_nvls_channels(), which indexes
  // GroupInfo::NVSwitchs[0] and segfaults on an empty vector
  // (MockNcclGroup.cc:1967). So the NVSwitch ranks exist purely to satisfy that
  // data structure. They carry no traffic unless the NVLS algorithm is enabled,
  // and the BookSim topology just needs that many extra endpoints.
  const int gpu_num = args.gpus;
  int nvswitch_num = gpu_num / args.gpus_per_server;
  if(nvswitch_num < 1) nvswitch_num = 1;
  const int nodes_num = gpu_num + nvswitch_num;

  if(BookSimSim::GetEngine()->NumNodes() < nodes_num) {
    std::cerr << "BookSim topology has " << BookSimSim::GetEngine()->NumNodes()
              << " nodes but " << nodes_num << " are required ("
              << gpu_num << " GPUs + " << nvswitch_num
              << " NVSwitch placeholder(s) for SimCCL)." << std::endl;
    return 1;
  }

  std::map<int, int> node2nvswitch;
  std::vector<int> NVswitchs;
  for(int i = 0; i < gpu_num; ++i) {
    node2nvswitch[i] = gpu_num + i / args.gpus_per_server;
  }
  for(int i = gpu_num; i < nodes_num; ++i) {
    node2nvswitch[i] = i;
    NVswitchs.push_back(i);
  }

  std::vector<int> physical_dims;
  physical_dims.push_back(nodes_num);
  std::vector<int> queues_per_dim(1, 1);
  std::vector<int> all_gpus;
  all_gpus.push_back(gpu_num);

  std::vector<BookSimNetwork *> networks(nodes_num, NULL);
  std::vector<AstraSim::Sys *>  systems(nodes_num, NULL);

  for(int j = 0; j < nodes_num; ++j) {
    networks[j] = new BookSimNetwork(j);
    systems[j] = new AstraSim::Sys(
        networks[j],
        NULL,
        j,
        0,
        1,
        physical_dims,
        queues_per_dim,
        "",
        args.workload,
        1,
        1,
        1,
        1,
        0,
        args.result_prefix,
        "booksim_test",
        true,
        false,
        gpu_type,
        all_gpus,
        NVswitchs,
        args.gpus_per_server);
    systems[j]->nvswitch_id = node2nvswitch[j];
    systems[j]->num_gpus    = nodes_num - nvswitch_num;
  }

  // Sys.cc guards these behind #if defined(NS3_MTP) || NS3_MPI || PHY_MTP, but
  // NcclFlowModel is the unconditional default collective and
  // generate_collective_phase dereferences mock_nccl_comms. Both methods are
  // public and grobal_group_init is idempotent, so call them here rather than
  // relying on a build macro. Ordering matters: every init must precede any
  // fire(), because comms setup is collective across ranks.
  for(int j = 0; j < nodes_num; ++j) {
    systems[j]->mock_nccl_grobal_group_init();
    systems[j]->mock_nccl_comms_init();
  }

  for(int j = 0; j < nodes_num; ++j) {
    systems[j]->workload->fire();
  }

  std::cout << "SimAI-BookSim: running " << nodes_num << " ranks over "
            << args.booksim_config << std::endl;

  BookSimSim::Run();

  std::cout << "SimAI-BookSim finished at " << BookSimSim::Now() << " ns"
            << std::endl;
  BookSimSim::ReportExitState();

  bool clean = expeRecvHash.empty() && sentHash.empty() &&
               BookSimSim::ScheduleViolations() == 0;
  return clean ? 0 : 1;
}

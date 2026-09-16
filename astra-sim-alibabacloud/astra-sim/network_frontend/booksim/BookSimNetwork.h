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

#ifndef __BOOKSIMNETWORK_H__
#define __BOOKSIMNETWORK_H__

#include <cstdint>

#include "astra-sim/system/AstraNetworkAPI.hh"

// One per rank. All instances share the single global BookSim engine and clock
// held by BookSimSim -- Sys::boostedTick() always reads rank 0's NI, so every
// rank must report the same time.
class BookSimNetwork : public AstraSim::AstraNetworkAPI {
public:
  BookSimNetwork(int rank);
  ~BookSimNetwork();

  int sim_comm_size(AstraSim::sim_comm comm, int * size);
  int sim_finish();
  double sim_time_resolution();
  int sim_init(AstraSim::AstraMemoryAPI * MEM);
  AstraSim::timespec_t sim_get_time();

  void sim_schedule(AstraSim::timespec_t delta,
                    void (*fun_ptr)(void * fun_arg), void * fun_arg);

  int sim_send(void * buffer, uint64_t count, int type, int dst, int tag,
               AstraSim::sim_request * request,
               void (*msg_handler)(void * fun_arg), void * fun_arg);

  int sim_recv(void * buffer, uint64_t count, int type, int src, int tag,
               AstraSim::sim_request * request,
               void (*msg_handler)(void * fun_arg), void * fun_arg);
};

// Software send latency applied before a transfer is injected, in ns.
// ns-3 takes this from NCCL's per-(algorithm, protocol) tuning tables, where it
// is 7.2-15.9 us. For an intra-host PCIe study that term dominates and would
// swamp the fabric differences being measured, so it defaults to 0 here and is
// opt-in via --send-lat-ns.
extern uint64_t g_send_lat_ns;

#endif

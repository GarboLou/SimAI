/*
 *Copyright (c) 2024, Alibaba Group;
 *Copyright (c) 2026, Jiaqi Lou;
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
#include "SendLatency.h"

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

// Software latency applied before a transfer is injected; see SendLatency.h.
// Defaults to NCCL's own latency model with its PCIe numbers. Set by main().
extern SendLatency::Config g_sw_lat;

// Prints how much software latency was charged. Called at exit.
void BookSimNetworkReportSendLatency();

#endif

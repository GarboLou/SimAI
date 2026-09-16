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

#ifndef __BOOKSIM_ENTRY_H__
#define __BOOKSIM_ENTRY_H__

#include <cstdint>
#include <iostream>
#include <map>
#include <utility>

#include "astra-sim/system/AstraNetworkAPI.hh"
#include "astra-sim/system/RecvPacketEventHadndlerData.hh"
#include "astra-sim/system/SendPacketEventHandlerData.hh"
#include "SimCCL/mock/MockNcclLog.h"

// Send/receive rendezvous for the BookSim backend.
//
// Ported from network_frontend/ns3/entry.h. What is kept here encodes the
// ASTRA-sim contract, not anything about ns-3: a sim_send and its matching
// sim_recv can arrive in either order, so both the "data arrived early"
// (recvHash) and "receive posted early" (expeRecvHash) sides are needed.
//
// What is dropped: the whole RDMA queue-pair layer -- sender_src_port_map,
// waiting_to_sent_callback, waiting_to_notify_receiver, sent_chunksize,
// received_chunksize, qp_finish, send_finish, SendFlow. ns-3 needs those to
// reassemble one logical transfer from several queue pairs, but
// _QPS_PER_CONNECTION_ is 1 (ns3/entry.h:21) so every one of those counters is
// 1-deep, and BookSim carries the ncclFlowTag on the flow handle instead of
// recovering it from a source port.

struct task1 {
  int      src;
  int      dest;
  int      type;
  uint64_t count;
  void *   fun_arg;
  void (*msg_handler)(void * fun_arg);
  double   schTime;
};

// Keyed by (tag, (src, dst)) throughout, where tag is always flowTag.tag_id.
extern std::map<std::pair<int, std::pair<int, int> >, struct task1>  expeRecvHash;
extern std::map<std::pair<int, std::pair<int, int> >, uint64_t>      recvHash;
extern std::map<std::pair<int, std::pair<int, int> >, struct task1>  sentHash;
extern std::map<std::pair<int, int>, int64_t>                        nodeHash;
extern std::map<std::pair<std::pair<int, int>, int>, AstraSim::ncclFlowTag>
                                                                     receiver_pending_queue;

// Called when the last byte of a transfer has been delivered by the fabric.
void notify_receiver_receive_data(int sender_node, int receiver_node,
                                  uint64_t message_size,
                                  AstraSim::ncclFlowTag flowTag);

// Called when the last byte has left the sender.
void notify_sender_sending_finished(int sender_node, int receiver_node,
                                    uint64_t message_size,
                                    AstraSim::ncclFlowTag flowTag);

#endif

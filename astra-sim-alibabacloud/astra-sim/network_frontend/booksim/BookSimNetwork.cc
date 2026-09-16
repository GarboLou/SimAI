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

#include "engine/booksim_engine.hpp"

#include <cassert>
#include <iostream>

uint64_t g_send_lat_ns = 0;

namespace {

// Everything the completion callbacks need to reconstruct the notify_* call.
// Allocated per transfer and freed when the receive completes.
struct PendingFlow {
  int                   src;
  int                   dst;
  uint64_t              bytes;
  AstraSim::ncclFlowTag flowTag;
  bool                  sent_notified;
  bool                  recv_notified;
};

void OnSendComplete(booksim::FlowId, void * user, uint64_t, void *) {
  PendingFlow * pf = (PendingFlow *)user;
  if(!pf || pf->sent_notified) return;
  pf->sent_notified = true;
  notify_sender_sending_finished(pf->src, pf->dst, pf->bytes, pf->flowTag);
}

void OnRecvComplete(booksim::FlowId, void * user, uint64_t, void *) {
  PendingFlow * pf = (PendingFlow *)user;
  if(!pf || pf->recv_notified) return;
  pf->recv_notified = true;
  notify_receiver_receive_data(pf->src, pf->dst, pf->bytes, pf->flowTag);
  // The receive is the later of the two completions, so the record is dead now.
  delete pf;
}

// Deferred injection, used when a non-zero software send latency is configured.
void InjectDeferred(void * arg) {
  PendingFlow * pf = (PendingFlow *)arg;
  booksim::FlowDesc d;
  d.src = pf->src; d.dst = pf->dst; d.bytes = pf->bytes; d.cls = 0; d.user = pf;
  BookSimSim::GetEngine()->InjectFlow(d);
}

} // namespace

BookSimNetwork::BookSimNetwork(int rank) : AstraNetworkAPI(rank) {}
BookSimNetwork::~BookSimNetwork() {}

int BookSimNetwork::sim_comm_size(AstraSim::sim_comm comm, int * size) { return 0; }
double BookSimNetwork::sim_time_resolution() { return 0; }
int BookSimNetwork::sim_init(AstraSim::AstraMemoryAPI * MEM) { return 0; }

AstraSim::timespec_t BookSimNetwork::sim_get_time() {
  AstraSim::timespec_t t;
  t.time_res = AstraSim::NS;
  // Global, not per-rank: Sys::boostedTick() reads all_generators[0]->NI
  // unconditionally, so every rank must agree.
  t.time_val = (double)BookSimSim::Now();
  return t;
}

void BookSimNetwork::sim_schedule(AstraSim::timespec_t delta,
                                  void (*fun_ptr)(void * fun_arg),
                                  void * fun_arg) {
  // delta is RELATIVE ns: Sys::generate_time() overwrites time_val with
  // cycles * CLOCK_PERIOD and discards the current time.
  BookSimSim::Schedule((uint64_t)delta.time_val, fun_ptr, fun_arg);
}

int BookSimNetwork::sim_send(void * buffer, uint64_t count, int type, int dst,
                             int tag, AstraSim::sim_request * request,
                             void (*msg_handler)(void * fun_arg),
                             void * fun_arg) {
  task1 t;
  t.src         = rank;
  t.dest        = dst;
  t.count       = count;
  t.type        = 0;
  t.fun_arg     = fun_arg;
  t.msg_handler = msg_handler;
  sentHash[std::make_pair(tag, std::make_pair(t.src, t.dest))] = t;

  PendingFlow * pf = new PendingFlow;
  pf->src           = rank;
  pf->dst           = dst;
  pf->bytes         = count;
  pf->flowTag       = request->flowTag;
  pf->sent_notified = false;
  pf->recv_notified = false;

  if(g_send_lat_ns > 0) {
    BookSimSim::Schedule(g_send_lat_ns, &InjectDeferred, pf);
  } else {
    booksim::FlowDesc d;
    d.src = rank; d.dst = dst; d.bytes = count; d.cls = 0; d.user = pf;
    BookSimSim::GetEngine()->InjectFlow(d);
  }
  return 0;
}

int BookSimNetwork::sim_recv(void * buffer, uint64_t count, int type, int src,
                             int tag, AstraSim::sim_request * request,
                             void (*msg_handler)(void * fun_arg),
                             void * fun_arg) {
  MockNcclLog * NcclLog = MockNcclLog::getInstance();
  AstraSim::RecvPacketEventHadndlerData * ehd =
      (AstraSim::RecvPacketEventHadndlerData *)fun_arg;

  // NcclFlowModel calls sim_recv with tag == channel_id but sim_send with
  // tag == flowTag.tag_id. Without this re-derivation the two key spaces never
  // intersect and every receive hangs. (ns3/AstraSimNetwork.cc:152)
  tag = ehd->flowTag.tag_id;

  task1 t;
  t.src         = src;
  t.dest        = rank;
  t.count       = count;
  t.type        = 1;
  t.fun_arg     = fun_arg;
  t.msg_handler = msg_handler;

  NcclLog->writeLog(NcclLogLevel::DEBUG,
      "[Receive event registration] src %d sim_recv on rank %d tag_id %d channel id %d",
      src, rank, tag, ehd->flowTag.channel_id);

  const std::pair<int, std::pair<int, int> > key =
      std::make_pair(tag, std::make_pair(t.src, t.dest));

  std::map<std::pair<int, std::pair<int, int> >, uint64_t>::iterator rit =
      recvHash.find(key);

  if(rit != recvHash.end()) {
    // Data already arrived; complete inline. This path is expected and is what
    // the ns-3 backend does too.
    uint64_t arrived = rit->second;
    if(arrived == t.count || arrived > t.count) {
      if(arrived == t.count) {
        recvHash.erase(rit);
      } else {
        rit->second = arrived - t.count;
      }
      assert(ehd->flowTag.child_flow_id == -1 && ehd->flowTag.current_flow_id == -1);
      std::pair<std::pair<int, int>, int> pk =
          std::make_pair(std::make_pair(rank, src), tag);
      std::map<std::pair<std::pair<int, int>, int>, AstraSim::ncclFlowTag>::iterator
          pit = receiver_pending_queue.find(pk);
      if(pit != receiver_pending_queue.end()) {
        ehd->flowTag = pit->second;
        receiver_pending_queue.erase(pit);
      }
      t.msg_handler(t.fun_arg);
      return 0;
    }
    // Less arrived than requested: consume it and wait for the rest.
    recvHash.erase(rit);
    t.count -= arrived;
    expeRecvHash[key] = t;
    return 0;
  }

  if(expeRecvHash.find(key) == expeRecvHash.end()) {
    expeRecvHash[key] = t;
    NcclLog->writeLog(NcclLogLevel::DEBUG,
        "[Receive posted before data] src %d dest %d count %lu tag_id %d",
        t.src, t.dest, t.count, tag);
  } else {
    NcclLog->writeLog(NcclLogLevel::DEBUG,
        "[Receive re-posted] src %d dest %d count %lu tag_id %d",
        t.src, t.dest, t.count, tag);
  }
  return 0;
}

int BookSimNetwork::sim_finish() {
  for(std::map<std::pair<int, int>, int64_t>::iterator it = nodeHash.begin();
      it != nodeHash.end(); ++it) {
    std::pair<int, int> p = it->first;
    if(p.second == 0) {
      std::cout << "All data sent from node " << p.first << " is " << it->second
                << std::endl;
    } else {
      std::cout << "All data received by node " << p.first << " is "
                << it->second << std::endl;
    }
  }
  BookSimSim::ReportExitState();
  exit(0);
  return 0;
}

// Registered once, by main(), against the single shared engine.
void BookSimNetworkInstallCallbacks() {
  BookSimSim::GetEngine()->SetOnSendComplete(&OnSendComplete, NULL);
  BookSimSim::GetEngine()->SetOnRecvComplete(&OnRecvComplete, NULL);
}

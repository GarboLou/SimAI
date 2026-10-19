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

#include "BookSimNetwork.h"
#include "BookSimSim.h"
#include "entry.h"

#include "engine/booksim_engine.hpp"

#include <cassert>
#include <iostream>

SendLatency::Config g_sw_lat;

namespace {

// What the software latency charged, for the exit report.
struct SwLatStats {
  uint64_t transfers;
  uint64_t startups;        // transfers that also paid the startup
  uint64_t startup_ns;
  uint64_t handoff_ns;
  uint64_t by_proto[SendLatency::kNumProtos];  // kNccl: protocol looked up
  uint64_t ll128_as_simple;
  uint64_t unset_as_simple;
  SwLatStats() : transfers(0), startups(0), startup_ns(0), handoff_ns(0),
                 ll128_as_simple(0), unset_as_simple(0) {
    for(int p = 0; p < SendLatency::kNumProtos; ++p) by_proto[p] = 0;
  }
};
SwLatStats g_sw_stats;

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

// Deferred injection, used when a transfer has a non-zero software latency.
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
  // delta is relative ns: Sys::generate_time() overwrites time_val with
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

  const AstraSim::ncclFlowTag & ft = request->flowTag;
  SendLatency::Delay lat;
  if(!SendLatency::Lookup(g_sw_lat, ft.algorithm, ft.protocol, ft.chunk_id, lat)) {
    std::cerr << "SimAI-BookSim: transfer " << rank << " -> " << dst
              << " has NCCL algorithm " << ft.algorithm << ", which has no "
              << "PCIe latency (only Tree, Ring and PAT run without a switch "
              << "that adds). Use --send-lat-ns or --sw-lat off." << std::endl;
    exit(1);
  }
  const uint64_t delay = lat.startup_ns + lat.handoff_ns;
  ++g_sw_stats.transfers;
  if(lat.startup_ns > 0) ++g_sw_stats.startups;
  g_sw_stats.startup_ns += lat.startup_ns;
  g_sw_stats.handoff_ns += lat.handoff_ns;
  if(lat.proto >= 0) ++g_sw_stats.by_proto[lat.proto];
  if(lat.proto_remapped) {
    if(ft.protocol == SendLatency::kProtoLL128) ++g_sw_stats.ll128_as_simple;
    else ++g_sw_stats.unset_as_simple;
  }

  if(delay > 0) {
    BookSimSim::Schedule(delay, &InjectDeferred, pf);
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
  BookSimNetworkReportSendLatency();
  BookSimSim::ReportExitState();
  exit(0);
  return 0;
}

void BookSimNetworkReportSendLatency() {
  const SwLatStats & s = g_sw_stats;
  std::cout << "--- software latency ---" << std::endl;
  std::cout << "  transfers             " << s.transfers << std::endl;
  std::cout << "  startups charged      " << s.startups << " ("
            << s.startup_ns << " ns summed)" << std::endl;
  std::cout << "  hand-offs charged     " << s.handoff_ns << " ns summed"
            << std::endl;
  if(g_sw_lat.mode == SendLatency::kNccl) {
    std::cout << "  looked up as LL       " << s.by_proto[SendLatency::kProtoLL]
              << std::endl;
    std::cout << "  looked up as Simple   " << s.by_proto[SendLatency::kProtoSimple]
              << " (" << s.ll128_as_simple << " chosen as LL128, "
              << s.unset_as_simple << " with no protocol)" << std::endl;
  }
}

// Registered once, by main(), against the single shared engine.
void BookSimNetworkInstallCallbacks() {
  BookSimSim::GetEngine()->SetOnSendComplete(&OnSendComplete, NULL);
  BookSimSim::GetEngine()->SetOnRecvComplete(&OnRecvComplete, NULL);
}

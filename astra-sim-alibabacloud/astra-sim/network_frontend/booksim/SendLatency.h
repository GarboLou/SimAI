/*
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

#ifndef __BOOKSIM_SENDLATENCY_H__
#define __BOOKSIM_SENDLATENCY_H__

#include <cstdint>

// Software latency charged before a transfer is handed to BookSim: the time
// NCCL spends that is not bytes on the wire.
//
// Follows NCCL's own latency model (nccl v2.30.7, src/graph/tuning.cc:148-176
// and 384-412). For a ring collective on one host NCCL estimates
//   latency = baseLatency[algo][proto] + nsteps * hwLatency[PCI][algo][proto]
// The base latency is a per-collective startup (kernel launch, setup); the
// hardware latency is a per-step hand-off (the "ready" flag write and the
// receiving GPU noticing it). NCCL uses the PCI row whenever the GPUs are not
// linked by NVLink (tuning.cc:285). Here every transfer waits one hand-off and
// the first transfer of each chain (chunk_id 0) also waits the startup, which
// puts exactly that sum on every chain's critical path.
//
// The ns-3 frontend (ns3/entry.h) charges base + hwLatency[NVLINK] on every
// transfer instead: the startup once per step, and NVLink numbers. Neither
// fits a PCIe fabric.
//
// These are NCCL's tuning estimates, used to choose an algorithm, not
// measurements. Replace them with values measured on the target machine
// (SimAI_booksim --sw-startup-ns / --sw-handoff-ns).
//
// Kept free of ASTRA-sim types so tests/test_send_latency.cc can build it alone.
namespace SendLatency {

enum Mode {
  kNccl,  // NCCL's model, PCI row (default)
  kFlat,  // the same fixed delay before every transfer, like ns-3's AS_SEND_LAT
  kOff    // no software latency
};

struct Config {
  Mode     mode;
  uint64_t flat_ns;     // kFlat: every transfer waits exactly this
  int64_t  startup_ns;  // kNccl: replaces the table's startup when >= 0
  int64_t  handoff_ns;  // kNccl: replaces the table's hand-off when >= 0
  Config() : mode(kNccl), flat_ns(0), startup_ns(-1), handoff_ns(-1) {}
};

// NCCL algorithm and protocol ids, as in SimCCL's MockNccl.h.
enum { kAlgoTree = 0, kAlgoRing = 1, kAlgoPat = 6, kNumAlgos = 7 };
enum { kProtoLL = 0, kProtoLL128 = 1, kProtoSimple = 2, kNumProtos = 3 };

// baseLatencies (tuning.cc:150-154), ns. Rows: Tree, Ring, CollNetDirect,
// CollNetChain, NVLS, NVLSTree, PAT. Columns: LL, LL128, Simple.
static const uint32_t kStartupNs[kNumAlgos][kNumProtos] = {
  { 6800, 14000, 8400 }, { 6600, 14000, 8400 },
  {    0,     0,    0 }, {    0,     0,    0 },
  {    0,     0,    0 }, {    0,     0,    0 },
  { 8000,  8000, 8000 }
};

// hwLatencies[NCCL_HW_PCI] (tuning.cc:165-168), ns. Same layout.
static const uint32_t kHandoffNs[kNumAlgos][kNumProtos] = {
  { 1000, 1900, 4000 }, { 1000, 2500, 5700 },
  {    0,    0, 3700 }, {    0,    0, 2800 },
  {    0,    0,    0 }, {    0,    0,    0 },
  {    0,    0, 4000 }
};

struct Delay {
  uint64_t startup_ns;      // 0 unless this transfer starts a chain
  uint64_t handoff_ns;
  int      proto;           // protocol the lookup used (kNccl only, else -1)
  bool     proto_remapped;  // proto differs from the one SimCCL chose
  Delay() : startup_ns(0), handoff_ns(0), proto(-1), proto_remapped(false) {}
};

// The protocol NCCL would run over PCIe. NCCL enables LL128 only when the GPUs
// are linked by NVLink (tuning.cc:530, typeIntra <= PATH_NVB), so it never
// runs LL128 over PCIe; SimCCL's size thresholds come from an NVLink machine
// and do pick it (4-16 MiB). At those sizes NCCL's choice without LL128 is
// Simple. An unset protocol (SIMAI_PROTO_AWARE=0) is taken as Simple, and PAT
// is Simple-only in NCCL.
inline int PcieProtocol(int algo, int proto) {
  if(algo == kAlgoPat) return kProtoSimple;
  return (proto == kProtoLL) ? kProtoLL : kProtoSimple;
}

// false for an algorithm that cannot run on a PCIe fabric (CollNet and NVLS
// need a switch that adds) or an unknown id, which means the transfer did not
// come from NcclFlowModel.
inline bool Lookup(const Config & c, int algo, int proto, int chunk_id, Delay & d) {
  d = Delay();
  if(c.mode == kOff) return true;
  if(c.mode == kFlat) {
    d.handoff_ns = c.flat_ns;
    return true;
  }
  if(algo != kAlgoTree && algo != kAlgoRing && algo != kAlgoPat) return false;
  d.proto = PcieProtocol(algo, proto);
  d.proto_remapped = (d.proto != proto);
  const uint64_t startup = (c.startup_ns >= 0) ? (uint64_t)c.startup_ns
                                               : kStartupNs[algo][d.proto];
  d.handoff_ns = (c.handoff_ns >= 0) ? (uint64_t)c.handoff_ns
                                     : kHandoffNs[algo][d.proto];
  d.startup_ns = (chunk_id == 0) ? startup : 0;
  return true;
}

} // namespace SendLatency

#endif

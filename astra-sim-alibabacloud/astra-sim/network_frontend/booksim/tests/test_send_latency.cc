// Unit test for SendLatency.h. Build and run with tests/run_tests.sh.

#include "../SendLatency.h"

#include <cstdio>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK_EQ(a, b) do {                                                   \
    ++g_checks;                                                               \
    if(!((a) == (b))) {                                                       \
      ++g_failures;                                                           \
      std::printf("FAIL %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, \
                  #a, #b, (unsigned long long)(a), (unsigned long long)(b));  \
    }                                                                         \
  } while(0)
#define CHECK(c) CHECK_EQ(!!(c), 1)

using namespace SendLatency;

// What one chain of `steps` dependent transfers waits in total: the sum over
// chunk ids 0..steps-1, as NcclFlowModel issues them.
static uint64_t ChainNs(const Config & c, int algo, int proto, int steps) {
  uint64_t sum = 0;
  for(int k = 0; k < steps; ++k) {
    Delay d;
    if(!Lookup(c, algo, proto, k, d)) return ~0ull;
    sum += d.startup_ns + d.handoff_ns;
  }
  return sum;
}

int main() {
  Config nccl;

  // NCCL's latency for an 8-GPU ring all-reduce on PCIe (14 steps):
  // base + 14 * hwLat[PCI], tuning.cc:384-412.
  CHECK_EQ(ChainNs(nccl, kAlgoRing, kProtoLL, 14), 6600u + 14u * 1000u);
  CHECK_EQ(ChainNs(nccl, kAlgoRing, kProtoSimple, 14), 8400u + 14u * 5700u);
  // Reduce-scatter / all-gather: 7 steps.
  CHECK_EQ(ChainNs(nccl, kAlgoRing, kProtoSimple, 7), 8400u + 7u * 5700u);
  CHECK_EQ(ChainNs(nccl, kAlgoTree, kProtoLL, 3), 6800u + 3u * 1000u);
  // An all-to-all transfer is a chain of one.
  CHECK_EQ(ChainNs(nccl, kAlgoRing, kProtoLL, 1), 6600u + 1000u);

  // The startup is charged on chunk 0 only.
  for(int k = 0; k < 20; ++k) {
    Delay d;
    CHECK(Lookup(nccl, kAlgoRing, kProtoSimple, k, d));
    CHECK_EQ(d.startup_ns, k == 0 ? 8400u : 0u);
    CHECK_EQ(d.handoff_ns, 5700u);
  }

  // Protocols: LL128 never runs over PCIe and an unset protocol is Simple;
  // both are reported as remapped. LL and Simple are kept.
  {
    Delay d;
    CHECK(Lookup(nccl, kAlgoRing, kProtoLL128, 0, d));
    CHECK_EQ(d.proto, (int)kProtoSimple);
    CHECK(d.proto_remapped);
    CHECK_EQ(d.startup_ns, 8400u);
    CHECK_EQ(d.handoff_ns, 5700u);
    CHECK(Lookup(nccl, kAlgoRing, -1, 3, d));
    CHECK_EQ(d.proto, (int)kProtoSimple);
    CHECK(d.proto_remapped);
    CHECK(Lookup(nccl, kAlgoRing, kProtoLL, 0, d));
    CHECK_EQ(d.proto, (int)kProtoLL);
    CHECK(!d.proto_remapped);
    CHECK(Lookup(nccl, kAlgoRing, kProtoSimple, 0, d));
    CHECK(!d.proto_remapped);
    CHECK(Lookup(nccl, kAlgoPat, kProtoLL, 0, d));   // PAT is Simple-only
    CHECK_EQ(d.proto, (int)kProtoSimple);
    CHECK_EQ(d.startup_ns, 8000u);
    CHECK_EQ(d.handoff_ns, 4000u);
  }

  // Every usable (algorithm, protocol) has a non-zero hand-off and startup,
  // so no combination silently costs nothing.
  const int algos[] = { kAlgoTree, kAlgoRing, kAlgoPat };
  for(int a = 0; a < 3; ++a) {
    for(int p = -1; p < kNumProtos; ++p) {
      Delay d;
      CHECK(Lookup(nccl, algos[a], p, 0, d));
      CHECK(d.startup_ns > 0);
      CHECK(d.handoff_ns > 0);
    }
  }

  // Algorithms that need a switch that adds, and unknown ids, are refused.
  const int bad[] = { -1, 2, 3, 4, 5, 7, 100 };
  for(int i = 0; i < 7; ++i) {
    Delay d;
    CHECK(!Lookup(nccl, bad[i], kProtoSimple, 0, d));
  }

  // Overrides replace the table, for every algorithm and protocol.
  {
    Config c;
    c.startup_ns = 0;
    c.handoff_ns = 1000;
    CHECK_EQ(ChainNs(c, kAlgoRing, kProtoLL, 14), 14000u);
    CHECK_EQ(ChainNs(c, kAlgoTree, kProtoSimple, 14), 14000u);
    c.startup_ns = 20000;
    c.handoff_ns = 0;
    CHECK_EQ(ChainNs(c, kAlgoRing, kProtoSimple, 14), 20000u);
  }

  // Flat: the same delay before every transfer, whatever it is.
  {
    Config c;
    c.mode = kFlat;
    c.flat_ns = 6000;
    CHECK_EQ(ChainNs(c, kAlgoRing, kProtoLL128, 14), 14u * 6000u);
    Delay d;
    CHECK(Lookup(c, 4, kProtoSimple, 0, d));     // no algorithm check
    CHECK_EQ(d.startup_ns, 0u);
    CHECK_EQ(d.handoff_ns, 6000u);
  }

  // Off: nothing, and nothing is refused.
  {
    Config c;
    c.mode = kOff;
    CHECK_EQ(ChainNs(c, kAlgoRing, kProtoSimple, 14), 0u);
    Delay d;
    CHECK(Lookup(c, 4, kProtoSimple, 0, d));
  }

  std::printf("test_send_latency: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}

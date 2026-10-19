#!/usr/bin/env python3
"""End-to-end check of SimAI_booksim's software latency (SendLatency.h).

Runs the 8-GPU microAllReduce workload (16 MiB and 64 MiB all-reduce) and a
copy with 64 KiB all-reduces on the flat PCIe topology, and checks:

  1. Accounting: every transfer pays one hand-off and only the first step of
     each chain (8 GPUs x 8 channels per collective) pays the startup, with
     NCCL 2.30's PCIe numbers; SimCCL's LL128 choice is looked up as Simple.
  2. --send-lat-ns: the same delay before every transfer, no startup.
  3. Finish time: per collective the run gets at least one startup plus one
     hand-off later, and never more than NCCL's whole estimate (startup +
     14 hand-offs). With 64 KiB nearly all of the estimate shows (>= 90%),
     which needs the hand-offs to add up along the ring.
  4. Bad flag combinations are refused.
  5. BookSim's fabric stats are printed at exit: one TLP per 256 B sent, and
     one switch per TLP on the flat topology.

    tests/sw_latency_check.py --bin <SimAI>/bin/SimAI_booksim \
        --booksim2 <booksim2 root> --workload <SimAI>/example/microAllReduce.txt
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile

GPUS, CHANNELS, STEPS = 8, 8, 14          # 8-GPU ring all-reduce, SimCCL default
# NCCL 2.30.7 tuning.cc, Ring, PCI row (ns): startup, hand-off.
RING = {"LL": (6600, 1000), "Simple": (8400, 5700)}


def run(args, tmp, workload, extra):
    cmd = [args.bin, "-w", workload, "-c", args.cfg, "-g", str(GPUS), "-g_p_s", str(GPUS)] + extra
    out = subprocess.run(cmd, cwd=tmp, capture_output=True, text=True, timeout=args.timeout)
    return out.returncode, out.stdout + out.stderr


def stat(text, label):
    m = re.search(r"^  %s\s+(\d+)" % re.escape(label), text, re.M)
    return int(m.group(1)) if m else None


def finish(text):
    return stat(text, "time")


def summed(text, label):
    m = re.search(r"^  %s.*?\((\d+) ns summed\)|^  %s\s+(\d+) ns summed" % (label, label), text, re.M)
    return int(m.group(1) or m.group(2)) if m else None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--booksim2", required=True, help="booksim2 root (for runfiles/)")
    ap.add_argument("--workload", required=True, help="SimAI example/microAllReduce.txt")
    ap.add_argument("--timeout", type=int, default=600)
    args = ap.parse_args()
    args.bin = os.path.abspath(args.bin)
    args.workload = os.path.abspath(args.workload)   # runs execute in a temp dir
    fails = []

    def check(ok, what):
        print("%s %s" % ("ok  " if ok else "FAIL", what))
        if not ok:
            fails.append(what)

    with tempfile.TemporaryDirectory() as tmp:
        # The repo config names its topology relative to the booksim2 root.
        root = os.path.abspath(args.booksim2)
        text = open(os.path.join(root, "runfiles/simai_flat.cfg")).read()
        text = re.sub(r"^\s*network_file\s*=\s*([^;]+);",
                      lambda m: "network_file = %s;" % os.path.join(root, m.group(1).strip()),
                      text, flags=re.M)
        args.cfg = os.path.join(tmp, "flat.cfg")
        open(args.cfg, "w").write(text)
        work = open(args.workload).read()
        sizes = [int(s) for s in re.findall(r"ALLREDUCE\s+(\d+)", work)]
        small = os.path.join(tmp, "ar64k.txt")
        open(small, "w").write(re.sub(r"(ALLREDUCE\s+)\d+", r"\g<1>65536", work))
        ncoll = len(sizes)
        transfers = ncoll * GPUS * CHANNELS * STEPS
        startups = ncoll * GPUS * CHANNELS

        rc_off, off = run(args, tmp, args.workload, ["--sw-lat", "off"])
        rc_on, on = run(args, tmp, args.workload, [])
        rc_flat, flat = run(args, tmp, args.workload, ["--send-lat-ns", "6000"])
        check(rc_off == rc_on == rc_flat == 0, "runs complete (rc %s %s %s)" % (rc_off, rc_on, rc_flat))
        if fails:
            print(off[-600:])
            print("1 failure(s)")
            return 1

        # SimCCL: LL up to 4 MiB, LL128 up to 16 MiB, Simple above; LL128 -> Simple.
        proto = ["LL" if s <= 4 << 20 else "Simple" for s in sizes]
        ll128 = sum(1 for s in sizes if 4 << 20 < s <= 16 << 20) * GPUS * CHANNELS * STEPS
        want_start = sum(RING[p][0] for p in proto) * GPUS * CHANNELS
        want_hand = sum(RING[p][1] for p in proto) * GPUS * CHANNELS * STEPS
        print("  collectives %s, protocols %s" % (sizes, proto))

        # 1. accounting
        check(stat(off, "transfers") == transfers and stat(off, "startups charged") == 0
              and summed(off, "hand-offs charged") == 0, "off: %d transfers, nothing charged" % transfers)
        check(stat(on, "transfers") == transfers, "nccl: %d transfers" % transfers)
        check(stat(on, "startups charged") == startups, "nccl: %d startups (chunk 0 of every chain), got %s"
              % (startups, stat(on, "startups charged")))
        check(summed(on, "startups charged") == want_start, "nccl: startup sum %d, got %s"
              % (want_start, summed(on, "startups charged")))
        check(summed(on, "hand-offs charged") == want_hand, "nccl: hand-off sum %d, got %s"
              % (want_hand, summed(on, "hand-offs charged")))
        m = re.search(r"looked up as Simple\s+(\d+) \((\d+) chosen as LL128", on)
        check(m is not None and int(m.group(2)) == ll128, "nccl: %d LL128 transfers looked up as Simple" % ll128)

        # BookSim's fabric stats at exit: one 256 B TLP per 256 B sent (every
        # flow is a multiple of the MPS), and on the flat topology every TLP
        # passes through exactly one switch.
        sent = sum(int(b) for b in re.findall(r"^All data sent from node \d+ is (\d+)", off, re.M))
        m_tlp = re.search(r"^tlp_count\s+(\d+)", off, re.M)
        m_sw = re.search(r"^tlp_switches_mean\s+(\S+)", off, re.M)
        m_lat = re.search(r"^tlp_lat_net_mean\s+(\S+)", off, re.M)
        check(sent > 0 and m_tlp is not None and int(m_tlp.group(1)) * 256 == sent,
              "fabric stats: %s TLPs for %d B sent" % (m_tlp and m_tlp.group(1), sent))
        check(m_sw is not None and float(m_sw.group(1)) == 1.0 and m_lat is not None
              and float(m_lat.group(1)) > 0, "fabric stats: 1 switch per TLP on flat, latency printed")

        # 2. flat
        check(stat(flat, "startups charged") == 0 and summed(flat, "hand-offs charged") == transfers * 6000,
              "flat: 6000 ns on each of %d transfers, no startup" % transfers)

        # 3. finish time
        t_off, t_on, t_flat = finish(off), finish(on), finish(flat)
        added = t_on - t_off
        lo = sum(RING[p][0] + RING[p][1] for p in proto)
        hi = sum(RING[p][0] + STEPS * RING[p][1] for p in proto)
        print("  finish: off %d, nccl %d (+%d), flat %d (+%d); NCCL's estimate +%d"
              % (t_off, t_on, added, t_flat, t_flat - t_off, hi))
        check(lo <= added <= hi, "nccl: added %d within [%d, %d]" % (added, lo, hi))
        check(ncoll * 6000 <= t_flat - t_off <= ncoll * STEPS * 6000, "flat: added within bounds")

        rc_a, s_off = run(args, tmp, small, ["--sw-lat", "off"])
        rc_b, s_on = run(args, tmp, small, [])
        s_added = finish(s_on) - finish(s_off)
        s_hi = ncoll * (RING["LL"][0] + STEPS * RING["LL"][1])
        print("  64 KiB: added %d of NCCL's estimate %d (%.0f%%)" % (s_added, s_hi, 100.0 * s_added / s_hi))
        check(rc_a == rc_b == 0 and 0.9 * s_hi <= s_added <= s_hi,
              "64 KiB: hand-offs add up along the ring (>= 90% of the estimate)")

        # 4. refused combinations
        for bad in (["--send-lat-ns", "10", "--sw-lat", "nccl"], ["--send-lat-ns", "10", "--sw-handoff-ns", "1"],
                    ["--sw-lat", "off", "--sw-startup-ns", "1"], ["--sw-lat", "fast"], ["--sw-handoff-ns", "-5"],
                    ["--send-lat-ns", "12x"],
                    # digits only: no space, sign or wrapped negative
                    ["--sw-startup-ns", " -18446744073709551615"], ["--sw-handoff-ns", " -5"],
                    ["--sw-handoff-ns", " 5"], ["--send-lat-ns", "+5"],
                    ["--sw-startup-ns", "99999999999999999999"]):
            rc, text = run(args, tmp, args.workload, bad)
            check(rc != 0 and "running" not in text, "refused: %s" % " ".join(bad))

    print("OK" if not fails else "%d failure(s)" % len(fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

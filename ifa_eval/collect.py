#!/usr/bin/env python3
"""Collects the results of run.py into a CSV and checks them.

usage: collect.py OUT_DIR CSV [--baseline CONFIG]

For every <config>/<workload> run directory, reads stats.txt and the program
output and writes one CSV row.  With --baseline, also checks that every run
of a workload commits the same number of instructions and produces the same
program output (stdout and files written) as the baseline configuration.
"""

import argparse
import csv
import hashlib
import os
import re
import sys

CPU = "system.cpu."

# executeCommitLimit of the configuration used (see config.ini)
COMMIT_WIDTH = 2

# CSV column -> stats.txt name
STATS = {
    "sim_seconds": "simSeconds",
    "cycles": CPU + "numCycles",
    "insts": CPU + "commitStats0.numInsts",
    "ops": CPU + "commitStats0.numOps",
    "ipc": CPU + "ipc",
    "cpi": CPU + "cpi",
    "branches": CPU + "branchPred.lookups_0::total",
    "branch_misses": CPU + "branchPred.mispredicted_0::total",
    "cond_incorrect": CPU + "branchPred.condIncorrect",
    "dcache_miss_rate": CPU + "dcache.demandMissRate::total",
    "dcache_avg_miss_latency_ticks": CPU + "dcache.demandAvgMissLatency::total",
    "icache_miss_rate": CPU + "icache.demandMissRate::total",
    "fetch2_decoded": CPU + "fetch2.totalInstructions",
    "ifa_dispatched": CPU + "fetch2.ifaDispatched",
    "ifa_dispatched_ooo": CPU + "fetch2.ifaDispatchedOutOfOrder",
    "ifa_hoisted": CPU + "fetch2.ifaHoisted",
    "ifa_skipped": CPU + "fetch2.ifaSkipped",
    "ifa_head_blocked_cycles": CPU + "fetch2.ifaHeadBlockedCycles",
    "ifa_reorder_cycles": CPU + "fetch2.ifaReorderCycles",
    "ifa_buffer_full_cycles": CPU + "fetch2.ifaBufferFullCycles",
    "ifa_flushes": CPU + "fetch2.ifaFlushes",
    "ifa_flushed_insts": CPU + "fetch2.ifaFlushedInsts",
    "ifa_occupancy_mean": CPU + "fetch2.ifaOccupancy::mean",
    "issue_stop_full": CPU + "issueStopCycles::Full",
    "issue_stop_no_input": CPU + "issueStopCycles::NoInput",
    "issue_stop_operands": CPU + "issueStopCycles::Operands",
    "issue_stop_fu": CPU + "issueStopCycles::FU",
    "issue_stop_mem_limit": CPU + "issueStopCycles::MemLimit",
    "issue_stop_other": CPU + "issueStopCycles::Other",
}


def read_stats(path):
    """First stats dump of a stats.txt as name -> float."""
    stats = {}
    with open(path) as f:
        for line in f:
            if line.startswith("---------- End"):
                break
            m = re.match(r"(\S+)\s+([-+0-9.eE]+|nan|inf)\s", line)
            if m:
                stats[m.group(1)] = float(m.group(2))
    return stats


def program_output(run_dir):
    """Digest of everything the program produced: stdout, stderr and the
    files it wrote into its cwd."""
    h = hashlib.sha256()
    gem5_files = {"stats.txt", "config.ini", "config.json", "cmd.txt",
                  "gem5.log", "citations.bib", "fs"}
    for name in sorted(os.listdir(run_dir)):
        if name in gem5_files or name.endswith(".dot") or \
                name.endswith(".svg") or name.endswith(".pdf"):
            continue
        path = os.path.join(run_dir, name)
        if os.path.isfile(path):
            h.update(name.encode() + b"\0")
            with open(path, "rb") as f:
                h.update(f.read())
    return h.hexdigest()[:16]


def exit_code(run_dir):
    with open(os.path.join(run_dir, "gem5.log"), errors="replace") as f:
        log = f.read()
    m = re.search(r"Simulated exit code not 0! Exit code is (\d+)", log)
    if m:
        return int(m.group(1))
    return 0 if "exiting with last active thread context" in log else None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("out")
    p.add_argument("csv")
    p.add_argument("--baseline", default="")
    a = p.parse_args()

    rows = []
    for cfg in sorted(os.listdir(a.out)):
        cfg_dir = os.path.join(a.out, cfg)
        if not os.path.isdir(cfg_dir):
            continue
        for wl in sorted(os.listdir(cfg_dir)):
            run_dir = os.path.join(cfg_dir, wl)
            stats_path = os.path.join(run_dir, "stats.txt")
            row = {"config": cfg, "workload": wl}
            if os.path.exists(stats_path):
                stats = read_stats(stats_path)
                for col, name in STATS.items():
                    row[col] = stats.get(name, "")
                # Stalls, from the commit side of a core which can commit
                # COMMIT_WIDTH (executeCommitLimit) instructions a cycle:
                #   stall_slots: commit slots left unused,
                #       COMMIT_WIDTH * cycles - insts
                #   stall_cycles: cycles beyond the ideal,
                #       cycles - insts / COMMIT_WIDTH
                # legacy_2x_c_minus_i is the metric used before this work,
                # 2 * (cycles - insts), for comparison only.
                if row["cycles"] != "" and row["insts"] != "":
                    c, i = row["cycles"], row["insts"]
                    row["stall_slots"] = COMMIT_WIDTH * c - i
                    row["stall_cycles"] = c - i / COMMIT_WIDTH
                    row["legacy_2x_c_minus_i"] = 2 * (c - i)
            row["exit_code"] = exit_code(run_dir)
            row["output_digest"] = program_output(run_dir)
            rows.append(row)

    cols = ["config", "workload"] + list(STATS) + \
        ["stall_slots", "stall_cycles", "legacy_2x_c_minus_i", "exit_code",
         "output_digest"]
    with open(a.csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=cols)
        w.writeheader()
        for r in rows:
            w.writerow({c: r.get(c, "") for c in cols})

    if not a.baseline:
        return
    base = {r["workload"]: r for r in rows if r["config"] == a.baseline}
    bad = 0
    for r in rows:
        b = base.get(r["workload"])
        if b is None:
            continue
        problems = []
        if r.get("insts") != b.get("insts"):
            problems.append(f"insts {r.get('insts')} != {b.get('insts')}")
        if r["output_digest"] != b["output_digest"]:
            problems.append("program output differs")
        if r["exit_code"] != b["exit_code"]:
            problems.append(f"exit code {r['exit_code']} != {b['exit_code']}")
        if problems:
            bad += 1
            print(f"MISMATCH {r['config']}/{r['workload']}: "
                  + "; ".join(problems))
    print(f"checked {len(rows)} runs against '{a.baseline}': {bad} mismatches")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()

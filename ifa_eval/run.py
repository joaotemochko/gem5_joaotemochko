#!/usr/bin/env python3
"""Runs the IFA evaluation workloads under gem5 (se.py) in parallel.

Every run gets its own directory <out>/<config>/<workload> holding the gem5
output (stats.txt, config.ini, ...), the program's stdout/stderr
(prog.out/prog.err) and any file the program writes (its cwd).

usage: run.py --gem5 BIN --bench DIR --mibench DIR --out DIR
              [--jobs N] [--workloads a,b,..] CONFIG [CONFIG ...]

A CONFIG is NAME=OPTIONS, OPTIONS being extra se.py options separated by
spaces, e.g. 'm2=--ifa-mode 2'.  The common system options are in COMMON.
"""

import argparse
import os
import shlex
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.realpath(__file__))
GEM5_ROOT = os.path.dirname(HERE)
SE_PY = os.path.join(GEM5_ROOT, "configs", "deprecated", "example", "se.py")

# RV64 MinorCPU at 1 GHz, L1-I 32 KiB 2-way, L1-D 64 KiB 2-way, no L2,
# DDR3-1600.  The branch predictor is the MinorCPU default (Tournament).
COMMON = [
    "--cpu-type=RiscvMinorCPU",
    "--cpu-clock=1GHz",
    "--sys-clock=1GHz",
    "--caches",
    "--l1i_size=32KiB",
    "--l1i_assoc=2",
    "--l1d_size=64KiB",
    "--l1d_assoc=2",
    "--mem-type=DDR3_1600_8x8",
    "--mem-size=512MiB",
]


def workloads(bench, mibench):
    """name -> (binary, args).  Args are run with the run directory as cwd;
    input files are given by absolute path."""
    mb = lambda *p: os.path.join(mibench, *p)
    w = {}
    embench = [
        "aha-mont64", "crc32", "depthconv", "edn", "huffbench",
        "matmult-int", "md5sum", "nettle-aes", "nettle-sha256", "nsichneu",
        "picojpeg", "qrduino", "sglib-combined", "slre", "statemate",
        "tarfind", "ud", "wikisort", "xgboost",
    ]
    for name in embench:
        w[name] = (os.path.join(bench, name), [])
    key = "1234567890abcdeffedcba0987654321"
    w["blowfish"] = (os.path.join(bench, "blowfish"),
                     ["e", mb("security", "blowfish", "input_small.asc"),
                      "out.enc", key])
    w["blowfish-d"] = (os.path.join(bench, "blowfish"),
                       ["d", os.path.join(bench, "blowfish_small.enc"),
                        "out.asc", key])
    w["sha"] = (os.path.join(bench, "sha"),
                [mb("security", "sha", "input_small.asc")])
    w["crc32-mib"] = (os.path.join(bench, "crc32-mib"),
                      [mb("telecomm", "adpcm", "data", "small.pcm")])
    w["qsort"] = (os.path.join(bench, "qsort"),
                  [mb("automotive", "qsort", "input_small.dat")])
    w["dijkstra"] = (os.path.join(bench, "dijkstra"),
                     [mb("network", "dijkstra", "input.dat")])
    w["stringsearch"] = (os.path.join(bench, "stringsearch"), [])
    w["basicmath"] = (os.path.join(bench, "basicmath"), [])
    w["ifa_stress"] = (os.path.join(bench, "ifa_stress"), [])
    return w


def run_one(gem5, se_py, cfg_name, cfg_opts, name, binary, args, out,
            common):
    run_dir = os.path.join(out, cfg_name, name)
    os.makedirs(run_dir, exist_ok=True)
    cmd = [gem5, "-d", run_dir, se_py] + common + cfg_opts + [
        "--output=" + os.path.join(run_dir, "prog.out"),
        "--errout=" + os.path.join(run_dir, "prog.err"),
        "-c", binary,
    ]
    if args:
        cmd += ["-o", " ".join(args)]
    with open(os.path.join(run_dir, "cmd.txt"), "w") as f:
        f.write(shlex.join(cmd) + "\n")
    start = time.time()
    with open(os.path.join(run_dir, "gem5.log"), "w") as log:
        rc = subprocess.call(cmd, cwd=run_dir, stdout=log,
                             stderr=subprocess.STDOUT)
    took = time.time() - start
    print(f"{cfg_name:>14} {name:<16} rc={rc} {took:7.1f}s", flush=True)
    return rc


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--gem5", required=True)
    p.add_argument("--bench", required=True)
    p.add_argument("--mibench", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--jobs", type=int, default=os.cpu_count())
    p.add_argument("--workloads", default="")
    p.add_argument("--se", default=SE_PY, help="se.py to use")
    p.add_argument("--no-common", action="store_true",
                   help="don't add the common Minor system options")
    p.add_argument("configs", nargs="+")
    a = p.parse_args()

    w = workloads(os.path.realpath(a.bench), os.path.realpath(a.mibench))
    names = a.workloads.split(",") if a.workloads else list(w)
    common = [] if a.no_common else COMMON
    jobs = []
    for cfg in a.configs:
        cfg_name, _, opts = cfg.partition("=")
        for name in names:
            binary, args = w[name]
            jobs.append((os.path.realpath(a.gem5), os.path.realpath(a.se),
                         cfg_name, shlex.split(opts),
                         name, binary, args, os.path.realpath(a.out),
                         common))
    with ThreadPoolExecutor(a.jobs) as ex:
        rcs = list(ex.map(lambda j: run_one(*j), jobs))
    failed = sum(1 for rc in rcs if rc != 0)
    print(f"{len(rcs)} runs, {failed} with non-zero exit")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()

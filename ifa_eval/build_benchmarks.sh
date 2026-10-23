#!/bin/sh
# Cross-compiles the workloads used to evaluate the IFA (RV64GC, static,
# newlib) into $OUT.
#
# usage: build_benchmarks.sh <embench-iot dir> <mibench dir> <out dir>
set -e

EMBENCH=$(realpath "$1")
MIBENCH=$(realpath "$2")
OUT=$(realpath -m "$3")
HERE=$(dirname "$(realpath "$0")")

CC=${CC:-riscv64-unknown-elf-gcc}
CFLAGS=${CFLAGS:-"-O2 -std=gnu17 -march=rv64gc -mabi=lp64d -static"}

mkdir -p "$OUT"
$CC --version | head -1 > "$OUT/toolchain.txt"
echo "CFLAGS=$CFLAGS" >> "$OUT/toolchain.txt"

# Embench-IoT: every benchmark in src/, with a main() that prints the result.
for dir in "$EMBENCH"/src/*/; do
    name=$(basename "$dir")
    $CC $CFLAGS -DWARMUP_HEAT=1 -DGLOBAL_SCALE_FACTOR=1 \
        -I"$EMBENCH/support" -I"$HERE/support" \
        "$dir"/*.c "$HERE/support/main_gem5.c" "$EMBENCH/support/beebsc.c" \
        "$HERE/support/boardsupport.c" -lm -o "$OUT/$name"
done

# MiBench: programs whose inputs ship with the suite.
mb() {
    name=$1; shift
    src=$1; shift
    (cd "$MIBENCH/$src" && $CC $CFLAGS -w -fpermissive -Wno-error=implicit-function-declaration -Wno-error=implicit-int -Wno-error=int-conversion -Wno-error=incompatible-pointer-types "$@" "$HERE/support/gem5_open.c" -lm -o "$OUT/$name")
}
mb blowfish     security/blowfish  bf.c bf_skey.c bf_ecb.c bf_enc.c bf_cbc.c \
                                   bf_cfb64.c bf_ofb64.c
mb sha          security/sha       sha_driver.c sha.c
mb crc32-mib    telecomm/CRC32     crc_32.c
mb bitcount     automotive/bitcount bitcnt_1.c bitcnt_2.c bitcnt_3.c \
                                   bitcnt_4.c bitcnts.c bitfiles.c bitstrng.c bstr_i.c
mb basicmath    automotive/basicmath basicmath_small.c rad2deg.c cubic.c isqrt.c
mb qsort        automotive/qsort   qsort_small.c
mb dijkstra     network/dijkstra   dijkstra_small.c
mb stringsearch office/stringsearch bmhasrch.c bmhisrch.c bmhsrch.c pbmsrch_small.c

# Hand-written stress test for the reordering rules.
$CC $CFLAGS "$HERE/support/ifa_stress.c" -lm -o "$OUT/ifa_stress"

ls "$OUT"

#!/bin/sh
# Builds the QPC measurement tools into <dest> (default ./out):
#   libqpcprobe.so  aarch64 glibc, LD_PRELOADed into the game (qpcprobe.c); only GLIBC_2.17 symbols, so the
#                   Debian bullseye guest (glibc 2.31) loads it whatever glibc builds it
#   qpcbench.exe    x86_64 Windows (x86_64-w64-mingw32-gcc), run with the game's Wine (qpcbench.c)
# Needs cc (aarch64) and x86_64-w64-mingw32-gcc.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:-$HERE/out}
mkdir -p "$OUT"
${CC:-cc} -std=gnu11 -O2 -fPIC -shared -fvisibility=hidden -Wall -Wextra -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 \
    -fno-stack-protector -o "$OUT/libqpcprobe.so" "$HERE/qpcprobe.c" -ldl -lpthread
newest=$(objdump -T "$OUT/libqpcprobe.so" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)
case $newest in
  GLIBC_2.17|GLIBC_2.2.5) ;;
  *) echo "libqpcprobe.so needs $newest: too new for the guest" >&2; exit 1 ;;
esac
${MINGW_CC:-x86_64-w64-mingw32-gcc} -O2 -Wall -s -o "$OUT/qpcbench.exe" "$HERE/qpcbench.c"
echo "built $OUT/libqpcprobe.so ($newest) and $OUT/qpcbench.exe"

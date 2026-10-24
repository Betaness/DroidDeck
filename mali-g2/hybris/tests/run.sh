#!/bin/sh
# Runs a command with the build-tree ICD (build.sh's output) as the only Vulkan driver, outside the
# guest (PalmCode's Ubuntu, or a guest shell before the files are installed):
#   tests/run.sh vulkaninfo --summary
# OUT (default ../out/guest) is build.sh's <dest-dir>; GUEST_DIR as for build.sh.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/../out/guest}
GUEST_DIR=${GUEST_DIR:-/usr/local/lib/droiddeck-hybris}
D=$(cd "$OUT$GUEST_DIR" && pwd)
TMP=${TMPDIR:-/tmp}/droiddeck-hybris-$(id -u)
mkdir -p "$TMP"
sed "s#\"library_path\": *\"[^\"]*\"#\"library_path\": \"$D/libdroiddeck_hybris_vk.so\"#" "$D/droiddeck_hybris_icd.aarch64.json" > "$TMP/icd.json"
export LD_PRELOAD="$D/lib/libhybris-common.so.1${LD_PRELOAD:+:$LD_PRELOAD}"
export LD_LIBRARY_PATH="$D/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export HYBRIS_LINKER_DIR="$D/lib/libhybris/linker"
export VK_ICD_FILENAMES="$TMP/icd.json" VK_DRIVER_FILES="$TMP/icd.json"
exec "$@"

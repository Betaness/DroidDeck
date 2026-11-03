#!/bin/sh
# VK_EXT_graphics_pipeline_library through the build-tree ICD (tests/run.sh), the way DXVK 3.x uses
# it: ../vortek/tests/gpl_test.c builds the four library parts twice, links them (fast link with
# flags2 and inline SPIR-V; LINK_TIME_OPTIMIZATION with modules), draws with both and checks the
# pixels. Its create infos carry dangling pointers and handles (0x10) in the state the spec
# ignores for the subsets a call does not provide (pStages of the fragment output part and of the
# fast link, pRasterizationState of the vertex input part, ...): mali_fixes.c must drop that state
# (../vortek/src/gpl_library_state.h) before its shader rewrites, the implicit pipeline cache and
# the xfb statistics read pStages, and hand the driver the copies (the Mali follows them too).
# Before that fix every run here crashed while creating the libraries.
# Runs, all with GPL_KEEP_LIBS=1 (the Mali crashes, also natively, when fast-linked pipelines are
# used after their libraries were destroyed; the Vortek server keeps them alive, this path does
# not, and DXVK keeps them):
#   default    shader rewrites on, implicit pipeline cache in a temporary directory
#   norewrite  VORTEK_KEEP_CLIP_DISTANCE=1, DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX=0 (no stage copies)
#   nocache    DROIDDECK_HYBRIS_PIPELINE_CACHE=0 as well
#   sh tests/gpl_test.sh          after build.sh; needs libvulkan-dev. Prints GPLTEST-PASS or GPLTEST-FAIL.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TESTS=$HERE/../out/tests
mkdir -p "$TESTS"
cc -O2 -o "$TESTS/gpl_test" "$HERE/../../vortek/tests/gpl_test.c" -lvulkan || exit 1
unset VORTEK_KEEP_CLIP_DISTANCE DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX VORTEK_SPEC_COMPOSITE_FIX DROIDDECK_HYBRIS_PIPELINE_CACHE \
  VORTEK_PIPELINE_CACHE DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR VORTEK_PIPELINE_CACHE_DIR
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
failed=0
run() { # name env...
  name=$1
  shift
  env GPL_KEEP_LIBS=1 DROIDDECK_HYBRIS_PIPELINE_CACHE_DIR="$tmp/cache" "$@" timeout 120 sh "$HERE/run.sh" "$TESTS/gpl_test" >"$tmp/$name.out" 2>&1
  rc=$?
  if [ $rc = 0 ] && grep -q '^GPL-PASS' "$tmp/$name.out"; then
    echo "$name: ok ($(grep -c '^pixel.* ok$' "$tmp/$name.out") pixels, $(grep '^covered' "$tmp/$name.out"))"
  else
    echo "$name: FAIL (exit $rc)"
    grep -v 'normalize\|not found\|MEMPROF' "$tmp/$name.out" | tail -15 | sed 's/^/  /'
    failed=1
  fi
}
mkdir -p "$tmp/cache"
run default
run norewrite VORTEK_KEEP_CLIP_DISTANCE=1 DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX=0
run nocache VORTEK_KEEP_CLIP_DISTANCE=1 DROIDDECK_HYBRIS_SPEC_COMPOSITE_FIX=0 DROIDDECK_HYBRIS_PIPELINE_CACHE=0
if [ $failed = 0 ]; then echo GPLTEST-PASS; else echo GPLTEST-FAIL; exit 1; fi

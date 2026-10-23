#!/bin/sh
# Fetch the upstream Vortek client and the Winlator vortekrenderer server at
# pinned commits into ./work and apply our patches (nothing upstream is vendored).
#
#   VORTEK_REPO    default https://github.com/brunodev85/vortek
#   WINLATOR_REPO  default https://github.com/brunodev85/winlator-app
#   (a local clone path also works if it contains the pinned commit)
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
WORK=${WORK:-$HERE/work}

VORTEK_REPO=${VORTEK_REPO:-https://github.com/brunodev85/vortek}
VORTEK_COMMIT=b1730c5def9b575672e671aee11d79ae7adc63d1        # 2026-08-19
WINLATOR_REPO=${WINLATOR_REPO:-https://github.com/brunodev85/winlator-app}
WINLATOR_COMMIT=8077f374ef2d54127b088de9e8cd7b4af14d9c8b      # 2026-10-16

# fetch_pinned <dir> <repo> <commit> [sparse paths...]
fetch_pinned() {
  dir=$1 repo=$2 commit=$3
  shift 3
  if [ -d "$dir/.git" ] && [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$commit" ]; then
    echo "$dir: already at $commit (resetting)"
    git -C "$dir" checkout -q -f "$commit"
    git -C "$dir" clean -qfdx
    return
  fi
  rm -rf "$dir"
  git init -q "$dir"
  git -C "$dir" config core.autocrlf false
  if [ $# -gt 0 ]; then
    git -C "$dir" sparse-checkout set --no-cone "$@"
  fi
  git -C "$dir" fetch -q --depth 1 --filter=blob:none "$repo" "$commit" 2>/dev/null ||
    git -C "$dir" fetch -q --depth 1 "$repo" "$commit"
  git -C "$dir" checkout -q FETCH_HEAD
  echo "$dir: $(git -C "$dir" rev-parse HEAD)"
}

mkdir -p "$WORK"
fetch_pinned "$WORK/vortek" "$VORTEK_REPO" "$VORTEK_COMMIT"
fetch_pinned "$WORK/winlator-app" "$WINLATOR_REPO" "$WINLATOR_COMMIT" \
  /app/src/main/cpp/vortekrenderer/ /app/src/main/cpp/winlator/

for p in "$HERE"/patches/vortek-*.patch; do
  git -C "$WORK/vortek" apply --whitespace=nowarn "$p"
done
for p in "$HERE"/patches/winlator-*.patch; do
  git -C "$WORK/winlator-app" apply --whitespace=nowarn "$p"
done
echo "patched upstream sources in $WORK"

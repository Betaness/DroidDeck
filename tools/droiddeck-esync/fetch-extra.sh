#!/usr/bin/env bash
set -euo pipefail
repo_root=$1
list="${repo_root}/tools/droiddeck-esync/extra.list"
assets="${repo_root}/app/src/main/assets/droiddeck-esync-extra"
rm -rf "${assets}"
[[ -f "${list}" ]] || exit 0
work=$(mktemp -d)
trap 'rm -rf "${work}"' EXIT
mkdir -p "${work}/packs" "${work}/entries"

while IFS=$'\t' read -r repo tag asset sha rest; do
  [[ -z "${repo}" || "${repo}" == \#* ]] && continue
  [[ -n "${tag}" && -n "${asset}" && "${sha}" =~ ^[0-9a-f]{64}$ && -z "${rest:-}" ]] || {
    echo "extra.list: malformed line for ${asset:-?}" >&2; exit 1; }
  case "${asset}" in
    *.tzst) dest="${work}/packs/${asset}" ;;
    *.json) dest="${work}/entries/${asset}" ;;
    *) echo "extra.list: ${asset} is neither .tzst nor .json" >&2; exit 1 ;;
  esac
  [[ ! -e "${dest}" ]] || { echo "extra.list: ${asset} is listed twice" >&2; exit 1; }
  curl -fsSL --retry 3 -o "${dest}" "https://github.com/${repo}/releases/download/${tag}/${asset}"
  echo "${sha}  ${dest}" | sha256sum -c - >/dev/null || { echo "${asset}: checksum mismatch" >&2; exit 1; }
done < "${list}"

python3 -I - "${work}" <<'PY'
import hashlib, json, os, re, sys

work = sys.argv[1]
ident = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,127}")
packs = sorted(os.listdir(os.path.join(work, "packs")))
entries = sorted(os.listdir(os.path.join(work, "entries")))
if [p[:-5] for p in packs] != [e[:-5] for e in entries]:
    sys.exit("extra.list: every pack needs both <id>.tzst and <id>.json")
out = []
for name in entries:
    pack_id = name[:-5]
    with open(os.path.join(work, "entries", name), encoding="utf-8") as source:
        entry = json.load(source)
    archive = os.path.join(work, "packs", pack_id + ".tzst")
    with open(archive, "rb") as source:
        sha = hashlib.sha256(source.read()).hexdigest()
    asset = entry.get("asset") or {}
    if not ident.fullmatch(pack_id) or entry.get("id") != pack_id:
        sys.exit("%s: the entry names %r" % (name, entry.get("id")))
    if asset.get("sha256") != sha or asset.get("size") != os.path.getsize(archive):
        sys.exit("%s: the entry does not describe %s.tzst" % (name, pack_id))
    if entry.get("revoked"):
        sys.exit("%s: the entry is revoked" % name)
    out.append({"id": pack_id, "sha256": sha, "entry": entry})
with open(os.path.join(work, "manifest.json"), "w", encoding="utf-8") as sink:
    json.dump({"format": 1, "packs": out}, sink, indent=1, sort_keys=True)
    sink.write("\n")
PY

mkdir -p "${assets}"
mv "${work}/packs" "${assets}/packs"
mv "${work}/manifest.json" "${assets}/manifest.json"
ls -lR "${assets}"

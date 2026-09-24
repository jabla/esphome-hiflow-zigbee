#!/usr/bin/env bash
# Regenerate the nanopb C sources in src/hiflow_pb/generated/ from the .proto
# files in this directory. Generator version is pinned to match the vendored
# runtime (nanopb 0.4.9.2). Network access is required (git clone + PyPI).
#
# Usage:  src/hiflow_pb/regen.sh
set -euo pipefail

NANOPB_TAG="nanopb-0.4.9.2"
WORK="/tmp/hiflow-nanopb-regen"
SRC="$WORK/nanopb-src"
VENV="$WORK/venv"
GEN_DIR="$(cd "$(dirname "$0")" && pwd)/generated"

# 1. nanopb checkout (generator + runtime, pinned to the release tag)
if [ ! -d "$SRC/.git" ]; then
    rm -rf "$SRC"
    git clone --quiet https://github.com/nanopb/nanopb.git "$SRC"
fi
git -C "$SRC" fetch --quiet --tags
git -C "$SRC" checkout --quiet "$NANOPB_TAG"
echo "nanopb: $(git -C "$SRC" describe --tags --exact-match)"
echo "        $(git -C "$SRC" rev-parse HEAD)"

# 2. python venv with protobuf (generator dependency)
if [ ! -x "$VENV/bin/python" ]; then
    uv venv "$VENV"
fi
uv pip install --quiet --python "$VENV/bin/python" protobuf

# 3. regenerate
cd "$GEN_DIR"
for p in RealDataNew.proto APPInfomationData.proto APPHeartbeatPB.proto CommandPB.proto CommCmdPB.proto; do
    echo "generating $p"
    "$VENV/bin/python" "$SRC/generator/nanopb_generator.py" "$p"
done
echo "done: $(ls -1 "$GEN_DIR"/*.pb.c | wc -l) .pb.c files in $GEN_DIR"

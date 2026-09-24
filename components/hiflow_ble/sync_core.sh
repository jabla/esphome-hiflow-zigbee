#!/usr/bin/env bash
#
# sync_core.sh — flatten the pre-verified C core into this component directory.
#
# WHY THIS EXISTS
#   ESPHome's external-component loader indexes only files that sit DIRECTLY in
#   components/<name>/ (ComponentManifest(recursive_sources=False)); arbitrary
#   subdirectories are invisible to it. And include paths cannot be injected
#   (cg.add_build_flag() drops -I flags). So the sources the firmware needs are
#   copied flat in here, and the generated protobuf headers get their
#   `#include <pb.h>` rewritten to the quote form (which resolves against the
#   including file's own directory, i.e. this one).
#
#   The originals under src/ are never modified, and the host test suite keeps
#   compiling them from their original locations.
#
# Idempotent: safe to re-run after touching anything under src/.
#
# Note: only the mbedTLS crypto backend is shipped. Both backends are
# #if-guarded, but the OpenSSL one needs OpenSSL headers (host only) and would
# contribute nothing on target.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"

CORE="$root/src/hiflow_core"
NANOPB="$root/src/hiflow_pb/nanopb"
GEN="$root/src/hiflow_pb/generated"

copy() {
  local src="$1"
  [ -f "$src" ] || { echo "sync_core.sh: missing $src" >&2; exit 1; }
  cp -f "$src" "$here/$(basename "$src")"
}

# --- protocol / crypto core ---
copy "$CORE/hiflow_frame.h"
copy "$CORE/hiflow_frame.c"
copy "$CORE/hiflow_crypto.h"
copy "$CORE/hiflow_crypto_mbedtls.c"
copy "$CORE/hiflow_appinfo.h"
copy "$CORE/hiflow_appinfo.c"
copy "$CORE/hiflow_clock.h"
copy "$CORE/hiflow_clock.c"
copy "$CORE/hiflow_proto.h"
copy "$CORE/hiflow_proto.c"
copy "$CORE/hiflow_session.h"
copy "$CORE/hiflow_session.c"

# --- vendored nanopb 0.4.9.2 ---
for f in pb.h pb_common.h pb_common.c pb_decode.h pb_decode.c pb_encode.h pb_encode.c; do
  copy "$NANOPB/$f"
done

# --- generated protobuf: the messages the session core uses ---
for f in RealDataNew APPInfomationData CommCmdPB; do
  copy "$GEN/$f.pb.h"
  copy "$GEN/$f.pb.c"
done

# --- make the nanopb includes resolve inside the (flat) component dir ---
for f in "$here"/*.pb.h; do
  sed -i 's|#include <pb\.h>|#include "pb.h"|' "$f"
done

echo "sync_core.sh: $(ls -1 "$here"/*.c "$here"/*.h 2>/dev/null | wc -l) C/H files synced into $here"

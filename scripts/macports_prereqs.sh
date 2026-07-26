#!/usr/bin/env bash
# macports_prereqs.sh -- the MacPorts packages ppcode is built against, and
# optionally a double-click installer for them.
#
# WHO NEEDS THIS
#
# Nobody who downloads a release. Both the application and the command line
# distribution carry their own copies of every MacPorts library they link (see
# scripts/bundle_dylibs.sh), so they run on a stock 10.5 machine with nothing
# installed.
#
# This is only for building ppcode from source, which additionally needs the
# compiler.
#
#   ./scripts/macports_prereqs.sh              list what is required
#   ./scripts/macports_prereqs.sh --pkg [DIR]  build a double-click .mpkg
#
# Run it on the G5; MacPorts is not on the Linux box.
set -euo pipefail

PORT="${PPCODE_PORT:-/opt/local/bin/port}"
MODE="list"
OUT="${2:-dist}"

case "${1:-}" in
  --pkg) MODE="pkg" ;;
  ""|--list) MODE="list" ;;
  -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
  *) echo "unknown option: $1" >&2; exit 2 ;;
esac

# Libraries to link against, then the compiler. All build-time only: what gets
# shipped carries copies of these.
RUNTIME_PORTS=(curl ncurses)
BUILD_PORTS=(gcc15)

if [ ! -x "$PORT" ]; then
  echo "MacPorts not found at $PORT -- run this on the G5." >&2
  exit 1
fi

echo "ppcode MacPorts prerequisites"
echo
echo "  runtime : ${RUNTIME_PORTS[*]}"
echo "  build   : ${BUILD_PORTS[*]}"
echo
echo "Currently installed:"
for p in "${RUNTIME_PORTS[@]}" "${BUILD_PORTS[@]}"; do
  line=$("$PORT" installed "$p" 2>/dev/null | grep '(active)' || true)
  if [ -n "$line" ]; then
    echo "  ok      $(echo "$line" | sed 's/^ *//')"
  else
    echo "  MISSING $p"
  fi
done

if [ "$MODE" = "list" ]; then
  cat <<'EOF'

To install from source (this compiles, and on a G5 that is hours to days):

  sudo port install curl ncurses gcc15

To build a double-click installer for these instead, run:

  ./scripts/macports_prereqs.sh --pkg dist
EOF
  exit 0
fi

# --- build the installer ---------------------------------------------------
#
# `port mpkg` produces a metapackage: the port plus each of its dependencies as
# a sub-package, which is what makes it double-click installable on a machine
# with no MacPorts at all.
#
# It packages what is already built. If a dependency is missing it will try to
# build it, and on this hardware that is an overnight job -- so check first and
# say so rather than silently starting one.
missing=0
for p in "${RUNTIME_PORTS[@]}" "${BUILD_PORTS[@]}"; do
  "$PORT" installed "$p" 2>/dev/null | grep -q '(active)' || { echo "not installed: $p" >&2; missing=1; }
done

if [ "$missing" -ne 0 ]; then
  cat >&2 <<'EOF'

Refusing to build packages while a port is missing: `port mpkg` would compile it
from source first, which on a PowerMac G5 takes hours to days. Install it
deliberately, then run this again.
EOF
  exit 1
fi

mkdir -p "$OUT"
OUT_ABS="$(cd "$OUT" && pwd)"

for p in "${RUNTIME_PORTS[@]}" "${BUILD_PORTS[@]}"; do
  echo ">> packaging $p"
  # Needs root: mpkg stages into the port's work directory under /opt/local.
  sudo "$PORT" mpkg "$p"
done

echo ">> collecting .mpkg files into $OUT_ABS"
find /opt/local/var/macports/build -name '*.mpkg' -maxdepth 6 -newer "$0" \
  -exec cp -R {} "$OUT_ABS/" \; 2>/dev/null || true

ls -1 "$OUT_ABS" | sed 's/^/   /'
echo ">> done"

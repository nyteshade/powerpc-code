#!/usr/bin/env bash
# Sync this tree to the PowerMac G5 and build it there.
#
#   ./deploy.sh              sync + build
#   ./deploy.sh -c           sync + clean build
#   ./deploy.sh -r ARGS...   sync + build + run ppcode with ARGS
#
# The G5 is the only machine with the ppc toolchain, so the build always
# happens remotely; this box is just the editor.

set -euo pipefail

HOST="${PPCODE_HOST:-brie@10.0.0.102}"
DEST="${PPCODE_DEST:-/Users/brie/src/ppcode}"
JOBS="${PPCODE_JOBS:-2}"     # the G5 has 2 cores

CLEAN=0
RUN=0
RUN_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -c|--clean) CLEAN=1; shift ;;
    -r|--run)   RUN=1; shift; RUN_ARGS=("$@"); break ;;
    -h|--help)  sed -n '2,12p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done

here="$(cd "$(dirname "$0")" && pwd)"

echo ">> syncing to ${HOST}:${DEST}"
ssh "$HOST" "mkdir -p '$DEST'"
rsync -az --delete \
  --exclude 'build/' \
  --exclude '.git/' \
  --exclude '*.o' \
  --exclude '*.d' \
  -e ssh \
  "$here"/ "$HOST:$DEST/"

if [[ $CLEAN -eq 1 ]]; then
  echo ">> clean"
  ssh "$HOST" "cd '$DEST' && /opt/local/bin/gmake clean"
fi

echo ">> building (-j${JOBS})"
# gmake, not the Leopard-vintage /usr/bin/make.
ssh "$HOST" "cd '$DEST' && /opt/local/bin/gmake -j${JOBS} 2>&1"

if [[ $RUN -eq 1 ]]; then
  echo ">> running: ppcode ${RUN_ARGS[*]}"
  # -t so ncurses gets a real terminal when running interactively.
  ssh -t "$HOST" "cd '$DEST' && ./build/ppcode $(printf '%q ' "${RUN_ARGS[@]}")"
fi

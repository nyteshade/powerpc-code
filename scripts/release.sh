#!/usr/bin/env bash
# release.sh -- cut a versioned release and publish it to GitHub.
#
#   ./scripts/release.sh 0.3.0        set the version, build, tag, publish
#   ./scripts/release.sh --current    release whatever VERSION already says
#   ./scripts/release.sh 0.3.0 --dry  build and package, publish nothing
#
# Run from the Linux box: this is where git lives. The build happens on the G5
# over ssh, because that is the only machine with the toolchain.
#
# What it produces:
#   ppcode-<v>-ppc-macos10.5-app.tar.gz   the .app, self-contained, no MacPorts
#   ppcode-<v>-ppc-macos10.5-cli.tar.gz   the CLI, needs MacPorts (see below)
set -euo pipefail

HOST="${PPCODE_HOST:-brie@10.0.0.102}"
DEST="${PPCODE_DEST:-/Users/brie/src/ppcode}"
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here"

DRY=0
VERSION=""

for arg in "$@"; do
  case "$arg" in
    --dry|--dry-run) DRY=1 ;;
    --current) VERSION="$(cat VERSION)" ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    -*) echo "unknown option: $arg" >&2; exit 2 ;;
    *) VERSION="$arg" ;;
  esac
done

[ -n "$VERSION" ] || { echo "usage: release.sh <x.y.z> | --current" >&2; exit 2; }

# Semantic versioning, so the tag, the Info.plist and --version all agree and a
# release can never be cut from something like "v0.3" or "0.3.0rc1".
if ! printf '%s' "$VERSION" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?$'; then
  echo "not a semantic version: $VERSION" >&2
  exit 2
fi

TAG="v$VERSION"

if [ -n "$(git status --porcelain)" ]; then
  echo "working tree is dirty; commit first." >&2
  git status --short >&2
  exit 1
fi

if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "tag $TAG already exists." >&2
  exit 1
fi

# --- version bump ----------------------------------------------------------

if [ "$(cat VERSION)" != "$VERSION" ]; then
  echo ">> setting VERSION to $VERSION"
  printf '%s\n' "$VERSION" > VERSION
  git add VERSION
  git commit -q -m "Release $TAG"
fi

# --- build and gate --------------------------------------------------------
#
# A clean build, because a release must not depend on stale objects, and both
# test gates must pass before anything is packaged or tagged.

echo ">> syncing to $HOST"
git rev-parse --short HEAD > .build-rev
rsync -az --delete --exclude 'build/' --exclude '.git/' -e ssh ./ "$HOST:$DEST/"

echo ">> clean build on $HOST"
ssh "$HOST" "cd '$DEST' && /opt/local/bin/gmake clean >/dev/null && \
             /opt/local/bin/gmake -j2 && /opt/local/bin/gmake gui && \
             /opt/local/bin/gmake app" 2>&1 | grep -E 'built|bundled|MISSING|error' || true

echo ">> selftest"
ssh "$HOST" "cd '$DEST' && ./build/ppcode --selftest" | tail -1

if ! ssh "$HOST" "cd '$DEST' && ./build/ppcode --selftest >/dev/null 2>&1"; then
  echo "selftest failed; not releasing." >&2
  exit 1
fi

echo ">> gui check"
if ! ssh "$HOST" "cd '$DEST' && ./build/ppcode-gui --check >/dev/null 2>&1"; then
  echo "gui self-check failed; not releasing." >&2
  exit 1
fi

# The bundle is only worth shipping if it carries its own libraries; a release
# that dyld-errors on a stock machine is worse than no release.
echo ">> verifying the bundle is self-contained"
if ssh "$HOST" "cd '$DEST' && /opt/local/bin/otool -L build/ppcode.app/Contents/MacOS/ppcode | grep -q '/opt/local'"; then
  echo "bundle still links /opt/local; not releasing." >&2
  exit 1
fi

# --- package ---------------------------------------------------------------

APP_TGZ="ppcode-$VERSION-ppc-macos10.5-app.tar.gz"
CLI_TGZ="ppcode-$VERSION-ppc-macos10.5-cli.tar.gz"

echo ">> packaging"
ssh "$HOST" "cd '$DEST/build' && \
  tar czf '$APP_TGZ' ppcode.app && \
  tar czf '$CLI_TGZ' ppcode"

mkdir -p dist
scp -q "$HOST:$DEST/build/$APP_TGZ" "$HOST:$DEST/build/$CLI_TGZ" dist/
ls -lh "dist/$APP_TGZ" "dist/$CLI_TGZ" | sed 's/^/   /'

NOTES_FILE="$(mktemp)"
trap 'rm -f "$NOTES_FILE"' EXIT

{
  echo "PowerPC build for Mac OS X 10.5 Leopard, built on a PowerMac G5."
  echo
  echo "### Downloads"
  echo
  echo "| File | What it is | Prerequisites |"
  echo "| --- | --- | --- |"
  echo "| \`$APP_TGZ\` | the Cocoa application | **none** |"
  echo "| \`$CLI_TGZ\` | the \`ppcode\` terminal tool | MacPorts \`curl\`, \`ncurses\`, \`gcc15\` |"
  echo
  echo "The application bundle carries its own copy of every MacPorts library it"
  echo "links -- libcurl and its TLS, HTTP/2, compression and IDN dependencies,"
  echo "plus gcc15's libstdc++ and libgcc_s. It runs on a stock 10.5 machine."
  echo
  echo "The command line tool is deliberately left linked against \`/opt/local\`,"
  echo "so that installing it into \`~/bin\` from the Settings window keeps working."
  echo "Run \`scripts/macports_prereqs.sh\` for what it needs, or"
  echo "\`scripts/macports_prereqs.sh --pkg\` to build a double-click installer."
  echo
  echo "### Verified on this build"
  echo
  echo '```'
  ssh "$HOST" "cd '$DEST' && ./build/ppcode --selftest 2>/dev/null | tail -1"
  ssh "$HOST" "cd '$DEST' && ./build/ppcode-gui --check 2>/dev/null | tail -1"
  ssh "$HOST" "cd '$DEST' && ./build/ppcode --version"
  echo '```'
  echo
  if git tag --list 'v*' | head -1 >/dev/null 2>&1 && [ -n "$(git tag --list 'v*')" ]; then
    prev="$(git tag --list 'v*' --sort=-v:refname | head -1)"
    echo "### Changes since $prev"
    echo
    git log --no-merges --pretty='- %s' "$prev..HEAD"
  else
    echo "### Changes"
    echo
    git log --no-merges --pretty='- %s' -20
  fi
} > "$NOTES_FILE"

if [ "$DRY" -eq 1 ]; then
  echo ">> dry run: not tagging or publishing"
  echo "---- release notes ----"
  cat "$NOTES_FILE"
  exit 0
fi

# --- tag and publish -------------------------------------------------------

echo ">> tagging $TAG"
git tag -a "$TAG" -m "ppcode $VERSION"
git push origin main
git push origin "$TAG"

echo ">> publishing the GitHub release"
gh release create "$TAG" \
  --title "ppcode $VERSION" \
  --notes-file "$NOTES_FILE" \
  "dist/$APP_TGZ" "dist/$CLI_TGZ"

echo ">> released $TAG"
gh release view "$TAG" --json url --jq .url

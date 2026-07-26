#!/bin/bash
# bundle_dylibs.sh -- make a build self-contained, so it runs without MacPorts.
#
# The binaries link four MacPorts dylibs directly (libcurl, libncurses, and
# gcc15's libstdc++ and libgcc_s), and libcurl in turn drags in its own tree of
# TLS, HTTP/2, compression and IDN libraries -- seventeen in all. Anything
# shipped without them fails to launch on a stock 10.5 machine with a dyld
# error, which is not an acceptable thing to put in a release.
#
# This walks the dependency closure of the given executables, copies every
# /opt/local library in beside them, and rewrites the install names to a path
# relative to the executable so dyld finds them wherever the tree is put.
#
#   ./scripts/bundle_dylibs.sh --app  build/ppcode.app
#   ./scripts/bundle_dylibs.sh --tree dist/ppcode-cli
#
# --app  bundles Contents/MacOS/* and Contents/Resources/ppcode against
#        Contents/Frameworks.
# --tree bundles bin/* against lib/, for a directory the user can untar
#        anywhere.
set -euo pipefail

MODE="${1:-}"
TARGET="${2:-}"
PREFIX="${PPCODE_PORTS_PREFIX:-/opt/local}"

# MacPorts' cctools, not Leopard's: the system install_name_tool is from 2007
# and chokes on some of these libraries.
OTOOL="$PREFIX/bin/otool"
INT="$PREFIX/bin/install_name_tool"
[ -x "$OTOOL" ] || OTOOL=otool
[ -x "$INT" ] || INT=install_name_tool

usage() { sed -n '2,20p' "$0"; exit 2; }
[ -n "$TARGET" ] || usage
[ -d "$TARGET" ] || { echo "no such directory: $TARGET" >&2; exit 1; }

case "$MODE" in
  --app)
    LIBDIR="$TARGET/Contents/Frameworks"
    # Contents/MacOS/x and Contents/Resources/ppcode are at the same depth, so
    # one relative token serves both.
    RELDIR="@executable_path/../Frameworks"
    BINARIES="$(ls "$TARGET/Contents/MacOS" | sed "s|^|$TARGET/Contents/MacOS/|")"
    if [ -f "$TARGET/Contents/Resources/ppcode" ]; then
      BINARIES="$BINARIES $TARGET/Contents/Resources/ppcode"
    fi
    ;;
  --tree)
    LIBDIR="$TARGET/lib"
    RELDIR="@executable_path/../lib"
    BINARIES="$(ls "$TARGET/bin" | sed "s|^|$TARGET/bin/|")"
    ;;
  *)
    usage
    ;;
esac

mkdir -p "$LIBDIR"

# Dependencies of a binary that live under the MacPorts prefix. The first line
# of otool -L is the file itself, so it is dropped.
deps_of() {
  "$OTOOL" -L "$1" | sed 1d | awk '{print $1}' | grep "^$PREFIX/" || true
}

echo ">> collecting dependencies under $PREFIX"

pending="$BINARIES"
collected=""

while [ -n "$pending" ]; do
  next=""
  for bin in $pending; do
    for dep in $(deps_of "$bin"); do
      base=$(basename "$dep")

      case " $collected " in
        *" $base "*) continue ;;
      esac

      if [ ! -f "$dep" ]; then
        echo "   MISSING $dep (referenced by $(basename "$bin"))" >&2
        exit 1
      fi

      cp -f "$dep" "$LIBDIR/$base"
      chmod u+w "$LIBDIR/$base"
      collected="$collected $base"
      next="$next $LIBDIR/$base"
      echo "   + $base"
    done
  done
  pending="$next"
done

if [ -z "$collected" ]; then
  echo ">> nothing to bundle"
  exit 0
fi

echo ">> rewriting install names"

# Each copied library is identified by where it now lives, so anything linking
# against it records the relative path rather than /opt/local.
for base in $collected; do
  "$INT" -id "$RELDIR/$base" "$LIBDIR/$base"
done

for bin in $BINARIES "$LIBDIR"/*; do
  [ -f "$bin" ] || continue

  for dep in $(deps_of "$bin"); do
    "$INT" -change "$dep" "$RELDIR/$(basename "$dep")" "$bin"
  done
done

echo ">> verifying nothing still points at $PREFIX"

leftover=0
for bin in $BINARIES "$LIBDIR"/*; do
  [ -f "$bin" ] || continue

  remaining=$(deps_of "$bin")
  if [ -n "$remaining" ]; then
    echo "   STILL LINKED $(basename "$bin"):" >&2
    echo "$remaining" | sed 's/^/     /' >&2
    leftover=1
  fi
done

if [ "$leftover" -ne 0 ]; then
  echo ">> FAILED: not self-contained" >&2
  exit 1
fi

count=$(echo $collected | wc -w | tr -d ' ')
echo ">> bundled $count libraries; $TARGET is self-contained"

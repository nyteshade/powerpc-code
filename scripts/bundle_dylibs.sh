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

# Arrays throughout, not space-separated strings. The bundle is called
# "PowerPC Code.app", and a word-split list turns that into two nonexistent
# paths -- otool then finds nothing, the script cheerfully reports "nothing to
# bundle", and ships an application that dyld-errors on any machine without
# MacPorts. It failed silently exactly once; hence the arrays and the check at
# the end that at least one dependency was found.
BINARIES=()

case "$MODE" in
  --app)
    LIBDIR="$TARGET/Contents/Frameworks"
    # Contents/MacOS/x and Contents/Resources/ppcode are at the same depth, so
    # one relative token serves both.
    RELDIR="@executable_path/../Frameworks"
    for f in "$TARGET/Contents/MacOS"/*; do
      [ -f "$f" ] && BINARIES+=("$f")
    done
    if [ -f "$TARGET/Contents/Resources/ppcode" ]; then
      BINARIES+=("$TARGET/Contents/Resources/ppcode")
    fi
    ;;
  --tree)
    LIBDIR="$TARGET/lib"
    RELDIR="@executable_path/../lib"
    for f in "$TARGET/bin"/*; do
      [ -f "$f" ] && BINARIES+=("$f")
    done
    ;;
  *)
    usage
    ;;
esac

if [ "${#BINARIES[@]}" -eq 0 ]; then
  echo "no executables found in $TARGET" >&2
  exit 1
fi

mkdir -p "$LIBDIR"

# Dependencies of a binary that live under the MacPorts prefix. The first line
# of otool -L is the file itself, so it is dropped.
deps_of() {
  "$OTOOL" -L "$1" | sed 1d | awk '{print $1}' | grep "^$PREFIX/" || true
}

echo ">> collecting dependencies under $PREFIX"

pending=("${BINARIES[@]}")
collected=""

while [ "${#pending[@]}" -gt 0 ]; do
  next=()
  for bin in "${pending[@]}"; do
    deps=$(deps_of "$bin")
    while IFS= read -r dep; do
      [ -n "$dep" ] || continue
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
      next+=("$LIBDIR/$base")
      echo "   + $base"
    done <<DEPS
$deps
DEPS
  done
  pending=("${next[@]}")
done

# Genuinely nothing to do is possible only if the binaries link nothing under
# the prefix, which for this project never happens -- so treat it as the bug it
# almost certainly is rather than exiting successfully.
if [ -z "$collected" ]; then
  echo ">> FAILED: no dependencies found under $PREFIX." >&2
  echo "   Either the binaries are already bundled, or the paths are wrong:" >&2
  for bin in "${BINARIES[@]}"; do echo "     $bin" >&2; done
  exit 1
fi

echo ">> rewriting install names"

# Each copied library is identified by where it now lives, so anything linking
# against it records the relative path rather than /opt/local.
for base in $collected; do
  "$INT" -id "$RELDIR/$base" "$LIBDIR/$base"
done

for bin in "${BINARIES[@]}" "$LIBDIR"/*; do
  [ -f "$bin" ] || continue

  deps=$(deps_of "$bin")
  while IFS= read -r dep; do
    [ -n "$dep" ] || continue
    "$INT" -change "$dep" "$RELDIR/$(basename "$dep")" "$bin"
  done <<DEPS
$deps
DEPS
done

echo ">> verifying nothing still points at $PREFIX"

leftover=0
for bin in "${BINARIES[@]}" "$LIBDIR"/*; do
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

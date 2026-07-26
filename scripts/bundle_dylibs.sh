#!/bin/bash
# bundle_dylibs.sh -- make an application bundle self-contained.
#
# The binaries link four MacPorts dylibs directly (libcurl, libncurses, and
# gcc15's libstdc++ and libgcc_s), and libcurl in turn drags in its own tree of
# TLS, HTTP/2, compression and IDN libraries. A bundle copied to a machine
# without MacPorts therefore fails to launch with a dyld error, which is not an
# acceptable thing to put in a release.
#
# This walks the dependency closure of the bundle's executable, copies every
# /opt/local library into Contents/Frameworks, and rewrites the install names to
# @executable_path/../Frameworks so dyld finds them inside the bundle.
#
# Deliberately NOT applied to Contents/Resources/ppcode, the copy of the command
# line tool that the Settings window installs into ~/bin. Once that copy leaves
# the bundle @executable_path points somewhere else entirely, so rewriting it
# would break the very case it exists for. It keeps its /opt/local paths and so
# still needs MacPorts -- see scripts/macports_prereqs.sh.
#
#   ./scripts/bundle_dylibs.sh build/ppcode.app
set -euo pipefail

APP="${1:?usage: bundle_dylibs.sh <bundle.app>}"
MACOS="$APP/Contents/MacOS"
FRAMEWORKS="$APP/Contents/Frameworks"
PREFIX="${PPCODE_PORTS_PREFIX:-/opt/local}"

# MacPorts' cctools, not Leopard's: the system install_name_tool is from 2007
# and chokes on some of these libraries.
OTOOL="$PREFIX/bin/otool"
INT="$PREFIX/bin/install_name_tool"
[ -x "$OTOOL" ] || OTOOL=otool
[ -x "$INT" ] || INT=install_name_tool

[ -d "$APP" ] || { echo "no such bundle: $APP" >&2; exit 1; }
mkdir -p "$FRAMEWORKS"

# Dependencies of a binary that live under the MacPorts prefix. The first line
# of otool -L is the file itself, and for a dylib the second is its own id, so
# both are filtered out by matching only indented lines and skipping self.
deps_of() {
  "$OTOOL" -L "$1" | sed 1d | awk '{print $1}' | grep "^$PREFIX/" || true
}

echo ">> collecting dependencies under $PREFIX"

# Breadth-first over the closure. `pending` holds files still to be scanned.
pending=$(ls "$MACOS")
pending=$(for f in $pending; do echo "$MACOS/$f"; done)
collected=""

while [ -n "$pending" ]; do
  next=""
  for bin in $pending; do
    for dep in $(deps_of "$bin"); do
      base=$(basename "$dep")

      # Already have it: nothing to do.
      case " $collected " in
        *" $base "*) continue ;;
      esac

      if [ ! -f "$dep" ]; then
        echo "   MISSING $dep (referenced by $(basename "$bin"))" >&2
        exit 1
      fi

      cp -f "$dep" "$FRAMEWORKS/$base"
      chmod u+w "$FRAMEWORKS/$base"
      collected="$collected $base"
      next="$next $FRAMEWORKS/$base"
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

# Every copied library is identified by where it now lives, so anything linking
# against it records the bundle-relative path rather than /opt/local.
for base in $collected; do
  "$INT" -id "@executable_path/../Frameworks/$base" "$FRAMEWORKS/$base"
done

# Point each binary -- the executables and the copied libraries themselves --
# at the bundled copies.
for bin in "$MACOS"/* "$FRAMEWORKS"/*; do
  [ -f "$bin" ] || continue

  for dep in $(deps_of "$bin"); do
    "$INT" -change "$dep" "@executable_path/../Frameworks/$(basename "$dep")" "$bin"
  done
done

echo ">> verifying nothing still points at $PREFIX"

leftover=0
for bin in "$MACOS"/* "$FRAMEWORKS"/*; do
  [ -f "$bin" ] || continue

  remaining=$(deps_of "$bin")
  if [ -n "$remaining" ]; then
    echo "   STILL LINKED $(basename "$bin"):" >&2
    echo "$remaining" | sed 's/^/     /' >&2
    leftover=1
  fi
done

if [ "$leftover" -ne 0 ]; then
  echo ">> FAILED: the bundle is not self-contained" >&2
  exit 1
fi

count=$(echo $collected | wc -w | tr -d ' ')
echo ">> bundled $count librar$([ "$count" = 1 ] && echo y || echo ies); bundle is self-contained"

#!/bin/sh
# install.sh -- put ppcode on your PATH.
#
#   ./install.sh              into ~/bin
#   ./install.sh /usr/local/bin
#
# This links rather than copies, because bin/ppcode finds its libraries at
# ../lib relative to itself. Copying the bare executable somewhere else would
# leave it unable to find them.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
dest=${1:-$HOME/bin}

[ -x "$here/bin/ppcode" ] || {
  echo "bin/ppcode is missing or not executable" >&2
  exit 1
}

mkdir -p "$dest"

# Replacing a link that is currently in use is fine; replacing a running
# executable is not, which is the other reason not to copy.
rm -f "$dest/ppcode"
ln -s "$here/bin/ppcode" "$dest/ppcode"

echo "linked $dest/ppcode -> $here/bin/ppcode"

case ":${PATH}:" in
  *":$dest:"*) ;;
  *) echo "note: $dest is not on your PATH; add it in your shell profile." ;;
esac

"$dest/ppcode" --version

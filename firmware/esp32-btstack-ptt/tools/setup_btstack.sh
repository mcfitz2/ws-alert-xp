#!/bin/sh
# Check out BTstack at the pinned upstream commit into btstack/ and apply our
# local changes (patches/btstack.patch). Safe to re-run: it resets the tree
# first, so it also re-syncs btstack/ after checking out an older commit.
#
#   tools/setup_btstack.sh
set -e
cd "$(dirname "$0")/.."
REPO=https://github.com/bluekitchen/btstack.git
REV=9edb5d82ea6ec4466a393415d65030929330acdf   # v1.8.3 develop merge

if [ ! -d btstack/.git ]; then
    git clone --quiet "$REPO" btstack
fi
git -C btstack checkout --quiet --force "$REV"
git -C btstack clean --quiet -fd
git -C btstack apply ../patches/btstack.patch
echo "btstack at $(git -C btstack rev-parse --short HEAD) + patches/btstack.patch"

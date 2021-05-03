#!/bin/bash
cd "$(dirname "$0")"
set -e

if ! command -v gclient >/dev/null; then
  echo "Missing depot_tools (can't find gclient in PATH)" >&2
  exit 1
fi

# ----------------------------------------------------------------------------
# dawn

[ -d dawn ] || git clone https://dawn.googlesource.com/dawn dawn

cd dawn
echo "cd $PWD"
git checkout --detach c74af7037817b2b5324d2097ea0f277044424b19 --

cp scripts/standalone.gclient .gclient
echo gclient sync
     gclient sync
cd ..

# ----------------------------------------------------------------------------
# libev

mkdir -p libev
cd libev
[ -f libev-4.33.tar.gz ] || wget http://dist.schmorp.de/libev/libev-4.33.tar.gz
INSTALL_PREFIX=$PWD
tar -xf libev-4.33.tar.gz
cd libev-4.33
./configure --prefix="$INSTALL_PREFIX" --disable-shared
make -j$(nproc)
make install
cd ../..

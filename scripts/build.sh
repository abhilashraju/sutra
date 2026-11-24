#!/bin/sh
# Configures (if needed), builds and tests sutra. Run it ON the build host; the
# workspace is shared with the host so no file sync is needed:
#   ssh myubuntu 'cd ~/work/public/sources/sutra && scripts/build.sh'
set -e
export PATH=/usr/local/gcc-15/bin:$PATH
export CC=/usr/local/gcc-15/bin/gcc
export CXX=/usr/local/gcc-15/bin/g++
export LD_LIBRARY_PATH=/usr/local/gcc-15/lib64:${LD_LIBRARY_PATH:-}
STDEXEC_PREFIX="${STDEXEC_PREFIX:-$HOME/sutra_build/stdexec-install}"
export CMAKE_PREFIX_PATH="$STDEXEC_PREFIX:${CMAKE_PREFIX_PATH:-}"
export CPLUS_INCLUDE_PATH="$STDEXEC_PREFIX/include:/usr/local/include:${CPLUS_INCLUDE_PATH:-}"
export LIBRARY_PATH="/usr/local/lib/aarch64-linux-gnu:/usr/local/lib:${LIBRARY_PATH:-}"
export PKG_CONFIG_PATH="/usr/local/lib/pkgconfig:/usr/local/lib/aarch64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH:-}"
cd "$(dirname "$0")/.."
builddir="${BUILDDIR:-$HOME/sutra_build/sutra-build}"
if [ ! -d "$builddir" ]; then
    meson setup "$builddir" .
fi
meson compile -C "$builddir"
meson test -C "$builddir" --print-errorlogs "$@"

#!/usr/bin/env bash

set -e

set -v

export PATH=$DEPOT_TOOLS:${DEPOT_TOOLS}/python-bin:$PATH

cd ${SOURCE_DIR}

case "$(uname -s)" in
  Linux*)
    if [ "$TARGET_ARCH" == "arm" ]; then
      vpython3 build/linux/sysroot_scripts/install-sysroot.py --arch=arm
    elif [ "$TARGET_ARCH" == "arm64" ]; then
      vpython3 build/linux/sysroot_scripts/install-sysroot.py --arch=arm64
    else
      vpython3 build/linux/sysroot_scripts/install-sysroot.py --arch=amd64
    fi
esac

# NOTE(mroberts): Running hooks generates this file, but running hooks also
# takes too long in CI; so do this manually.
(cd build/util && vpython3 lastchange.py -o LASTCHANGE)

# On ARM64 Linux, depot_tools bundles an x64-only gn binary at
# buildtools/linux64/gn. The gn.py wrapper tries this path before falling
# back to PATH, causing "Exec format error". Replace it with the native
# ARM64 gn if GN_ARM64_BIN is set (provided by CI workflow).
if [ -n "$GN_ARM64_BIN" ] && [ -f "$GN_ARM64_BIN" ]; then
  echo "Replacing x64 gn with ARM64 binary from $GN_ARM64_BIN"
  cp "$GN_ARM64_BIN" buildtools/linux64/gn
  chmod +x buildtools/linux64/gn
fi

gn gen ${BINARY_DIR} "--args=${GN_GEN_ARGS}"

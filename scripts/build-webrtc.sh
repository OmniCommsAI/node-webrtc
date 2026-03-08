#!/usr/bin/env bash

set -e
set -v

# We want to use system ninja, _NOT_ depot_tools ninja, actually
export PATH="${DEPOT_TOOLS}/python-bin:${PATH}:${DEPOT_TOOLS}"

export TARGETS="webrtc libjingle_peerconnection libc++ libc++abi"

# Limit parallelism to avoid OOM on CI runners (7GB RAM).
# Each libwebrtc compilation unit can use 1-2GB RAM.
# NINJA_JOBS env var allows override; defaults to nproc (all cores).
ninja -j${NINJA_JOBS:-$(nproc)} $TARGETS

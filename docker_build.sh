#!/usr/bin/env bash

set -euo pipefail

source docker_data/docker_config.sh

# Override PARALLEL_JOBS in the environment if you want to compile on fewer cores.
PARALLEL_JOBS=${PARALLEL_JOBS:-$(nproc)}
# LLVM links are memory-heavy; keep them serialized by default while compiling on all cores.
LLVM_PARALLEL_LINK_JOBS=${LLVM_PARALLEL_LINK_JOBS:-1}
echo "Using $PARALLEL_JOBS CPU cores for compilation and $LLVM_PARALLEL_LINK_JOBS parallel LLVM link job(s)"
echo "We recommend running this only on servers with >= 32 cores and at least 64GB RAM"

export DOCKER_BUILDKIT=${DOCKER_BUILDKIT:-1}

PRO_ATTACH_CONFIG=${PRO_ATTACH_CONFIG:-pro-attach-config.yaml}
BUILD_ARGS=(
    --build-arg "USER_UID=$(id -u)"
    --build-arg "USER_GID=$(id -g)"
    --build-arg "PARALLEL_JOBS=$PARALLEL_JOBS"
    --build-arg "LLVM_PARALLEL_LINK_JOBS=$LLVM_PARALLEL_LINK_JOBS"
)

if [[ -f "$PRO_ATTACH_CONFIG" ]]; then
    echo "Using Ubuntu Pro attach config as a BuildKit secret: $PRO_ATTACH_CONFIG"
    BUILD_ARGS+=(--secret "id=pro-attach-config,src=$PRO_ATTACH_CONFIG")
else
    echo "No $PRO_ATTACH_CONFIG found; building without Ubuntu Pro/ESM"
fi

docker build "${BUILD_ARGS[@]}" "$@" -t "$IMAGE_NAME" .

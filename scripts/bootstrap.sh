#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

source_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
project_home=${MINIKV_HOME:-"$HOME/projects/minikv"}
jobs=${MINIKV_JOBS:-2}
brpc_version=1.18.0
brpc_sha=94f1bbd32845a45f0218dfe43e25791252bdcb72
deps="$project_home/.deps"
brpc_src="$deps/brpc-$brpc_version"
brpc_build="$deps/brpc-build"
brpc_install="$deps/brpc-install"

packages=(build-essential cmake git python3 pkg-config libssl-dev libgflags-dev
          libprotobuf-dev libprotoc-dev protobuf-compiler libleveldb-dev
          libsnappy-dev librocksdb-dev)
missing=()
for package in "${packages[@]}"; do
    if [[ $(dpkg-query -W -f='${Status}' "$package" 2>/dev/null || true) != 'install ok installed' ]]; then
        missing+=("$package")
    fi
done
if ((${#missing[@]})); then
    sudo apt-get update
    sudo apt-get install -y "${missing[@]}"
fi

mkdir -p "$deps"
if [[ ! -d "$brpc_src" ]]; then
    git clone --depth 1 --branch "$brpc_version" https://github.com/apache/brpc.git "$brpc_src"
fi
if [[ $(git -C "$brpc_src" rev-parse HEAD) != "$brpc_sha" ]]; then
    printf 'Unexpected bRPC commit in %s; expected %s\n' "$brpc_src" "$brpc_sha" >&2
    exit 1
fi

cmake -S "$brpc_src" -B "$brpc_build" \
    -DCMAKE_BUILD_TYPE=Release -DWITH_DEBUG_SYMBOLS=OFF \
    -DBUILD_SHARED_LIBS=ON -DBUILD_BRPC_TOOLS=OFF -DBUILD_UNIT_TESTS=OFF \
    -DCMAKE_INSTALL_PREFIX="$brpc_install"
cmake --build "$brpc_build" -j "$jobs"
cmake --install "$brpc_build"

cmake -S "$source_dir" -B "$project_home/build-service" \
    -DCMAKE_BUILD_TYPE=Debug -DBRPC_ROOT="$brpc_install"
cmake --build "$project_home/build-service" -j "$jobs"
ctest --test-dir "$project_home/build-service" --output-on-failure

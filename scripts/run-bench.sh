#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
project_home=${MINIKV_HOME:-"$HOME/projects/minikv"}
exec "$project_home/build-service/minikv_bench" "$@"

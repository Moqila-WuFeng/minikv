#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
project_home=${MINIKV_HOME:-"$HOME/projects/minikv"}
exec "$project_home/build-service/minikv_server" \
    --listen=127.0.0.1:18081 --db_path="$project_home/data" "$@"

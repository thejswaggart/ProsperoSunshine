#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# shellcheck disable=SC2154  # sdk, prefix, work, src_* and the rest come from scripts/deps.sh
# PBS-Payload's PS4 capture is compiled as part of the PS4 host (cmake/ps4.cmake); only its licence ships here.
set -euo pipefail
cp "$src_pbs_payload/LICENSE" "$prefix/licenses/pbs-payload.txt"

#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Loads the PS4 host on a console: stops a running host through its pairing page (POST /api/quit), waits until its ports
# are free, sends the ELF to GoldHEN's payload loader and waits until the host answers again.
#   PS4_HOST=<console> bash scripts/ps4_deploy.sh [elf]
# PS4_LOADER_PORT: the payload loader's port, 9090 (GoldHEN's BinLoader, which takes ELFs) unless set.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
host=${PS4_HOST:?set PS4_HOST to the console\'s address}
loader=${PS4_LOADER_PORT:-9090}
elf=${1:-$root/out/ps4/prospero_sunshine_ps4.elf}
[[ -f "$elf" ]] || { echo "ps4_deploy: no $elf; build with PROSPERO_TARGET=ps4 scripts/build.sh" >&2; exit 2; }
# The host's ports: HTTPS, HTTP, the pairing page.
ports=(47984 47989 47990)

port_open() { timeout 2 bash -c "echo > /dev/tcp/$host/$1" 2>/dev/null; }
any_open() { for p in "${ports[@]}"; do port_open "$p" && return 0; done; return 1; }
all_open() { for p in "${ports[@]}"; do port_open "$p" || return 1; done; }

if any_open; then
    echo "ps4_deploy: stopping the running host"
    curl -fsS --max-time 5 -X POST "http://$host:47990/api/quit" >/dev/null || echo "ps4_deploy: no answer from /api/quit"
    # The host exits within 10 s even if a thread hangs (main_ps4.cpp).
    for _ in $(seq 1 15); do any_open || break; sleep 1; done
    if any_open; then
        echo "ps4_deploy: the host's ports are still open; restart the console" >&2
        exit 1
    fi
fi
# No test connection to the loader first: GoldHEN takes every connection as a payload and warns about an empty one
# ("invalid payload format"). A loader that isn't running shows as a refused connection here.
echo "ps4_deploy: sending $(basename "$elf") to $host:$loader"
if ! timeout 60 bash -c "cat '$elf' > /dev/tcp/$host/$loader" 2>/dev/null; then
    echo "ps4_deploy: could not send to $host:$loader; is the payload loader running?" >&2
    exit 1
fi
for i in $(seq 1 20); do
    if all_open; then
        echo "ps4_deploy: host up after ${i} s, pairing page on http://$host:47990"
        exit 0
    fi
    sleep 1
done
echo "ps4_deploy: the host did not come up within 20 s; read the kernel log (port 3232) or host_diag.log" >&2
exit 1

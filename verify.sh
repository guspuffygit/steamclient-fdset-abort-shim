#!/bin/bash
# usage: verify.sh <path to linux64/steamclient.so>
# Each trigger must abort without the shim and survive with it.

here="$(cd "$(dirname "$0")" && pwd)"
lib="${1:?path to steamclient.so}"
failed=0
ulimit -c 0

run() {
    local mode="$1" preload="$2" want="$3" out code
    out="$(LD_PRELOAD="$preload" timeout 30 "$here/repro" "$lib" "$mode" 2>&1)"
    code=$?
    if [ "$code" -eq "$want" ]; then
        echo "PASS  $mode  shim=$([ -n "$preload" ] && echo yes || echo no)  exit=$code"
    else
        echo "FAIL  $mode  shim=$([ -n "$preload" ] && echo yes || echo no)  exit=$code (wanted $want)"
        failed=1
    fi
    echo "$out" | sed 's/^/      /'
}

for mode in high-fd race; do
    run "$mode" "" 134
    run "$mode" "$here/fdelt_shim.so" 0
done
exit $failed

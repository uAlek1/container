#!/usr/bin/env bash
set -euo pipefail

CLI="${1:?missing cli path}"
DAEMON="${2:?missing daemon path}"
ROOTFS="${3:?missing rootfs path}"

export XDG_RUNTIME_DIR="/tmp/test-rt-$$"
export XDG_STATE_HOME="/tmp/test-st-$$"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME"

LOG_FILE="/tmp/daemon-test-$$.log"

"$DAEMON" > "$LOG_FILE" 2>&1 &
DPID=$!

cleanup() {
    kill "$DPID" 2>/dev/null || true
    wait "$DPID" 2>/dev/null || true
    rm -rf "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME" "$LOG_FILE"
}
trap cleanup EXIT

sleep 1

if ! kill -0 "$DPID" 2>/dev/null; then
    echo "FAIL: daemon failed to start" >&2
    cat "$LOG_FILE" >&2
    exit 1
fi

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

wait_exited() {
    local id="$1"
    for _ in $(seq 20); do
        local state
        state=$("$CLI" list --all | grep "^$id " | awk '{print $2}' || true)
        if [ "$state" = "EXITED" ] || [ "$state" = "STOPPED" ]; then
            return 0
        fi
        sleep 0.2
    done
    fail "$id not exited (last state: ${state:-unknown})"
}

# 1. Вывод
ID=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'echo hello' | tail -n1)
wait_exited "$ID"
OUT=$("$CLI" logs "$ID")
[ "$OUT" = "hello" ] && echo "PASS: output" || fail "expected 'hello', got '$OUT'"

# 2. PID 1
ID=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'echo $$' | tail -n1)
wait_exited "$ID"
OUT=$("$CLI" logs "$ID")
[ "$OUT" = "1" ] && echo "PASS: pid 1" || fail "expected '1', got '$OUT'"

# 3. Exit code 42
ID=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'exit 42' | tail -n1)
wait_exited "$ID"
CODE=$("$CLI" list --all | grep "^$ID " | awk '{print $4}')
[ "$CODE" = "42" ] && echo "PASS: exit 42" || fail "expected 42, got '$CODE'"

# 4. Exit code 0
ID=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'true' | tail -n1)
wait_exited "$ID"
CODE=$("$CLI" list --all | grep "^$ID " | awk '{print $4}')
[ "$CODE" = "0" ] && echo "PASS: exit 0" || fail "expected 0, got '$CODE'"

echo "All tests passed"
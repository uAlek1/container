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

echo "==> Test: list empty"
OUT=$("$CLI" list --all || true)
if [ -z "$OUT" ] || ! echo "$OUT" | grep -qv "ID"; then
    echo "PASS: list empty"
else
    fail "expected empty list, got: $OUT"
fi

echo "==> Test: kill"
ID=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'sleep 100' | tail -n1)
sleep 0.5

STATE=$("$CLI" list --all | grep "^$ID " | awk '{print $2}')
[ "$STATE" = "RUNNING" ] || fail "expected RUNNING before kill, got '$STATE'"

"$CLI" kill "$ID"
sleep 0.5

STATE=$("$CLI" list --all | grep "^$ID " | awk '{print $2}')
if [ "$STATE" = "STOPPED" ] || [ "$STATE" = "EXITED" ]; then
    echo "PASS: kill"
else
    fail "expected STOPPED/EXITED after kill, got '$STATE'"
fi

echo "==> Test: follow-logs"
ID=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'for i in 1 2 3; do echo "line $i"; sleep 0.3; done' | tail -n1)
sleep 0.1

timeout 2 "$CLI" logs -f "$ID" > /tmp/logs-$$.out 2>&1 || true
LINES=$(wc -l < /tmp/logs-$$.out)
rm -f /tmp/logs-$$.out

if [ "$LINES" -ge 2 ]; then
    echo "PASS: follow-logs (captured $LINES lines)"
else
    fail "expected at least 2 lines from follow-logs, got $LINES"
fi

echo "==> Test: attach"
ID=$("$CLI" run -d --tty "$ROOTFS" /bin/sh | tail -n1)
sleep 0.5

if command -v script >/dev/null 2>&1; then
    echo -e "echo attach-test\nexit" | script -q -c "$CLI attach $ID" /dev/null > /tmp/attach-$$.out 2>&1 || true
    if grep -q "attach-test" /tmp/attach-$$.out; then
        echo "PASS: attach"
    else
        cat /tmp/attach-$$.out
        fail "expected 'attach-test' in attach output"
    fi
    rm -f /tmp/attach-$$.out
else
    echo "SKIP: attach (script utility not found)"
fi

echo "All daemon protocol tests passed!"
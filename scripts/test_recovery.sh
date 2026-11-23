#!/usr/bin/env bash
set -euo pipefail

CLI="${1:?missing cli path}"
DAEMON="${2:?missing daemon path}"
ROOTFS="${3:?missing rootfs path}"

export XDG_RUNTIME_DIR="/tmp/test-rt-$$"
export XDG_STATE_HOME="/tmp/test-st-$$"
mkdir -p "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME"

LOG_FILE="/tmp/daemon-test-$$.log"

fail() {
    echo "FAIL: $1" >&2
    if [ -f "$LOG_FILE" ]; then
        echo "--- daemon log ---" >&2
        cat "$LOG_FILE" >&2
    fi
    exit 1
}

cleanup() {
    # Убить все shim'ы, которые могли остаться от теста.
    # Ищем по уникальному XDG_RUNTIME_DIR в аргументах shim'а.
    pkill -f "container-shim.*${XDG_RUNTIME_DIR}" 2>/dev/null || true
    if [ -n "${DPID:-}" ]; then
        kill -TERM "$DPID" 2>/dev/null || true
        wait "$DPID" 2>/dev/null || true
    fi
    rm -rf "$XDG_RUNTIME_DIR" "$XDG_STATE_HOME" "$LOG_FILE"
}
trap cleanup EXIT

start_daemon() {
    # setsid изолирует демон в новой сессии. Тест убивает только DPID,
    # а shim'ы (которые тоже делают setsid) остаются жить.
    setsid "$DAEMON" >> "$LOG_FILE" 2>&1 &
    DPID=$!
    sleep 1
    if ! kill -0 "$DPID" 2>/dev/null; then
        fail "daemon failed to start"
    fi
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

# --- Тест 1: восстановление после SIGTERM ---

echo "==> Starting daemon"
start_daemon

echo "==> Launching long-lived container"
ID=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'sleep 30' | tail -n1)
if [ -z "$ID" ]; then
    fail "run returned empty id"
fi
echo "    container id: $ID"

sleep 0.5

STATE=$("$CLI" list --all | grep "^$ID " | awk '{print $2}')
[ "$STATE" = "RUNNING" ] || fail "expected RUNNING, got '$STATE'"

PID_BEFORE=$("$CLI" list --all | grep "^$ID " | awk '{print $3}')
[ -n "$PID_BEFORE" ] || fail "container pid is empty"
echo "    container pid before restart: $PID_BEFORE"

if ! kill -0 "$PID_BEFORE" 2>/dev/null; then
    fail "container process $PID_BEFORE is not alive before restart"
fi

echo "==> Killing daemon (SIGTERM), shim should survive"
kill -TERM "$DPID"
wait "$DPID" 2>/dev/null || true
DPID=""

sleep 0.5

# Процесс контейнера должен быть жив после смерти демона.
if ! kill -0 "$PID_BEFORE" 2>/dev/null; then
    fail "container process $PID_BEFORE died with the daemon"
fi
echo "PASS: container process survived daemon shutdown"

echo "==> Restarting daemon (recovery)"
start_daemon

# Дать recovery время просканировать state.pb и опросить shim'ы.
sleep 1

echo "==> Verifying recovery"
LIST_OUT=$("$CLI" list --all)
echo "$LIST_OUT" | grep -q "^$ID " || fail "container $ID not recovered"

STATE=$("$CLI" list --all | grep "^$ID " | awk '{print $2}')
[ "$STATE" = "RUNNING" ] || fail "expected RUNNING after recovery, got '$STATE'"

PID_AFTER=$("$CLI" list --all | grep "^$ID " | awk '{print $3}')
[ "$PID_AFTER" = "$PID_BEFORE" ] || fail "pid changed after recovery ($PID_BEFORE -> $PID_AFTER)"
echo "PASS: container recovered with same pid $PID_AFTER"

echo "==> Killing recovered container"
"$CLI" kill "$ID"
wait_exited "$ID"
echo "PASS: kill works on recovered container"

# --- Тест 2: восстановление после SIGKILL ---

echo "==> Test: recovery after daemon crash (SIGKILL)"
ID2=$("$CLI" run -d "$ROOTFS" /bin/sh -c 'sleep 30' | tail -n1)
sleep 0.5

PID2_BEFORE=$("$CLI" list --all | grep "^$ID2 " | awk '{print $3}')
kill -KILL "$DPID"
wait "$DPID" 2>/dev/null || true
DPID=""

sleep 0.5

if ! kill -0 "$PID2_BEFORE" 2>/dev/null; then
    fail "container process $PID2_BEFORE died with SIGKILL to daemon"
fi

start_daemon
sleep 1

STATE=$("$CLI" list --all | grep "^$ID2 " | awk '{print $2}')
[ "$STATE" = "RUNNING" ] || fail "expected RUNNING after SIGKILL recovery, got '$STATE'"
echo "PASS: recovery after SIGKILL"

"$CLI" kill "$ID2"
wait_exited "$ID2"

echo "All recovery tests passed!"
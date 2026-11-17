#!/usr/bin/env bash
set -euo pipefail

IF=${1:-wlan0}
LINK=${2:-78}
REPO_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$REPO_DIR"

if [[ $EUID -ne 0 ]]; then
    echo "run as root: sudo $0 [interface] [link-mbps]" >&2
    exit 1
fi
if [[ ! -x ./rmon-probe ]]; then
    echo "rmon-probe is missing; run make first" >&2
    exit 1
fi
if [[ ! -f ./config/snmpd.conf ]]; then
    echo "config/snmpd.conf is missing" >&2
    exit 1
fi
if ! ip link show "$IF" >/dev/null 2>&1; then
    echo "interface $IF does not exist" >&2
    exit 1
fi

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="$REPO_DIR/docs/results/$STAMP"
mkdir -p "$OUT/raw"
exec > >(tee -a "$OUT/run.log") 2>&1

IFINDEX=$(cat "/sys/class/net/$IF/ifindex")
MAC=$(cat "/sys/class/net/$IF/address")
GATEWAY=$(ip -4 route show default | awk '{print $3; exit}')
SNMPD_WAS_ACTIVE=0
PROBE_PID=""
SNMPD_PID=""
TSHARK_PID=""
TRAFFIC_PID=""

cleanup() {
    local code=$?
    set +e
    if [[ -n $TRAFFIC_PID ]]; then
        pkill -TERM -P "$TRAFFIC_PID" 2>/dev/null
        kill -TERM "$TRAFFIC_PID" 2>/dev/null
        wait "$TRAFFIC_PID" 2>/dev/null
    fi
    if [[ -n $TSHARK_PID ]]; then
        kill -TERM "$TSHARK_PID" 2>/dev/null
        wait "$TSHARK_PID" 2>/dev/null
    fi
    if [[ -n $PROBE_PID ]]; then
        kill -TERM "$PROBE_PID" 2>/dev/null
        wait "$PROBE_PID" 2>/dev/null
    fi
    if [[ -n $SNMPD_PID ]]; then
        kill -TERM "$SNMPD_PID" 2>/dev/null
        wait "$SNMPD_PID" 2>/dev/null
    fi
    pkill -x rmon-probe 2>/dev/null
    pkill -f 'snmpd -f -Lo -C -c ./config/snmpd.conf' 2>/dev/null
    pkill -f 'speed.cloudflare.com/__down' 2>/dev/null
    if [[ $SNMPD_WAS_ACTIVE -eq 1 ]]; then
        systemctl start snmpd.service 2>/dev/null
    fi
    git -c safe.directory="$REPO_DIR" status --short > "$OUT/raw/git-status.txt" 2>/dev/null
    git -c safe.directory="$REPO_DIR" diff > "$OUT/code-changes.diff" 2>/dev/null
    while IFS= read -r path; do
        [[ $path == docs/results/* ]] && continue
        git -c safe.directory="$REPO_DIR" diff --no-index -- /dev/null "$path" >> "$OUT/code-changes.diff" 2>/dev/null || true
    done < <(git -c safe.directory="$REPO_DIR" ls-files --others --exclude-standard 2>/dev/null)
    chown -R "${SUDO_UID:-0}:${SUDO_GID:-0}" "$OUT" 2>/dev/null
    echo
    echo "evidence: $OUT"
    exit "$code"
}
trap cleanup EXIT INT TERM

{
    echo "timestamp=$STAMP"
    echo "interface=$IF"
    echo "ifindex=$IFINDEX"
    echo "mac=$MAC"
    echo "link_mbps=$LINK"
    echo "gateway=$GATEWAY"
    echo "kernel=$(uname -r)"
    echo "netsnmp=$(snmpwalk -V 2>&1 | head -1)"
    echo "tshark=$(tshark -v 2>&1 | head -1)"
} > "$OUT/meta.txt"

echo "== setup =="
if systemctl is-active --quiet snmpd.service; then
    SNMPD_WAS_ACTIVE=1
fi
systemctl stop snmpd.service 2>/dev/null || true
install -d -m 0755 /var/agentx
pkill -x rmon-probe 2>/dev/null || true
pkill -f 'snmpd -f -Lo -C -c ./config/snmpd.conf' 2>/dev/null || true

ethtool -k "$IF" > "$OUT/raw/ethtool-before.txt" 2>&1 || true
for feature in gro lro gso tso; do
    ethtool -K "$IF" "$feature" off >/dev/null 2>&1 || true
done
ethtool -k "$IF" > "$OUT/raw/ethtool-after.txt" 2>&1 || true

snmpd -f -Lo -C -c ./config/snmpd.conf > "$OUT/snmpd.log" 2>&1 &
SNMPD_PID=$!
sleep 1
./rmon-probe "$IF" "$LINK" > "$OUT/probe.log" 2>&1 &
PROBE_PID=$!

ready=0
for _ in $(seq 1 60); do
    if ! kill -0 "$PROBE_PID" 2>/dev/null; then
        echo "rmon-probe exited during startup" >&2
        tail -20 "$OUT/probe.log" >&2
        exit 1
    fi
    if [[ -n "$(snmpwalk -v2c -c public -t 1 -r 0 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 2>/dev/null)" ]]; then
        ready=1
        break
    fi
    sleep 0.5
done
if [[ $ready -ne 1 ]]; then
    echo "AgentX registration did not appear within 30 seconds" >&2
    tail -20 "$OUT/probe.log" >&2
    exit 1
fi

echo "== smoke walks =="
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 > "$OUT/raw/smoke-stats.txt"
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.2 > "$OUT/raw/smoke-history.txt"
[[ -s "$OUT/raw/smoke-stats.txt" ]]
[[ -s "$OUT/raw/smoke-history.txt" ]]

echo "== accuracy window =="
ip -s link show dev "$IF" > "$OUT/raw/ip-before.txt"
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 > "$OUT/raw/stats-before.txt"
cat "/sys/class/net/$IF/statistics/rx_crc_errors" > "$OUT/raw/crc-before.txt"

FILTER="(ether dst $MAC or ether multicast) and not ether src $MAC"
tshark -i "$IF" -n -a duration:30 -f "$FILTER" \
    -T fields -e frame.len -e eth.dst > "$OUT/raw/frames.tsv" 2> "$OUT/raw/tshark.err" &
TSHARK_PID=$!
sleep 1

traffic() {
    local deadline=$((SECONDS + 26))
    local size
    while ((SECONDS < deadline)); do
        for size in 56 200 700 1400; do
            ping -c 4 -i 0.05 -W 1 -s "$size" "$GATEWAY" >/dev/null 2>&1 || true
        done
        curl -sL --max-time 5 -o /dev/null 'https://speed.cloudflare.com/__down?bytes=2000000' >/dev/null 2>&1 || true
        ping -c 2 -W 1 -b 255.255.255.255 >/dev/null 2>&1 || true
    done
}
traffic > "$OUT/raw/traffic.log" 2>&1 &
TRAFFIC_PID=$!

wait "$TSHARK_PID" || true
TSHARK_PID=""
if [[ -n $TRAFFIC_PID ]]; then
    pkill -TERM -P "$TRAFFIC_PID" 2>/dev/null || true
    kill -TERM "$TRAFFIC_PID" 2>/dev/null || true
    wait "$TRAFFIC_PID" 2>/dev/null || true
    TRAFFIC_PID=""
fi
sleep 0.5

ip -s link show dev "$IF" > "$OUT/raw/ip-after.txt"
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 > "$OUT/raw/stats-after.txt"
cat "/sys/class/net/$IF/statistics/rx_crc_errors" > "$OUT/raw/crc-after.txt"

echo "== history test =="
SOURCE="1.3.6.1.2.1.2.2.1.1.$IFINDEX"
HC="1.3.6.1.2.1.16.2.1.1"
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.2.2 > "$OUT/raw/history-row1-before.txt"
snmpset -v2c -c private -t 2 -r 1 127.0.0.1:1161 \
    "$HC.2.10" o "$SOURCE" \
    "$HC.3.10" i 4 \
    "$HC.5.10" i 10 \
    "$HC.6.10" s demo \
    "$HC.7.10" i 2 > "$OUT/raw/history-create.txt" 2>&1 || true
snmpset -v2c -c private -t 2 -r 1 127.0.0.1:1161 \
    "$HC.7.10" i 1 > "$OUT/raw/history-valid.txt" 2>&1 || true
sleep 47
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.2.2 > "$OUT/raw/history-after-47s.txt"
sleep 13
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.2.2 > "$OUT/raw/history-after-60s.txt"
snmpset -v2c -c private -t 2 -r 1 127.0.0.1:1161 \
    "$HC.7.10" i 4 > "$OUT/raw/history-delete.txt" 2>&1 || true
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.2.1 > "$OUT/raw/history-control-after-delete.txt"

echo "== statistics row lifecycle =="
ST="1.3.6.1.2.1.16.1.1.1"
snmpset -v2c -c private -t 2 -r 1 127.0.0.1:1161 \
    "$ST.2.10" o "$SOURCE" \
    "$ST.20.10" s demo \
    "$ST.21.10" i 2 > "$OUT/raw/stats10-create.txt" 2>&1 || true
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 > "$OUT/raw/stats10-created.txt"
snmpset -v2c -c private -t 2 -r 1 127.0.0.1:1161 \
    "$ST.21.10" i 1 > "$OUT/raw/stats10-valid.txt" 2>&1 || true
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 > "$OUT/raw/stats10-baseline.txt"
ping -c 10 -i 0.05 -W 1 -s 200 "$GATEWAY" >/dev/null 2>&1 || true
curl -sL --max-time 5 -o /dev/null 'https://speed.cloudflare.com/__down?bytes=1000000' >/dev/null 2>&1 || true
sleep 1
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 > "$OUT/raw/stats10-after.txt"
snmpset -v2c -c private -t 2 -r 1 127.0.0.1:1161 \
    "$ST.21.10" i 4 > "$OUT/raw/stats10-delete.txt" 2>&1 || true
snmpwalk -v2c -c public -On -Oq 127.0.0.1:1161 1.3.6.1.2.1.16.1.1 > "$OUT/raw/stats10-deleted.txt"

python3 "$REPO_DIR/scripts/analyze_results.py" "$OUT" "$IF" "$LINK"
echo "== done =="

#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "${BASH_SOURCE[0]}")"
AGENT="127.0.0.1:1161"
OID_STATS="1.3.6.1.2.1.16.1.1.1"
OID_STATS_PKTS="$OID_STATS.5.1"
OID_STATS_OCTETS="$OID_STATS.4.1"
OID_HISTORY="1.3.6.1.2.1.16.2.2"
OID_CONTROL="1.3.6.1.2.1.16.2.1.1"

DRY=0
POSITIONAL=()
for argument in "$@"; do
    case "$argument" in
        --dry-run) DRY=1 ;;
        -h|--help)
            echo "usage: $0 [interface] [link-mbps] [--dry-run]"
            exit 0
            ;;
        *)
            POSITIONAL+=("$argument")
            ;;
    esac
done
IF=${POSITIONAL[0]:-wlan0}
LINK=${POSITIONAL[1]:-}
if [[ -z $LINK ]]; then
    if [[ $DRY -eq 1 ]]; then
        LINK=100
    else
        LINK=$(iw dev "$IF" link 2>/dev/null | awk '/rx bitrate:/ {printf "%d", $3; exit}')
        if [[ -z $LINK || $LINK -lt 1 ]]; then
            LINK=100
        fi
    fi
fi

SUDO=()
if [[ $EUID -ne 0 ]]; then
    SUDO=(sudo)
fi

SNMPD_WAS_ACTIVE=0
SNMPD_PID=""
PROBE_PID=""
TRAFFIC_PID=""
STREAMLIT_PID=""
GW=$(ip -4 route show default 2>/dev/null | awk '{print $3; exit}')
GW=${GW:-1.1.1.1}

cleanup() {
    local code=$?
    trap - EXIT INT TERM
    set +e
    if [[ $DRY -eq 0 ]]; then
        if [[ -n $TRAFFIC_PID ]]; then
            pkill -TERM -P "$TRAFFIC_PID" 2>/dev/null
            kill -TERM "$TRAFFIC_PID" 2>/dev/null
            wait "$TRAFFIC_PID" 2>/dev/null
        fi
        if [[ -n $STREAMLIT_PID ]]; then
            kill -TERM "$STREAMLIT_PID" 2>/dev/null
            wait "$STREAMLIT_PID" 2>/dev/null
        fi
        if [[ -n $PROBE_PID ]]; then
            "${SUDO[@]}" kill -TERM "$PROBE_PID" 2>/dev/null
            wait "$PROBE_PID" 2>/dev/null
        fi
        if [[ -n $SNMPD_PID ]]; then
            "${SUDO[@]}" kill -TERM "$SNMPD_PID" 2>/dev/null
            wait "$SNMPD_PID" 2>/dev/null
        fi
        "${SUDO[@]}" pkill -x rmon-probe 2>/dev/null
        "${SUDO[@]}" pkill -f 'snmpd -f -Lo -C -c ./config/snmpd.conf' 2>/dev/null
        pkill -f 'streamlit run dash.py' 2>/dev/null
        pkill -f 'speed.cloudflare.com/__down' 2>/dev/null
        if [[ $SNMPD_WAS_ACTIVE -eq 1 ]]; then
            "${SUDO[@]}" systemctl start snmpd.service 2>/dev/null
        fi
    fi
    exit "$code"
}
trap cleanup EXIT INT TERM

run() {
    if [[ $DRY -eq 1 ]]; then
        printf '+'
        printf ' %q' "$@"
        printf '\n'
    else
        "$@"
    fi
}

say() {
    printf '\n=== %s ===\n' "$1"
}

pause() {
    if [[ $DRY -eq 1 ]]; then
        printf '[dry-run] %s\n' "$1"
    else
        read -rp "$1" _
    fi
}

wait_seconds() {
    if [[ $DRY -eq 1 ]]; then
        printf '+ sleep %s\n' "$1"
    else
        sleep "$1"
    fi
}

get_counter() {
    local value
    value=$(snmpget -v2c -c public -t 1 -r 0 -Oqv "$AGENT" "$1" 2>/dev/null | tr -cd '0-9')
    printf '%s' "${value:-0}"
}

history_samples() {
    snmpwalk -v2c -c public -t 1 -r 0 -On -Oq "$AGENT" "$OID_HISTORY" 2>/dev/null |
        awk -v prefix=".1.3.6.1.2.1.16.2.2.1.1.10." 'index($1, prefix) == 1 {print $1}'
}

sample_index_of() {
    local oid=$1
    printf '%s\n' "${oid##*.}"
}

traffic() {
    local size
    while :; do
        for size in 56 200 700 1400; do
            ping -c 5 -i 0.2 -W 1 -s "$size" "$GW" >/dev/null 2>&1 || true
        done
        curl -sL --max-time 5 -o /dev/null 'https://speed.cloudflare.com/__down?bytes=2000000' >/dev/null 2>&1 || true
        ping -c 2 -W 1 -b 255.255.255.255 >/dev/null 2>&1 || true
    done
}

show_deltas() {
    local previous_packets=0
    local previous_octets=0
    local previous_time
    local packets
    local octets
    local now
    local elapsed
    local line
    previous_time=$(date +%s%N)
    printf '%-4s %-14s %-12s %-12s %-10s\n' "t" "packets" "pkts/s" "octets" "Mbit/s"
    for step in $(seq 1 10); do
        packets=$(get_counter "$OID_STATS_PKTS")
        octets=$(get_counter "$OID_STATS_OCTETS")
        now=$(date +%s%N)
        elapsed=$((now - previous_time))
        line=$(awk -v p="$packets" -v pp="$previous_packets" \
            -v o="$octets" -v po="$previous_octets" -v e="$elapsed" -v l="$LINK" \
            'BEGIN {
                if (e <= 0) e = 1;
                dp = (p - pp + 4294967296) % 4294967296;
                do_ = (o - po + 4294967296) % 4294967296;
                printf "%-4d %-14d %-12.1f %-12d %-10.2f\n",
                    '"$step"', p, dp / (e / 1e9), o, do_ * 8 / (e / 1e9) / 1e6;
                if (l > 0) printf "     utilization %.2f%% of %s Mbit/s\n", do_ * 8 / (e / 1e9) / 1e6 / l * 100, l;
            }')
        printf '%s\n' "$line"
        previous_packets=$packets
        previous_octets=$octets
        previous_time=$now
        wait_seconds 2
    done
}

if [[ $DRY -eq 0 ]]; then
    for command in snmpwalk snmpget snmpset ethtool iw curl ping; do
        if ! command -v "$command" >/dev/null 2>&1; then
            echo "missing prerequisite: $command" >&2
            exit 1
        fi
    done
    if ! ip link show "$IF" >/dev/null 2>&1; then
        echo "interface $IF does not exist" >&2
        exit 1
    fi
fi

say "Step 0: privileges and setup"
if [[ $DRY -eq 1 ]]; then
    if [[ ${#SUDO[@]} -gt 0 ]]; then
        echo '+ sudo -v'
    else
        echo '+ # already root'
    fi
elif [[ $EUID -ne 0 ]]; then
    sudo -v
fi
if [[ $DRY -eq 0 ]]; then
    systemctl is-active --quiet snmpd.service && SNMPD_WAS_ACTIVE=1 || true
fi
run "${SUDO[@]}" systemctl stop snmpd.service 2>/dev/null || true
run "${SUDO[@]}" install -d -m 0755 /var/agentx
for feature in gro lro gso tso; do
    run "${SUDO[@]}" ethtool -K "$IF" "$feature" off 2>/dev/null || true
done
run "${SUDO[@]}" pkill -x rmon-probe 2>/dev/null || true
run "${SUDO[@]}" pkill -f 'snmpd -f -Lo -C -c ./config/snmpd.conf' 2>/dev/null || true
pkill -f 'streamlit run dash.py' 2>/dev/null || true
pkill -f 'speed.cloudflare.com/__down' 2>/dev/null || true
if [[ ! -x ./rmon-probe ]]; then
    run make
fi
if [[ $DRY -eq 1 ]]; then
    printf '+ %s snmpd -f -Lo -C -c ./config/snmpd.conf > /tmp/rmon-demo-snmpd.log 2>&1 &\n' "${SUDO[*]}"
    printf '+ %s ./rmon-probe %q %q > /tmp/rmon-demo-probe.log 2>&1 &\n' "${SUDO[*]}" "$IF" "$LINK"
else
    "${SUDO[@]}" snmpd -f -Lo -C -c ./config/snmpd.conf > /tmp/rmon-demo-snmpd.log 2>&1 &
    SNMPD_PID=$!
    sleep 1
    "${SUDO[@]}" ./rmon-probe "$IF" "$LINK" > /tmp/rmon-demo-probe.log 2>&1 &
    PROBE_PID=$!
fi

if [[ $DRY -eq 0 ]]; then
    registered=0
    for _ in $(seq 1 60); do
        if ! kill -0 "$PROBE_PID" 2>/dev/null; then
            echo "rmon-probe exited during startup; see /tmp/rmon-demo-probe.log" >&2
            tail -20 /tmp/rmon-demo-probe.log >&2
            exit 1
        fi
        if snmpwalk -v2c -c public -t 1 -r 0 -On -Oq "$AGENT" "$OID_STATS" 2>/dev/null |
            awk -v prefix=".1.3.6.1.2.1.16.1.1." 'index($1, prefix) == 1 {found=1} END {exit !found}'; then
            registered=1
            break
        fi
        sleep 0.5
    done
    if [[ $registered -ne 1 ]]; then
        echo "AgentX registration did not appear within 30 s" >&2
        tail -20 /tmp/rmon-demo-probe.log >&2
        exit 1
    fi
fi
echo "snmpd log: /tmp/rmon-demo-snmpd.log"
echo "probe log: /tmp/rmon-demo-probe.log"

say "Step 1: AgentX registration and etherStatsTable"
run snmpwalk -v2c -c public -t 1 -r 0 "$AGENT" "$OID_STATS"
pause "Press Enter for live traffic counters..."

say "Step 2: live traffic and counter deltas"
if [[ $DRY -eq 1 ]]; then
    echo "+ traffic (ping sizes 56/200/700/1400 + curl loop) &"
    echo "+ snmpget $OID_STATS_PKTS $OID_STATS_OCTETS every 2 s, print pkts/s and Mbit/s"
else
    traffic >/tmp/rmon-demo-traffic.log 2>&1 &
    TRAFFIC_PID=$!
    show_deltas
fi
pause "Press Enter for the size-bucket distribution..."

say "Step 3: frame size buckets"
for pair in "9:undersize" "14:64" "15:65-127" "16:128-255" "17:256-511" "18:512-1023" "19:1024-1518" "10:oversize"; do
    column=${pair%%:*}
    label=${pair#*:}
    if [[ $DRY -eq 1 ]]; then
        printf '+ snmpget -v2c -c public %s %s.%s.1\n' "$AGENT" "$OID_STATS" "$column"
    else
        value=$(get_counter "$OID_STATS.$column.1")
        printf '%-12s %s\n' "$label" "$value"
    fi
done
pause "Press Enter to create the 10 s history row..."

say "Step 4: history row 10 (interval 10 s, four buckets)"
IFINDEX=$(cat "/sys/class/net/$IF/ifindex")
SOURCE="1.3.6.1.2.1.2.2.1.1.$IFINDEX"
run snmpset -v2c -c private "$AGENT" "$OID_CONTROL.7.10" i 4 2>/dev/null || true
run snmpset -v2c -c private "$AGENT" \
    "$OID_CONTROL.2.10" o "$SOURCE" \
    "$OID_CONTROL.3.10" i 4 \
    "$OID_CONTROL.5.10" i 10 \
    "$OID_CONTROL.6.10" s demo \
    "$OID_CONTROL.7.10" i 2
run snmpset -v2c -c private "$AGENT" "$OID_CONTROL.7.10" i 1
if [[ $DRY -eq 1 ]]; then
    echo "+ poll etherHistoryTable until four samples, then wait one interval and show eviction"
else
    samples=0
    for _ in $(seq 1 60); do
        samples=$(history_samples | wc -l)
        if [[ $samples -ge 4 ]]; then
            break
        fi
        sleep 1
    done
    printf 'samples after first intervals: %s\n' "$samples"
    snmpwalk -v2c -c public "$AGENT" "$OID_HISTORY" | grep '\.10\.' || true
    first_before=$(history_samples | head -1 | while read -r oid; do sample_index_of "$oid"; done)
    printf 'oldest sample index before eviction: %s\n' "${first_before:-none}"
    for _ in $(seq 1 15); do
        first_now=$(history_samples | head -1 | while read -r oid; do sample_index_of "$oid"; done)
        if [[ -n $first_now && $first_now != "$first_before" ]]; then
            break
        fi
        sleep 1
    done
    printf 'oldest sample index after eviction: %s\n' "${first_now:-none}"
    snmpwalk -v2c -c public "$AGENT" "$OID_HISTORY" | grep '\.10\.' || true
fi
pause "Press Enter to delete history row 10..."

say "Step 5: delete history row 10"
run snmpset -v2c -c private "$AGENT" "$OID_CONTROL.7.10" i 4
if [[ $DRY -eq 0 ]]; then
    if snmpwalk -v2c -c public -t 1 -r 0 -On -Oq "$AGENT" "$OID_CONTROL" 2>/dev/null |
        awk -v oid=".1.3.6.1.2.1.16.2.1.1.7.10" '$1 == oid {found=1} END {exit !found}'; then
        echo "history row 10 is still present"
    else
        echo "history row 10 deleted"
    fi
fi
pause "Press Enter to launch the dashboard..."

say "Step 6: Streamlit dashboard"
if [[ $DRY -eq 1 ]]; then
    echo "+ RMON_AGENT=$AGENT RMON_LINK_MBPS=$LINK venv/bin/streamlit run dash.py --server.headless true --server.port 8501 --browser.gatherUsageStats false &"
    echo "dashboard: http://localhost:8501"
else
    if [[ ! -x venv/bin/streamlit ]]; then
        echo "venv/bin/streamlit is missing" >&2
        exit 1
    fi
    RMON_AGENT="$AGENT" RMON_LINK_MBPS="$LINK" \
        venv/bin/streamlit run dash.py --server.headless true --server.port 8501 \
        --browser.gatherUsageStats false >/tmp/rmon-demo-streamlit.log 2>&1 &
    STREAMLIT_PID=$!
    sleep 3
    echo "dashboard: http://localhost:8501"
    echo "streamlit log: /tmp/rmon-demo-streamlit.log"
fi
pause "Press Enter for the stored accuracy summary..."

say "Step 7: accuracy summary"
if [[ -f docs/results/LATEST.md ]]; then
    cat docs/results/LATEST.md
else
    echo "docs/results/LATEST.md is missing; run scripts/run_validation.sh first"
fi

say "Demo complete"

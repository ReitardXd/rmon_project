import os
import subprocess
import time

import pandas as pd
import streamlit as st

AGENT = os.environ.get("RMON_AGENT", "127.0.0.1:1161")
COMMUNITY = os.environ.get("RMON_COMMUNITY", "public")
LINK_MBPS = float(os.environ.get("RMON_LINK_MBPS", "0") or 0)

STATS_PREFIX = [1, 3, 6, 1, 2, 1, 16, 1, 1, 1]
CONTROL_PREFIX = [1, 3, 6, 1, 2, 1, 16, 2, 1, 1]
HISTORY_PREFIX = [1, 3, 6, 1, 2, 1, 16, 2, 2, 1]

STATS_NAMES = {
    1: "index", 2: "dataSource", 3: "dropEvents", 4: "octets", 5: "pkts",
    6: "broadcastPkts", 7: "multicastPkts", 8: "crcAlignErrors",
    9: "undersizePkts", 10: "oversizePkts", 11: "fragments", 12: "jabbers",
    13: "collisions", 14: "pkts64", 15: "pkts65to127", 16: "pkts128to255",
    17: "pkts256to511", 18: "pkts512to1023", 19: "pkts1024to1518",
    20: "owner", 21: "status",
}
CONTROL_NAMES = {
    1: "index", 2: "dataSource", 3: "bucketsRequested",
    4: "bucketsGranted", 5: "interval", 6: "owner", 7: "status",
}
HISTORY_NAMES = {
    1: "controlIndex", 2: "sampleIndex", 3: "intervalStart", 4: "dropEvents",
    5: "octets", 6: "pkts", 7: "broadcastPkts", 8: "multicastPkts",
    9: "crcAlignErrors", 10: "undersizePkts", 11: "oversizePkts",
    12: "fragments", 13: "jabbers", 14: "collisions", 15: "utilization",
}


def walk(oid):
    command = [
        "snmpwalk", "-v2c", "-c", COMMUNITY, "-On", "-Oq",
        "-t", "2", "-r", "1", AGENT, oid,
    ]
    result = subprocess.run(command, capture_output=True, text=True, timeout=15)
    if result.returncode != 0 and not result.stdout.strip():
        raise RuntimeError(result.stderr.strip() or "snmpwalk failed")
    rows = []
    for line in result.stdout.splitlines():
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        numbers = [int(part) for part in parts[0].strip(".").split(".")]
        value = parts[1].strip()
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        rows.append((numbers, value))
    return rows


def number(value):
    try:
        return int(value)
    except ValueError:
        return value


def table(prefix):
    entries = {}
    for numbers, value in walk(".".join(str(part) for part in prefix)):
        if numbers[:len(prefix)] != prefix:
            continue
        remaining = numbers[len(prefix):]
        if len(remaining) < 2:
            continue
        column = remaining[0]
        index = tuple(remaining[1:])
        entries.setdefault(index, {})[column] = number(value)
    return entries


def frame(entries, names, key_name):
    rows = []
    for index, values in sorted(entries.items()):
        row = {key_name: ".".join(str(part) for part in index)}
        for column, value in values.items():
            row[names.get(column, str(column))] = value
        rows.append(row)
    return pd.DataFrame(rows)


def counter_delta(new, old):
    try:
        return (int(new) - int(old)) & 0xFFFFFFFF
    except (TypeError, ValueError):
        return 0


st.set_page_config(page_title="RMON Probe", layout="wide")
st.title("RMON1 Probe")
st.caption(f"AgentX agent {AGENT} via {COMMUNITY}")

with st.sidebar:
    st.header("Dashboard")
    auto = st.checkbox("Auto refresh", value=True)
    period = st.number_input("Refresh seconds", min_value=1, max_value=60, value=5)
    link = st.number_input(
        "Link Mbps (0 = unknown)", min_value=0.0, value=LINK_MBPS, step=1.0
    )
    if st.button("Refresh now"):
        st.rerun()

try:
    stats = table(STATS_PREFIX)
    controls = table(CONTROL_PREFIX)
    history = table(HISTORY_PREFIX)
except (RuntimeError, subprocess.TimeoutExpired, ValueError) as error:
    st.error(f"Cannot read the probe: {error}")
    st.stop()

stats_rows = frame(stats, STATS_NAMES, "row")
controls_rows = frame(controls, CONTROL_NAMES, "row")
history_rows = frame(history, HISTORY_NAMES, "control.sample")

if stats_rows.empty:
    st.warning("No etherStats rows. Is rmon-probe running and registered?")
else:
    first = stats_rows.iloc[0]
    now = time.monotonic()
    previous = st.session_state.get("previous")
    st.session_state["previous"] = {
        "time": now,
        "octets": first.get("octets", 0),
        "pkts": first.get("pkts", 0),
    }
    columns = st.columns(5)
    columns[0].metric("Packets", f"{first.get('pkts', 0):,}")
    columns[1].metric("Octets", f"{first.get('octets', 0):,}")
    columns[2].metric("Broadcast", f"{first.get('broadcastPkts', 0):,}")
    columns[3].metric("Multicast", f"{first.get('multicastPkts', 0):,}")
    columns[4].metric("Drops", f"{first.get('dropEvents', 0):,}")
    if previous is not None and now > previous["time"]:
        elapsed = now - previous["time"]
        packets = counter_delta(first.get("pkts", 0), previous["pkts"])
        octets = counter_delta(first.get("octets", 0), previous["octets"])
        mbit = octets * 8.0 / elapsed / 1_000_000
        columns = st.columns(4)
        columns[0].metric("Packets/s", f"{packets / elapsed:,.1f}")
        columns[1].metric("Mbit/s", f"{mbit:,.2f}")
        if link > 0:
            columns[2].metric("Utilization", f"{mbit / link * 100:,.2f}%")
        else:
            columns[2].metric("Utilization", "set link Mbps")
        columns[3].metric("Interval", f"{elapsed:,.1f} s")

st.subheader("etherStatsTable")
st.dataframe(stats_rows, width="stretch", hide_index=True)

st.subheader("historyControlTable")
st.dataframe(controls_rows, width="stretch", hide_index=True)

st.subheader("etherHistoryTable")
if history_rows.empty:
    st.info("No completed history samples yet.")
else:
    st.dataframe(history_rows, width="stretch", hide_index=True)

if auto:
    time.sleep(float(period))
    st.rerun()

#!/usr/bin/env python3
import re
import sys
from pathlib import Path

STATS_PREFIX = [1, 3, 6, 1, 2, 1, 16, 1, 1, 1]
CONTROL_PREFIX = [1, 3, 6, 1, 2, 1, 16, 2, 1, 1]
HISTORY_PREFIX = [1, 3, 6, 1, 2, 1, 16, 2, 2, 1]
COUNTERS = list(range(3, 20))
HISTORY_COUNTERS = list(range(4, 15))


def read_lines(path):
    return Path(path).read_text(errors="replace").splitlines()


def parse_snmp(path):
    entries = []
    for line in read_lines(path):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        match = re.match(r"^\.?([0-9.]+)\s+(.*)$", line)
        if match is None:
            continue
        oid = [int(part) for part in match.group(1).strip(".").split(".")]
        value = match.group(2).strip()
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        entries.append((oid, value))
    return entries


def number(value):
    try:
        return int(value)
    except ValueError:
        return value


def parse_ticks(value):
    match = re.match(r"^(\d+):(\d+):(\d+):(\d+)\.(\d+)$", str(value))
    if match is None:
        return value
    days, hours, minutes, seconds, centis = (int(part) for part in match.groups())
    return (((days * 24 + hours) * 60 + minutes) * 60 + seconds) * 100 + centis


def stats_row(path, row):
    values = {}
    for oid, value in parse_snmp(path):
        if oid[:10] == STATS_PREFIX and len(oid) == 12 and oid[-1] == row:
            values[oid[10]] = number(value)
    return values


def control_rows(path):
    rows = {}
    for oid, value in parse_snmp(path):
        if oid[:10] == CONTROL_PREFIX and len(oid) == 12:
            rows.setdefault(oid[11], {})[oid[10]] = number(value)
    return rows


def history_samples(path, control):
    samples = {}
    for oid, value in parse_snmp(path):
        if oid[:10] == HISTORY_PREFIX and len(oid) == 13 and oid[11] == control:
            samples.setdefault(oid[12], {})[oid[10]] = parse_ticks(value) if oid[10] == 3 else number(value)
    return samples


def counter_delta(new, old):
    if not isinstance(new, int) or not isinstance(old, int):
        return 0
    return (new - old) & 0xFFFFFFFF


def ip_rx(path):
    lines = [line.strip() for line in read_lines(path)]
    for position, line in enumerate(lines):
        if line.startswith("RX:"):
            values = lines[position + 1].split()
            if len(values) >= 6:
                return {
                    "bytes": int(values[0]),
                    "packets": int(values[1]),
                    "errors": int(values[2]),
                    "dropped": int(values[3]),
                    "missed": int(values[4]),
                    "multicast": int(values[5]),
                }
    return {}


def read_u64(path):
    for line in read_lines(path):
        match = re.search(r"\d+", line)
        if match:
            return int(match.group(0))
    return 0


def frames(path):
    lengths = []
    destinations = []
    for line in read_lines(path):
        parts = line.split("\t")
        if len(parts) < 2:
            continue
        try:
            length = int(parts[0])
        except ValueError:
            continue
        lengths.append(length)
        destinations.append(parts[1].strip().lower())
    return lengths, destinations


def is_broadcast(destination):
    return destination == "ff:ff:ff:ff:ff:ff"


def is_multicast(destination):
    if not destination or is_broadcast(destination):
        return False
    try:
        return (int(destination.split(":")[0], 16) & 1) == 1
    except ValueError:
        return False


def percent(probe, reference):
    if reference == 0:
        return 0.0 if probe == 0 else None
    return (probe - reference) / reference * 100.0


def difference(probe, reference):
    value = percent(probe, reference)
    return "n/a" if value is None else f"{value:+.2f}%"


def result(probe, reference, tolerance=5.0, absolute=0):
    value = percent(probe, reference)
    if value is None:
        return False
    return abs(value) <= tolerance or abs(probe - reference) <= absolute


def check(condition, label):
    return (bool(condition), label)


def main():
    out = Path(sys.argv[1])
    interface = sys.argv[2]
    link = sys.argv[3]
    raw = out / "raw"

    meta = {}
    for line in read_lines(out / "meta.txt"):
        if "=" in line:
            key, value = line.split("=", 1)
            meta[key] = value

    stats_before = stats_row(raw / "stats-before.txt", 1)
    stats_after = stats_row(raw / "stats-after.txt", 1)
    ip_before = ip_rx(raw / "ip-before.txt")
    ip_after = ip_rx(raw / "ip-after.txt")
    crc_before = read_u64(raw / "crc-before.txt")
    crc_after = read_u64(raw / "crc-after.txt")
    lengths, destinations = frames(raw / "frames.tsv")

    probe_pkts = counter_delta(stats_after.get(5), stats_before.get(5))
    probe_octets = counter_delta(stats_after.get(4), stats_before.get(4))
    probe_broadcast = counter_delta(stats_after.get(6), stats_before.get(6))
    probe_multicast = counter_delta(stats_after.get(7), stats_before.get(7))
    probe_crc = counter_delta(stats_after.get(8), stats_before.get(8))
    probe_drops = counter_delta(stats_after.get(3), stats_before.get(3))
    probe_undersize = counter_delta(stats_after.get(9), stats_before.get(9))
    probe_oversize = counter_delta(stats_after.get(10), stats_before.get(10))
    probe_fragments = counter_delta(stats_after.get(11), stats_before.get(11))
    probe_jabbers = counter_delta(stats_after.get(12), stats_before.get(12))
    probe_collisions = counter_delta(stats_after.get(13), stats_before.get(13))

    wire = [length + 4 for length in lengths]
    tshark_pkts = len(lengths)
    tshark_octets = sum(wire)
    tshark_broadcast = sum(1 for destination in destinations if is_broadcast(destination))
    tshark_multicast = sum(1 for destination in destinations if is_multicast(destination))
    tshark_undersize = sum(1 for length in wire if length < 64)
    tshark_oversize = sum(1 for length in wire if length > 1518)
    tshark_buckets = {
        14: sum(1 for length in wire if length == 64),
        15: sum(1 for length in wire if 65 <= length <= 127),
        16: sum(1 for length in wire if 128 <= length <= 255),
        17: sum(1 for length in wire if 256 <= length <= 511),
        18: sum(1 for length in wire if 512 <= length <= 1023),
        19: sum(1 for length in wire if 1024 <= length <= 1518),
    }
    label_buckets = {
        14: "64 octets",
        15: "65-127 octets",
        16: "128-255 octets",
        17: "256-511 octets",
        18: "512-1023 octets",
        19: "1024-1518 octets",
    }
    drops_note = "not reported by tshark"

    checks = []
    checks.append(check(stats_before and stats_after, "etherStats row 1 present before and after"))
    checks.append(check(result(probe_pkts, tshark_pkts), "packet count within 5% of tshark"))
    checks.append(check(result(probe_octets, tshark_octets), "octet count within 5% of tshark"))
    checks.append(check(result(probe_broadcast, tshark_broadcast, absolute=10),
                       "broadcast count within 5% or 10 frames of tshark"))
    checks.append(check(result(probe_multicast, tshark_multicast, absolute=10),
                       "multicast count within 5% or 10 frames of tshark"))
    checks.append(check(result(probe_undersize, tshark_undersize), "undersize count within 5% of tshark"))
    checks.append(check(result(probe_oversize, tshark_oversize), "oversize count within 5% of tshark"))
    for column, label in label_buckets.items():
        checks.append(check(
            result(counter_delta(stats_after.get(column), stats_before.get(column)),
                   tshark_buckets[column]),
            f"{label} count within 5% of tshark",
        ))
    checks.append(check(result(probe_crc, crc_after - crc_before), "CRC delta matches sysfs rx_crc_errors"))

    history_before = history_samples(raw / "history-row1-before.txt", 1)
    history_47 = history_samples(raw / "history-after-47s.txt", 10)
    history_60 = history_samples(raw / "history-after-60s.txt", 10)
    history_row1_after = history_samples(raw / "history-after-60s.txt", 1)
    history_control_after = control_rows(raw / "history-control-after-delete.txt")

    indices_47 = sorted(history_47)
    indices_60 = sorted(history_60)
    starts_47 = [history_47[index].get(3) for index in indices_47]
    increasing_47 = all(
        history_47[indices_47[position]].get(2) < history_47[indices_47[position + 1]].get(2)
        for position in range(len(indices_47) - 1)
    )
    starts_increasing_47 = all(
        isinstance(starts_47[position], int)
        and isinstance(starts_47[position + 1], int)
        and starts_47[position] < starts_47[position + 1]
        for position in range(len(starts_47) - 1)
    )
    checks.append(check(len(history_47) == 4, "history row 10 has exactly four samples after 47 s"))
    checks.append(check(increasing_47, "history row 10 sample indices increase"))
    checks.append(check(starts_increasing_47, "history row 10 interval starts increase"))
    checks.append(check(len(history_60) == 4, "history row 10 still has four samples after 60 s"))
    checks.append(check(
        indices_47 and indices_60 and indices_60[0] > indices_47[0],
        "history row 10 oldest sample was evicted",
    ))
    checks.append(check(10 not in history_control_after, "history row 10 deleted"))

    row1_indices = sorted(history_row1_after)
    row1_starts = [history_row1_after[index].get(3) for index in row1_indices]
    row1_growth = len(history_row1_after) - len(history_before)
    row1_diffs = [
        row1_starts[position + 1] - row1_starts[position]
        for position in range(len(row1_starts) - 1)
        if isinstance(row1_starts[position], int) and isinstance(row1_starts[position + 1], int)
    ]
    row1_steady = bool(row1_diffs) and all(abs(value - 3000) <= 150 for value in row1_diffs[-3:])
    checks.append(check(row1_growth >= 1, "history row 1 gained samples during the 60 s window"))
    checks.append(check(row1_steady, "history row 1 interval starts advance by about 3000 ticks"))

    stats10_created = stats_row(raw / "stats10-created.txt", 10)
    stats10_baseline = stats_row(raw / "stats10-baseline.txt", 10)
    stats10_after = stats_row(raw / "stats10-after.txt", 10)
    stats10_deleted = stats_row(raw / "stats10-deleted.txt", 10)
    baseline_pkts = counter_delta(stats10_baseline.get(5), 0) if stats10_baseline else None
    after_pkts = counter_delta(stats10_after.get(5), stats10_baseline.get(5)) if stats10_after and stats10_baseline else 0
    checks.append(check(bool(stats10_created), "etherStats row 10 created"))
    checks.append(check(baseline_pkts is not None and baseline_pkts < 200, "etherStats row 10 baseline is near zero"))
    checks.append(check(after_pkts > 0, "etherStats row 10 counts traffic after creation"))
    checks.append(check(not stats10_deleted, "etherStats row 10 deleted"))

    checks.append(check((raw / "smoke-stats.txt").stat().st_size > 0, "smoke walk of etherStatsTable"))
    checks.append(check((raw / "smoke-history.txt").stat().st_size > 0, "smoke walk of historyControlTable/etherHistoryTable"))

    passed = sum(1 for ok, _ in checks if ok)
    failed = len(checks) - passed

    lines = []
    lines.append(f"# RMON probe validation {out.name}")
    lines.append("")
    lines.append(f"- Interface: `{interface}` (ifIndex {meta.get('ifindex', '?')}, MAC {meta.get('mac', '?')})")
    lines.append(f"- Link argument: {link} Mbit/s")
    lines.append(f"- Kernel: {meta.get('kernel', '?')}")
    lines.append(f"- Net-SNMP: {meta.get('netsnmp', '?')}")
    lines.append(f"- tshark: {meta.get('tshark', '?')}")
    lines.append(f"- Default gateway: {meta.get('gateway', '?')}")
    lines.append(f"- Checks: {passed} passed, {failed} failed")
    lines.append("")
    lines.append("## Checks")
    lines.append("")
    lines.append("| Check | Result |")
    lines.append("| --- | --- |")
    for ok, label in checks:
        lines.append(f"| {label} | {'PASS' if ok else 'FAIL'} |")
    lines.append("")
    lines.append("## Accuracy comparison (30 s window)")
    lines.append("")
    lines.append("The tshark filter was `(ether dst <local MAC> or ether multicast) and not ether src <local MAC>`, "
                 "which excludes locally transmitted frames so it matches the probe's `PCAP_D_IN` view.")
    lines.append("")
    lines.append("| Metric | Probe delta | tshark reference | Difference |")
    lines.append("| --- | ---: | ---: | ---: |")
    lines.append(f"| Packets | {probe_pkts} | {tshark_pkts} | {difference(probe_pkts, tshark_pkts)} |")
    lines.append(f"| Octets (tshark frame.len + 4 FCS) | {probe_octets} | {tshark_octets} | {difference(probe_octets, tshark_octets)} |")
    lines.append(f"| Broadcast | {probe_broadcast} | {tshark_broadcast} | {difference(probe_broadcast, tshark_broadcast)} |")
    lines.append(f"| Multicast | {probe_multicast} | {tshark_multicast} | {difference(probe_multicast, tshark_multicast)} |")
    lines.append(f"| Undersize | {probe_undersize} | {tshark_undersize} | {difference(probe_undersize, tshark_undersize)} |")
    lines.append(f"| Oversize | {probe_oversize} | {tshark_oversize} | {difference(probe_oversize, tshark_oversize)} |")
    for column, label in label_buckets.items():
        probe = counter_delta(stats_after.get(column), stats_before.get(column))
        lines.append(f"| {label} | {probe} | {tshark_buckets[column]} | {difference(probe, tshark_buckets[column])} |")
    lines.append(f"| CRC align errors | {probe_crc} | {crc_after - crc_before} | {difference(probe_crc, crc_after - crc_before)} |")
    lines.append(f"| Drop events | {probe_drops} | {drops_note} | n/a |")
    lines.append("")
    lines.append("`ip -s link` is a stack-level sanity check and is not expected to equal the probe exactly "
                 "because it counts the local RX path with different length semantics.")
    lines.append("")
    lines.append("| IP RX metric | Before | After | Delta | Probe delta |")
    lines.append("| --- | ---: | ---: | ---: | ---: |")
    for key in ("bytes", "packets", "errors", "dropped", "missed", "multicast"):
        before = ip_before.get(key, 0)
        after = ip_after.get(key, 0)
        probe = probe_octets if key == "bytes" else probe_pkts if key == "packets" else 0
        lines.append(f"| {key} | {before} | {after} | {after - before} | {probe} |")
    lines.append("")
    lines.append(f"Fragments: {probe_fragments}; jabbers: {probe_jabbers}; collisions: {probe_collisions}. "
                 "These are expected to remain zero on this platform.")
    lines.append("")
    lines.append("## History row 10 (interval 10 s, 4 buckets)")
    lines.append("")
    lines.append("| Time | Sample indices | Interval starts (ticks) | Count |")
    lines.append("| --- | --- | --- | ---: |")
    lines.append(f"| 47 s | {', '.join(str(index) for index in indices_47)} | "
                 f"{', '.join(str(value) for value in starts_47)} | {len(history_47)} |")
    starts_60 = [history_60[index].get(3) for index in indices_60]
    lines.append(f"| 60 s | {', '.join(str(index) for index in indices_60)} | "
                 f"{', '.join(str(value) for value in starts_60)} | {len(history_60)} |")
    lines.append("")
    lines.append(f"History row 1 gained {row1_growth} sample(s) in the 60 s window; "
                 f"completed interval-start deltas: {', '.join(str(value) for value in row1_diffs[-3:]) or 'none'} ticks.")
    lines.append("")
    lines.append("## etherStats row 10 lifecycle")
    lines.append("")
    lines.append("| Step | Row present | Packets |")
    lines.append("| --- | --- | ---: |")
    lines.append(f"| Created | {bool(stats10_created)} | {stats10_created.get(5, 'n/a')} |")
    lines.append(f"| Valid baseline | {bool(stats10_baseline)} | {stats10_baseline.get(5, 'n/a')} |")
    lines.append(f"| After traffic | {bool(stats10_after)} | {stats10_after.get(5, 'n/a')} |")
    lines.append(f"| Deleted | {bool(stats10_deleted)} | {stats10_deleted.get(5, 'n/a')} |")
    lines.append("")
    lines.append(f"Packets counted after the baseline: {after_pkts}.")
    lines.append("")
    lines.append("## Artifacts")
    lines.append("")
    for path in sorted(out.rglob("*")):
        if path.is_file():
            lines.append(f"- `{path.relative_to(out)}`")
    lines.append("")

    (out / "results.md").write_text("\n".join(lines))
    print("\n".join(lines))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

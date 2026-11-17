# RMON probe validation 20261117T153302Z

- Interface: `wlan0` (ifIndex 3, MAC 70:66:55:7c:92:6f)
- Link argument: 78 Mbit/s
- Kernel: 7.2.9-arch1-1
- Net-SNMP: NET-SNMP version: 5.9.5.2
- tshark: Running as user "root" and group "root". This could be dangerous.
- Default gateway: 192.168.1.254
- Checks: 28 passed, 0 failed

## Checks

| Check | Result |
| --- | --- |
| etherStats row 1 present before and after | PASS |
| packet count within 5% of tshark | PASS |
| octet count within 5% of tshark | PASS |
| broadcast count within 5% or 10 frames of tshark | PASS |
| multicast count within 5% or 10 frames of tshark | PASS |
| undersize count within 5% of tshark | PASS |
| oversize count within 5% of tshark | PASS |
| 64 octets count within 5% of tshark | PASS |
| 65-127 octets count within 5% of tshark | PASS |
| 128-255 octets count within 5% of tshark | PASS |
| 256-511 octets count within 5% of tshark | PASS |
| 512-1023 octets count within 5% of tshark | PASS |
| 1024-1518 octets count within 5% of tshark | PASS |
| CRC delta matches sysfs rx_crc_errors | PASS |
| history row 10 has exactly four samples after 47 s | PASS |
| history row 10 sample indices increase | PASS |
| history row 10 interval starts increase | PASS |
| history row 10 still has four samples after 60 s | PASS |
| history row 10 oldest sample was evicted | PASS |
| history row 10 deleted | PASS |
| history row 1 gained samples during the 60 s window | PASS |
| history row 1 interval starts advance by about 3000 ticks | PASS |
| etherStats row 10 created | PASS |
| etherStats row 10 baseline is near zero | PASS |
| etherStats row 10 counts traffic after creation | PASS |
| etherStats row 10 deleted | PASS |
| smoke walk of etherStatsTable | PASS |
| smoke walk of historyControlTable/etherHistoryTable | PASS |

## Accuracy comparison (30 s window)

The tshark filter was `(ether dst <local MAC> or ether multicast) and not ether src <local MAC>`, which excludes locally transmitted frames so it matches the probe's `PCAP_D_IN` view.

| Metric | Probe delta | tshark reference | Difference |
| --- | ---: | ---: | ---: |
| Packets | 15085 | 15049 | +0.24% |
| Octets (tshark frame.len + 4 FCS) | 21136965 | 21104747 | +0.15% |
| Broadcast | 0 | 0 | +0.00% |
| Multicast | 119 | 111 | +7.21% |
| Undersize | 1 | 1 | +0.00% |
| Oversize | 0 | 0 | +0.00% |
| 64 octets | 6 | 6 | +0.00% |
| 65-127 octets | 341 | 328 | +3.96% |
| 128-255 octets | 112 | 110 | +1.82% |
| 256-511 octets | 50 | 50 | +0.00% |
| 512-1023 octets | 88 | 88 | +0.00% |
| 1024-1518 octets | 14487 | 14466 | +0.15% |
| CRC align errors | 0 | 0 | +0.00% |
| Drop events | 0 | not reported by tshark | n/a |

`ip -s link` is a stack-level sanity check and is not expected to equal the probe exactly because it counts the local RX path with different length semantics.

| IP RX metric | Before | After | Delta | Probe delta |
| --- | ---: | ---: | ---: | ---: |
| bytes | 639215814 | 660292525 | 21076711 | 21136965 |
| packets | 514198 | 529284 | 15086 | 15085 |
| errors | 0 | 0 | 0 | 0 |
| dropped | 201 | 201 | 0 | 0 |
| missed | 0 | 0 | 0 | 0 |
| multicast | 0 | 0 | 0 | 0 |

Fragments: 0; jabbers: 0; collisions: 0. These are expected to remain zero on this platform.

## History row 10 (interval 10 s, 4 buckets)

| Time | Sample indices | Interval starts (ticks) | Count |
| --- | --- | --- | ---: |
| 47 s | 1, 2, 3, 4 | 3129, 4135, 5136, 6137 | 4 |
| 60 s | 3, 4, 5, 6 | 5136, 6137, 7138, 8139 | 4 |

History row 1 gained 2 sample(s) in the 60 s window; completed interval-start deltas: 3004, 3003 ticks.

## etherStats row 10 lifecycle

| Step | Row present | Packets |
| --- | --- | ---: |
| Created | True | 28200 |
| Valid baseline | True | 0 |
| After traffic | True | 783 |
| Deleted | False | n/a |

Packets counted after the baseline: 783.

## Artifacts

- `code-changes.diff`
- `meta.txt`
- `probe.log`
- `raw/crc-after.txt`
- `raw/crc-before.txt`
- `raw/ethtool-after.txt`
- `raw/ethtool-before.txt`
- `raw/frames.tsv`
- `raw/git-status.txt`
- `raw/history-after-47s.txt`
- `raw/history-after-60s.txt`
- `raw/history-control-after-delete.txt`
- `raw/history-create.txt`
- `raw/history-delete.txt`
- `raw/history-row1-before.txt`
- `raw/history-valid.txt`
- `raw/ip-after.txt`
- `raw/ip-before.txt`
- `raw/smoke-history.txt`
- `raw/smoke-stats.txt`
- `raw/stats-after.txt`
- `raw/stats-before.txt`
- `raw/stats10-after.txt`
- `raw/stats10-baseline.txt`
- `raw/stats10-create.txt`
- `raw/stats10-created.txt`
- `raw/stats10-delete.txt`
- `raw/stats10-deleted.txt`
- `raw/stats10-valid.txt`
- `raw/traffic.log`
- `raw/tshark.err`
- `results.md`
- `run.log`
- `snmpd.log`

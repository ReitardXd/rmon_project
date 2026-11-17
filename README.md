# rmon-probe

A real RMON1 (RFC 2819) probe for Linux, written in C11 and exposed through
Net-SNMP as an AgentX subagent.

It captures received Ethernet frames with libpcap, maintains the RMON
Statistics and History groups, and answers ordinary `snmpwalk` / `snmpset`
requests. Nothing is simulated: every counter comes from traffic actually
received on the monitored interface.

Implemented groups:

| Group | Table | OID |
| --- | --- | --- |
| Statistics | `etherStatsTable` | `1.3.6.1.2.1.16.1.1` |
| History control | `historyControlTable` | `1.3.6.1.2.1.16.2.1` |
| History data | `etherHistoryTable` | `1.3.6.1.2.1.16.2.2` |

The Alarm, Event, Hosts, HostTopN, Matrix, Filter and Capture groups are not
implemented.

The probe was validated against a simultaneous `tshark` capture: **28/28
checks passed**, with packet counts within 0.24% and octets within 0.15%.
See [docs/results/LATEST.md](docs/results/LATEST.md).

---

## Contents

- [Why RMON](#why-rmon)
- [How it works](#how-it-works)
- [Requirements](#requirements)
- [Build](#build)
- [Quick start: classroom demo](#quick-start-classroom-demo)
- [Manual run](#manual-run)
- [Create RMON rows](#create-rmon-rows)
- [Dashboard](#dashboard)
- [Validation](#validation)
- [Counter details](#counter-details)
- [Limitations](#limitations)
- [Troubleshooting](#troubleshooting)
- [Project layout](#project-layout)
- [License](#license)

---

## Why RMON

Normal SNMP interface counters tell you how many bytes and packets crossed an
interface. They do not tell you the frame-size mix, how much was broadcast or
multicast, or what the traffic looked like over time.

RMON is designed to measure a **network segment** without installing an agent
on every host. A probe sits on one interface — a bridge, a SPAN/mirror port, or
a router port — and reports what it sees. This project implements that idea on
a single Linux interface, usually `wlan0` for a laptop demo.

---

## How it works

```
Interface (wlan0 / bridge)
        |
        v
  libpcap capture thread ──────┐
  (inbound frames only)        │
                               v
                        mutex-protected state
                               ^
        ┌──────────────────────┴──────────────────────┐
        │                                             │
  history timer thread                        SNMP / AgentX thread
  (snapshots counters,                        (registers 1.3.6.1.2.1.16,
   appends ring-buffer                         handles GET and SET)
   samples)                                           │
                                                      v
                                             snmpd master agent
                                             UDP 127.0.0.1:1161
                                                      ^
                                                      │
                                          snmpwalk / snmpset / dashboard
```

- The capture thread owns libpcap and updates the counters.
- The history timer snapshots completed intervals into fixed-size ring buffers.
- The AgentX thread serves the MIB tables.
- `snmpd` handles SNMP transport, communities and access control.
- All shared state is protected by a mutex.

Capture is inbound only (`PCAP_D_IN`). RMON measures received traffic; counting
local transmissions would include TSO super-packets and unpadded short frames
that are not on the wire.

At startup the probe creates:

- Statistics row 1.
- History row 1: 30-second interval, 120 buckets.
- History row 2: 1800-second interval, 50 buckets.

No manual `snmpset` is needed for these rows. Use index 10 or higher for rows
you create yourself.

---

## Requirements

Arch Linux packages:

```sh
sudo pacman -Syu --needed base-devel libpcap net-snmp
```

For the dashboard, validation and demo:

```sh
sudo pacman -S --needed wireshark-cli ethtool iw curl
```

Python dashboard dependencies:

```sh
python -m venv venv
venv/bin/pip install streamlit pandas
```

Tested with Streamlit 1.67 and pandas 3.1. Raw capture, the demo, and the
validation script require root. Validation also requires `tshark`.

---

## Build

```sh
make
```

The build uses `pkg-config` for Net-SNMP and libpcap, C11, and
`-Wall -Wextra -Wpedantic -Werror`.

```sh
make clean
```

---

## Quick start: classroom demo

`demo.sh` starts an isolated `snmpd` and the probe, generates real traffic,
creates and deletes a history row, opens the dashboard, prints the validation
summary, and cleans up everything it started.

```sh
make
./demo.sh wlan0 78
```

Pass an interface and an optional link speed in Mbit/s. The demo asks for sudo
once. It stops the packaged `snmpd.service` if active and restores it on exit.

Dry run:

```sh
./demo.sh wlan0 78 --dry-run
```

The demo dashboard is at <http://localhost:8501>. Rate metrics appear after the
first 5-second refresh because two samples are needed for a delta. View the
dashboard before ending the demo; cleanup stops the probe and traffic
generator.

Presenter notes and a talk track are in
[docs/PRESENTATION_GUIDE.md](docs/PRESENTATION_GUIDE.md).

---

## Manual run

Terminal 1 — isolated master agent on UDP/1161:

```sh
sudo systemctl stop snmpd.service
sudo install -d -m 0755 /var/agentx
sudo snmpd -f -Lo -C -c ./config/snmpd.conf
```

Terminal 2 — the probe:

```sh
sudo ./rmon-probe wlan0 78
```

Replace `wlan0` with the interface to monitor and `78` with its link speed in
Mbit/s. The link speed is used for history utilization. If
`/sys/class/net/<if>/speed` is readable, it takes precedence; Wi-Fi and bridges
usually do not expose it.

Then query from any terminal:

```sh
snmpwalk -v2c -c public localhost:1161 1.3.6.1.2.1.16
```

The supplied `config/snmpd.conf` binds to `127.0.0.1` only and uses the
communities `public` (read-only) and `private` (read-write). Do not expose this
configuration to an untrusted network without changing the communities and
adding SNMPv3/access controls.

---

## Create RMON rows

RMON uses a two-phase state machine:

1. Create the row with `EntryStatus = createRequest(2)`; it appears as
   `underCreation(3)`.
2. Set `EntryStatus = valid(1)`; counters start from that moment.
3. Set `EntryStatus = invalid(4)` to delete the row.

### Statistics row

```sh
ifindex=$(cat /sys/class/net/wlan0/ifindex)

snmpset -v2c -c private localhost:1161 \
  1.3.6.1.2.1.16.1.1.1.2.10 o 1.3.6.1.2.1.2.2.1.1.$ifindex \
  1.3.6.1.2.1.16.1.1.1.21.10 i 2

snmpset -v2c -c private localhost:1161 \
  1.3.6.1.2.1.16.1.1.1.21.10 i 1

snmpwalk -v2c -c public localhost:1161 1.3.6.1.2.1.16.1.1
```

### History row

This creates a ten-second interval with four buckets:

```sh
ifindex=$(cat /sys/class/net/wlan0/ifindex)

snmpset -v2c -c private localhost:1161 \
  1.3.6.1.2.1.16.2.1.1.2.10 o 1.3.6.1.2.1.2.2.1.1.$ifindex \
  1.3.6.1.2.1.16.2.1.1.3.10 i 4 \
  1.3.6.1.2.1.16.2.1.1.5.10 i 10 \
  1.3.6.1.2.1.16.2.1.1.7.10 i 2

snmpset -v2c -c private localhost:1161 \
  1.3.6.1.2.1.16.2.1.1.7.10 i 1

sleep 11
snmpwalk -v2c -c public localhost:1161 1.3.6.1.2.1.16.2.2
```

Only completed intervals are exposed. When the ring buffer is full, the newest
sample overwrites the oldest. `bucketsRequested` up to 4096 is granted exactly;
larger requests are capped at 4096 to bound memory. Reducing the requested
bucket count keeps the newest samples and removes the oldest.

---

## Dashboard

`dash.py` is a Streamlit dashboard that reads the probe with `snmpwalk`:

- totals for packets, octets, broadcast, multicast and drops;
- packets/s, Mbit/s and utilization;
- `etherStatsTable`, `historyControlTable` and `etherHistoryTable`;
- five-second auto-refresh.

The demo launches it automatically. To run it manually:

```sh
RMON_AGENT=127.0.0.1:1161 RMON_LINK_MBPS=78 \
  venv/bin/streamlit run dash.py
```

---

## Validation

`scripts/run_validation.sh` compares the probe with `tshark` over the same
30-second window, then exercises history creation, eviction and deletion plus
the `etherStats` row lifecycle. Raw output is written under
`docs/results/<timestamp>/`.

```sh
sudo bash scripts/run_validation.sh wlan0 78
```

The latest published summary is
[docs/results/LATEST.md](docs/results/LATEST.md). Individual validation runs
are ignored by Git because they contain machine-specific logs and interface
details; `LATEST.md` is kept as the published result.

The comparison uses this tshark filter, which excludes locally transmitted
frames:

```
(ether dst <local MAC> or ether multicast) and not ether src <local MAC>
```

Latest result: **28 passed, 0 failed**. Packets: 15,085 vs 15,049 (+0.24%).
Octets with FCS: 21,136,965 vs 21,104,747 (+0.15%).

---

## Counter details

- `etherStatsOctets` and the frame-size buckets include the 4-byte Ethernet FCS.
  Linux capture normally omits it, so the probe adds 4 to the captured
  original length.
- `etherStatsCRCAlignErrors` is derived from
  `/sys/class/net/<if>/statistics/rx_crc_errors`.
- `etherStatsDropEvents` comes from libpcap `pcap_stats().ps_drop`, i.e.
  capture-ring drops.
- Counters are 32-bit and wrap; delta calculations handle wrap.
- `etherStatsFragments` and `etherStatsJabbers` remain zero on Linux because
  bad-FCS frames are normally discarded before libpcap sees them, and sysfs does
  not split CRC failures into those two categories.
- `etherStatsCollisions` remains zero on Wi-Fi; collisions are an Ethernet
  shared-medium concept.

---

## Limitations

- Only the Statistics and History groups are implemented.
- On Wi-Fi the probe sees its own received traffic plus broadcast/multicast,
  not other clients' unicast traffic. Use a bridge or SPAN/mirror port for full
  segment visibility.
- Raw capture requires root privileges.
- The supplied SNMP configuration is loopback-only and uses SNMPv2c community
  strings; it is a classroom/demo configuration, not a hardened deployment.
- Fragment, jabber and collision counters are zero for the platform reasons
  above rather than being guessed.

---

## Troubleshooting

**`AgentX registration did not appear`**  
Check `/tmp/rmon-demo-probe.log` and `/tmp/rmon-demo-snmpd.log`. Ensure
`/var/agentx` exists and no other process is using UDP/1161.

**Address already in use**  
Stop the packaged service: `sudo systemctl stop snmpd.service`.

**Dashboard shows no data**  
Ensure the probe is running and `snmpwalk -v2c -c public localhost:1161
1.3.6.1.2.1.16.1.1` returns rows. Wait for the first refresh before expecting
Packets/s and Mbit/s.

**Rates are zero but totals are not**  
Wait one refresh interval. Rates need two samples.

**No packets increase**  
The probe counts received traffic on the selected interface. Generate inbound
traffic, or choose the interface carrying the traffic.

**Dashboard freezes after the demo**  
The demo cleanup stops the probe and traffic generator. Rerun the demo and
observe the dashboard before pressing Enter on the final step.

---

## Project layout

```
src/main.c            AgentX startup, capture thread, history timer
src/capture.c         libpcap, per-frame counting, CRC and drop reads
src/rmon_store.c      rows, counters, history ring buffers
src/snmp_tables.c     RMON tables, GET/SET handlers, bootstrap rows
include/              public headers
generated/            mib2c-generated RMON scaffolding (reference only)
config/snmpd.conf     isolated loopback snmpd configuration
dash.py               Streamlit dashboard
demo.sh               one-command classroom demo
scripts/              validation and result analysis
docs/                 test plan, presenter guide, published results
```

The code in `generated/` is mib2c output kept as scaffolding/reference; the
live implementation is in `src/`.

---

## License

Copyright (C) 2026 rmon-probe contributors.

This project is free software: you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation, version 3 of the License.

This project is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this project. If not, see <https://www.gnu.org/licenses/>.

The full license text is in [LICENSE](LICENSE).

# RMON Probe — Demo and Presentation Guide

This document is for the person presenting the project. It explains what the
program does, how to run the demo, what to say, and what the numbers mean.
Read sections 1–3 before starting; the rest is reference and preparation for
questions.

---

## 1. What this project is

This is a real RMON1 (Remote MONitoring, RFC 2819) probe for a Linux network
interface. It captures received Ethernet frames with libpcap, counts them, and
exposes the measurements through SNMP using Net-SNMP. Nothing is simulated: the
counters come from packets actually received on the interface.

The probe is an AgentX subagent. The normal `snmpd` process handles SNMP
requests while the probe focuses on capture and measurement.

Two RMON groups are implemented:

- **Statistics** — `etherStatsTable`, OID `1.3.6.1.2.1.16.1.1`
- **History** — `historyControlTable` + `etherHistoryTable`,
  OID `1.3.6.1.2.1.16.2`

The other RMON groups (Alarm, Event, Hosts, HostTopN, Matrix, Filter, Capture)
are not implemented. This is stated openly in the presentation.

---

## 2. Quick start

Open a terminal in the project directory:

```sh
cd /home/reitard/Projects/mp/rmon-probe
make
./demo.sh wlan0 78
```

- The demo asks for the sudo password once.
- It starts `snmpd` and the probe itself. Do not start them separately.
- It runs for roughly 60–90 seconds before the dashboard appears.
- Follow the prompts in the terminal. Each prompt waits for **Enter**.
- The dashboard opens at `http://localhost:8501`.
- Keep the terminal visible; it prints the evidence at every step.

Before the presentation, do one full rehearsal with the same command. Check
that the dashboard opens and that the live rates are non-zero in Step 6.

Optional dry run (prints commands without capturing traffic):

```sh
./demo.sh wlan0 78 --dry-run
```

---

## 3. How to run the presentation

### Before the teacher arrives

1. Connect to the network used during the rehearsal.
2. Open one terminal in the project directory.
3. Run `make` once.
4. Keep `docs/results/LATEST.md` open in a second tab as a backup.
5. Do not run `sudo ./rmon-probe` manually; the demo does it.

### During the demo

The script prints a numbered step and then waits. Suggested actions:

| Step | What appears | What to do / say |
| --- | --- | --- |
| 0 | Setup messages | “The script starts an isolated `snmpd` and the probe.” |
| 1 | `etherStatsTable` row 1 | “This is the live table the probe registered over AgentX.” |
| 2 | 10 lines of packets and Mbit/s | “The demo is generating real traffic; these are receive-side counters.” |
| 3 | Frame-size breakdown | “Most traffic is large data frames; small frames are control traffic.” |
| 4 | History samples and eviction | “Four 10-second buckets; the oldest is overwritten when full.” |
| 5 | `history row 10 deleted` | “Deleting an RMON row is part of the standard state machine.” |
| 6 | Dashboard URL | Open `http://localhost:8501`. Wait about 5 seconds for rates. |
| 7 | Validation summary | “The probe was checked against tshark: 28/28 checks passed.” |

**Important:** view the dashboard during **Step 6 before pressing Enter for
Step 7**. Pressing Enter ends the demo; cleanup then stops the probe and the
traffic generator, and the browser tab freezes on its last numbers.

### What the dashboard shows

- Total packets, octets, broadcast, multicast, and drops
- Packets/s, Mbit/s, and utilization (after the first refresh)
- `etherStatsTable`
- `historyControlTable`
- `etherHistoryTable`
- Refresh every 5 seconds by default

The rate metrics appear after the first 5-second refresh because the dashboard
needs two samples to compute a delta. Totals are correct immediately.

### If something goes wrong

- **Port 1161 is busy:** `sudo systemctl stop snmpd.service`, then rerun.
- **Dashboard does not open:** check `http://localhost:8501`; inspect
  `/tmp/rmon-demo-streamlit.log`.
- **“AgentX registration did not appear”:** inspect
  `/tmp/rmon-demo-probe.log` and `/tmp/rmon-demo-snmpd.log`.
- **Rates still show zero:** wait one refresh; make sure the traffic step has
  been running and the interface is `wlan0`.
- **Demo already ended:** the page is frozen. Rerun the demo; do not present a
  frozen page as live data.

If the demo cannot run, show `docs/results/LATEST.md` and explain that the
stored validation ran 28 checks with real traffic. Do not invent numbers.

---

## 4. Why RMON exists

Normal SNMP interface counters (the `ifTable`) only provide totals per
interface: bytes in, bytes out, packets, errors. They do not show the
frame-size mix, broadcast and multicast counts, or how traffic changed over
time.

RMON was designed to measure a **network segment** without installing software
on every host. A probe sits on one interface — a bridge, a mirror/SPAN port, or
a router port — and reports what it sees.

On a laptop, the monitored interface is usually `wlan0`. On Wi-Fi the laptop
sees its own received traffic plus broadcast and multicast traffic. It cannot
see other clients' unicast traffic unless the access point mirrors it. On a
Linux bridge, all traffic crossing the bridge is visible.

---

## 5. Architecture

```
packets on the wire
        |
        v
+-------------------+        +----------------------+
| libpcap capture   |        | SNMP / AgentX        |
| thread            |        | event loop thread    |
|                   |        |                      |
| - inbound frames  |        | - registers          |
| - counters        |<------>|   1.3.6.1.2.1.16     |
|                   | mutex  | - answers snmpwalk   |
+-------------------+        | - handles snmpset    |
        |                    +----------------------+
        |                              |
        v                              v
+-------------------+        +----------------------+
| history timer     |        | snmpd (master agent) |
| thread            |        | UDP 1161             |
| - takes snapshots |        | forwards AgentX      |
| - appends samples |        +----------------------+
+-------------------+                    ^
                                         |
                                     NMS tools
                              snmpwalk / snmpset / dashboard
```

All shared state is protected by a mutex because capture, the history timer,
and SNMP handling run in different threads.

The probe registers the RMON OID subtree `1.3.6.1.2.1.16` with `snmpd` over
AgentX. `snmpd` owns the UDP port and is the only process that speaks SNMP to
the network management station.

Key files:

| File | Role |
| --- | --- |
| `src/main.c` | starts AgentX, capture thread, history timer |
| `src/capture.c` | libpcap setup, per-packet counting, CRC/drop reads |
| `src/rmon_store.c` | rows, counters, history ring buffers, timer |
| `src/snmp_tables.c` | RMON tables, GET/SET handlers, bootstrap rows |
| `dash.py` | Streamlit dashboard that reads the probe with `snmpwalk` |
| `demo.sh` | one-command classroom demonstration |
| `scripts/run_validation.sh` | automated comparison against tshark |

---

## 6. What the Statistics group measures

For each `etherStatsEntry` row:

| RMON field | Meaning in this probe |
| --- | --- |
| `etherStatsIndex` | row index |
| `etherStatsDataSource` | interface OID being monitored |
| `etherStatsDropEvents` | drops reported by `pcap_stats()` |
| `etherStatsOctets` | received octets, including 4 FCS bytes |
| `etherStatsPkts` | received packets |
| `etherStatsBroadcastPkts` | destination `ff:ff:ff:ff:ff:ff` |
| `etherStatsMulticastPkts` | multicast destination, excluding broadcast |
| `etherStatsCRCAlignErrors` | delta from `/sys/class/net/<if>/statistics/rx_crc_errors` |
| `etherStatsUndersizePkts` | frames shorter than 64 bytes on the wire |
| `etherStatsOversizePkts` | frames longer than 1518 bytes on the wire |
| `etherStatsFragments` | zero on this platform (see limitations) |
| `etherStatsJabbers` | zero on this platform (see limitations) |
| `etherStatsCollisions` | zero on this platform (see limitations) |
| `etherStatsPkts64Octets` ... `1024to1518Octets` | frame-size distribution |
| `etherStatsOwner` | free-form owner string |
| `etherStatsStatus` | `valid(1)`, `underCreation(3)`, `invalid(4)` |

RMON includes the 4-byte Ethernet FCS in frame length. Linux capture usually
omits the FCS, so the probe adds 4 to the captured original length. This is why
the validation compares the probe with `tshark frame.len + 4`.

The size buckets are mutually exclusive and cumulative for the row's lifetime,
not per-second buckets.

---

## 7. What History does

A history control row defines:

- the interface to watch,
- how many buckets to keep,
- the interval length in seconds,
- owner and status.

Every completed interval becomes one read-only `etherHistoryEntry` containing
the same counters for that interval plus utilization. The interval still being
collected is deliberately not exposed, so a manager never reads a half-finished
sample.

The history buffer is a ring:

1. samples are appended until the granted bucket count is reached;
2. once full, the newest sample overwrites the oldest;
3. sample indices keep increasing while the row still holds N samples.

The demo creates a row with a 10-second interval and four buckets. After about
47 seconds it shows samples 1–4. After 60 seconds it shows samples 3–6: still
four samples, but the two oldest were evicted.

Two history rows are created automatically at startup:

- row 1: 30-second interval, 120 buckets (one hour of history)
- row 2: 1800-second interval, 50 buckets

Rows created manually use index 10 or higher.

---

## 8. How RMON row creation and deletion work

RMON tables use a two-phase state machine:

1. Set `EntryStatus = createRequest(2)`. The row appears as
   `underCreation(3)`.
2. Set `EntryStatus = valid(1)`. The row becomes active and its counters start
   from that moment.
3. Set `EntryStatus = invalid(4)` to delete it.

This is why the demo uses two `snmpset` commands for each new row. It is part of
the standard, not an implementation quirk.

---

## 9. Proof that the numbers are real

`scripts/run_validation.sh` captures the same traffic independently with
`tshark` for 30 seconds and compares the deltas. The filter excludes locally
transmitted frames, matching the probe's inbound view. The latest run passed
**28 of 28 checks**.

| Metric | Probe | tshark | Difference |
| --- | ---: | ---: | ---: |
| Packets | 15,085 | 15,049 | +0.24% |
| Octets (with FCS) | 21,136,965 | 21,104,747 | +0.15% |
| Broadcast | 0 | 0 | 0% |
| Multicast | 119 | 111 | +8 frames |
| Undersize | 1 | 1 | 0% |
| Oversize | 0 | 0 | 0% |
| 64-byte frames | 6 | 6 | 0% |
| 65–127 bytes | 341 | 328 | +3.96% |
| 128–255 bytes | 112 | 110 | +1.82% |
| 256–511 bytes | 50 | 50 | 0% |
| 512–1023 bytes | 88 | 88 | 0% |
| 1024–1518 bytes | 14,487 | 14,466 | +0.15% |

The small differences are capture-window edges. Multicast is only 111 frames in
the reference, so an 8-frame difference looks large as a percentage; the check
allows 10 frames for small counts.

`ip -s link` is recorded as a sanity check but is not expected to match exactly:
it reports the local IP stack's bytes and packets with different length and
drop semantics.

Raw evidence: `docs/results/20261117T153302Z/`
Summary: `docs/results/LATEST.md`

---

## 10. Honest limitations

State these plainly; they make the project more credible.

- Only the Statistics and History groups are implemented. Alarm/Event/traps,
  Hosts/HostTopN, Matrix, Filter, and Capture are not.
- On Wi-Fi the laptop sees its own RX plus broadcast/multicast, not other
  clients' unicast. A bridge or SPAN port is needed for full segment visibility.
- `etherStatsFragments` and `etherStatsJabbers` stay zero because bad-FCS frames
  are discarded before libpcap sees them, and sysfs does not split CRC failures
  into fragments and jabbers. The probe reports zero rather than inventing data.
- `etherStatsCollisions` stays zero on Wi-Fi; collisions are an Ethernet
  shared-medium concept.
- Counters are 32-bit and wrap after about 4.29 billion; delta calculations
  handle wrap.
- `etherStatsCRCAlignErrors` comes from sysfs, not per-frame inspection.
- Drop events come from libpcap `ps_drop`, so they measure capture-ring drops,
  not every drop in the network stack.
- Raw capture requires root privileges.

---

## 11. A 60-second talk track

> RMON is a standard SNMP MIB for measuring a network segment. Instead of
> installing an agent on every host, a probe sits on one interface and reports
> what it sees.
>
> This project is a real probe written in C. libpcap captures received Ethernet
> frames on `wlan0` and counts packets, octets, broadcasts, multicasts, CRC
> errors and frame-size buckets. A separate thread samples those counters into
> a history ring buffer.
>
> The measurements are exposed through Net-SNMP as an AgentX subagent under OID
> `1.3.6.1.2.1.16`. The normal `snmpd` process speaks SNMP; the probe registers
> the RMON tables with it.
>
> The demo shows live counters, the frame-size distribution, and a 10-second
> history row. When the history ring is full, the oldest sample is overwritten,
> which is why the sample indices keep increasing while the row still holds four
> samples.
>
> The numbers are not simulated. We compared a 30-second window with tshark:
> packets were within 0.24% and octets within 0.15%. The full validation passed
> all 28 checks.

---

## 12. Likely questions and short answers

**Is this simulated or read from a file?**
No. It opens the interface with libpcap and counts frames as they arrive. The
validation captures the same traffic independently with tshark and compares.

**Why use AgentX instead of a full SNMP agent?**
`snmpd` already handles SNMP versions, communities, transport, and access
control. The subagent only registers the RMON subtree and supplies data. This
is the normal Net-SNMP extension model.

**Why inbound only?**
RMON measures traffic received on a segment. Counting local transmissions would
include TSO super-packets and unpadded short frames that are not on the wire.

**Why do octets not match `ip -s link` exactly?**
RMON includes the 4-byte FCS; the Linux IP stack counts bytes after driver
processing, with different padding and drop rules. The tshark comparison is the
meaningful check.

**Why is multicast slightly over 5%?**
Only about 111 multicast frames were in the reference window. An 8-frame
difference at the window edges is 7.2%, so the check allows 10 frames for small
counts. Packet and octet totals are within 0.24% and 0.15%.

**Why are fragments and jabbers zero?**
Bad-FCS frames are normally discarded before libpcap sees them, and sysfs does
not distinguish fragments from jabbers. Reporting zero is honest.

**What happens when the history ring fills?**
The next sample overwrites the oldest. The buffer size stays fixed and sample
indices keep increasing. The demo shows samples 1–4, then 3–6.

**Why is the current interval not visible?**
Only completed intervals are published, so a manager never reads a partially
filled bucket.

**Can this monitor a switch port?**
Yes, if the port is mirrored/SPAN or the interface is a Linux bridge. Without
mirroring, Wi-Fi cannot see other clients' unicast traffic.

**What is not implemented?**
Alarm/Event/traps, Hosts/HostTopN, Matrix, Filter, and Capture.

**Why does it need root?**
Raw packet capture needs `CAP_NET_RAW`, which normally means root.

---

## 13. One-page cheat sheet

Commands:

```sh
cd /home/reitard/Projects/mp/rmon-probe
make
./demo.sh wlan0 78
```

Remember:

- Enter once for sudo; the demo starts and stops everything itself.
- Follow the numbered steps; press Enter to continue.
- Dashboard: `http://localhost:8501`
- Wait ~5 seconds on the dashboard for Packets/s and Mbit/s.
- View the dashboard at Step 6 **before** pressing Enter for Step 7.
- Backup evidence: `docs/results/LATEST.md`
- Expected final line: `Checks: 28 passed, 0 failed`

If asked what is real:

> The packet, octet, broadcast, multicast, CRC, size-bucket and history values
> are all derived from frames captured on the live interface; they were
> validated against a simultaneous tshark capture.

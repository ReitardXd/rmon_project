# rmon-probe

An RMON1 AgentX subagent for a real Ethernet interface. This repository
currently implements only the Statistics and History groups:

- `etherStatsTable` — `1.3.6.1.2.1.16.1.1`
- `historyControlTable` — `1.3.6.1.2.1.16.2.1`
- `etherHistoryTable` — `1.3.6.1.2.1.16.2.2`

It is C11/Linux code. The capture thread owns libpcap; the AgentX event loop
and history timer run separately. All shared probe state is mutex protected.

## Build

On Arch Linux, install the build and runtime dependencies with pacman:

```sh
sudo pacman -Syu --needed base-devel libpcap net-snmp
make
```

The optional interval-comparison test also needs Arch's `wireshark-cli`
package, which provides `tshark`:

```sh
sudo pacman -S --needed wireshark-cli
```

## Run

Terminal 1 starts an isolated foreground master agent on UDP/1161. On Arch,
stop the packaged systemd service first if it is active, then ensure the
default Net-SNMP AgentX directory exists. The local Arch Net-SNMP build uses
`/var/agentx/master`, which matches `config/snmpd.conf` and the subagent.

```sh
sudo systemctl stop snmpd.service
sudo install -d -m 0755 /var/agentx
sudo snmpd -f -Lo -C -c ./config/snmpd.conf
```

Terminal 2 starts the subagent and capture thread. Replace `br0` with the
Linux bridge/interface to monitor.

```sh
sudo ./rmon-probe br0
```

`rmon-probe` accepts only the selected interface as an argument. Its data
source must be that interface's IF-MIB `ifIndex` instance.

## Create Statistics row 1

Get the interface index, then issue the RMON two-phase creation sequence.
`createRequest(2)` creates an `underCreation(3)` row; `valid(1)` begins its
counter interval at zero.

```sh
ifindex=$(cat /sys/class/net/br0/ifindex)
snmpset -v2c -c private localhost:1161 \
  1.3.6.1.2.1.16.1.1.1.2.1 o 1.3.6.1.2.1.2.2.1.1.$ifindex \
  1.3.6.1.2.1.16.1.1.1.21.1 i 2
snmpset -v2c -c private localhost:1161 \
  1.3.6.1.2.1.16.1.1.1.21.1 i 1
snmpwalk -v2c -c public localhost:1161 1.3.6.1.2.1.16.1.1
```

## Create a four-bucket, ten-second History row

History rows use the same two-phase `EntryStatus` state machine. Each complete
ten-second interval becomes one read-only `etherHistoryEntry`; the currently
collecting interval is deliberately not exposed.

```sh
ifindex=$(cat /sys/class/net/br0/ifindex)
snmpset -v2c -c private localhost:1161 \
  1.3.6.1.2.1.16.2.1.1.2.1 o 1.3.6.1.2.1.2.2.1.1.$ifindex \
  1.3.6.1.2.1.16.2.1.1.3.1 i 4 \
  1.3.6.1.2.1.16.2.1.1.5.1 i 10 \
  1.3.6.1.2.1.16.2.1.1.7.1 i 2
snmpset -v2c -c private localhost:1161 1.3.6.1.2.1.16.2.1.1.7.1 i 1
sleep 11
snmpwalk -v2c -c public localhost:1161 1.3.6.1.2.1.16.2.2
```

`historyControlBucketsGranted` is the actual ring-buffer capacity. Requests
up to 4096 are granted exactly; larger requests are granted as 4096 to bound
per-row memory. Reducing `bucketsRequested` retains the newest samples and
deletes the oldest; once full, each new sample overwrites the oldest bucket.

## Counter notes

RMON frame-size definitions include the four FCS octets. Linux packet capture
normally omits them, so the probe adds four to the captured original length
for octet and size-bucket accounting. `etherStatsCRCAlignErrors` is based on
`/sys/class/net/<if>/statistics/rx_crc_errors`; capture drops use
`pcap_stats().ps_drop`.

Linux normally discards bad-FCS frames before libpcap sees them, so standard
sysfs statistics cannot allocate CRC failures between RMON fragments and
jabbers. Those two counters remain zero rather than reporting invented values.

See [the test plan](docs/test-plan.md) for real-host validation.

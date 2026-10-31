# Statistics and History test plan

Do this on the machine that can run `snmpd` and capture traffic. Do not use
these commands as expected output: record the actual values and paste them
back for review.

1. Start `snmpd`, `rmon-probe`, and Statistics row 1 using the commands in the
   README. Confirm the registration is not shadowed by a built-in RMON module:

   ```sh
   snmpwalk -v2c -c public localhost:1161 1.3.6.1.2.1.16.1.1
   ```

2. Take a baseline from all three measurement views immediately before an
   interval. `ip` is a NIC/stack sanity check; the tshark capture is the
   closest packet-level comparator to this probe.

   ```sh
   ip -s link show dev br0
   snmpwalk -v2c -c public localhost:1161 1.3.6.1.2.1.16.1.1 > /tmp/rmon-before.txt
   sudo tshark -i br0 -n -a duration:30 -T fields -e frame.len > /tmp/frame-lens.txt
   ```

3. Generate known traffic during that same 30-second window, then repeat
   `ip -s link` and the Statistics walk. Compare:

   - `etherStatsPkts` delta with `wc -l /tmp/frame-lens.txt`;
   - `etherStatsOctets` delta with the sum of every tshark `frame.len` plus
     four FCS octets per frame;
   - broadcast, multicast, and six size buckets with tshark display filters
     applied to an equivalent capture;
   - `etherStatsCRCAlignErrors` delta with the interface's `rx_crc_errors`;
   - `etherStatsDropEvents` with the pcap capture-drop counter, not `ip`'s
     generic RX drop field.

4. Create a history row with interval 10 and four buckets. After 45 seconds,
   walk `etherHistoryTable`. It must show four rows for that history index,
   with increasing sample indices and interval-start ticks. Wait one more
   interval and verify the oldest sample index was evicted.

Expected differences must be investigated, not ignored: GRO/LRO/VLAN
offloads, `PCAP_D_IN` support, bridge forwarding semantics, and packet timing
can make `ip -s link` differ from libpcap. If necessary, temporarily disable
offloads for a controlled comparison and restore them afterward.

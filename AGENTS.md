# rmon-probe

Goal: a real, non-simulated RMON1 (RFC 2819) probe in C for a university
Network Design and Management demo. No Python, no Streamlit, no fake data.

## Architecture
- Capture: libpcap, promiscuous mode, on a configurable interface (a Linux bridge).
- SNMP: Net-SNMP. The probe is an AgentX subagent registering OID
  1.3.6.1.2.1.16 under snmpd. Use mib2c-generated scaffolding from RMON-MIB.
- The NMS side is just net-snmp CLI tools (snmpset/snmpwalk/snmptrapd) on another laptop.

## Rules
- C11, Makefile build, warnings on (-Wall -Wextra). Linux only (Arch).
- Packet counting and SNMP handling run in separate threads; protect shared state with mutexes.
- Do not assume how Net-SNMP's built-in RMON code behaves. Read its source/docs
  and test with snmpwalk before relying on it.
- You cannot capture packets or run snmpd inside your sandbox. Never fake output.
  Write the code, then give me exact commands to run and tell me what output to paste back.
- Small commits per milestone. Update README.md with build/run steps each time.

## Milestones
1 Statistics (etherStatsTable)  2 History  3 Alarm+Event+traps  4 Hosts+HostTopN
5 (optional) Matrix, Filter, Capture

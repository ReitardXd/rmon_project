# Generated Net-SNMP table scaffolding

These files were generated on Arch with Net-SNMP 5.9.5.2. They are deliberately
kept separate from the handwritten capture, state, and AgentX code.

```sh
MIBS=+RMON-MIB mib2c -i -s -c /usr/share/snmp/mib2c.iterate.conf -S cache=0 \
  -f generated/etherStatsTable etherStatsTable
MIBS=+RMON-MIB mib2c -i -s -c /usr/share/snmp/mib2c.iterate.conf -S cache=0 \
  -f generated/historyControlTable historyControlTable
MIBS=+RMON-MIB mib2c -i -s -c /usr/share/snmp/mib2c.iterate.conf -S cache=0 \
  -f generated/etherHistoryTable etherHistoryTable
```

`src/snmp_tables.c` is the handwritten implementation of the generated
iterator/table contracts. Keeping generated templates untouched makes future
MIB regeneration reviewable and avoids mixing generated boilerplate with
probe-specific state management.

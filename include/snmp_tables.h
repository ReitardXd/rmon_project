#ifndef SNMP_TABLES_H
#define SNMP_TABLES_H

#include "rmon.h"

void rmon_register_tables(struct rmon_state *state);
void rmon_bootstrap_rows(void);

#endif

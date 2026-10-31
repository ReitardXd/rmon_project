PROGRAM := rmon-probe
SOURCES := src/main.c src/rmon_store.c src/capture.c src/snmp_tables.c
OBJECTS := $(SOURCES:src/%.c=build/%.o)

CPPFLAGS += -D_DEFAULT_SOURCE -Iinclude $(shell pkg-config --cflags netsnmp-agent libpcap)
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -pthread
LDLIBS += $(shell pkg-config --libs netsnmp-agent libpcap) -pthread

.PHONY: all clean

all: $(PROGRAM)

$(PROGRAM): $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c | build
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

build:
	mkdir -p $@

clean:
	rm -rf build $(PROGRAM)

-include $(OBJECTS:.o=.d)

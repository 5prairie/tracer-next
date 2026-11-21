# tracer-next: `make` builds everything into build/.
# `make bench CPU=n` measures the cost of an event at several ring sizes.
ifeq ($(origin CC),default)  # make's own default is cc
CC      := gcc
endif
CFLAGS  ?= -O2 -g
override CFLAGS += -std=gnu17 -Wall -Wextra -Werror

TRACE   := $(wildcard src/*.h)
DAEMON  := $(wildcard daemon/*.c)
HEADERS := $(TRACE) $(wildcard daemon/*.h)

all: build/tracerd build/demo build/bench build/disturb

# The probe uses the program's counter header, hence the writer that trace_attach.c defines.
build/tracerd: $(DAEMON) src/trace_attach.c $(HEADERS) $(wildcard ui/*)
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $(DAEMON) src/trace_attach.c -lm

# The programs that are traced: one file each, plus the tracer's cold half.
vpath %.c demo bench

build/%: %.c src/trace_attach.c $(TRACE) daemon/td_util.h
	@mkdir -p build
	$(CC) $(CFLAGS) -o $@ $< src/trace_attach.c

# The event cost at each ring size from 2^12 to 2^16 events, on CPU $(CPU) if given.
bench: build/bench
	@for events in 4096 8192 16384 32768 65536; do \
	  TRACE_RING_EVENTS=$$events ./build/bench $(CPU) || exit 1; done

clean:
	rm -rf build

.PHONY: all bench clean

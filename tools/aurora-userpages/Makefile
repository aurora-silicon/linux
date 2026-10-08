# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra
LDFLAGS += -Wl,-z,max-page-size=65536

.PHONY: all clean
all: userpages

userpages: userpages.c ../../include/uapi/linux/prctl.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $< $(LDLIBS) -o $@

clean:
	rm -f userpages

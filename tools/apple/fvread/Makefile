# SPDX-License-Identifier: GPL-2.0-only

CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -D__EXPORTED_HEADERS__ -I../../../include/uapi

all: fvreadctl

fvreadctl: fvreadctl.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

clean:
	$(RM) fvreadctl

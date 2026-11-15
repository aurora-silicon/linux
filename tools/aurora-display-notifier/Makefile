# SPDX-License-Identifier: GPL-2.0-only
PREFIX ?= /usr
LIBEXECDIR ?= $(PREFIX)/libexec
UDEVDIR ?= $(PREFIX)/lib/udev
USERUNITDIR ?= $(PREFIX)/lib/systemd/user
DATADIR ?= $(PREFIX)/share
PYTHON ?= python3

.PHONY: test install
test:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s tests -v

install:
	install -Dm755 notifier.py $(DESTDIR)$(LIBEXECDIR)/aurora-display-notifier
	install -Dm644 70-aurora-display-notifier.rules \
		$(DESTDIR)$(UDEVDIR)/rules.d/70-aurora-display-notifier.rules
	install -d $(DESTDIR)$(USERUNITDIR)
	sed 's|@LIBEXECDIR@|$(LIBEXECDIR)|g' aurora-display-notifier.service.in \
		> $(DESTDIR)$(USERUNITDIR)/aurora-display-notifier.service
	chmod 644 $(DESTDIR)$(USERUNITDIR)/aurora-display-notifier.service
	install -Dm644 org.aurorasilicon.DisplayHint.desktop \
		$(DESTDIR)$(DATADIR)/applications/org.aurorasilicon.DisplayHint.desktop

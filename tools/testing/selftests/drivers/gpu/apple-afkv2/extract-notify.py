#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""Exercise actual late-EDID publication notification guards with mock work."""
import pathlib
import re
import sys
source = pathlib.Path(sys.argv[1]).read_text()
match = re.search(r"static void dcpavserv_notify_edid\([^;]*?\)\n\{", source)
if not match:
    raise RuntimeError("EDID notification helper missing")
end, depth = match.end(), 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
prefix = r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define READ_ONCE(x) (x)
#define DRM_CONNECTOR_REGISTERED 1
struct base { int registration_state; };
struct apple_connector { bool connected;struct base base;void *dcp;int hotplug_wq; };
struct apple_dcp { struct apple_connector *connector;bool quiescing;unsigned int nr_modes;void *dev; };
struct ep { struct apple_dcp *dcp;bool wire_v2; };
struct apple_epic_service { struct ep *ep; };
static unsigned int queued;
static void *to_platform_device(void *p) { return p; }
static void schedule_work(int *w) { (*w)++;queued++; }
'''
suffix = r'''
int main(void)
{
 int device,other;struct apple_dcp dcp={.dev=&device};
 struct ep ep={.dcp=&dcp,.wire_v2=true};struct apple_epic_service service={.ep=&ep};
 struct apple_connector connector={.dcp=&device};
 dcpavserv_notify_edid(&service);assert(queued==0);
 dcp.connector=&connector;dcpavserv_notify_edid(&service);assert(queued==0);
 dcp.nr_modes=1;connector.connected=true;dcpavserv_notify_edid(&service);assert(queued==0);
 connector.base.registration_state=DRM_CONNECTOR_REGISTERED;
 dcpavserv_notify_edid(&service);assert(queued==1 && connector.hotplug_wq==1);
 dcp.quiescing=true;dcpavserv_notify_edid(&service);assert(queued==1);
 dcp.quiescing=false;connector.dcp=&other;dcpavserv_notify_edid(&service);assert(queued==1);
 connector.dcp=&device;ep.wire_v2=false;dcpavserv_notify_edid(&service);assert(queued==1);
 ep.wire_v2=true;connector.connected=false;dcpavserv_notify_edid(&service);assert(queued==1);
 puts("PASS: actual late-EDID notification queues ready connector only; legacy/quiesce/unrouted/unregistered guarded");
}
'''
pathlib.Path(sys.argv[2]).write_text(prefix + source[match.start():end] + suffix)

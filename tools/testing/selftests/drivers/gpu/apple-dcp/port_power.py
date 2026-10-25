# SPDX-License-Identifier: GPL-2.0-only
# Copyright 2026 Ryan Murray
"""CD321x power commands and their placement in cable work and removal.

Production bodies are extracted. I2C, firmware and workqueue leaves are models;
these checks do not qualify electrical wake or native suspend.
"""
from common import block, source_defines
import lifecycle
import quiesce


def helpers(root):
    core = (root / 'drivers/usb/typec/tipd/core.c').read_text()
    header = (root / 'drivers/usb/typec/tipd/tps6598x.h').read_text()
    constants = '\n'.join(d for d in source_defines(core + '\n' + header)
                          if any(d.startswith('#define ' + n + '\t') for n in
                                 ('TPS_REG_STATUS', 'TPS_SYSTEM_POWER_STATE_S0',
                                  'TPS_STATUS_PLUG_PRESENT', 'CD321X_REG_0X50',
                                  'CD321X_0X50_BIT13', 'CD321X_SYSTEM_POWER_STATE_UNPLUGGED')))
    types = block(header, 'enum cd321x_port_power {') + ';\n'
    functions = '\n'.join(block(core, 'static void ' + n + '(') for n in
                          ('cd321x_write_0x50', 'cd321x_port_sleep', 'cd321x_port_wake'))
    return constants, types, functions


def commands(root):
    constants, types, functions = helpers(root)
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
typedef uint32_t u32; typedef uint32_t __le32;
#define BIT(n) (1U << (n))
#define cpu_to_le32(v) (v)
#define dev_warn(...) ((void)0)
#define dev_warn_ratelimited(...) ((void)0)
struct tps6598x;
struct tipd_data { bool port_sleep; int (*switch_power_state)(struct tps6598x *, unsigned char); };
struct tps6598x { struct tipd_data *data; void *dev; pthread_mutex_t lock; };
'''
    prefix += constants + '\n' + types + r'''
struct cd321x { struct tps6598x tps; enum cd321x_port_power port_power; };
static char events[32]; static unsigned count;
static int read_error, write_error, switch_error, warnings;
static u32 status;
static bool locked;
static void event(char e) { assert(locked); assert(count+1<sizeof(events)); events[count++]=e; events[count]=0; }
static int tps6598x_read32(struct tps6598x *t, unsigned reg, u32 *v)
{ (void)t; assert(reg==TPS_REG_STATUS);event('R');*v=status;return read_error; }
static int tps6598x_block_write(struct tps6598x *t,unsigned reg,const void *p,size_t size)
{
 (void)t;assert(reg==CD321X_REG_0X50&&size==4);u32 v;memcpy(&v,p,4);
 assert(v==0||v==CD321X_0X50_BIT13);event(v?'B':'C');return write_error;
}
static int switch_power_state(struct tps6598x *t, unsigned char target)
{
 assert(pthread_mutex_trylock(&t->lock)==EBUSY);
 assert(target==TPS_SYSTEM_POWER_STATE_S0||target==CD321X_SYSTEM_POWER_STATE_UNPLUGGED);
 event(target==TPS_SYSTEM_POWER_STATE_S0?'W':'S');return switch_error;
}
'''
    return prefix + functions + r'''
static void reset(void) { count=0;events[0]=0;read_error=write_error=switch_error=warnings=0;status=0; }
int main(void)
{
 struct tipd_data data={.port_sleep=true,.switch_power_state=switch_power_state};
 struct cd321x c={.tps={.data=&data,.lock=PTHREAD_MUTEX_INITIALIZER}};
 assert(!pthread_mutex_lock(&c.tps.lock));locked=true;
 reset();cd321x_port_sleep(&c);assert(!strcmp(events,"RBSBC")&&c.port_power==CD321X_PORT_ASLEEP);
 reset();cd321x_port_wake(&c);assert(!strcmp(events,"W")&&c.port_power==CD321X_PORT_AWAKE);
 reset();read_error=-EIO;cd321x_port_sleep(&c);assert(!strcmp(events,"R")&&c.port_power==CD321X_PORT_UNKNOWN);
 reset();status=TPS_STATUS_PLUG_PRESENT;cd321x_port_sleep(&c);assert(!strcmp(events,"R")&&c.port_power==CD321X_PORT_UNKNOWN);
 reset();switch_error=-ETIMEDOUT;cd321x_port_sleep(&c);assert(!strcmp(events,"RBSBC")&&c.port_power==CD321X_PORT_UNKNOWN);
 reset();switch_error=-EIO;cd321x_port_wake(&c);assert(!strcmp(events,"W")&&c.port_power==CD321X_PORT_UNKNOWN);
 reset();cd321x_port_wake(&c);assert(c.port_power==CD321X_PORT_AWAKE);
 /* Undocumented register writes warn only; the state follows SSPS's result. */
 reset();write_error=-EIO;cd321x_port_sleep(&c);assert(!strcmp(events,"RBSBC")&&c.port_power==CD321X_PORT_ASLEEP);
 reset();data.port_sleep=false;c.port_power=CD321X_PORT_UNKNOWN;
 cd321x_port_sleep(&c);cd321x_port_wake(&c);assert(!count&&c.port_power==CD321X_PORT_UNKNOWN);
 locked=false;assert(!pthread_mutex_unlock(&c.tps.lock));assert(!pthread_mutex_destroy(&c.tps.lock));
 puts("PASS actual sleep/wake, status recheck, failures, retry and capability gate");
}
'''


def cable_order(root):
    unit = lifecycle.disconnect(root)
    unit = unit.replace('int main(void)', 'int __attribute__((unused)) prior_main(void)', 1)
    # A wake must precede both old-session teardown and partner registration.
    unit = unit.replace('static int typec_thunderbolt_switch_set(void *p, void *s)\n{',
                        'static int typec_thunderbolt_switch_set(void *p, void *s)\n{ assert(!port_sleep_enabled || port_wakes);', 1)
    unit = unit.replace('static void *typec_register_partner(void *p, struct typec_partner_desc *d)\n{',
                        'static void *typec_register_partner(void *p, struct typec_partner_desc *d)\n{ assert(!port_sleep_enabled || port_wakes);', 1)
    unit = unit.replace('static int port_wakes, port_sleeps;',
                        'static int port_wakes, port_sleeps;\nstatic bool port_sleep_enabled;', 1)
    return unit + r'''
int main(void)
{
 static const struct tipd_data enabled={.port_sleep=true};
 init();port.tps.data=&enabled;port_sleep_enabled=true;
 port.port_power=CD321X_PORT_ASLEEP;
 port.update_status.status=TPS_STATUS_PLUG_PRESENT;
 port.update_status.status_changed=TPS_STATUS_PLUG_PRESENT;
 cd321x_update_work(&port.update_work.work);
 assert(port_wakes==1&&!port_sleeps&&port.port_power==CD321X_PORT_AWAKE);
 /* An already awake plug does not issue another wake. */
 cd321x_update_work(&port.update_work.work);assert(port_wakes==1);
 port.update_status.status=0;port.update_status.status_changed=TPS_STATUS_PLUG_PRESENT;
 cd321x_update_work(&port.update_work.work);
 assert(!port.tps.partner&&!port.display_route_active&&port_sleeps==1);
 cd321x_update_work(&port.update_work.work);assert(port_sleeps==1);
 puts("PASS wake precedes teardown/registration, sleep follows release, repeated states skip commands");
}
'''


def removal_order(root):
    unit = quiesce.make_unit(root)
    unit = unit.replace('int main(', 'int __attribute__((unused)) prior_main(', 1)
    unit = unit.replace('struct tipd_data { bool port_sleep;',
                        'struct tipd_data { bool port_sleep; int (*switch_power_state)(struct tps6598x *, unsigned char);', 1)
    old = block(unit, 'static void cd321x_port_wake(')
    wake = block((root / 'drivers/usb/typec/tipd/core.c').read_text(), 'static void cd321x_port_wake(')
    unit = unit.replace(old, '#define TPS_SYSTEM_POWER_STATE_S0 0\n#define dev_warn_ratelimited(...) ((void)0)\n' + wake, 1)
    # Other scoped locks keep their original fixture model. The controller's
    # newly extracted wake must hold its real pthread mutex, released before SAFE.
    unit = unit.replace('#define scoped_guard(k,l) for(int once=((void)(l),1);once;once=0)',
                        '#define scoped_guard(k,l) for(struct power_guard __attribute__((cleanup(power_unlock))) g=power_lock(l);g.once;g.once=false)', 1)
    marker='static void cd321x_stop_work('
    code=r'''
static struct cd321x *power_port;
struct power_guard { bool once; struct mutex *mutex; };
static struct power_guard power_lock(void *key)
{
 struct mutex *m=power_port&&key==&power_port->tps.lock?&power_port->tps.lock:NULL;
 if(m)assert(!pthread_mutex_lock(&m->value));
 return (struct power_guard){true,m};
}
static void power_unlock(struct power_guard *g) { if(g->mutex)assert(!pthread_mutex_unlock(&g->mutex->value)); }
static int wake_error;
static int switch_power_state(struct tps6598x *t,unsigned char state)
{ assert(state==0&&pthread_mutex_trylock(&t->lock.value)==EBUSY);event('A');return wake_error; }
'''
    unit=unit.replace(marker,code+'\n'+marker,1)
    return unit+r'''
int main(void)
{
 struct cd321x c;
 for(int scenario=0;scenario<3;scenario++) {
  initial(&c);power_port=&c;data.port_sleep=true;data.switch_power_state=switch_power_state;
  assert(!pthread_mutex_init(&c.tps.lock.value,NULL));
  c.port_power=scenario==1?CD321X_PORT_AWAKE:CD321X_PORT_ASLEEP;
  wake_error=scenario==2?-EIO:0;
  tipd_remove(&c.tps);
  assert(position('W')>=0&&position('W')<position('Q')&&position('Q')<position('L')&&position('L')<position('R'));
  if(scenario==1)assert(position('A')<0);
  else assert(position('W')<position('A')&&position('A')<position('Q'));
  assert(c.port_power==(scenario==2?CD321X_PORT_UNKNOWN:CD321X_PORT_AWAKE));
  assert(!pthread_mutex_trylock(&c.tps.lock.value));assert(!pthread_mutex_unlock(&c.tps.lock.value));
  assert(!pthread_mutex_destroy(&c.tps.lock.value));
  assert(!last_error&&reset_calls==1&&unregisters==1&&!port.owner);
 }
 puts("PASS drained work, locked S0 restore, SAFE/role order and wake failure at remove");
}
'''

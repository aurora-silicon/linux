/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
/*
 * The display coprocessors' late stop on T6050, firmware 27: each DCP registers its stop, the
 * sleep checks them before SEP's request and stops them after the always-on GPU. Included
 * after the T6050 profile definition.
 */
#include <linux/module.h>

static DEFINE_MUTEX(sleep_v27_lock);
static struct {
	struct device *dev;
	const struct apple_sleep_v27_ops *ops;
	struct module *module;
	void *data;
	bool prepared, armed, stopped, retain_frontend;
} sleep_v27[SLEEP_MAX_DISPLAYS];
static bool sleep_v27_held;

static bool sleep_v27_selected_for(struct apple_sleep *s)
{
	return s && s->soc == &t6050_sleep_soc;
}

bool apple_sleep_v27_late(void)
{
	return sleep_v27_selected_for(apple_sleep);
}
EXPORT_SYMBOL_GPL(apple_sleep_v27_late);

int apple_sleep_v27_register(struct device *dev, const struct apple_sleep_v27_ops *ops,
	void *data, struct module *owner, bool retain_frontend)
{
	struct apple_sleep *s = apple_sleep;
	unsigned int i;

	if (!apple_sleep_v27_late() || !ops || !ops->arm || !ops->check ||
	    !ops->stop || !ops->disarm)
		return -EOPNOTSUPP;
	guard(mutex)(&sleep_v27_lock);
	for (i = 0; i < s->soc->display_count; i++) {
		if (s->display[i].np != dev->of_node)
			continue;
		if (sleep_v27[i].dev && (sleep_v27[i].dev != dev || sleep_v27[i].data != data))
			return -EBUSY;
		if (!sleep_v27[i].dev)
			sleep_v27[i].dev = get_device(dev);
		sleep_v27[i].ops = ops;
		sleep_v27[i].module = owner;
		sleep_v27[i].data = data;
		sleep_v27[i].retain_frontend = retain_frontend;
		return 0;
	}
	return -ENODEV;
}
EXPORT_SYMBOL_GPL(apple_sleep_v27_register);

void apple_sleep_v27_unregister(struct device *dev)
{
	unsigned int i;

	guard(mutex)(&sleep_v27_lock);
	for (i = 0; i < ARRAY_SIZE(sleep_v27); i++)
		if (sleep_v27[i].dev == dev) {
			put_device(dev);
			memset(&sleep_v27[i], 0, sizeof(sleep_v27[i]));
		}
}
EXPORT_SYMBOL_GPL(apple_sleep_v27_unregister);

void apple_sleep_v27_prepared(struct device *dev, bool prepared)
{
	unsigned int i;

	/* PM owns the state; no registry lock under a held final scope. */
	for (i = 0; i < ARRAY_SIZE(sleep_v27); i++)
		if (READ_ONCE(sleep_v27[i].dev) == dev)
			WRITE_ONCE(sleep_v27[i].prepared, prepared);
}
EXPORT_SYMBOL_GPL(apple_sleep_v27_prepared);

static void sleep_v27_scope_end(void)
{
	int i, ret;

	if (!sleep_v27_held)
		return;
	for (i = ARRAY_SIZE(sleep_v27) - 1; i >= 0; i--) {
		if (!sleep_v27[i].armed)
			continue;
		ret = sleep_v27[i].ops->disarm(sleep_v27[i].data, sleep_v27[i].stopped);
		/* A lost retained provider cannot be restarted by device resume. */
		if (ret)
			dev_err(sleep_v27[i].dev, "display state not restored after the sleep: %d; DMA pinned\n", ret);
		sleep_v27[i].armed = false;
		module_put(sleep_v27[i].module);
	}
	sleep_v27_held = false;
	mutex_unlock(&sleep_v27_lock);
}

static int sleep_v27_scope_begin(void)
{
	unsigned int i;
	int ret;

	if (!apple_sleep_v27_late())
		return 0;
	if (!mutex_trylock(&sleep_v27_lock))
		return -EBUSY;
	sleep_v27_held = true;
	for (i = 0; i < apple_sleep->soc->display_count; i++) {
		if (!sleep_v27[i].dev || !sleep_v27[i].prepared ||
		    !device_is_bound(sleep_v27[i].dev)) {
			dev_info(apple_sleep->dev, "refused: display %u: %s\n", i,
				 !sleep_v27[i].dev ? "no provider" :
				 !sleep_v27[i].prepared ? "not prepared" : "not bound");
			ret = -EBUSY;
			goto fail;
		}
		if (!try_module_get(sleep_v27[i].module)) {
			ret = -ENODEV;
			goto fail;
		}
		sleep_v27[i].stopped = false;
		ret = sleep_v27[i].ops->arm(sleep_v27[i].data);
		if (ret) {
			dev_info(apple_sleep->dev, "refused: display %u: arm %d\n", i, ret);
			module_put(sleep_v27[i].module);
			goto fail;
		}
		sleep_v27[i].armed = true;
	}
	return 0;
fail:
	sleep_v27_scope_end();
	return ret;
}

static int sleep_v27_final_check(unsigned int i)
{
	if (!sleep_v27_held || i >= ARRAY_SIZE(sleep_v27) ||
	    !sleep_v27[i].armed || !READ_ONCE(sleep_v27[i].prepared))
		return -EBUSY;
	return sleep_v27[i].ops->check(sleep_v27[i].data);
}

static bool sleep_v27_gate_prepared(const struct device_node *np)
{
	unsigned int i;

	if (!apple_sleep_v27_late() || !sleep_v27_held)
		return false;
	for (i = 0; i < ARRAY_SIZE(sleep_v27); i++)
		if (sleep_v27[i].dev && sleep_v27[i].dev->of_node == np)
			return !sleep_v27_final_check(i);
	return false;
}

static int sleep_v27_final_stop(struct apple_sleep *s)
{
	static const u8 order[] = { 0, 1, 2, 4, 3 };
	unsigned int n, i;
	int ret;

	if (!s->entry_committed || !sleep_v27_held || !irqs_disabled())
		return -EPERM;
	for (n = 0; n < ARRAY_SIZE(order); n++) {
		i = order[n];
		ret = sleep_v27_final_check(i);
		if (!ret)
			ret = sleep_v27[i].ops->stop(sleep_v27[i].data);
		if (ret)
			return ret; /* common caller panics; no rollback below SEP */
		sleep_v27[i].stopped = true;
		WRITE_ONCE(s->display[i].quiesced, true);
		/* Each set's words right after its own stop, before the next set's */
		ret = t6050_sleep_display_off_one(s, i);
		if (ret)
			return ret;
	}
	return 0;
}

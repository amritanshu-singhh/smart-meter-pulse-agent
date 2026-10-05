// SPDX-License-Identifier: GPL-2.0
/*
 * smeter_pulse.c - Smart-meter pulse counter character driver
 *
 * A smart energy meter emits one pulse (S0 / LED impulse) for every fixed
 * amount of energy (e.g. 1000 imp/kWh => 1 Wh per pulse).  This driver
 * counts those pulses, timestamps them, and exposes them to user space.
 *
 * On a PC / VM there is no real pulse input, so the "interrupt" is
 * emulated by a high-resolution timer whose period follows the simulated
 * load.  Writing 'p' to /dev/smeter0 injects a pulse by hand.  To use real
 * hardware, call smeter_record_pulse() from a GPIO IRQ handler instead.
 *
 * Kernel concepts used (LDD module):
 *   - module parameters, printk/pr_*            - char device (cdev, fops)
 *   - spinlock (IRQ-safe) + mutex               - hrtimer (top half)
 *   - workqueue (bottom half / deferred work)   - wait queue + poll
 *   - copy_to_user / copy_from_user, ioctl      - device class + sysfs
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/workqueue.h>
#include <linux/uaccess.h>
#include <linux/math64.h>
#include <linux/version.h>

#include "smeter_ioctl.h"

#define DRV_NAME	"smeter"
#define RING_SIZE	256			/* must be a power of two */
#define RING_MASK	(RING_SIZE - 1)
#define MIN_PERIOD_NS	1000000ULL		/* simulator limited to 1 kHz */
/* 3600 s/h * 1000 Wh/kWh = 3.6e6 ; scaled by 1e9 (ns) and 1e3 (mW) */
#define NS_CONST_PERIOD	3600000000000000ULL	/* 3.6e15 */
#define NS_CONST_POWER	3600000000000000000ULL	/* 3.6e18 */

static unsigned int pulses_per_kwh = 1000;
module_param(pulses_per_kwh, uint, 0444);
MODULE_PARM_DESC(pulses_per_kwh, "Meter constant in impulses per kWh (default 1000)");

static unsigned int sim_load_w = 1500;
module_param(sim_load_w, uint, 0444);
MODULE_PARM_DESC(sim_load_w, "Initial simulated load in watts (default 1500)");

static bool sim_enable = true;
module_param(sim_enable, bool, 0444);
MODULE_PARM_DESC(sim_enable, "Start the pulse simulator on load (default 1)");

struct smeter_dev {
	dev_t devt;
	struct cdev cdev;
	struct class *cls;
	struct device *dev;

	struct hrtimer timer;		/* emulated pulse interrupt (top half) */
	struct work_struct work;	/* power computation (bottom half)     */
	wait_queue_head_t wq;		/* readers wait for pulses             */

	spinlock_t lock;		/* protects everything below, IRQ-safe */
	struct mutex cfg_lock;		/* serialises configuration (ioctl)    */

	struct smeter_event ring[RING_SIZE];
	unsigned int head, tail;	/* free-running indices */
	u64 seq, total_pulses;
	u64 last_ts_ns, prev_dt_ns;
	u64 power_mw, peak_mw;
	u32 dropped;
	u32 ppk, load_w;
	bool sim;
};

static struct smeter_dev sm;

/* ---------------------------------------------------------------- core */

/* Called for every pulse, from hrtimer (hard-IRQ) or write() context. */
static void smeter_record_pulse(void)
{
	unsigned long flags;
	u64 ts = ktime_get_ns();

	spin_lock_irqsave(&sm.lock, flags);
	sm.total_pulses++;
	if (sm.head - sm.tail >= RING_SIZE) {	/* ring full: drop oldest */
		sm.tail++;
		sm.dropped++;
	}
	sm.ring[sm.head & RING_MASK] = (struct smeter_event){ ++sm.seq, ts };
	sm.head++;
	sm.prev_dt_ns = sm.last_ts_ns ? ts - sm.last_ts_ns : 0;
	sm.last_ts_ns = ts;
	spin_unlock_irqrestore(&sm.lock, flags);

	wake_up_interruptible(&sm.wq);
	schedule_work(&sm.work);		/* defer the maths */
}

/* Bottom half: instantaneous power = energy per pulse / time between pulses */
static void smeter_work_fn(struct work_struct *w)
{
	unsigned long flags;
	u64 p;

	spin_lock_irqsave(&sm.lock, flags);
	if (sm.prev_dt_ns && sm.ppk) {
		p = div64_u64(NS_CONST_POWER, (u64)sm.ppk * sm.prev_dt_ns);
		sm.power_mw = p;
		if (p > sm.peak_mw)
			sm.peak_mw = p;
	}
	spin_unlock_irqrestore(&sm.lock, flags);
}

static u64 smeter_period_ns(u32 ppk, u32 load_w)
{
	u64 ns = div64_u64(NS_CONST_PERIOD, (u64)ppk * load_w);

	return ns < MIN_PERIOD_NS ? MIN_PERIOD_NS : ns;
}

static enum hrtimer_restart smeter_timer_fn(struct hrtimer *t)
{
	unsigned long flags;
	u32 ppk, load;
	bool run;

	smeter_record_pulse();

	spin_lock_irqsave(&sm.lock, flags);
	ppk = sm.ppk;
	load = sm.load_w;
	run = sm.sim && ppk && load;
	spin_unlock_irqrestore(&sm.lock, flags);

	if (!run)
		return HRTIMER_NORESTART;
	hrtimer_forward_now(t, ns_to_ktime(smeter_period_ns(ppk, load)));
	return HRTIMER_RESTART;
}

/* Start/stop the simulator to match the configuration. cfg_lock held. */
static void smeter_sim_apply(void)
{
	unsigned long flags;
	u32 ppk, load;
	bool want;

	spin_lock_irqsave(&sm.lock, flags);
	ppk = sm.ppk;
	load = sm.load_w;
	want = sm.sim && ppk && load;
	spin_unlock_irqrestore(&sm.lock, flags);

	if (want && !hrtimer_active(&sm.timer))
		hrtimer_start(&sm.timer, ns_to_ktime(smeter_period_ns(ppk, load)),
			      HRTIMER_MODE_REL);
	else if (!want)
		hrtimer_cancel(&sm.timer);
}

static void smeter_snapshot(struct smeter_stats *st)
{
	unsigned long flags;
	u64 now = ktime_get_ns(), age, power;

	spin_lock_irqsave(&sm.lock, flags);
	age = sm.last_ts_ns ? now - sm.last_ts_ns : 0;
	power = sm.power_mw;
	/* no pulse for a long time => load is gone, do not report stale power */
	if (!sm.last_ts_ns || (age > 2 * sm.prev_dt_ns && age > 5000000000ULL))
		power = 0;
	st->total_pulses = sm.total_pulses;
	st->energy_mwh = sm.ppk ? div64_u64(sm.total_pulses * 1000000ULL, sm.ppk) : 0;
	st->power_mw = power;
	st->peak_power_mw = sm.peak_mw;
	st->pulses_per_kwh = sm.ppk;
	st->sim_load_w = sm.load_w;
	st->sim_enabled = sm.sim;
	st->dropped = sm.dropped;
	spin_unlock_irqrestore(&sm.lock, flags);
}

/* --------------------------------------------------------- file ops */

static int smeter_open(struct inode *ino, struct file *f)
{
	return nonseekable_open(ino, f);
}

static ssize_t smeter_read(struct file *f, char __user *buf, size_t len, loff_t *off)
{
	const size_t esz = sizeof(struct smeter_event);
	struct smeter_event tmp[16];
	size_t want = len / esz, done = 0;

	if (!want)
		return -EINVAL;

	while (want) {
		unsigned long flags;
		size_t n = 0;

		spin_lock_irqsave(&sm.lock, flags);
		while (n < want && n < ARRAY_SIZE(tmp) && sm.tail != sm.head)
			tmp[n++] = sm.ring[sm.tail++ & RING_MASK];
		spin_unlock_irqrestore(&sm.lock, flags);

		if (!n) {
			if (done)
				break;
			if (f->f_flags & O_NONBLOCK)
				return -EAGAIN;
			if (wait_event_interruptible(sm.wq,
					READ_ONCE(sm.tail) != READ_ONCE(sm.head)))
				return -ERESTARTSYS;
			continue;
		}
		if (copy_to_user(buf + done * esz, tmp, n * esz))
			return done ? (ssize_t)(done * esz) : -EFAULT;
		done += n;
		want -= n;
	}
	return done * esz;
}

/* Every 'p' written injects one manual pulse (useful for testing). */
static ssize_t smeter_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
	char kbuf[64];
	size_t n = min(len, sizeof(kbuf)), i;

	if (copy_from_user(kbuf, buf, n))
		return -EFAULT;
	for (i = 0; i < n; i++)
		if (kbuf[i] == 'p')
			smeter_record_pulse();
	return n;
}

static __poll_t smeter_poll(struct file *f, poll_table *wait)
{
	poll_wait(f, &sm.wq, wait);
	return (READ_ONCE(sm.tail) != READ_ONCE(sm.head)) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static long smeter_do_reset(void)
{
	unsigned long flags;

	if (mutex_lock_interruptible(&sm.cfg_lock))
		return -ERESTARTSYS;
	spin_lock_irqsave(&sm.lock, flags);
	sm.head = sm.tail = 0;
	sm.seq = sm.total_pulses = 0;
	sm.last_ts_ns = sm.prev_dt_ns = 0;
	sm.power_mw = sm.peak_mw = 0;
	sm.dropped = 0;
	spin_unlock_irqrestore(&sm.lock, flags);
	mutex_unlock(&sm.cfg_lock);
	return 0;
}

static long smeter_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	void __user *uarg = (void __user *)arg;
	struct smeter_stats st;
	unsigned long flags;
	u32 val;
	long ret = 0;

	if (_IOC_TYPE(cmd) != SMETER_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case SMETER_IOC_GET_STATS:
		smeter_snapshot(&st);
		return copy_to_user(uarg, &st, sizeof(st)) ? -EFAULT : 0;
	case SMETER_IOC_RESET:
		return smeter_do_reset();
	case SMETER_IOC_SET_LOAD:
	case SMETER_IOC_SET_PPK:
	case SMETER_IOC_SIM_ENABLE:
		break;
	default:
		return -ENOTTY;
	}

	if (get_user(val, (u32 __user *)uarg))
		return -EFAULT;
	if (mutex_lock_interruptible(&sm.cfg_lock))
		return -ERESTARTSYS;

	spin_lock_irqsave(&sm.lock, flags);
	if (cmd == SMETER_IOC_SET_LOAD) {
		if (val > 100000)
			ret = -EINVAL;
		else
			sm.load_w = val;
	} else if (cmd == SMETER_IOC_SET_PPK) {
		if (!val || val > 1000000)
			ret = -EINVAL;
		else
			sm.ppk = val;
	} else {
		sm.sim = !!val;
	}
	spin_unlock_irqrestore(&sm.lock, flags);

	if (!ret)
		smeter_sim_apply();
	mutex_unlock(&sm.cfg_lock);
	return ret;
}

static const struct file_operations smeter_fops = {
	.owner		= THIS_MODULE,
	.open		= smeter_open,
	.read		= smeter_read,
	.write		= smeter_write,
	.poll		= smeter_poll,
	.unlocked_ioctl	= smeter_ioctl,
	.compat_ioctl	= smeter_ioctl,
};

/* ------------------------------------------------------------- sysfs */
/* /sys/class/smeter/smeter0/{total_pulses,energy_mwh,power_mw,pulses_per_kwh} */

#define SMETER_ATTR(field)						\
static ssize_t field##_show(struct device *d,				\
			    struct device_attribute *a, char *buf)	\
{									\
	struct smeter_stats st;						\
	smeter_snapshot(&st);						\
	return sysfs_emit(buf, "%llu\n", (unsigned long long)st.field);	\
}									\
static DEVICE_ATTR_RO(field)

SMETER_ATTR(total_pulses);
SMETER_ATTR(energy_mwh);
SMETER_ATTR(power_mw);
SMETER_ATTR(pulses_per_kwh);

static struct attribute *smeter_attrs[] = {
	&dev_attr_total_pulses.attr,
	&dev_attr_energy_mwh.attr,
	&dev_attr_power_mw.attr,
	&dev_attr_pulses_per_kwh.attr,
	NULL,
};
ATTRIBUTE_GROUPS(smeter);

/* -------------------------------------------------------- module init */

static int __init smeter_init(void)
{
	int ret;

	if (!pulses_per_kwh || pulses_per_kwh > 1000000)
		return -EINVAL;

	spin_lock_init(&sm.lock);
	mutex_init(&sm.cfg_lock);
	init_waitqueue_head(&sm.wq);
	INIT_WORK(&sm.work, smeter_work_fn);
	sm.ppk = pulses_per_kwh;
	sm.load_w = sim_load_w;
	sm.sim = sim_enable;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
	hrtimer_setup(&sm.timer, smeter_timer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
#else
	hrtimer_init(&sm.timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	sm.timer.function = smeter_timer_fn;
#endif

	ret = alloc_chrdev_region(&sm.devt, 0, 1, DRV_NAME);
	if (ret)
		return ret;

	cdev_init(&sm.cdev, &smeter_fops);
	sm.cdev.owner = THIS_MODULE;
	ret = cdev_add(&sm.cdev, sm.devt, 1);
	if (ret)
		goto err_region;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
	sm.cls = class_create(DRV_NAME);
#else
	sm.cls = class_create(THIS_MODULE, DRV_NAME);
#endif
	if (IS_ERR(sm.cls)) {
		ret = PTR_ERR(sm.cls);
		goto err_cdev;
	}

	sm.dev = device_create_with_groups(sm.cls, NULL, sm.devt, NULL,
					   smeter_groups, SMETER_DEV_NAME);
	if (IS_ERR(sm.dev)) {
		ret = PTR_ERR(sm.dev);
		goto err_class;
	}

	mutex_lock(&sm.cfg_lock);
	smeter_sim_apply();
	mutex_unlock(&sm.cfg_lock);

	pr_info(DRV_NAME ": loaded, /dev/%s, %u imp/kWh, sim=%d load=%uW\n",
		SMETER_DEV_NAME, sm.ppk, sm.sim, sm.load_w);
	return 0;

err_class:
	class_destroy(sm.cls);
err_cdev:
	cdev_del(&sm.cdev);
err_region:
	unregister_chrdev_region(sm.devt, 1);
	return ret;
}

static void __exit smeter_exit(void)
{
	hrtimer_cancel(&sm.timer);
	cancel_work_sync(&sm.work);
	device_destroy(sm.cls, sm.devt);
	class_destroy(sm.cls);
	cdev_del(&sm.cdev);
	unregister_chrdev_region(sm.devt, 1);
	pr_info(DRV_NAME ": unloaded after %llu pulses\n", sm.total_pulses);
}

module_init(smeter_init);
module_exit(smeter_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Capstone student");
MODULE_DESCRIPTION("Smart energy meter pulse counter (simulated S0 input)");

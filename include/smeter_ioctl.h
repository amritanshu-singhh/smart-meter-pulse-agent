/* SPDX-License-Identifier: GPL-2.0 */
/*
 * smeter_ioctl.h - user/kernel ABI of the smart-meter pulse counter driver.
 * Shared by driver/smeter_pulse.c (kernel) and the C++ user-space tools.
 */
#ifndef SMETER_IOCTL_H
#define SMETER_IOCTL_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define SMETER_DEV_NAME "smeter0"

/* One metering pulse, as returned by read(2) on /dev/smeter0 */
struct smeter_event {
	__u64 seq;	/* monotonically increasing pulse number   */
	__u64 ts_ns;	/* CLOCK_MONOTONIC timestamp, nanoseconds  */
};

/* Snapshot of the driver state, returned by SMETER_IOC_GET_STATS */
struct smeter_stats {
	__u64 total_pulses;
	__u64 energy_mwh;	/* accumulated energy in milli-watt-hours */
	__u64 power_mw;		/* instantaneous power in milli-watts     */
	__u64 peak_power_mw;
	__u32 pulses_per_kwh;	/* meter constant (imp/kWh)               */
	__u32 sim_load_w;	/* simulated load in watts                */
	__u32 sim_enabled;
	__u32 dropped;		/* events lost because the ring was full  */
};

#define SMETER_IOC_MAGIC	'm'
#define SMETER_IOC_GET_STATS	_IOR(SMETER_IOC_MAGIC, 1, struct smeter_stats)
#define SMETER_IOC_SET_LOAD	_IOW(SMETER_IOC_MAGIC, 2, __u32)
#define SMETER_IOC_SET_PPK	_IOW(SMETER_IOC_MAGIC, 3, __u32)
#define SMETER_IOC_SIM_ENABLE	_IOW(SMETER_IOC_MAGIC, 4, __u32)
#define SMETER_IOC_RESET	_IO(SMETER_IOC_MAGIC, 5)

#endif /* SMETER_IOCTL_H */

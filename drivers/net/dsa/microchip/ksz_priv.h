/* SPDX-License-Identifier: GPL-2.0
 *
 * Microchip KSZ series switch common definitions
 *
 * Copyright (C) 2017-2018 Microchip Technology Inc.
 */

#ifndef __KSZ_PRIV_H
#define __KSZ_PRIV_H

#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/phy.h>
#include <linux/etherdevice.h>
#include <net/dsa.h>

#include "ksz9477_reg.h"

struct ksz_io_ops;

struct vlan_table {
	u32 table[3];
};

struct ksz_port_mib {
	u8 cnt_ptr;
	u64 *counters;
};

struct ksz_port {
	u16 member;
	u16 vid_member;
	int stp_state;
	struct phy_device phydev;

	u32 on:1;			/* port is not disabled by hardware */
	u32 phy:1;			/* port has a PHY */
	u32 fiber:1;			/* port is fiber */
	u32 sgmii:1;			/* port is SGMII */
	u32 force:1;
	u32 link_just_down:1;		/* link just goes down */

	struct ksz_port_mib mib;
};

struct ksz_device {
	struct dsa_switch *ds;
	struct ksz_platform_data *pdata;
	const char *name;

	struct mutex reg_mutex;		/* register access */
	struct mutex stats_mutex;	/* status access */
	struct mutex alu_mutex;		/* ALU access */
	struct mutex vlan_mutex;	/* vlan access */
	const struct ksz_io_ops *ops;
	const struct ksz_dev_ops *dev_ops;

	struct device *dev;

	void *priv;

	struct gpio_desc *reset_gpio;	/* Optional reset GPIO */

	/* chip specific data */
	u32 chip_id;
	int num_vlans;
	int num_alus;
	int num_statics;
	int cpu_port;			/* port connected to CPU */
	int cpu_ports;			/* port bitmap can be cpu port */
	int phy_port_cnt;
	int port_cnt;
	int reg_mib_cnt;
	int mib_cnt;
	int mib_port_cnt;
	int last_port;			/* ports after that not used */
	phy_interface_t interface;
	u32 regs_size;

	struct vlan_table *vlan_cache;

	u64 mib_value[TOTAL_SWITCH_COUNTER_NUM];

	u8 *txbuf;

	struct ksz_port *ports;
	struct timer_list mib_read_timer;
	struct work_struct mib_read;
	unsigned long mib_read_interval;
	u16 br_member;
	u16 member;
	u16 live_ports;
	u16 on_ports;			/* ports enabled by DSA */
	u16 rx_ports;
	u16 tx_ports;
	u16 mirror_rx;
	u16 mirror_tx;
	u32 features;			/* chip specific features */
	u32 overrides;			/* chip functions set by user */
	u16 host_mask;
	u16 port_mask;
};

struct ksz_io_ops {
	int (*read8)(struct ksz_device *dev, u32 reg, u8 *value);
	int (*read16)(struct ksz_device *dev, u32 reg, u16 *value);
	int (*read24)(struct ksz_device *dev, u32 reg, u32 *value);
	int (*read32)(struct ksz_device *dev, u32 reg, u32 *value);
	int (*write8)(struct ksz_device *dev, u32 reg, u8 value);
	int (*write16)(struct ksz_device *dev, u32 reg, u16 value);
	int (*write24)(struct ksz_device *dev, u32 reg, u32 value);
	int (*write32)(struct ksz_device *dev, u32 reg, u32 value);
	int (*get)(struct ksz_device *dev, u32 reg, void *data, size_t len);
	int (*set)(struct ksz_device *dev, u32 reg, void *data, size_t len);
};

struct alu_struct {
	/* entry 1 */
	u8	is_static:1;
	u8	is_src_filter:1;
	u8	is_dst_filter:1;
	u8	prio_age:3;
	u32	_reserv_0_1:23;
	u8	mstp:3;
	/* entry 2 */
	u8	is_override:1;
	u8	is_use_fid:1;
	u32	_reserv_1_1:23;
	u8	port_forward:7;
	/* entry 3 & 4*/
	u32	_reserv_2_1:9;
	u8	fid:7;
	u8	mac[ETH_ALEN];
};

struct ksz_dev_ops {
	u32 (*get_port_addr)(int port, int offset);
	void (*cfg_port_member)(struct ksz_device *dev, int port, u8 member);
	void (*flush_dyn_mac_table)(struct ksz_device *dev, int port);
	void (*port_setup)(struct ksz_device *dev, int port, bool cpu_port);
	void (*r_phy)(struct ksz_device *dev, u16 phy, u16 reg, u16 *val);
	void (*w_phy)(struct ksz_device *dev, u16 phy, u16 reg, u16 val);
	int (*r_dyn_mac_table)(struct ksz_device *dev, u16 addr, u8 *mac_addr,
			       u8 *fid, u8 *src_port, u8 *timestamp,
			       u16 *entries);
	int (*r_sta_mac_table)(struct ksz_device *dev, u16 addr,
			       struct alu_struct *alu);
	void (*w_sta_mac_table)(struct ksz_device *dev, u16 addr,
				struct alu_struct *alu);
	void (*r_mib_cnt)(struct ksz_device *dev, int port, u16 addr,
			  u64 *cnt);
	void (*r_mib_pkt)(struct ksz_device *dev, int port, u16 addr,
			  u64 *dropped, u64 *cnt);
	void (*port_init_cnt)(struct ksz_device *dev, int port);
	int (*shutdown)(struct ksz_device *dev);
	int (*detect)(struct ksz_device *dev);
	int (*init)(struct ksz_device *dev);
	void (*exit)(struct ksz_device *dev);
};

struct ksz_device *ksz_switch_alloc(struct device *base,
				    const struct ksz_io_ops *ops, void *priv);
int ksz_switch_register(struct ksz_device *dev,
			const struct ksz_dev_ops *ops);
void ksz_switch_remove(struct ksz_device *dev);

int ksz9477_switch_register(struct ksz_device *dev);

#endif

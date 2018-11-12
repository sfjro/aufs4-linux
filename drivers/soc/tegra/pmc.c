/*
 * drivers/soc/tegra/pmc.c
 *
 * Copyright (c) 2010 Google, Inc
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "tegra-pmc: " fmt

#include <linux/kernel.h>
#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_clk.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/platform_device.h>
#include <linux/pm_domain.h>
#include <linux/reboot.h>
#include <linux/reset.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include <soc/tegra/common.h>
#include <soc/tegra/fuse.h>
#include <soc/tegra/pmc.h>

#include <dt-bindings/pinctrl/pinctrl-tegra-io-pad.h>

#define PMC_CNTRL			0x0
#define  PMC_CNTRL_INTR_POLARITY	BIT(17) /* inverts INTR polarity */
#define  PMC_CNTRL_CPU_PWRREQ_OE	BIT(16) /* CPU pwr req enable */
#define  PMC_CNTRL_CPU_PWRREQ_POLARITY	BIT(15) /* CPU pwr req polarity */
#define  PMC_CNTRL_SIDE_EFFECT_LP0	BIT(14) /* LP0 when CPU pwr gated */
#define  PMC_CNTRL_SYSCLK_OE		BIT(11) /* system clock enable */
#define  PMC_CNTRL_SYSCLK_POLARITY	BIT(10) /* sys clk polarity */
#define  PMC_CNTRL_MAIN_RST		BIT(4)

#define DPD_SAMPLE			0x020
#define  DPD_SAMPLE_ENABLE		BIT(0)
#define  DPD_SAMPLE_DISABLE		(0 << 0)

#define PWRGATE_TOGGLE			0x30
#define  PWRGATE_TOGGLE_START		BIT(8)

#define REMOVE_CLAMPING			0x34

#define PWRGATE_STATUS			0x38

#define PMC_IMPL_E_33V_PWR		0x40

#define PMC_PWR_DET			0x48

#define PMC_SCRATCH0_MODE_RECOVERY	BIT(31)
#define PMC_SCRATCH0_MODE_BOOTLOADER	BIT(30)
#define PMC_SCRATCH0_MODE_RCM		BIT(1)
#define PMC_SCRATCH0_MODE_MASK		(PMC_SCRATCH0_MODE_RECOVERY | \
					 PMC_SCRATCH0_MODE_BOOTLOADER | \
					 PMC_SCRATCH0_MODE_RCM)

#define PMC_CPUPWRGOOD_TIMER		0xc8
#define PMC_CPUPWROFF_TIMER		0xcc

#define PMC_PWR_DET_VALUE		0xe4

#define PMC_SCRATCH41			0x140

#define PMC_SENSOR_CTRL			0x1b0
#define  PMC_SENSOR_CTRL_SCRATCH_WRITE	BIT(2)
#define  PMC_SENSOR_CTRL_ENABLE_RST	BIT(1)

#define PMC_RST_STATUS			0x1b4
#define  PMC_RST_STATUS_POR		0
#define  PMC_RST_STATUS_WATCHDOG	1
#define  PMC_RST_STATUS_SENSOR		2
#define  PMC_RST_STATUS_SW_MAIN		3
#define  PMC_RST_STATUS_LP0		4
#define  PMC_RST_STATUS_AOTAG		5

#define IO_DPD_REQ			0x1b8
#define  IO_DPD_REQ_CODE_IDLE		(0U << 30)
#define  IO_DPD_REQ_CODE_OFF		(1U << 30)
#define  IO_DPD_REQ_CODE_ON		(2U << 30)
#define  IO_DPD_REQ_CODE_MASK		(3U << 30)

#define IO_DPD_STATUS			0x1bc
#define IO_DPD2_REQ			0x1c0
#define IO_DPD2_STATUS			0x1c4
#define SEL_DPD_TIM			0x1c8

#define PMC_SCRATCH54			0x258
#define  PMC_SCRATCH54_DATA_SHIFT	8
#define  PMC_SCRATCH54_ADDR_SHIFT	0

#define PMC_SCRATCH55			0x25c
#define  PMC_SCRATCH55_RESET_TEGRA	BIT(31)
#define  PMC_SCRATCH55_CNTRL_ID_SHIFT	27
#define  PMC_SCRATCH55_PINMUX_SHIFT	24
#define  PMC_SCRATCH55_16BITOP		BIT(15)
#define  PMC_SCRATCH55_CHECKSUM_SHIFT	16
#define  PMC_SCRATCH55_I2CSLV1_SHIFT	0

#define GPU_RG_CNTRL			0x2d4

/* Tegra186 and later */
#define WAKE_AOWAKE_CTRL 0x4f4
#define  WAKE_AOWAKE_CTRL_INTR_POLARITY BIT(0)

struct tegra_powergate {
	struct generic_pm_domain genpd;
	struct tegra_pmc *pmc;
	unsigned int id;
	struct clk **clks;
	unsigned int num_clks;
	struct reset_control *reset;
};

struct tegra_io_pad_soc {
	enum tegra_io_pad id;
	unsigned int dpd;
	unsigned int voltage;
	const char *name;
};

struct tegra_pmc_regs {
	unsigned int scratch0;
	unsigned int dpd_req;
	unsigned int dpd_status;
	unsigned int dpd2_req;
	unsigned int dpd2_status;
};

struct tegra_pmc_soc {
	unsigned int num_powergates;
	const char *const *powergates;
	unsigned int num_cpu_powergates;
	const u8 *cpu_powergates;

	bool has_tsense_reset;
	bool has_gpu_clamps;
	bool needs_mbist_war;
	bool has_impl_33v_pwr;

	const struct tegra_io_pad_soc *io_pads;
	unsigned int num_io_pads;

	const struct pinctrl_pin_desc *pin_descs;
	unsigned int num_pin_descs;

	const struct tegra_pmc_regs *regs;
	void (*init)(struct tegra_pmc *pmc);
	void (*setup_irq_polarity)(struct tegra_pmc *pmc,
				   struct device_node *np,
				   bool invert);
};

/**
 * struct tegra_pmc - NVIDIA Tegra PMC
 * @dev: pointer to PMC device structure
 * @base: pointer to I/O remapped register region
 * @clk: pointer to pclk clock
 * @soc: pointer to SoC data structure
 * @debugfs: pointer to debugfs entry
 * @rate: currently configured rate of pclk
 * @suspend_mode: lowest suspend mode available
 * @cpu_good_time: CPU power good time (in microseconds)
 * @cpu_off_time: CPU power off time (in microsecends)
 * @core_osc_time: core power good OSC time (in microseconds)
 * @core_pmu_time: core power good PMU time (in microseconds)
 * @core_off_time: core power off time (in microseconds)
 * @corereq_high: core power request is active-high
 * @sysclkreq_high: system clock request is active-high
 * @combined_req: combined power request for CPU & core
 * @cpu_pwr_good_en: CPU power good signal is enabled
 * @lp0_vec_phys: physical base address of the LP0 warm boot code
 * @lp0_vec_size: size of the LP0 warm boot code
 * @powergates_available: Bitmap of available power gates
 * @powergates_lock: mutex for power gate register access
 */
struct tegra_pmc {
	struct device *dev;
	void __iomem *base;
	void __iomem *wake;
	void __iomem *aotag;
	void __iomem *scratch;
	struct clk *clk;
	struct dentry *debugfs;

	const struct tegra_pmc_soc *soc;

	unsigned long rate;

	enum tegra_suspend_mode suspend_mode;
	u32 cpu_good_time;
	u32 cpu_off_time;
	u32 core_osc_time;
	u32 core_pmu_time;
	u32 core_off_time;
	bool corereq_high;
	bool sysclkreq_high;
	bool combined_req;
	bool cpu_pwr_good_en;
	u32 lp0_vec_phys;
	u32 lp0_vec_size;
	DECLARE_BITMAP(powergates_available, TEGRA_POWERGATE_MAX);

	struct mutex powergates_lock;

	struct pinctrl_dev *pctl_dev;
};

static struct tegra_pmc *pmc = &(struct tegra_pmc) {
	.base = NULL,
	.suspend_mode = TEGRA_SUSPEND_NONE,
};

static inline struct tegra_powergate *
to_powergate(struct generic_pm_domain *domain)
{
	return container_of(domain, struct tegra_powergate, genpd);
}

static u32 tegra_pmc_readl(unsigned long offset)
{
	return readl(pmc->base + offset);
}

static void tegra_pmc_writel(u32 value, unsigned long offset)
{
	writel(value, pmc->base + offset);
}

static inline bool tegra_powergate_state(int id)
{
	if (id == TEGRA_POWERGATE_3D && pmc->soc->has_gpu_clamps)
		return (tegra_pmc_readl(GPU_RG_CNTRL) & 0x1) == 0;
	else
		return (tegra_pmc_readl(PWRGATE_STATUS) & BIT(id)) != 0;
}

static inline bool tegra_powergate_is_valid(int id)
{
	return (pmc->soc && pmc->soc->powergates[id]);
}

static inline bool tegra_powergate_is_available(int id)
{
	return test_bit(id, pmc->powergates_available);
}

static int tegra_powergate_lookup(struct tegra_pmc *pmc, const char *name)
{
	unsigned int i;

	if (!pmc || !pmc->soc || !name)
		return -EINVAL;

	for (i = 0; i < pmc->soc->num_powergates; i++) {
		if (!tegra_powergate_is_valid(i))
			continue;

		if (!strcmp(name, pmc->soc->powergates[i]))
			return i;
	}

	return -ENODEV;
}

/**
 * tegra_powergate_set() - set the state of a partition
 * @id: partition ID
 * @new_state: new state of the partition
 */
static int tegra_powergate_set(unsigned int id, bool new_state)
{
	bool status;
	int err;

	if (id == TEGRA_POWERGATE_3D && pmc->soc->has_gpu_clamps)
		return -EINVAL;

	mutex_lock(&pmc->powergates_lock);

	if (tegra_powergate_state(id) == new_state) {
		mutex_unlock(&pmc->powergates_lock);
		return 0;
	}

	tegra_pmc_writel(PWRGATE_TOGGLE_START | id, PWRGATE_TOGGLE);

	err = readx_poll_timeout(tegra_powergate_state, id, status,
				 status == new_state, 10, 100000);

	mutex_unlock(&pmc->powergates_lock);

	return err;
}

static int __tegra_powergate_remove_clamping(unsigned int id)
{
	u32 mask;

	mutex_lock(&pmc->powergates_lock);

	/*
	 * On Tegra124 and later, the clamps for the GPU are controlled by a
	 * separate register (with different semantics).
	 */
	if (id == TEGRA_POWERGATE_3D) {
		if (pmc->soc->has_gpu_clamps) {
			tegra_pmc_writel(0, GPU_RG_CNTRL);
			goto out;
		}
	}

	/*
	 * Tegra 2 has a bug where PCIE and VDE clamping masks are
	 * swapped relatively to the partition ids
	 */
	if (id == TEGRA_POWERGATE_VDEC)
		mask = (1 << TEGRA_POWERGATE_PCIE);
	else if (id == TEGRA_POWERGATE_PCIE)
		mask = (1 << TEGRA_POWERGATE_VDEC);
	else
		mask = (1 << id);

	tegra_pmc_writel(mask, REMOVE_CLAMPING);

out:
	mutex_unlock(&pmc->powergates_lock);

	return 0;
}

static void tegra_powergate_disable_clocks(struct tegra_powergate *pg)
{
	unsigned int i;

	for (i = 0; i < pg->num_clks; i++)
		clk_disable_unprepare(pg->clks[i]);
}

static int tegra_powergate_enable_clocks(struct tegra_powergate *pg)
{
	unsigned int i;
	int err;

	for (i = 0; i < pg->num_clks; i++) {
		err = clk_prepare_enable(pg->clks[i]);
		if (err)
			goto out;
	}

	return 0;

out:
	while (i--)
		clk_disable_unprepare(pg->clks[i]);

	return err;
}

int __weak tegra210_clk_handle_mbist_war(unsigned int id)
{
	return 0;
}

static int tegra_powergate_power_up(struct tegra_powergate *pg,
				    bool disable_clocks)
{
	int err;

	err = reset_control_assert(pg->reset);
	if (err)
		return err;

	usleep_range(10, 20);

	err = tegra_powergate_set(pg->id, true);
	if (err < 0)
		return err;

	usleep_range(10, 20);

	err = tegra_powergate_enable_clocks(pg);
	if (err)
		goto disable_clks;

	usleep_range(10, 20);

	err = __tegra_powergate_remove_clamping(pg->id);
	if (err)
		goto disable_clks;

	usleep_range(10, 20);

	err = reset_control_deassert(pg->reset);
	if (err)
		goto powergate_off;

	usleep_range(10, 20);

	if (pg->pmc->soc->needs_mbist_war)
		err = tegra210_clk_handle_mbist_war(pg->id);
	if (err)
		goto disable_clks;

	if (disable_clocks)
		tegra_powergate_disable_clocks(pg);

	return 0;

disable_clks:
	tegra_powergate_disable_clocks(pg);
	usleep_range(10, 20);

powergate_off:
	tegra_powergate_set(pg->id, false);

	return err;
}

static int tegra_powergate_power_down(struct tegra_powergate *pg)
{
	int err;

	err = tegra_powergate_enable_clocks(pg);
	if (err)
		return err;

	usleep_range(10, 20);

	err = reset_control_assert(pg->reset);
	if (err)
		goto disable_clks;

	usleep_range(10, 20);

	tegra_powergate_disable_clocks(pg);

	usleep_range(10, 20);

	err = tegra_powergate_set(pg->id, false);
	if (err)
		goto assert_resets;

	return 0;

assert_resets:
	tegra_powergate_enable_clocks(pg);
	usleep_range(10, 20);
	reset_control_deassert(pg->reset);
	usleep_range(10, 20);

disable_clks:
	tegra_powergate_disable_clocks(pg);

	return err;
}

static int tegra_genpd_power_on(struct generic_pm_domain *domain)
{
	struct tegra_powergate *pg = to_powergate(domain);
	int err;

	err = tegra_powergate_power_up(pg, true);
	if (err)
		pr_err("failed to turn on PM domain %s: %d\n", pg->genpd.name,
		       err);

	return err;
}

static int tegra_genpd_power_off(struct generic_pm_domain *domain)
{
	struct tegra_powergate *pg = to_powergate(domain);
	int err;

	err = tegra_powergate_power_down(pg);
	if (err)
		pr_err("failed to turn off PM domain %s: %d\n",
		       pg->genpd.name, err);

	return err;
}

/**
 * tegra_powergate_power_on() - power on partition
 * @id: partition ID
 */
int tegra_powergate_power_on(unsigned int id)
{
	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	return tegra_powergate_set(id, true);
}

/**
 * tegra_powergate_power_off() - power off partition
 * @id: partition ID
 */
int tegra_powergate_power_off(unsigned int id)
{
	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	return tegra_powergate_set(id, false);
}
EXPORT_SYMBOL(tegra_powergate_power_off);

/**
 * tegra_powergate_is_powered() - check if partition is powered
 * @id: partition ID
 */
int tegra_powergate_is_powered(unsigned int id)
{
	int status;

	if (!tegra_powergate_is_valid(id))
		return -EINVAL;

	mutex_lock(&pmc->powergates_lock);
	status = tegra_powergate_state(id);
	mutex_unlock(&pmc->powergates_lock);

	return status;
}

/**
 * tegra_powergate_remove_clamping() - remove power clamps for partition
 * @id: partition ID
 */
int tegra_powergate_remove_clamping(unsigned int id)
{
	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	return __tegra_powergate_remove_clamping(id);
}
EXPORT_SYMBOL(tegra_powergate_remove_clamping);

/**
 * tegra_powergate_sequence_power_up() - power up partition
 * @id: partition ID
 * @clk: clock for partition
 * @rst: reset for partition
 *
 * Must be called with clk disabled, and returns with clk enabled.
 */
int tegra_powergate_sequence_power_up(unsigned int id, struct clk *clk,
				      struct reset_control *rst)
{
	struct tegra_powergate *pg;
	int err;

	if (!tegra_powergate_is_available(id))
		return -EINVAL;

	pg = kzalloc(sizeof(*pg), GFP_KERNEL);
	if (!pg)
		return -ENOMEM;

	pg->id = id;
	pg->clks = &clk;
	pg->num_clks = 1;
	pg->reset = rst;
	pg->pmc = pmc;

	err = tegra_powergate_power_up(pg, false);
	if (err)
		pr_err("failed to turn on partition %d: %d\n", id, err);

	kfree(pg);

	return err;
}
EXPORT_SYMBOL(tegra_powergate_sequence_power_up);

#ifdef CONFIG_SMP
/**
 * tegra_get_cpu_powergate_id() - convert from CPU ID to partition ID
 * @cpuid: CPU partition ID
 *
 * Returns the partition ID corresponding to the CPU partition ID or a
 * negative error code on failure.
 */
static int tegra_get_cpu_powergate_id(unsigned int cpuid)
{
	if (pmc->soc && cpuid < pmc->soc->num_cpu_powergates)
		return pmc->soc->cpu_powergates[cpuid];

	return -EINVAL;
}

/**
 * tegra_pmc_cpu_is_powered() - check if CPU partition is powered
 * @cpuid: CPU partition ID
 */
bool tegra_pmc_cpu_is_powered(unsigned int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return false;

	return tegra_powergate_is_powered(id);
}

/**
 * tegra_pmc_cpu_power_on() - power on CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_power_on(unsigned int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	return tegra_powergate_set(id, true);
}

/**
 * tegra_pmc_cpu_remove_clamping() - remove power clamps for CPU partition
 * @cpuid: CPU partition ID
 */
int tegra_pmc_cpu_remove_clamping(unsigned int cpuid)
{
	int id;

	id = tegra_get_cpu_powergate_id(cpuid);
	if (id < 0)
		return id;

	return tegra_powergate_remove_clamping(id);
}
#endif /* CONFIG_SMP */

static int tegra_pmc_restart_notify(struct notifier_block *this,
				    unsigned long action, void *data)
{
	const char *cmd = data;
	u32 value;

	value = readl(pmc->scratch + pmc->soc->regs->scratch0);
	value &= ~PMC_SCRATCH0_MODE_MASK;

	if (cmd) {
		if (strcmp(cmd, "recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RECOVERY;

		if (strcmp(cmd, "bootloader") == 0)
			value |= PMC_SCRATCH0_MODE_BOOTLOADER;

		if (strcmp(cmd, "forced-recovery") == 0)
			value |= PMC_SCRATCH0_MODE_RCM;
	}

	writel(value, pmc->scratch + pmc->soc->regs->scratch0);

	/* reset everything but PMC_SCRATCH0 and PMC_RST_STATUS */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_MAIN_RST;
	tegra_pmc_writel(value, PMC_CNTRL);

	return NOTIFY_DONE;
}

static struct notifier_block tegra_pmc_restart_handler = {
	.notifier_call = tegra_pmc_restart_notify,
	.priority = 128,
};

static int powergate_show(struct seq_file *s, void *data)
{
	unsigned int i;
	int status;

	seq_printf(s, " powergate powered\n");
	seq_printf(s, "------------------\n");

	for (i = 0; i < pmc->soc->num_powergates; i++) {
		status = tegra_powergate_is_powered(i);
		if (status < 0)
			continue;

		seq_printf(s, " %9s %7s\n", pmc->soc->powergates[i],
			   status ? "yes" : "no");
	}

	return 0;
}

static int powergate_open(struct inode *inode, struct file *file)
{
	return single_open(file, powergate_show, inode->i_private);
}

static const struct file_operations powergate_fops = {
	.open = powergate_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int tegra_powergate_debugfs_init(void)
{
	pmc->debugfs = debugfs_create_file("powergate", S_IRUGO, NULL, NULL,
					   &powergate_fops);
	if (!pmc->debugfs)
		return -ENOMEM;

	return 0;
}

static int tegra_powergate_of_get_clks(struct tegra_powergate *pg,
				       struct device_node *np)
{
	struct clk *clk;
	unsigned int i, count;
	int err;

	count = of_clk_get_parent_count(np);
	if (count == 0)
		return -ENODEV;

	pg->clks = kcalloc(count, sizeof(clk), GFP_KERNEL);
	if (!pg->clks)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		pg->clks[i] = of_clk_get(np, i);
		if (IS_ERR(pg->clks[i])) {
			err = PTR_ERR(pg->clks[i]);
			goto err;
		}
	}

	pg->num_clks = count;

	return 0;

err:
	while (i--)
		clk_put(pg->clks[i]);

	kfree(pg->clks);

	return err;
}

static int tegra_powergate_of_get_resets(struct tegra_powergate *pg,
					 struct device_node *np, bool off)
{
	int err;

	pg->reset = of_reset_control_array_get_exclusive(np);
	if (IS_ERR(pg->reset)) {
		err = PTR_ERR(pg->reset);
		pr_err("failed to get device resets: %d\n", err);
		return err;
	}

	if (off)
		err = reset_control_assert(pg->reset);
	else
		err = reset_control_deassert(pg->reset);

	if (err)
		reset_control_put(pg->reset);

	return err;
}

static void tegra_powergate_add(struct tegra_pmc *pmc, struct device_node *np)
{
	struct tegra_powergate *pg;
	int id, err;
	bool off;

	pg = kzalloc(sizeof(*pg), GFP_KERNEL);
	if (!pg)
		return;

	id = tegra_powergate_lookup(pmc, np->name);
	if (id < 0) {
		pr_err("powergate lookup failed for %pOFn: %d\n", np, id);
		goto free_mem;
	}

	/*
	 * Clear the bit for this powergate so it cannot be managed
	 * directly via the legacy APIs for controlling powergates.
	 */
	clear_bit(id, pmc->powergates_available);

	pg->id = id;
	pg->genpd.name = np->name;
	pg->genpd.power_off = tegra_genpd_power_off;
	pg->genpd.power_on = tegra_genpd_power_on;
	pg->pmc = pmc;

	off = !tegra_powergate_is_powered(pg->id);

	err = tegra_powergate_of_get_clks(pg, np);
	if (err < 0) {
		pr_err("failed to get clocks for %pOFn: %d\n", np, err);
		goto set_available;
	}

	err = tegra_powergate_of_get_resets(pg, np, off);
	if (err < 0) {
		pr_err("failed to get resets for %pOFn: %d\n", np, err);
		goto remove_clks;
	}

	if (!IS_ENABLED(CONFIG_PM_GENERIC_DOMAINS)) {
		if (off)
			WARN_ON(tegra_powergate_power_up(pg, true));

		goto remove_resets;
	}

	/*
	 * FIXME: If XHCI is enabled for Tegra, then power-up the XUSB
	 * host and super-speed partitions. Once the XHCI driver
	 * manages the partitions itself this code can be removed. Note
	 * that we don't register these partitions with the genpd core
	 * to avoid it from powering down the partitions as they appear
	 * to be unused.
	 */
	if (IS_ENABLED(CONFIG_USB_XHCI_TEGRA) &&
	    (id == TEGRA_POWERGATE_XUSBA || id == TEGRA_POWERGATE_XUSBC)) {
		if (off)
			WARN_ON(tegra_powergate_power_up(pg, true));

		goto remove_resets;
	}

	err = pm_genpd_init(&pg->genpd, NULL, off);
	if (err < 0) {
		pr_err("failed to initialise PM domain %pOFn: %d\n", np,
		       err);
		goto remove_resets;
	}

	err = of_genpd_add_provider_simple(np, &pg->genpd);
	if (err < 0) {
		pr_err("failed to add PM domain provider for %pOFn: %d\n",
		       np, err);
		goto remove_genpd;
	}

	pr_debug("added PM domain %s\n", pg->genpd.name);

	return;

remove_genpd:
	pm_genpd_remove(&pg->genpd);

remove_resets:
	reset_control_put(pg->reset);

remove_clks:
	while (pg->num_clks--)
		clk_put(pg->clks[pg->num_clks]);

	kfree(pg->clks);

set_available:
	set_bit(id, pmc->powergates_available);

free_mem:
	kfree(pg);
}

static void tegra_powergate_init(struct tegra_pmc *pmc,
				 struct device_node *parent)
{
	struct device_node *np, *child;
	unsigned int i;

	/* Create a bitmap of the available and valid partitions */
	for (i = 0; i < pmc->soc->num_powergates; i++)
		if (pmc->soc->powergates[i])
			set_bit(i, pmc->powergates_available);

	np = of_get_child_by_name(parent, "powergates");
	if (!np)
		return;

	for_each_child_of_node(np, child)
		tegra_powergate_add(pmc, child);

	of_node_put(np);
}

static const struct tegra_io_pad_soc *
tegra_io_pad_find(struct tegra_pmc *pmc, enum tegra_io_pad id)
{
	unsigned int i;

	for (i = 0; i < pmc->soc->num_io_pads; i++)
		if (pmc->soc->io_pads[i].id == id)
			return &pmc->soc->io_pads[i];

	return NULL;
}

static int tegra_io_pad_get_dpd_register_bit(enum tegra_io_pad id,
					     unsigned long *request,
					     unsigned long *status,
					     u32 *mask)
{
	const struct tegra_io_pad_soc *pad;

	pad = tegra_io_pad_find(pmc, id);
	if (!pad) {
		pr_err("invalid I/O pad ID %u\n", id);
		return -ENOENT;
	}

	if (pad->dpd == UINT_MAX)
		return -ENOTSUPP;

	*mask = BIT(pad->dpd % 32);

	if (pad->dpd < 32) {
		*status = pmc->soc->regs->dpd_status;
		*request = pmc->soc->regs->dpd_req;
	} else {
		*status = pmc->soc->regs->dpd2_status;
		*request = pmc->soc->regs->dpd2_req;
	}

	return 0;
}

static int tegra_io_pad_prepare(enum tegra_io_pad id, unsigned long *request,
				unsigned long *status, u32 *mask)
{
	unsigned long rate, value;
	int err;

	err = tegra_io_pad_get_dpd_register_bit(id, request, status, mask);
	if (err)
		return err;

	if (pmc->clk) {
		rate = clk_get_rate(pmc->clk);
		if (!rate) {
			pr_err("failed to get clock rate\n");
			return -ENODEV;
		}

		tegra_pmc_writel(DPD_SAMPLE_ENABLE, DPD_SAMPLE);

		/* must be at least 200 ns, in APB (PCLK) clock cycles */
		value = DIV_ROUND_UP(1000000000, rate);
		value = DIV_ROUND_UP(200, value);
		tegra_pmc_writel(value, SEL_DPD_TIM);
	}

	return 0;
}

static int tegra_io_pad_poll(unsigned long offset, u32 mask,
			     u32 val, unsigned long timeout)
{
	u32 value;

	timeout = jiffies + msecs_to_jiffies(timeout);

	while (time_after(timeout, jiffies)) {
		value = tegra_pmc_readl(offset);
		if ((value & mask) == val)
			return 0;

		usleep_range(250, 1000);
	}

	return -ETIMEDOUT;
}

static void tegra_io_pad_unprepare(void)
{
	if (pmc->clk)
		tegra_pmc_writel(DPD_SAMPLE_DISABLE, DPD_SAMPLE);
}

/**
 * tegra_io_pad_power_enable() - enable power to I/O pad
 * @id: Tegra I/O pad ID for which to enable power
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int tegra_io_pad_power_enable(enum tegra_io_pad id)
{
	unsigned long request, status;
	u32 mask;
	int err;

	mutex_lock(&pmc->powergates_lock);

	err = tegra_io_pad_prepare(id, &request, &status, &mask);
	if (err < 0) {
		pr_err("failed to prepare I/O pad: %d\n", err);
		goto unlock;
	}

	tegra_pmc_writel(IO_DPD_REQ_CODE_OFF | mask, request);

	err = tegra_io_pad_poll(status, mask, 0, 250);
	if (err < 0) {
		pr_err("failed to enable I/O pad: %d\n", err);
		goto unlock;
	}

	tegra_io_pad_unprepare();

unlock:
	mutex_unlock(&pmc->powergates_lock);
	return err;
}
EXPORT_SYMBOL(tegra_io_pad_power_enable);

/**
 * tegra_io_pad_power_disable() - disable power to I/O pad
 * @id: Tegra I/O pad ID for which to disable power
 *
 * Returns: 0 on success or a negative error code on failure.
 */
int tegra_io_pad_power_disable(enum tegra_io_pad id)
{
	unsigned long request, status;
	u32 mask;
	int err;

	mutex_lock(&pmc->powergates_lock);

	err = tegra_io_pad_prepare(id, &request, &status, &mask);
	if (err < 0) {
		pr_err("failed to prepare I/O pad: %d\n", err);
		goto unlock;
	}

	tegra_pmc_writel(IO_DPD_REQ_CODE_ON | mask, request);

	err = tegra_io_pad_poll(status, mask, mask, 250);
	if (err < 0) {
		pr_err("failed to disable I/O pad: %d\n", err);
		goto unlock;
	}

	tegra_io_pad_unprepare();

unlock:
	mutex_unlock(&pmc->powergates_lock);
	return err;
}
EXPORT_SYMBOL(tegra_io_pad_power_disable);

static int tegra_io_pad_is_powered(enum tegra_io_pad id)
{
	unsigned long request, status;
	u32 mask, value;
	int err;

	err = tegra_io_pad_get_dpd_register_bit(id, &request, &status, &mask);
	if (err)
		return err;

	value = tegra_pmc_readl(status);

	return !(value & mask);
}

static int tegra_io_pad_set_voltage(enum tegra_io_pad id, int voltage)
{
	const struct tegra_io_pad_soc *pad;
	u32 value;

	pad = tegra_io_pad_find(pmc, id);
	if (!pad)
		return -ENOENT;

	if (pad->voltage == UINT_MAX)
		return -ENOTSUPP;

	mutex_lock(&pmc->powergates_lock);

	if (pmc->soc->has_impl_33v_pwr) {
		value = tegra_pmc_readl(PMC_IMPL_E_33V_PWR);

		if (voltage == TEGRA_IO_PAD_VOLTAGE_1V8)
			value &= ~BIT(pad->voltage);
		else
			value |= BIT(pad->voltage);

		tegra_pmc_writel(value, PMC_IMPL_E_33V_PWR);
	} else {
		/* write-enable PMC_PWR_DET_VALUE[pad->voltage] */
		value = tegra_pmc_readl(PMC_PWR_DET);
		value |= BIT(pad->voltage);
		tegra_pmc_writel(value, PMC_PWR_DET);

		/* update I/O voltage */
		value = tegra_pmc_readl(PMC_PWR_DET_VALUE);

		if (voltage == TEGRA_IO_PAD_VOLTAGE_1V8)
			value &= ~BIT(pad->voltage);
		else
			value |= BIT(pad->voltage);

		tegra_pmc_writel(value, PMC_PWR_DET_VALUE);
	}

	mutex_unlock(&pmc->powergates_lock);

	usleep_range(100, 250);

	return 0;
}

static int tegra_io_pad_get_voltage(enum tegra_io_pad id)
{
	const struct tegra_io_pad_soc *pad;
	u32 value;

	pad = tegra_io_pad_find(pmc, id);
	if (!pad)
		return -ENOENT;

	if (pad->voltage == UINT_MAX)
		return -ENOTSUPP;

	if (pmc->soc->has_impl_33v_pwr)
		value = tegra_pmc_readl(PMC_IMPL_E_33V_PWR);
	else
		value = tegra_pmc_readl(PMC_PWR_DET_VALUE);

	if ((value & BIT(pad->voltage)) == 0)
		return TEGRA_IO_PAD_VOLTAGE_1V8;

	return TEGRA_IO_PAD_VOLTAGE_3V3;
}

/**
 * tegra_io_rail_power_on() - enable power to I/O rail
 * @id: Tegra I/O pad ID for which to enable power
 *
 * See also: tegra_io_pad_power_enable()
 */
int tegra_io_rail_power_on(unsigned int id)
{
	return tegra_io_pad_power_enable(id);
}
EXPORT_SYMBOL(tegra_io_rail_power_on);

/**
 * tegra_io_rail_power_off() - disable power to I/O rail
 * @id: Tegra I/O pad ID for which to disable power
 *
 * See also: tegra_io_pad_power_disable()
 */
int tegra_io_rail_power_off(unsigned int id)
{
	return tegra_io_pad_power_disable(id);
}
EXPORT_SYMBOL(tegra_io_rail_power_off);

#ifdef CONFIG_PM_SLEEP
enum tegra_suspend_mode tegra_pmc_get_suspend_mode(void)
{
	return pmc->suspend_mode;
}

void tegra_pmc_set_suspend_mode(enum tegra_suspend_mode mode)
{
	if (mode < TEGRA_SUSPEND_NONE || mode >= TEGRA_MAX_SUSPEND_MODE)
		return;

	pmc->suspend_mode = mode;
}

void tegra_pmc_enter_suspend_mode(enum tegra_suspend_mode mode)
{
	unsigned long long rate = 0;
	u32 value;

	switch (mode) {
	case TEGRA_SUSPEND_LP1:
		rate = 32768;
		break;

	case TEGRA_SUSPEND_LP2:
		rate = clk_get_rate(pmc->clk);
		break;

	default:
		break;
	}

	if (WARN_ON_ONCE(rate == 0))
		rate = 100000000;

	if (rate != pmc->rate) {
		u64 ticks;

		ticks = pmc->cpu_good_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWRGOOD_TIMER);

		ticks = pmc->cpu_off_time * rate + USEC_PER_SEC - 1;
		do_div(ticks, USEC_PER_SEC);
		tegra_pmc_writel(ticks, PMC_CPUPWROFF_TIMER);

		wmb();

		pmc->rate = rate;
	}

	value = tegra_pmc_readl(PMC_CNTRL);
	value &= ~PMC_CNTRL_SIDE_EFFECT_LP0;
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}
#endif

static int tegra_pmc_parse_dt(struct tegra_pmc *pmc, struct device_node *np)
{
	u32 value, values[2];

	if (of_property_read_u32(np, "nvidia,suspend-mode", &value)) {
	} else {
		switch (value) {
		case 0:
			pmc->suspend_mode = TEGRA_SUSPEND_LP0;
			break;

		case 1:
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;
			break;

		case 2:
			pmc->suspend_mode = TEGRA_SUSPEND_LP2;
			break;

		default:
			pmc->suspend_mode = TEGRA_SUSPEND_NONE;
			break;
		}
	}

	pmc->suspend_mode = tegra_pm_validate_suspend_mode(pmc->suspend_mode);

	if (of_property_read_u32(np, "nvidia,cpu-pwr-good-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_good_time = value;

	if (of_property_read_u32(np, "nvidia,cpu-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->cpu_off_time = value;

	if (of_property_read_u32_array(np, "nvidia,core-pwr-good-time",
				       values, ARRAY_SIZE(values)))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_osc_time = values[0];
	pmc->core_pmu_time = values[1];

	if (of_property_read_u32(np, "nvidia,core-pwr-off-time", &value))
		pmc->suspend_mode = TEGRA_SUSPEND_NONE;

	pmc->core_off_time = value;

	pmc->corereq_high = of_property_read_bool(np,
				"nvidia,core-power-req-active-high");

	pmc->sysclkreq_high = of_property_read_bool(np,
				"nvidia,sys-clock-req-active-high");

	pmc->combined_req = of_property_read_bool(np,
				"nvidia,combined-power-req");

	pmc->cpu_pwr_good_en = of_property_read_bool(np,
				"nvidia,cpu-pwr-good-en");

	if (of_property_read_u32_array(np, "nvidia,lp0-vec", values,
				       ARRAY_SIZE(values)))
		if (pmc->suspend_mode == TEGRA_SUSPEND_LP0)
			pmc->suspend_mode = TEGRA_SUSPEND_LP1;

	pmc->lp0_vec_phys = values[0];
	pmc->lp0_vec_size = values[1];

	return 0;
}

static void tegra_pmc_init(struct tegra_pmc *pmc)
{
	if (pmc->soc->init)
		pmc->soc->init(pmc);
}

static void tegra_pmc_init_tsense_reset(struct tegra_pmc *pmc)
{
	static const char disabled[] = "emergency thermal reset disabled";
	u32 pmu_addr, ctrl_id, reg_addr, reg_data, pinmux;
	struct device *dev = pmc->dev;
	struct device_node *np;
	u32 value, checksum;

	if (!pmc->soc->has_tsense_reset)
		return;

	np = of_get_child_by_name(pmc->dev->of_node, "i2c-thermtrip");
	if (!np) {
		dev_warn(dev, "i2c-thermtrip node not found, %s.\n", disabled);
		return;
	}

	if (of_property_read_u32(np, "nvidia,i2c-controller-id", &ctrl_id)) {
		dev_err(dev, "I2C controller ID missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,bus-addr", &pmu_addr)) {
		dev_err(dev, "nvidia,bus-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-addr", &reg_addr)) {
		dev_err(dev, "nvidia,reg-addr missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,reg-data", &reg_data)) {
		dev_err(dev, "nvidia,reg-data missing, %s.\n", disabled);
		goto out;
	}

	if (of_property_read_u32(np, "nvidia,pinmux-id", &pinmux))
		pinmux = 0;

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_SCRATCH_WRITE;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	value = (reg_data << PMC_SCRATCH54_DATA_SHIFT) |
		(reg_addr << PMC_SCRATCH54_ADDR_SHIFT);
	tegra_pmc_writel(value, PMC_SCRATCH54);

	value = PMC_SCRATCH55_RESET_TEGRA;
	value |= ctrl_id << PMC_SCRATCH55_CNTRL_ID_SHIFT;
	value |= pinmux << PMC_SCRATCH55_PINMUX_SHIFT;
	value |= pmu_addr << PMC_SCRATCH55_I2CSLV1_SHIFT;

	/*
	 * Calculate checksum of SCRATCH54, SCRATCH55 fields. Bits 23:16 will
	 * contain the checksum and are currently zero, so they are not added.
	 */
	checksum = reg_addr + reg_data + (value & 0xff) + ((value >> 8) & 0xff)
		+ ((value >> 24) & 0xff);
	checksum &= 0xff;
	checksum = 0x100 - checksum;

	value |= checksum << PMC_SCRATCH55_CHECKSUM_SHIFT;

	tegra_pmc_writel(value, PMC_SCRATCH55);

	value = tegra_pmc_readl(PMC_SENSOR_CTRL);
	value |= PMC_SENSOR_CTRL_ENABLE_RST;
	tegra_pmc_writel(value, PMC_SENSOR_CTRL);

	dev_info(pmc->dev, "emergency thermal reset enabled\n");

out:
	of_node_put(np);
}

static int tegra_io_pad_pinctrl_get_groups_count(struct pinctrl_dev *pctl_dev)
{
	return pmc->soc->num_io_pads;
}

static const char *tegra_io_pad_pinctrl_get_group_name(
		struct pinctrl_dev *pctl, unsigned int group)
{
	return pmc->soc->io_pads[group].name;
}

static int tegra_io_pad_pinctrl_get_group_pins(struct pinctrl_dev *pctl_dev,
					       unsigned int group,
					       const unsigned int **pins,
					       unsigned int *num_pins)
{
	*pins = &pmc->soc->io_pads[group].id;
	*num_pins = 1;
	return 0;
}

static const struct pinctrl_ops tegra_io_pad_pinctrl_ops = {
	.get_groups_count = tegra_io_pad_pinctrl_get_groups_count,
	.get_group_name = tegra_io_pad_pinctrl_get_group_name,
	.get_group_pins = tegra_io_pad_pinctrl_get_group_pins,
	.dt_node_to_map = pinconf_generic_dt_node_to_map_pin,
	.dt_free_map = pinconf_generic_dt_free_map,
};

static int tegra_io_pad_pinconf_get(struct pinctrl_dev *pctl_dev,
				    unsigned int pin, unsigned long *config)
{
	const struct tegra_io_pad_soc *pad = tegra_io_pad_find(pmc, pin);
	enum pin_config_param param = pinconf_to_config_param(*config);
	int ret;
	u32 arg;

	if (!pad)
		return -EINVAL;

	switch (param) {
	case PIN_CONFIG_POWER_SOURCE:
		ret = tegra_io_pad_get_voltage(pad->id);
		if (ret < 0)
			return ret;
		arg = ret;
		break;
	case PIN_CONFIG_LOW_POWER_MODE:
		ret = tegra_io_pad_is_powered(pad->id);
		if (ret < 0)
			return ret;
		arg = !ret;
		break;
	default:
		return -EINVAL;
	}

	*config = pinconf_to_config_packed(param, arg);

	return 0;
}

static int tegra_io_pad_pinconf_set(struct pinctrl_dev *pctl_dev,
				    unsigned int pin, unsigned long *configs,
				    unsigned int num_configs)
{
	const struct tegra_io_pad_soc *pad = tegra_io_pad_find(pmc, pin);
	enum pin_config_param param;
	unsigned int i;
	int err;
	u32 arg;

	if (!pad)
		return -EINVAL;

	for (i = 0; i < num_configs; ++i) {
		param = pinconf_to_config_param(configs[i]);
		arg = pinconf_to_config_argument(configs[i]);

		switch (param) {
		case PIN_CONFIG_LOW_POWER_MODE:
			if (arg)
				err = tegra_io_pad_power_disable(pad->id);
			else
				err = tegra_io_pad_power_enable(pad->id);
			if (err)
				return err;
			break;
		case PIN_CONFIG_POWER_SOURCE:
			if (arg != TEGRA_IO_PAD_VOLTAGE_1V8 &&
			    arg != TEGRA_IO_PAD_VOLTAGE_3V3)
				return -EINVAL;
			err = tegra_io_pad_set_voltage(pad->id, arg);
			if (err)
				return err;
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static const struct pinconf_ops tegra_io_pad_pinconf_ops = {
	.pin_config_get = tegra_io_pad_pinconf_get,
	.pin_config_set = tegra_io_pad_pinconf_set,
	.is_generic = true,
};

static struct pinctrl_desc tegra_pmc_pctl_desc = {
	.pctlops = &tegra_io_pad_pinctrl_ops,
	.confops = &tegra_io_pad_pinconf_ops,
};

static int tegra_pmc_pinctrl_init(struct tegra_pmc *pmc)
{
	int err = 0;

	if (!pmc->soc->num_pin_descs)
		return 0;

	tegra_pmc_pctl_desc.name = dev_name(pmc->dev);
	tegra_pmc_pctl_desc.pins = pmc->soc->pin_descs;
	tegra_pmc_pctl_desc.npins = pmc->soc->num_pin_descs;

	pmc->pctl_dev = devm_pinctrl_register(pmc->dev, &tegra_pmc_pctl_desc,
					      pmc);
	if (IS_ERR(pmc->pctl_dev)) {
		err = PTR_ERR(pmc->pctl_dev);
		dev_err(pmc->dev, "unable to register pinctrl, %d\n", err);
	}

	return err;
}

static int tegra_pmc_probe(struct platform_device *pdev)
{
	void __iomem *base;
	struct resource *res;
	int err;

	/*
	 * Early initialisation should have configured an initial
	 * register mapping and setup the soc data pointer. If these
	 * are not valid then something went badly wrong!
	 */
	if (WARN_ON(!pmc->base || !pmc->soc))
		return -ENODEV;

	err = tegra_pmc_parse_dt(pmc, pdev->dev.of_node);
	if (err < 0)
		return err;

	/* take over the memory region from the early initialization */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "wake");
	if (res) {
		pmc->wake = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pmc->wake))
			return PTR_ERR(pmc->wake);
	} else {
		pmc->wake = base;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "aotag");
	if (res) {
		pmc->aotag = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pmc->aotag))
			return PTR_ERR(pmc->aotag);
	} else {
		pmc->aotag = base;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "scratch");
	if (res) {
		pmc->scratch = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(pmc->scratch))
			return PTR_ERR(pmc->scratch);
	} else {
		pmc->scratch = base;
	}

	pmc->clk = devm_clk_get(&pdev->dev, "pclk");
	if (IS_ERR(pmc->clk)) {
		err = PTR_ERR(pmc->clk);

		if (err != -ENOENT) {
			dev_err(&pdev->dev, "failed to get pclk: %d\n", err);
			return err;
		}

		pmc->clk = NULL;
	}

	pmc->dev = &pdev->dev;

	tegra_pmc_init(pmc);

	tegra_pmc_init_tsense_reset(pmc);

	if (IS_ENABLED(CONFIG_DEBUG_FS)) {
		err = tegra_powergate_debugfs_init();
		if (err < 0)
			return err;
	}

	err = register_restart_handler(&tegra_pmc_restart_handler);
	if (err) {
		dev_err(&pdev->dev, "unable to register restart handler, %d\n",
			err);
		goto cleanup_debugfs;
	}

	err = tegra_pmc_pinctrl_init(pmc);
	if (err)
		goto cleanup_restart_handler;

	mutex_lock(&pmc->powergates_lock);
	iounmap(pmc->base);
	pmc->base = base;
	mutex_unlock(&pmc->powergates_lock);

	return 0;

cleanup_restart_handler:
	unregister_restart_handler(&tegra_pmc_restart_handler);
cleanup_debugfs:
	debugfs_remove(pmc->debugfs);
	return err;
}

#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
static int tegra_pmc_suspend(struct device *dev)
{
	tegra_pmc_writel(virt_to_phys(tegra_resume), PMC_SCRATCH41);

	return 0;
}

static int tegra_pmc_resume(struct device *dev)
{
	tegra_pmc_writel(0x0, PMC_SCRATCH41);

	return 0;
}

static SIMPLE_DEV_PM_OPS(tegra_pmc_pm_ops, tegra_pmc_suspend, tegra_pmc_resume);

#endif

static const char * const tegra20_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
};

static const struct tegra_pmc_regs tegra20_pmc_regs = {
	.scratch0 = 0x50,
	.dpd_req = 0x1b8,
	.dpd_status = 0x1bc,
	.dpd2_req = 0x1c0,
	.dpd2_status = 0x1c4,
};

static void tegra20_pmc_init(struct tegra_pmc *pmc)
{
	u32 value;

	/* Always enable CPU power request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_CPU_PWRREQ_OE;
	tegra_pmc_writel(value, PMC_CNTRL);

	value = tegra_pmc_readl(PMC_CNTRL);

	if (pmc->sysclkreq_high)
		value &= ~PMC_CNTRL_SYSCLK_POLARITY;
	else
		value |= PMC_CNTRL_SYSCLK_POLARITY;

	/* configure the output polarity while the request is tristated */
	tegra_pmc_writel(value, PMC_CNTRL);

	/* now enable the request */
	value = tegra_pmc_readl(PMC_CNTRL);
	value |= PMC_CNTRL_SYSCLK_OE;
	tegra_pmc_writel(value, PMC_CNTRL);
}

static void tegra20_pmc_setup_irq_polarity(struct tegra_pmc *pmc,
					   struct device_node *np,
					   bool invert)
{
	u32 value;

	value = tegra_pmc_readl(PMC_CNTRL);

	if (invert)
		value |= PMC_CNTRL_INTR_POLARITY;
	else
		value &= ~PMC_CNTRL_INTR_POLARITY;

	tegra_pmc_writel(value, PMC_CNTRL);
}

static const struct tegra_pmc_soc tegra20_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra20_powergates),
	.powergates = tegra20_powergates,
	.num_cpu_powergates = 0,
	.cpu_powergates = NULL,
	.has_tsense_reset = false,
	.has_gpu_clamps = false,
	.num_io_pads = 0,
	.io_pads = NULL,
	.num_pin_descs = 0,
	.pin_descs = NULL,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const char * const tegra30_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "cpu0",
	[TEGRA_POWERGATE_3D] = "3d0",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_L2] = "l2",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_3D1] = "3d1",
};

static const u8 tegra30_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra30_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra30_powergates),
	.powergates = tegra30_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra30_cpu_powergates),
	.cpu_powergates = tegra30_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
	.has_impl_33v_pwr = false,
	.num_io_pads = 0,
	.io_pads = NULL,
	.num_pin_descs = 0,
	.pin_descs = NULL,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const char * const tegra114_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
};

static const u8 tegra114_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

static const struct tegra_pmc_soc tegra114_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra114_powergates),
	.powergates = tegra114_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra114_cpu_powergates),
	.cpu_powergates = tegra114_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = false,
	.has_impl_33v_pwr = false,
	.num_io_pads = 0,
	.io_pads = NULL,
	.num_pin_descs = 0,
	.pin_descs = NULL,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const char * const tegra124_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_VDEC] = "vdec",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_HEG] = "heg",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CELP] = "celp",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_C1NC] = "c1nc",
	[TEGRA_POWERGATE_SOR] = "sor",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
	[TEGRA_POWERGATE_VIC] = "vic",
	[TEGRA_POWERGATE_IRAM] = "iram",
};

static const u8 tegra124_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

#define TEGRA_IO_PAD(_id, _dpd, _voltage, _name)	\
	((struct tegra_io_pad_soc) {			\
		.id	= (_id),			\
		.dpd	= (_dpd),			\
		.voltage = (_voltage),			\
		.name	= (_name),			\
	})

#define TEGRA_IO_PIN_DESC(_id, _dpd, _voltage, _name)	\
	((struct pinctrl_pin_desc) {			\
		.number = (_id),			\
		.name	= (_name)			\
	})

#define TEGRA124_IO_PAD_TABLE(_pad)					\
	/* .id                          .dpd    .voltage  .name	*/	\
	_pad(TEGRA_IO_PAD_AUDIO,	17,	UINT_MAX, "audio"),	\
	_pad(TEGRA_IO_PAD_BB,		15,	UINT_MAX, "bb"),	\
	_pad(TEGRA_IO_PAD_CAM,		36,	UINT_MAX, "cam"),	\
	_pad(TEGRA_IO_PAD_COMP,		22,	UINT_MAX, "comp"),	\
	_pad(TEGRA_IO_PAD_CSIA,		0,	UINT_MAX, "csia"),	\
	_pad(TEGRA_IO_PAD_CSIB,		1,	UINT_MAX, "csb"),	\
	_pad(TEGRA_IO_PAD_CSIE,		44,	UINT_MAX, "cse"),	\
	_pad(TEGRA_IO_PAD_DSI,		2,	UINT_MAX, "dsi"),	\
	_pad(TEGRA_IO_PAD_DSIB,		39,	UINT_MAX, "dsib"),	\
	_pad(TEGRA_IO_PAD_DSIC,		40,	UINT_MAX, "dsic"),	\
	_pad(TEGRA_IO_PAD_DSID,		41,	UINT_MAX, "dsid"),	\
	_pad(TEGRA_IO_PAD_HDMI,		28,	UINT_MAX, "hdmi"),	\
	_pad(TEGRA_IO_PAD_HSIC,		19,	UINT_MAX, "hsic"),	\
	_pad(TEGRA_IO_PAD_HV,		38,	UINT_MAX, "hv"),	\
	_pad(TEGRA_IO_PAD_LVDS,		57,	UINT_MAX, "lvds"),	\
	_pad(TEGRA_IO_PAD_MIPI_BIAS,	3,	UINT_MAX, "mipi-bias"),	\
	_pad(TEGRA_IO_PAD_NAND,		13,	UINT_MAX, "nand"),	\
	_pad(TEGRA_IO_PAD_PEX_BIAS,	4,	UINT_MAX, "pex-bias"),	\
	_pad(TEGRA_IO_PAD_PEX_CLK1,	5,	UINT_MAX, "pex-clk1"),	\
	_pad(TEGRA_IO_PAD_PEX_CLK2,	6,	UINT_MAX, "pex-clk2"),	\
	_pad(TEGRA_IO_PAD_PEX_CNTRL,	32,	UINT_MAX, "pex-cntrl"),	\
	_pad(TEGRA_IO_PAD_SDMMC1,	33,	UINT_MAX, "sdmmc1"),	\
	_pad(TEGRA_IO_PAD_SDMMC3,	34,	UINT_MAX, "sdmmc3"),	\
	_pad(TEGRA_IO_PAD_SDMMC4,	35,	UINT_MAX, "sdmmc4"),	\
	_pad(TEGRA_IO_PAD_SYS_DDC,	58,	UINT_MAX, "sys_ddc"),	\
	_pad(TEGRA_IO_PAD_UART,		14,	UINT_MAX, "uart"),	\
	_pad(TEGRA_IO_PAD_USB0,		9,	UINT_MAX, "usb0"),	\
	_pad(TEGRA_IO_PAD_USB1,		10,	UINT_MAX, "usb1"),	\
	_pad(TEGRA_IO_PAD_USB2,		11,	UINT_MAX, "usb2"),	\
	_pad(TEGRA_IO_PAD_USB_BIAS,	12,	UINT_MAX, "usb_bias")

static const struct tegra_io_pad_soc tegra124_io_pads[] = {
	TEGRA124_IO_PAD_TABLE(TEGRA_IO_PAD)
};

static const struct pinctrl_pin_desc tegra124_pin_descs[] = {
	TEGRA124_IO_PAD_TABLE(TEGRA_IO_PIN_DESC)
};

static const struct tegra_pmc_soc tegra124_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra124_powergates),
	.powergates = tegra124_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra124_cpu_powergates),
	.cpu_powergates = tegra124_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = true,
	.has_impl_33v_pwr = false,
	.num_io_pads = ARRAY_SIZE(tegra124_io_pads),
	.io_pads = tegra124_io_pads,
	.num_pin_descs = ARRAY_SIZE(tegra124_pin_descs),
	.pin_descs = tegra124_pin_descs,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

static const char * const tegra210_powergates[] = {
	[TEGRA_POWERGATE_CPU] = "crail",
	[TEGRA_POWERGATE_3D] = "3d",
	[TEGRA_POWERGATE_VENC] = "venc",
	[TEGRA_POWERGATE_PCIE] = "pcie",
	[TEGRA_POWERGATE_MPE] = "mpe",
	[TEGRA_POWERGATE_SATA] = "sata",
	[TEGRA_POWERGATE_CPU1] = "cpu1",
	[TEGRA_POWERGATE_CPU2] = "cpu2",
	[TEGRA_POWERGATE_CPU3] = "cpu3",
	[TEGRA_POWERGATE_CPU0] = "cpu0",
	[TEGRA_POWERGATE_C0NC] = "c0nc",
	[TEGRA_POWERGATE_SOR] = "sor",
	[TEGRA_POWERGATE_DIS] = "dis",
	[TEGRA_POWERGATE_DISB] = "disb",
	[TEGRA_POWERGATE_XUSBA] = "xusba",
	[TEGRA_POWERGATE_XUSBB] = "xusbb",
	[TEGRA_POWERGATE_XUSBC] = "xusbc",
	[TEGRA_POWERGATE_VIC] = "vic",
	[TEGRA_POWERGATE_IRAM] = "iram",
	[TEGRA_POWERGATE_NVDEC] = "nvdec",
	[TEGRA_POWERGATE_NVJPG] = "nvjpg",
	[TEGRA_POWERGATE_AUD] = "aud",
	[TEGRA_POWERGATE_DFD] = "dfd",
	[TEGRA_POWERGATE_VE2] = "ve2",
};

static const u8 tegra210_cpu_powergates[] = {
	TEGRA_POWERGATE_CPU0,
	TEGRA_POWERGATE_CPU1,
	TEGRA_POWERGATE_CPU2,
	TEGRA_POWERGATE_CPU3,
};

#define TEGRA210_IO_PAD_TABLE(_pad)					   \
	/*   .id                        .dpd     .voltage  .name */	   \
	_pad(TEGRA_IO_PAD_AUDIO,       17,	 5,	   "audio"),	   \
	_pad(TEGRA_IO_PAD_AUDIO_HV,    61,	 18,	   "audio-hv"),	   \
	_pad(TEGRA_IO_PAD_CAM,	       36,	 10,	   "cam"),	   \
	_pad(TEGRA_IO_PAD_CSIA,	       0,	 UINT_MAX, "csia"),	   \
	_pad(TEGRA_IO_PAD_CSIB,	       1,	 UINT_MAX, "csib"),	   \
	_pad(TEGRA_IO_PAD_CSIC,	       42,	 UINT_MAX, "csic"),	   \
	_pad(TEGRA_IO_PAD_CSID,	       43,	 UINT_MAX, "csid"),	   \
	_pad(TEGRA_IO_PAD_CSIE,	       44,	 UINT_MAX, "csie"),	   \
	_pad(TEGRA_IO_PAD_CSIF,	       45,	 UINT_MAX, "csif"),	   \
	_pad(TEGRA_IO_PAD_DBG,	       25,	 19,	   "dbg"),	   \
	_pad(TEGRA_IO_PAD_DEBUG_NONAO, 26,	 UINT_MAX, "debug-nonao"), \
	_pad(TEGRA_IO_PAD_DMIC,	       50,	 20,	   "dmic"),	   \
	_pad(TEGRA_IO_PAD_DP,	       51,	 UINT_MAX, "dp"),	   \
	_pad(TEGRA_IO_PAD_DSI,	       2,	 UINT_MAX, "dsi"),	   \
	_pad(TEGRA_IO_PAD_DSIB,	       39,	 UINT_MAX, "dsib"),	   \
	_pad(TEGRA_IO_PAD_DSIC,	       40,	 UINT_MAX, "dsic"),	   \
	_pad(TEGRA_IO_PAD_DSID,	       41,	 UINT_MAX, "dsid"),	   \
	_pad(TEGRA_IO_PAD_EMMC,	       35,	 UINT_MAX, "emmc"),	   \
	_pad(TEGRA_IO_PAD_EMMC2,       37,	 UINT_MAX, "emmc2"),	   \
	_pad(TEGRA_IO_PAD_GPIO,	       27,	 21,	   "gpio"),	   \
	_pad(TEGRA_IO_PAD_HDMI,	       28,	 UINT_MAX, "hdmi"),	   \
	_pad(TEGRA_IO_PAD_HSIC,	       19,	 UINT_MAX, "hsic"),	   \
	_pad(TEGRA_IO_PAD_LVDS,	       57,	 UINT_MAX, "lvds"),	   \
	_pad(TEGRA_IO_PAD_MIPI_BIAS,   3,	 UINT_MAX, "mipi-bias"),   \
	_pad(TEGRA_IO_PAD_PEX_BIAS,    4,	 UINT_MAX, "pex-bias"),    \
	_pad(TEGRA_IO_PAD_PEX_CLK1,    5,	 UINT_MAX, "pex-clk1"),    \
	_pad(TEGRA_IO_PAD_PEX_CLK2,    6,	 UINT_MAX, "pex-clk2"),    \
	_pad(TEGRA_IO_PAD_PEX_CNTRL,   UINT_MAX, 11,	   "pex-cntrl"),   \
	_pad(TEGRA_IO_PAD_SDMMC1,      33,	 12,	   "sdmmc1"),	   \
	_pad(TEGRA_IO_PAD_SDMMC3,      34,	 13,	   "sdmmc3"),	   \
	_pad(TEGRA_IO_PAD_SPI,	       46,	 22,	   "spi"),	   \
	_pad(TEGRA_IO_PAD_SPI_HV,      47,	 23,	   "spi-hv"),	   \
	_pad(TEGRA_IO_PAD_UART,	       14,	 2,	   "uart"),	   \
	_pad(TEGRA_IO_PAD_USB0,	       9,	 UINT_MAX, "usb0"),	   \
	_pad(TEGRA_IO_PAD_USB1,	       10,	 UINT_MAX, "usb1"),	   \
	_pad(TEGRA_IO_PAD_USB2,	       11,	 UINT_MAX, "usb2"),	   \
	_pad(TEGRA_IO_PAD_USB3,	       18,	 UINT_MAX, "usb3"),	   \
	_pad(TEGRA_IO_PAD_USB_BIAS,    12,	 UINT_MAX, "usb-bias")

static const struct tegra_io_pad_soc tegra210_io_pads[] = {
	TEGRA210_IO_PAD_TABLE(TEGRA_IO_PAD)
};

static const struct pinctrl_pin_desc tegra210_pin_descs[] = {
	TEGRA210_IO_PAD_TABLE(TEGRA_IO_PIN_DESC)
};

static const struct tegra_pmc_soc tegra210_pmc_soc = {
	.num_powergates = ARRAY_SIZE(tegra210_powergates),
	.powergates = tegra210_powergates,
	.num_cpu_powergates = ARRAY_SIZE(tegra210_cpu_powergates),
	.cpu_powergates = tegra210_cpu_powergates,
	.has_tsense_reset = true,
	.has_gpu_clamps = true,
	.has_impl_33v_pwr = false,
	.needs_mbist_war = true,
	.num_io_pads = ARRAY_SIZE(tegra210_io_pads),
	.io_pads = tegra210_io_pads,
	.num_pin_descs = ARRAY_SIZE(tegra210_pin_descs),
	.pin_descs = tegra210_pin_descs,
	.regs = &tegra20_pmc_regs,
	.init = tegra20_pmc_init,
	.setup_irq_polarity = tegra20_pmc_setup_irq_polarity,
};

#define TEGRA186_IO_PAD_TABLE(_pad)					     \
	/*   .id                        .dpd      .voltage  .name */	     \
	_pad(TEGRA_IO_PAD_CSIA,		0,	  UINT_MAX, "csia"),	     \
	_pad(TEGRA_IO_PAD_CSIB,		1,	  UINT_MAX, "csib"),	     \
	_pad(TEGRA_IO_PAD_DSI,		2,	  UINT_MAX, "dsi"),	     \
	_pad(TEGRA_IO_PAD_MIPI_BIAS,	3,	  UINT_MAX, "mipi-bias"),    \
	_pad(TEGRA_IO_PAD_PEX_CLK_BIAS,	4,	  UINT_MAX, "pex-clk-bias"), \
	_pad(TEGRA_IO_PAD_PEX_CLK3,	5,	  UINT_MAX, "pex-clk3"),     \
	_pad(TEGRA_IO_PAD_PEX_CLK2,	6,	  UINT_MAX, "pex-clk2"),     \
	_pad(TEGRA_IO_PAD_PEX_CLK1,	7,	  UINT_MAX, "pex-clk1"),     \
	_pad(TEGRA_IO_PAD_USB0,		9,	  UINT_MAX, "usb0"),	     \
	_pad(TEGRA_IO_PAD_USB1,		10,	  UINT_MAX, "usb1"),	     \
	_pad(TEGRA_IO_PAD_USB2,		11,	  UINT_MAX, "usb2"),	     \
	_pad(TEGRA_IO_PAD_USB_BIAS,	12,	  UINT_MAX, "usb-bias"),     \
	_pad(TEGRA_IO_PAD_UART,		14,	  UINT_MAX, "uart"),	     \
	_pad(TEGRA_IO_PAD_AUDIO,	17,	  UINT_MAX, "audio"),	     \
	_pad(TEGRA_IO_PAD_HSIC,		19,	  UINT_MAX, "hsic"),	     \
	_pad(TEGRA_IO_PAD_DBG,		25,	  UINT_MAX, "dbg"),	     \
	_pad(TEGRA_IO_PAD_HDMI_DP0,	28,	  UINT_MAX, "hdmi-dp0"),     \
	_pad(TEGRA_IO_PAD_HDMI_DP1,	29,	  UINT_MAX, "hdmi-dp1"),     \
	_pad(TEGRA_IO_PAD_PEX_CNTRL,	32,	  UINT_MAX, "pex-cntrl"),    \
	_pad(TEGRA_IO_PAD_SDMMC2_HV,	34,	  5,	    "sdmmc2-hv"),    \
	_pad(TEGRA_IO_PAD_SDMMC4,	36,	  UINT_MAX, "sdmmc4"),	     \
	_pad(TEGRA_IO_PAD_CAM,		38,	  UINT_MAX, "cam"),	     \
	_pad(TEGRA_IO_PAD_DSIB,		40,	  UINT_MAX, "dsib"),	     \
	_pad(TEGRA_IO_PAD_DSIC,		41,	  UINT_MAX, "dsic"),	     \
	_pad(TEGRA_IO_PAD_DSID,		42,	  UINT_MAX, "dsid"),	     \
	_pad(TEGRA_IO_PAD_CSIC,		43,	  UINT_MAX, "csic"),	     \
	_pad(TEGRA_IO_PAD_CSID,		44,	  UINT_MAX, "csid"),	     \
	_pad(TEGRA_IO_PAD_CSIE,		45,	  UINT_MAX, "csie"),	     \
	_pad(TEGRA_IO_PAD_CSIF,		46,	  UINT_MAX, "csif"),	     \
	_pad(TEGRA_IO_PAD_SPI,		47,	  UINT_MAX, "spi"),	     \
	_pad(TEGRA_IO_PAD_UFS,		49,	  UINT_MAX, "ufs"),	     \
	_pad(TEGRA_IO_PAD_DMIC_HV,	52,	  2,	    "dmic-hv"),	     \
	_pad(TEGRA_IO_PAD_EDP,		53,	  UINT_MAX, "edp"),	     \
	_pad(TEGRA_IO_PAD_SDMMC1_HV,	55,	  4,	    "sdmmc1-hv"),    \
	_pad(TEGRA_IO_PAD_SDMMC3_HV,	56,	  6,	    "sdmmc3-hv"),    \
	_pad(TEGRA_IO_PAD_CONN,		60,	  UINT_MAX, "conn"),	     \
	_pad(TEGRA_IO_PAD_AUDIO_HV,	61,	  1,	    "audio-hv"),     \
	_pad(TEGRA_IO_PAD_AO_HV,	UINT_MAX, 0,	    "ao-hv")

static const struct tegra_io_pad_soc tegra186_io_pads[] = {
	TEGRA186_IO_PAD_TABLE(TEGRA_IO_PAD)
};

static const struct pinctrl_pin_desc tegra186_pin_descs[] = {
	TEGRA186_IO_PAD_TABLE(TEGRA_IO_PIN_DESC)
};

static const struct tegra_pmc_regs tegra186_pmc_regs = {
	.scratch0 = 0x2000,
	.dpd_req = 0x74,
	.dpd_status = 0x78,
	.dpd2_req = 0x7c,
	.dpd2_status = 0x80,
};

static void tegra186_pmc_setup_irq_polarity(struct tegra_pmc *pmc,
					    struct device_node *np,
					    bool invert)
{
	struct resource regs;
	void __iomem *wake;
	u32 value;
	int index;

	index = of_property_match_string(np, "reg-names", "wake");
	if (index < 0) {
		pr_err("failed to find PMC wake registers\n");
		return;
	}

	of_address_to_resource(np, index, &regs);

	wake = ioremap_nocache(regs.start, resource_size(&regs));
	if (!wake) {
		pr_err("failed to map PMC wake registers\n");
		return;
	}

	value = readl(wake + WAKE_AOWAKE_CTRL);

	if (invert)
		value |= WAKE_AOWAKE_CTRL_INTR_POLARITY;
	else
		value &= ~WAKE_AOWAKE_CTRL_INTR_POLARITY;

	writel(value, wake + WAKE_AOWAKE_CTRL);

	iounmap(wake);
}

static const struct tegra_pmc_soc tegra186_pmc_soc = {
	.num_powergates = 0,
	.powergates = NULL,
	.num_cpu_powergates = 0,
	.cpu_powergates = NULL,
	.has_tsense_reset = false,
	.has_gpu_clamps = false,
	.has_impl_33v_pwr = true,
	.num_io_pads = ARRAY_SIZE(tegra186_io_pads),
	.io_pads = tegra186_io_pads,
	.num_pin_descs = ARRAY_SIZE(tegra186_pin_descs),
	.pin_descs = tegra186_pin_descs,
	.regs = &tegra186_pmc_regs,
	.init = NULL,
	.setup_irq_polarity = tegra186_pmc_setup_irq_polarity,
};

static const struct of_device_id tegra_pmc_match[] = {
	{ .compatible = "nvidia,tegra194-pmc", .data = &tegra186_pmc_soc },
	{ .compatible = "nvidia,tegra186-pmc", .data = &tegra186_pmc_soc },
	{ .compatible = "nvidia,tegra210-pmc", .data = &tegra210_pmc_soc },
	{ .compatible = "nvidia,tegra132-pmc", .data = &tegra124_pmc_soc },
	{ .compatible = "nvidia,tegra124-pmc", .data = &tegra124_pmc_soc },
	{ .compatible = "nvidia,tegra114-pmc", .data = &tegra114_pmc_soc },
	{ .compatible = "nvidia,tegra30-pmc", .data = &tegra30_pmc_soc },
	{ .compatible = "nvidia,tegra20-pmc", .data = &tegra20_pmc_soc },
	{ }
};

static struct platform_driver tegra_pmc_driver = {
	.driver = {
		.name = "tegra-pmc",
		.suppress_bind_attrs = true,
		.of_match_table = tegra_pmc_match,
#if defined(CONFIG_PM_SLEEP) && defined(CONFIG_ARM)
		.pm = &tegra_pmc_pm_ops,
#endif
	},
	.probe = tegra_pmc_probe,
};
builtin_platform_driver(tegra_pmc_driver);

/*
 * Early initialization to allow access to registers in the very early boot
 * process.
 */
static int __init tegra_pmc_early_init(void)
{
	const struct of_device_id *match;
	struct device_node *np;
	struct resource regs;
	bool invert;

	mutex_init(&pmc->powergates_lock);

	np = of_find_matching_node_and_match(NULL, tegra_pmc_match, &match);
	if (!np) {
		/*
		 * Fall back to legacy initialization for 32-bit ARM only. All
		 * 64-bit ARM device tree files for Tegra are required to have
		 * a PMC node.
		 *
		 * This is for backwards-compatibility with old device trees
		 * that didn't contain a PMC node. Note that in this case the
		 * SoC data can't be matched and therefore powergating is
		 * disabled.
		 */
		if (IS_ENABLED(CONFIG_ARM) && soc_is_tegra()) {
			pr_warn("DT node not found, powergating disabled\n");

			regs.start = 0x7000e400;
			regs.end = 0x7000e7ff;
			regs.flags = IORESOURCE_MEM;

			pr_warn("Using memory region %pR\n", &regs);
		} else {
			/*
			 * At this point we're not running on Tegra, so play
			 * nice with multi-platform kernels.
			 */
			return 0;
		}
	} else {
		/*
		 * Extract information from the device tree if we've found a
		 * matching node.
		 */
		if (of_address_to_resource(np, 0, &regs) < 0) {
			pr_err("failed to get PMC registers\n");
			of_node_put(np);
			return -ENXIO;
		}
	}

	pmc->base = ioremap_nocache(regs.start, resource_size(&regs));
	if (!pmc->base) {
		pr_err("failed to map PMC registers\n");
		of_node_put(np);
		return -ENXIO;
	}

	if (np) {
		pmc->soc = match->data;

		tegra_powergate_init(pmc, np);

		/*
		 * Invert the interrupt polarity if a PMC device tree node
		 * exists and contains the nvidia,invert-interrupt property.
		 */
		invert = of_property_read_bool(np, "nvidia,invert-interrupt");

		pmc->soc->setup_irq_polarity(pmc, np, invert);

		of_node_put(np);
	}

	return 0;
}
early_initcall(tegra_pmc_early_init);

/*
 * Copyright (C) 2010 Google, Inc.
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

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/iopoll.h>
#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/reset.h>
#include <linux/mmc/card.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/slot-gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/ktime.h>

#include "sdhci-pltfm.h"

/* Tegra SDHOST controller vendor register definitions */
#define SDHCI_TEGRA_VENDOR_CLOCK_CTRL			0x100
#define SDHCI_CLOCK_CTRL_TAP_MASK			0x00ff0000
#define SDHCI_CLOCK_CTRL_TAP_SHIFT			16
#define SDHCI_CLOCK_CTRL_TRIM_MASK			0x1f000000
#define SDHCI_CLOCK_CTRL_TRIM_SHIFT			24
#define SDHCI_CLOCK_CTRL_SDR50_TUNING_OVERRIDE		BIT(5)
#define SDHCI_CLOCK_CTRL_PADPIPE_CLKEN_OVERRIDE		BIT(3)
#define SDHCI_CLOCK_CTRL_SPI_MODE_CLKEN_OVERRIDE	BIT(2)

#define SDHCI_TEGRA_VENDOR_SYS_SW_CTRL			0x104
#define SDHCI_TEGRA_SYS_SW_CTRL_ENHANCED_STROBE		BIT(31)

#define SDHCI_TEGRA_VENDOR_CAP_OVERRIDES		0x10c
#define SDHCI_TEGRA_CAP_OVERRIDES_DQS_TRIM_MASK		0x00003f00
#define SDHCI_TEGRA_CAP_OVERRIDES_DQS_TRIM_SHIFT	8

#define SDHCI_TEGRA_VENDOR_MISC_CTRL			0x120
#define SDHCI_MISC_CTRL_ENABLE_SDR104			0x8
#define SDHCI_MISC_CTRL_ENABLE_SDR50			0x10
#define SDHCI_MISC_CTRL_ENABLE_SDHCI_SPEC_300		0x20
#define SDHCI_MISC_CTRL_ENABLE_DDR50			0x200

#define SDHCI_TEGRA_VENDOR_DLLCAL_CFG			0x1b0
#define SDHCI_TEGRA_DLLCAL_CALIBRATE			BIT(31)

#define SDHCI_TEGRA_VENDOR_DLLCAL_STA			0x1bc
#define SDHCI_TEGRA_DLLCAL_STA_ACTIVE			BIT(31)

#define SDHCI_VNDR_TUN_CTRL0_0				0x1c0
#define SDHCI_VNDR_TUN_CTRL0_TUN_HW_TAP			0x20000

#define SDHCI_TEGRA_AUTO_CAL_CONFIG			0x1e4
#define SDHCI_AUTO_CAL_START				BIT(31)
#define SDHCI_AUTO_CAL_ENABLE				BIT(29)
#define SDHCI_AUTO_CAL_PDPU_OFFSET_MASK			0x0000ffff

#define SDHCI_TEGRA_SDMEM_COMP_PADCTRL			0x1e0
#define SDHCI_TEGRA_SDMEM_COMP_PADCTRL_VREF_SEL_MASK	0x0000000f
#define SDHCI_TEGRA_SDMEM_COMP_PADCTRL_VREF_SEL_VAL	0x7
#define SDHCI_TEGRA_SDMEM_COMP_PADCTRL_E_INPUT_E_PWRD	BIT(31)

#define SDHCI_TEGRA_AUTO_CAL_STATUS			0x1ec
#define SDHCI_TEGRA_AUTO_CAL_ACTIVE			BIT(31)

#define NVQUIRK_FORCE_SDHCI_SPEC_200			BIT(0)
#define NVQUIRK_ENABLE_BLOCK_GAP_DET			BIT(1)
#define NVQUIRK_ENABLE_SDHCI_SPEC_300			BIT(2)
#define NVQUIRK_ENABLE_SDR50				BIT(3)
#define NVQUIRK_ENABLE_SDR104				BIT(4)
#define NVQUIRK_ENABLE_DDR50				BIT(5)
#define NVQUIRK_HAS_PADCALIB				BIT(6)
#define NVQUIRK_NEEDS_PAD_CONTROL			BIT(7)
#define NVQUIRK_DIS_CARD_CLK_CONFIG_TAP			BIT(8)

struct sdhci_tegra_soc_data {
	const struct sdhci_pltfm_data *pdata;
	u32 nvquirks;
};

/* Magic pull up and pull down pad calibration offsets */
struct sdhci_tegra_autocal_offsets {
	u32 pull_up_3v3;
	u32 pull_down_3v3;
	u32 pull_up_3v3_timeout;
	u32 pull_down_3v3_timeout;
	u32 pull_up_1v8;
	u32 pull_down_1v8;
	u32 pull_up_1v8_timeout;
	u32 pull_down_1v8_timeout;
	u32 pull_up_sdr104;
	u32 pull_down_sdr104;
	u32 pull_up_hs400;
	u32 pull_down_hs400;
};

struct sdhci_tegra {
	const struct sdhci_tegra_soc_data *soc_data;
	struct gpio_desc *power_gpio;
	bool ddr_signaling;
	bool pad_calib_required;
	bool pad_control_available;

	struct reset_control *rst;
	struct pinctrl *pinctrl_sdmmc;
	struct pinctrl_state *pinctrl_state_3v3;
	struct pinctrl_state *pinctrl_state_1v8;

	struct sdhci_tegra_autocal_offsets autocal_offsets;
	ktime_t last_calib;

	u32 default_tap;
	u32 default_trim;
	u32 dqs_trim;
};

static u16 tegra_sdhci_readw(struct sdhci_host *host, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	if (unlikely((soc_data->nvquirks & NVQUIRK_FORCE_SDHCI_SPEC_200) &&
			(reg == SDHCI_HOST_VERSION))) {
		/* Erratum: Version register is invalid in HW. */
		return SDHCI_SPEC_200;
	}

	return readw(host->ioaddr + reg);
}

static void tegra_sdhci_writew(struct sdhci_host *host, u16 val, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	switch (reg) {
	case SDHCI_TRANSFER_MODE:
		/*
		 * Postpone this write, we must do it together with a
		 * command write that is down below.
		 */
		pltfm_host->xfer_mode_shadow = val;
		return;
	case SDHCI_COMMAND:
		writel((val << 16) | pltfm_host->xfer_mode_shadow,
			host->ioaddr + SDHCI_TRANSFER_MODE);
		return;
	}

	writew(val, host->ioaddr + reg);
}

static void tegra_sdhci_writel(struct sdhci_host *host, u32 val, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	/* Seems like we're getting spurious timeout and crc errors, so
	 * disable signalling of them. In case of real errors software
	 * timers should take care of eventually detecting them.
	 */
	if (unlikely(reg == SDHCI_SIGNAL_ENABLE))
		val &= ~(SDHCI_INT_TIMEOUT|SDHCI_INT_CRC);

	writel(val, host->ioaddr + reg);

	if (unlikely((soc_data->nvquirks & NVQUIRK_ENABLE_BLOCK_GAP_DET) &&
			(reg == SDHCI_INT_ENABLE))) {
		/* Erratum: Must enable block gap interrupt detection */
		u8 gap_ctrl = readb(host->ioaddr + SDHCI_BLOCK_GAP_CONTROL);
		if (val & SDHCI_INT_CARD_INT)
			gap_ctrl |= 0x8;
		else
			gap_ctrl &= ~0x8;
		writeb(gap_ctrl, host->ioaddr + SDHCI_BLOCK_GAP_CONTROL);
	}
}

static bool tegra_sdhci_configure_card_clk(struct sdhci_host *host, bool enable)
{
	bool status;
	u32 reg;

	reg = sdhci_readw(host, SDHCI_CLOCK_CONTROL);
	status = !!(reg & SDHCI_CLOCK_CARD_EN);

	if (status == enable)
		return status;

	if (enable)
		reg |= SDHCI_CLOCK_CARD_EN;
	else
		reg &= ~SDHCI_CLOCK_CARD_EN;

	sdhci_writew(host, reg, SDHCI_CLOCK_CONTROL);

	return status;
}

static void tegra210_sdhci_writew(struct sdhci_host *host, u16 val, int reg)
{
	bool is_tuning_cmd = 0;
	bool clk_enabled;
	u8 cmd;

	if (reg == SDHCI_COMMAND) {
		cmd = SDHCI_GET_CMD(val);
		is_tuning_cmd = cmd == MMC_SEND_TUNING_BLOCK ||
				cmd == MMC_SEND_TUNING_BLOCK_HS200;
	}

	if (is_tuning_cmd)
		clk_enabled = tegra_sdhci_configure_card_clk(host, 0);

	writew(val, host->ioaddr + reg);

	if (is_tuning_cmd) {
		udelay(1);
		tegra_sdhci_configure_card_clk(host, clk_enabled);
	}
}

static unsigned int tegra_sdhci_get_ro(struct sdhci_host *host)
{
	return mmc_gpio_get_ro(host->mmc);
}

static bool tegra_sdhci_is_pad_and_regulator_valid(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int has_1v8, has_3v3;

	/*
	 * The SoCs which have NVQUIRK_NEEDS_PAD_CONTROL require software pad
	 * voltage configuration in order to perform voltage switching. This
	 * means that valid pinctrl info is required on SDHCI instances capable
	 * of performing voltage switching. Whether or not an SDHCI instance is
	 * capable of voltage switching is determined based on the regulator.
	 */

	if (!(tegra_host->soc_data->nvquirks & NVQUIRK_NEEDS_PAD_CONTROL))
		return true;

	if (IS_ERR(host->mmc->supply.vqmmc))
		return false;

	has_1v8 = regulator_is_supported_voltage(host->mmc->supply.vqmmc,
						 1700000, 1950000);

	has_3v3 = regulator_is_supported_voltage(host->mmc->supply.vqmmc,
						 2700000, 3600000);

	if (has_1v8 == 1 && has_3v3 == 1)
		return tegra_host->pad_control_available;

	/* Fixed voltage, no pad control required. */
	return true;
}

static void tegra_sdhci_set_tap(struct sdhci_host *host, unsigned int tap)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;
	bool card_clk_enabled = false;
	u32 reg;

	/*
	 * Touching the tap values is a bit tricky on some SoC generations.
	 * The quirk enables a workaround for a glitch that sometimes occurs if
	 * the tap values are changed.
	 */

	if (soc_data->nvquirks & NVQUIRK_DIS_CARD_CLK_CONFIG_TAP)
		card_clk_enabled = tegra_sdhci_configure_card_clk(host, false);

	reg = sdhci_readl(host, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);
	reg &= ~SDHCI_CLOCK_CTRL_TAP_MASK;
	reg |= tap << SDHCI_CLOCK_CTRL_TAP_SHIFT;
	sdhci_writel(host, reg, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);

	if (soc_data->nvquirks & NVQUIRK_DIS_CARD_CLK_CONFIG_TAP &&
	    card_clk_enabled) {
		udelay(1);
		sdhci_reset(host, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		tegra_sdhci_configure_card_clk(host, card_clk_enabled);
	}
}

static void tegra_sdhci_hs400_enhanced_strobe(struct mmc_host *mmc,
					      struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	u32 val;

	val = sdhci_readl(host, SDHCI_TEGRA_VENDOR_SYS_SW_CTRL);

	if (ios->enhanced_strobe)
		val |= SDHCI_TEGRA_SYS_SW_CTRL_ENHANCED_STROBE;
	else
		val &= ~SDHCI_TEGRA_SYS_SW_CTRL_ENHANCED_STROBE;

	sdhci_writel(host, val, SDHCI_TEGRA_VENDOR_SYS_SW_CTRL);

}

static void tegra_sdhci_reset(struct sdhci_host *host, u8 mask)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;
	u32 misc_ctrl, clk_ctrl, pad_ctrl;

	sdhci_reset(host, mask);

	if (!(mask & SDHCI_RESET_ALL))
		return;

	tegra_sdhci_set_tap(host, tegra_host->default_tap);

	misc_ctrl = sdhci_readl(host, SDHCI_TEGRA_VENDOR_MISC_CTRL);
	clk_ctrl = sdhci_readl(host, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);

	misc_ctrl &= ~(SDHCI_MISC_CTRL_ENABLE_SDHCI_SPEC_300 |
		       SDHCI_MISC_CTRL_ENABLE_SDR50 |
		       SDHCI_MISC_CTRL_ENABLE_DDR50 |
		       SDHCI_MISC_CTRL_ENABLE_SDR104);

	clk_ctrl &= ~(SDHCI_CLOCK_CTRL_TRIM_MASK |
		      SDHCI_CLOCK_CTRL_SPI_MODE_CLKEN_OVERRIDE);

	if (tegra_sdhci_is_pad_and_regulator_valid(host)) {
		/* Erratum: Enable SDHCI spec v3.00 support */
		if (soc_data->nvquirks & NVQUIRK_ENABLE_SDHCI_SPEC_300)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_SDHCI_SPEC_300;
		/* Advertise UHS modes as supported by host */
		if (soc_data->nvquirks & NVQUIRK_ENABLE_SDR50)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_SDR50;
		if (soc_data->nvquirks & NVQUIRK_ENABLE_DDR50)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_DDR50;
		if (soc_data->nvquirks & NVQUIRK_ENABLE_SDR104)
			misc_ctrl |= SDHCI_MISC_CTRL_ENABLE_SDR104;
		if (soc_data->nvquirks & SDHCI_MISC_CTRL_ENABLE_SDR50)
			clk_ctrl |= SDHCI_CLOCK_CTRL_SDR50_TUNING_OVERRIDE;
	}

	clk_ctrl |= tegra_host->default_trim << SDHCI_CLOCK_CTRL_TRIM_SHIFT;

	sdhci_writel(host, misc_ctrl, SDHCI_TEGRA_VENDOR_MISC_CTRL);
	sdhci_writel(host, clk_ctrl, SDHCI_TEGRA_VENDOR_CLOCK_CTRL);

	if (soc_data->nvquirks & NVQUIRK_HAS_PADCALIB) {
		pad_ctrl = sdhci_readl(host, SDHCI_TEGRA_SDMEM_COMP_PADCTRL);
		pad_ctrl &= ~SDHCI_TEGRA_SDMEM_COMP_PADCTRL_VREF_SEL_MASK;
		pad_ctrl |= SDHCI_TEGRA_SDMEM_COMP_PADCTRL_VREF_SEL_VAL;
		sdhci_writel(host, pad_ctrl, SDHCI_TEGRA_SDMEM_COMP_PADCTRL);

		tegra_host->pad_calib_required = true;
	}

	tegra_host->ddr_signaling = false;
}

static void tegra_sdhci_configure_cal_pad(struct sdhci_host *host, bool enable)
{
	u32 val;

	/*
	 * Enable or disable the additional I/O pad used by the drive strength
	 * calibration process.
	 */
	val = sdhci_readl(host, SDHCI_TEGRA_SDMEM_COMP_PADCTRL);

	if (enable)
		val |= SDHCI_TEGRA_SDMEM_COMP_PADCTRL_E_INPUT_E_PWRD;
	else
		val &= ~SDHCI_TEGRA_SDMEM_COMP_PADCTRL_E_INPUT_E_PWRD;

	sdhci_writel(host, val, SDHCI_TEGRA_SDMEM_COMP_PADCTRL);

	if (enable)
		usleep_range(1, 2);
}

static void tegra_sdhci_set_pad_autocal_offset(struct sdhci_host *host,
					       u16 pdpu)
{
	u32 reg;

	reg = sdhci_readl(host, SDHCI_TEGRA_AUTO_CAL_CONFIG);
	reg &= ~SDHCI_AUTO_CAL_PDPU_OFFSET_MASK;
	reg |= pdpu;
	sdhci_writel(host, reg, SDHCI_TEGRA_AUTO_CAL_CONFIG);
}

static void tegra_sdhci_pad_autocalib(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	struct sdhci_tegra_autocal_offsets offsets =
			tegra_host->autocal_offsets;
	struct mmc_ios *ios = &host->mmc->ios;
	bool card_clk_enabled;
	u16 pdpu;
	u32 reg;
	int ret;

	switch (ios->timing) {
	case MMC_TIMING_UHS_SDR104:
		pdpu = offsets.pull_down_sdr104 << 8 | offsets.pull_up_sdr104;
		break;
	case MMC_TIMING_MMC_HS400:
		pdpu = offsets.pull_down_hs400 << 8 | offsets.pull_up_hs400;
		break;
	default:
		if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180)
			pdpu = offsets.pull_down_1v8 << 8 | offsets.pull_up_1v8;
		else
			pdpu = offsets.pull_down_3v3 << 8 | offsets.pull_up_3v3;
	}

	tegra_sdhci_set_pad_autocal_offset(host, pdpu);

	card_clk_enabled = tegra_sdhci_configure_card_clk(host, false);

	tegra_sdhci_configure_cal_pad(host, true);

	reg = sdhci_readl(host, SDHCI_TEGRA_AUTO_CAL_CONFIG);
	reg |= SDHCI_AUTO_CAL_ENABLE | SDHCI_AUTO_CAL_START;
	sdhci_writel(host, reg, SDHCI_TEGRA_AUTO_CAL_CONFIG);

	usleep_range(1, 2);
	/* 10 ms timeout */
	ret = readl_poll_timeout(host->ioaddr + SDHCI_TEGRA_AUTO_CAL_STATUS,
				 reg, !(reg & SDHCI_TEGRA_AUTO_CAL_ACTIVE),
				 1000, 10000);

	tegra_sdhci_configure_cal_pad(host, false);

	tegra_sdhci_configure_card_clk(host, card_clk_enabled);

	if (ret) {
		dev_err(mmc_dev(host->mmc), "Pad autocal timed out\n");

		if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180)
			pdpu = offsets.pull_down_1v8_timeout << 8 |
			       offsets.pull_up_1v8_timeout;
		else
			pdpu = offsets.pull_down_3v3_timeout << 8 |
			       offsets.pull_up_3v3_timeout;

		/* Disable automatic calibration and use fixed offsets */
		reg = sdhci_readl(host, SDHCI_TEGRA_AUTO_CAL_CONFIG);
		reg &= ~SDHCI_AUTO_CAL_ENABLE;
		sdhci_writel(host, reg, SDHCI_TEGRA_AUTO_CAL_CONFIG);

		tegra_sdhci_set_pad_autocal_offset(host, pdpu);
	}
}

static void tegra_sdhci_parse_pad_autocal_dt(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	struct sdhci_tegra_autocal_offsets *autocal =
			&tegra_host->autocal_offsets;
	int err;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-up-offset-3v3",
			&autocal->pull_up_3v3);
	if (err)
		autocal->pull_up_3v3 = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-down-offset-3v3",
			&autocal->pull_down_3v3);
	if (err)
		autocal->pull_down_3v3 = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-up-offset-1v8",
			&autocal->pull_up_1v8);
	if (err)
		autocal->pull_up_1v8 = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-down-offset-1v8",
			&autocal->pull_down_1v8);
	if (err)
		autocal->pull_down_1v8 = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-up-offset-3v3-timeout",
			&autocal->pull_up_3v3_timeout);
	if (err)
		autocal->pull_up_3v3_timeout = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-down-offset-3v3-timeout",
			&autocal->pull_down_3v3_timeout);
	if (err)
		autocal->pull_down_3v3_timeout = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-up-offset-1v8-timeout",
			&autocal->pull_up_1v8_timeout);
	if (err)
		autocal->pull_up_1v8_timeout = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-down-offset-1v8-timeout",
			&autocal->pull_down_1v8_timeout);
	if (err)
		autocal->pull_down_1v8_timeout = 0;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-up-offset-sdr104",
			&autocal->pull_up_sdr104);
	if (err)
		autocal->pull_up_sdr104 = autocal->pull_up_1v8;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-down-offset-sdr104",
			&autocal->pull_down_sdr104);
	if (err)
		autocal->pull_down_sdr104 = autocal->pull_down_1v8;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-up-offset-hs400",
			&autocal->pull_up_hs400);
	if (err)
		autocal->pull_up_hs400 = autocal->pull_up_1v8;

	err = device_property_read_u32(host->mmc->parent,
			"nvidia,pad-autocal-pull-down-offset-hs400",
			&autocal->pull_down_hs400);
	if (err)
		autocal->pull_down_hs400 = autocal->pull_down_1v8;
}

static void tegra_sdhci_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	ktime_t since_calib = ktime_sub(ktime_get(), tegra_host->last_calib);

	/* 100 ms calibration interval is specified in the TRM */
	if (ktime_to_ms(since_calib) > 100) {
		tegra_sdhci_pad_autocalib(host);
		tegra_host->last_calib = ktime_get();
	}

	sdhci_request(mmc, mrq);
}

static void tegra_sdhci_parse_tap_and_trim(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int err;

	err = device_property_read_u32(host->mmc->parent, "nvidia,default-tap",
				       &tegra_host->default_tap);
	if (err)
		tegra_host->default_tap = 0;

	err = device_property_read_u32(host->mmc->parent, "nvidia,default-trim",
				       &tegra_host->default_trim);
	if (err)
		tegra_host->default_trim = 0;

	err = device_property_read_u32(host->mmc->parent, "nvidia,dqs-trim",
				       &tegra_host->dqs_trim);
	if (err)
		tegra_host->dqs_trim = 0x11;
}

static void tegra_sdhci_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	unsigned long host_clk;

	if (!clock)
		return sdhci_set_clock(host, clock);

	/*
	 * In DDR50/52 modes the Tegra SDHCI controllers require the SDHCI
	 * divider to be configured to divided the host clock by two. The SDHCI
	 * clock divider is calculated as part of sdhci_set_clock() by
	 * sdhci_calc_clk(). The divider is calculated from host->max_clk and
	 * the requested clock rate.
	 *
	 * By setting the host->max_clk to clock * 2 the divider calculation
	 * will always result in the correct value for DDR50/52 modes,
	 * regardless of clock rate rounding, which may happen if the value
	 * from clk_get_rate() is used.
	 */
	host_clk = tegra_host->ddr_signaling ? clock * 2 : clock;
	clk_set_rate(pltfm_host->clk, host_clk);
	if (tegra_host->ddr_signaling)
		host->max_clk = host_clk;
	else
		host->max_clk = clk_get_rate(pltfm_host->clk);

	sdhci_set_clock(host, clock);

	if (tegra_host->pad_calib_required) {
		tegra_sdhci_pad_autocalib(host);
		tegra_host->pad_calib_required = false;
	}
}

static unsigned int tegra_sdhci_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);

	return clk_round_rate(pltfm_host->clk, UINT_MAX);
}

static void tegra_sdhci_set_dqs_trim(struct sdhci_host *host, u8 trim)
{
	u32 val;

	val = sdhci_readl(host, SDHCI_TEGRA_VENDOR_CAP_OVERRIDES);
	val &= ~SDHCI_TEGRA_CAP_OVERRIDES_DQS_TRIM_MASK;
	val |= trim << SDHCI_TEGRA_CAP_OVERRIDES_DQS_TRIM_SHIFT;
	sdhci_writel(host, val, SDHCI_TEGRA_VENDOR_CAP_OVERRIDES);
}

static void tegra_sdhci_hs400_dll_cal(struct sdhci_host *host)
{
	u32 reg;
	int err;

	reg = sdhci_readl(host, SDHCI_TEGRA_VENDOR_DLLCAL_CFG);
	reg |= SDHCI_TEGRA_DLLCAL_CALIBRATE;
	sdhci_writel(host, reg, SDHCI_TEGRA_VENDOR_DLLCAL_CFG);

	/* 1 ms sleep, 5 ms timeout */
	err = readl_poll_timeout(host->ioaddr + SDHCI_TEGRA_VENDOR_DLLCAL_STA,
				 reg, !(reg & SDHCI_TEGRA_DLLCAL_STA_ACTIVE),
				 1000, 5000);
	if (err)
		dev_err(mmc_dev(host->mmc),
			"HS400 delay line calibration timed out\n");
}

static void tegra_sdhci_set_uhs_signaling(struct sdhci_host *host,
					  unsigned timing)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	bool set_default_tap = false;
	bool set_dqs_trim = false;
	bool do_hs400_dll_cal = false;

	switch (timing) {
	case MMC_TIMING_UHS_SDR50:
	case MMC_TIMING_UHS_SDR104:
	case MMC_TIMING_MMC_HS200:
		/* Don't set default tap on tunable modes. */
		break;
	case MMC_TIMING_MMC_HS400:
		set_dqs_trim = true;
		do_hs400_dll_cal = true;
		break;
	case MMC_TIMING_MMC_DDR52:
	case MMC_TIMING_UHS_DDR50:
		tegra_host->ddr_signaling = true;
		set_default_tap = true;
		break;
	default:
		set_default_tap = true;
		break;
	}

	sdhci_set_uhs_signaling(host, timing);

	tegra_sdhci_pad_autocalib(host);

	if (set_default_tap)
		tegra_sdhci_set_tap(host, tegra_host->default_tap);

	if (set_dqs_trim)
		tegra_sdhci_set_dqs_trim(host, tegra_host->dqs_trim);

	if (do_hs400_dll_cal)
		tegra_sdhci_hs400_dll_cal(host);
}

static int tegra_sdhci_execute_tuning(struct sdhci_host *host, u32 opcode)
{
	unsigned int min, max;

	/*
	 * Start search for minimum tap value at 10, as smaller values are
	 * may wrongly be reported as working but fail at higher speeds,
	 * according to the TRM.
	 */
	min = 10;
	while (min < 255) {
		tegra_sdhci_set_tap(host, min);
		if (!mmc_send_tuning(host->mmc, opcode, NULL))
			break;
		min++;
	}

	/* Find the maximum tap value that still passes. */
	max = min + 1;
	while (max < 255) {
		tegra_sdhci_set_tap(host, max);
		if (mmc_send_tuning(host->mmc, opcode, NULL)) {
			max--;
			break;
		}
		max++;
	}

	/* The TRM states the ideal tap value is at 75% in the passing range. */
	tegra_sdhci_set_tap(host, min + ((max - min) * 3 / 4));

	return mmc_send_tuning(host->mmc, opcode, NULL);
}

static int tegra_sdhci_set_padctrl(struct sdhci_host *host, int voltage)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int ret;

	if (!tegra_host->pad_control_available)
		return 0;

	if (voltage == MMC_SIGNAL_VOLTAGE_180) {
		ret = pinctrl_select_state(tegra_host->pinctrl_sdmmc,
					   tegra_host->pinctrl_state_1v8);
		if (ret < 0)
			dev_err(mmc_dev(host->mmc),
				"setting 1.8V failed, ret: %d\n", ret);
	} else {
		ret = pinctrl_select_state(tegra_host->pinctrl_sdmmc,
					   tegra_host->pinctrl_state_3v3);
		if (ret < 0)
			dev_err(mmc_dev(host->mmc),
				"setting 3.3V failed, ret: %d\n", ret);
	}

	return ret;
}

static int sdhci_tegra_start_signal_voltage_switch(struct mmc_host *mmc,
						   struct mmc_ios *ios)
{
	struct sdhci_host *host = mmc_priv(mmc);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	int ret = 0;

	if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_330) {
		ret = tegra_sdhci_set_padctrl(host, ios->signal_voltage);
		if (ret < 0)
			return ret;
		ret = sdhci_start_signal_voltage_switch(mmc, ios);
	} else if (ios->signal_voltage == MMC_SIGNAL_VOLTAGE_180) {
		ret = sdhci_start_signal_voltage_switch(mmc, ios);
		if (ret < 0)
			return ret;
		ret = tegra_sdhci_set_padctrl(host, ios->signal_voltage);
	}

	if (tegra_host->pad_calib_required)
		tegra_sdhci_pad_autocalib(host);

	return ret;
}

static int tegra_sdhci_init_pinctrl_info(struct device *dev,
					 struct sdhci_tegra *tegra_host)
{
	tegra_host->pinctrl_sdmmc = devm_pinctrl_get(dev);
	if (IS_ERR(tegra_host->pinctrl_sdmmc)) {
		dev_dbg(dev, "No pinctrl info, err: %ld\n",
			PTR_ERR(tegra_host->pinctrl_sdmmc));
		return -1;
	}

	tegra_host->pinctrl_state_3v3 =
		pinctrl_lookup_state(tegra_host->pinctrl_sdmmc, "sdmmc-3v3");
	if (IS_ERR(tegra_host->pinctrl_state_3v3)) {
		dev_warn(dev, "Missing 3.3V pad state, err: %ld\n",
			 PTR_ERR(tegra_host->pinctrl_state_3v3));
		return -1;
	}

	tegra_host->pinctrl_state_1v8 =
		pinctrl_lookup_state(tegra_host->pinctrl_sdmmc, "sdmmc-1v8");
	if (IS_ERR(tegra_host->pinctrl_state_1v8)) {
		dev_warn(dev, "Missing 1.8V pad state, err: %ld\n",
			 PTR_ERR(tegra_host->pinctrl_state_1v8));
		return -1;
	}

	tegra_host->pad_control_available = true;

	return 0;
}

static void tegra_sdhci_voltage_switch(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_tegra_soc_data *soc_data = tegra_host->soc_data;

	if (soc_data->nvquirks & NVQUIRK_HAS_PADCALIB)
		tegra_host->pad_calib_required = true;
}

static const struct sdhci_ops tegra_sdhci_ops = {
	.get_ro     = tegra_sdhci_get_ro,
	.read_w     = tegra_sdhci_readw,
	.write_l    = tegra_sdhci_writel,
	.set_clock  = tegra_sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset      = tegra_sdhci_reset,
	.platform_execute_tuning = tegra_sdhci_execute_tuning,
	.set_uhs_signaling = tegra_sdhci_set_uhs_signaling,
	.voltage_switch = tegra_sdhci_voltage_switch,
	.get_max_clock = tegra_sdhci_get_max_clock,
};

static const struct sdhci_pltfm_data sdhci_tegra20_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.ops  = &tegra_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra20 = {
	.pdata = &sdhci_tegra20_pdata,
	.nvquirks = NVQUIRK_FORCE_SDHCI_SPEC_200 |
		    NVQUIRK_ENABLE_BLOCK_GAP_DET,
};

static const struct sdhci_pltfm_data sdhci_tegra30_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN |
		   SDHCI_QUIRK2_BROKEN_HS200 |
		   /*
		    * Auto-CMD23 leads to "Got command interrupt 0x00010000 even
		    * though no command operation was in progress."
		    *
		    * The exact reason is unknown, as the same hardware seems
		    * to support Auto CMD23 on a downstream 3.1 kernel.
		    */
		   SDHCI_QUIRK2_ACMD23_BROKEN,
	.ops  = &tegra_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra30 = {
	.pdata = &sdhci_tegra30_pdata,
	.nvquirks = NVQUIRK_ENABLE_SDHCI_SPEC_300 |
		    NVQUIRK_ENABLE_SDR50 |
		    NVQUIRK_ENABLE_SDR104 |
		    NVQUIRK_HAS_PADCALIB,
};

static const struct sdhci_ops tegra114_sdhci_ops = {
	.get_ro     = tegra_sdhci_get_ro,
	.read_w     = tegra_sdhci_readw,
	.write_w    = tegra_sdhci_writew,
	.write_l    = tegra_sdhci_writel,
	.set_clock  = tegra_sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset      = tegra_sdhci_reset,
	.platform_execute_tuning = tegra_sdhci_execute_tuning,
	.set_uhs_signaling = tegra_sdhci_set_uhs_signaling,
	.voltage_switch = tegra_sdhci_voltage_switch,
	.get_max_clock = tegra_sdhci_get_max_clock,
};

static const struct sdhci_pltfm_data sdhci_tegra114_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
	.ops  = &tegra114_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra114 = {
	.pdata = &sdhci_tegra114_pdata,
};

static const struct sdhci_pltfm_data sdhci_tegra124_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN |
		   /*
		    * The TRM states that the SD/MMC controller found on
		    * Tegra124 can address 34 bits (the maximum supported by
		    * the Tegra memory controller), but tests show that DMA
		    * to or from above 4 GiB doesn't work. This is possibly
		    * caused by missing programming, though it's not obvious
		    * what sequence is required. Mark 64-bit DMA broken for
		    * now to fix this for existing users (e.g. Nyan boards).
		    */
		   SDHCI_QUIRK2_BROKEN_64_BIT_DMA,
	.ops  = &tegra114_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra124 = {
	.pdata = &sdhci_tegra124_pdata,
};

static const struct sdhci_ops tegra210_sdhci_ops = {
	.get_ro     = tegra_sdhci_get_ro,
	.read_w     = tegra_sdhci_readw,
	.write_w    = tegra210_sdhci_writew,
	.write_l    = tegra_sdhci_writel,
	.set_clock  = tegra_sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset      = tegra_sdhci_reset,
	.set_uhs_signaling = tegra_sdhci_set_uhs_signaling,
	.voltage_switch = tegra_sdhci_voltage_switch,
	.get_max_clock = tegra_sdhci_get_max_clock,
};

static const struct sdhci_pltfm_data sdhci_tegra210_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
	.ops  = &tegra210_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra210 = {
	.pdata = &sdhci_tegra210_pdata,
	.nvquirks = NVQUIRK_NEEDS_PAD_CONTROL |
		    NVQUIRK_HAS_PADCALIB |
		    NVQUIRK_DIS_CARD_CLK_CONFIG_TAP |
		    NVQUIRK_ENABLE_SDR50 |
		    NVQUIRK_ENABLE_SDR104,
};

static const struct sdhci_ops tegra186_sdhci_ops = {
	.get_ro     = tegra_sdhci_get_ro,
	.read_w     = tegra_sdhci_readw,
	.write_l    = tegra_sdhci_writel,
	.set_clock  = tegra_sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset      = tegra_sdhci_reset,
	.set_uhs_signaling = tegra_sdhci_set_uhs_signaling,
	.voltage_switch = tegra_sdhci_voltage_switch,
	.get_max_clock = tegra_sdhci_get_max_clock,
};

static const struct sdhci_pltfm_data sdhci_tegra186_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK |
		  SDHCI_QUIRK_SINGLE_POWER_WRITE |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC |
		  SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN |
		   /* SDHCI controllers on Tegra186 support 40-bit addressing.
		    * IOVA addresses are 48-bit wide on Tegra186.
		    * With 64-bit dma mask used for SDHCI, accesses can
		    * be broken. Disable 64-bit dma, which would fall back
		    * to 32-bit dma mask. Ideally 40-bit dma mask would work,
		    * But it is not supported as of now.
		    */
		   SDHCI_QUIRK2_BROKEN_64_BIT_DMA,
	.ops  = &tegra186_sdhci_ops,
};

static const struct sdhci_tegra_soc_data soc_data_tegra186 = {
	.pdata = &sdhci_tegra186_pdata,
	.nvquirks = NVQUIRK_NEEDS_PAD_CONTROL |
		    NVQUIRK_HAS_PADCALIB |
		    NVQUIRK_DIS_CARD_CLK_CONFIG_TAP |
		    NVQUIRK_ENABLE_SDR50 |
		    NVQUIRK_ENABLE_SDR104,
};

static const struct of_device_id sdhci_tegra_dt_match[] = {
	{ .compatible = "nvidia,tegra186-sdhci", .data = &soc_data_tegra186 },
	{ .compatible = "nvidia,tegra210-sdhci", .data = &soc_data_tegra210 },
	{ .compatible = "nvidia,tegra124-sdhci", .data = &soc_data_tegra124 },
	{ .compatible = "nvidia,tegra114-sdhci", .data = &soc_data_tegra114 },
	{ .compatible = "nvidia,tegra30-sdhci", .data = &soc_data_tegra30 },
	{ .compatible = "nvidia,tegra20-sdhci", .data = &soc_data_tegra20 },
	{}
};
MODULE_DEVICE_TABLE(of, sdhci_tegra_dt_match);

static int sdhci_tegra_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	const struct sdhci_tegra_soc_data *soc_data;
	struct sdhci_host *host;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_tegra *tegra_host;
	struct clk *clk;
	int rc;

	match = of_match_device(sdhci_tegra_dt_match, &pdev->dev);
	if (!match)
		return -EINVAL;
	soc_data = match->data;

	host = sdhci_pltfm_init(pdev, soc_data->pdata, sizeof(*tegra_host));
	if (IS_ERR(host))
		return PTR_ERR(host);
	pltfm_host = sdhci_priv(host);

	tegra_host = sdhci_pltfm_priv(pltfm_host);
	tegra_host->ddr_signaling = false;
	tegra_host->pad_calib_required = false;
	tegra_host->pad_control_available = false;
	tegra_host->soc_data = soc_data;

	if (soc_data->nvquirks & NVQUIRK_NEEDS_PAD_CONTROL) {
		rc = tegra_sdhci_init_pinctrl_info(&pdev->dev, tegra_host);
		if (rc == 0)
			host->mmc_host_ops.start_signal_voltage_switch =
				sdhci_tegra_start_signal_voltage_switch;
	}

	/* Hook to periodically rerun pad calibration */
	if (soc_data->nvquirks & NVQUIRK_HAS_PADCALIB)
		host->mmc_host_ops.request = tegra_sdhci_request;

	host->mmc_host_ops.hs400_enhanced_strobe =
			tegra_sdhci_hs400_enhanced_strobe;

	rc = mmc_of_parse(host->mmc);
	if (rc)
		goto err_parse_dt;

	if (tegra_host->soc_data->nvquirks & NVQUIRK_ENABLE_DDR50)
		host->mmc->caps |= MMC_CAP_1_8V_DDR;

	tegra_sdhci_parse_pad_autocal_dt(host);

	tegra_sdhci_parse_tap_and_trim(host);

	tegra_host->power_gpio = devm_gpiod_get_optional(&pdev->dev, "power",
							 GPIOD_OUT_HIGH);
	if (IS_ERR(tegra_host->power_gpio)) {
		rc = PTR_ERR(tegra_host->power_gpio);
		goto err_power_req;
	}

	clk = devm_clk_get(mmc_dev(host->mmc), NULL);
	if (IS_ERR(clk)) {
		dev_err(mmc_dev(host->mmc), "clk err\n");
		rc = PTR_ERR(clk);
		goto err_clk_get;
	}
	clk_prepare_enable(clk);
	pltfm_host->clk = clk;

	tegra_host->rst = devm_reset_control_get_exclusive(&pdev->dev,
							   "sdhci");
	if (IS_ERR(tegra_host->rst)) {
		rc = PTR_ERR(tegra_host->rst);
		dev_err(&pdev->dev, "failed to get reset control: %d\n", rc);
		goto err_rst_get;
	}

	rc = reset_control_assert(tegra_host->rst);
	if (rc)
		goto err_rst_get;

	usleep_range(2000, 4000);

	rc = reset_control_deassert(tegra_host->rst);
	if (rc)
		goto err_rst_get;

	usleep_range(2000, 4000);

	rc = sdhci_add_host(host);
	if (rc)
		goto err_add_host;

	return 0;

err_add_host:
	reset_control_assert(tegra_host->rst);
err_rst_get:
	clk_disable_unprepare(pltfm_host->clk);
err_clk_get:
err_power_req:
err_parse_dt:
	sdhci_pltfm_free(pdev);
	return rc;
}

static int sdhci_tegra_remove(struct platform_device *pdev)
{
	struct sdhci_host *host = platform_get_drvdata(pdev);
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_tegra *tegra_host = sdhci_pltfm_priv(pltfm_host);

	sdhci_remove_host(host, 0);

	reset_control_assert(tegra_host->rst);
	usleep_range(2000, 4000);
	clk_disable_unprepare(pltfm_host->clk);

	sdhci_pltfm_free(pdev);

	return 0;
}

static struct platform_driver sdhci_tegra_driver = {
	.driver		= {
		.name	= "sdhci-tegra",
		.of_match_table = sdhci_tegra_dt_match,
		.pm	= &sdhci_pltfm_pmops,
	},
	.probe		= sdhci_tegra_probe,
	.remove		= sdhci_tegra_remove,
};

module_platform_driver(sdhci_tegra_driver);

MODULE_DESCRIPTION("SDHCI driver for Tegra");
MODULE_AUTHOR("Google, Inc.");
MODULE_LICENSE("GPL v2");

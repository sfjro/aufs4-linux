/*
 * Copyright (C) 2016 Felix Fietkau <nbd@nbd.name>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>

#include "mt76x0.h"
#include "mcu.h"

static int mt76x0e_start(struct ieee80211_hw *hw)
{
	struct mt76x02_dev *dev = hw->priv;

	mutex_lock(&dev->mt76.mutex);

	mt76x02_mac_start(dev);
	mt76x0_phy_calibrate(dev, true);
	ieee80211_queue_delayed_work(dev->mt76.hw, &dev->mac_work,
				     MT_CALIBRATE_INTERVAL);
	ieee80211_queue_delayed_work(dev->mt76.hw, &dev->cal_work,
				     MT_CALIBRATE_INTERVAL);
	set_bit(MT76_STATE_RUNNING, &dev->mt76.state);

	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static void mt76x0e_stop_hw(struct mt76x02_dev *dev)
{
	cancel_delayed_work_sync(&dev->cal_work);
	cancel_delayed_work_sync(&dev->mac_work);

	if (!mt76_poll(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_TX_DMA_BUSY,
		       0, 1000))
		dev_warn(dev->mt76.dev, "TX DMA did not stop\n");
	mt76_clear(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_TX_DMA_EN);

	mt76x0_mac_stop(dev);

	if (!mt76_poll(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_RX_DMA_BUSY,
		       0, 1000))
		dev_warn(dev->mt76.dev, "TX DMA did not stop\n");
	mt76_clear(dev, MT_WPDMA_GLO_CFG, MT_WPDMA_GLO_CFG_RX_DMA_EN);
}

static void mt76x0e_stop(struct ieee80211_hw *hw)
{
	struct mt76x02_dev *dev = hw->priv;

	mutex_lock(&dev->mt76.mutex);
	clear_bit(MT76_STATE_RUNNING, &dev->mt76.state);
	mt76x0e_stop_hw(dev);
	mutex_unlock(&dev->mt76.mutex);
}

static const struct ieee80211_ops mt76x0e_ops = {
	.tx = mt76x02_tx,
	.start = mt76x0e_start,
	.stop = mt76x0e_stop,
	.add_interface = mt76x02_add_interface,
	.remove_interface = mt76x02_remove_interface,
	.config = mt76x0_config,
	.configure_filter = mt76x02_configure_filter,
	.sta_add = mt76x02_sta_add,
	.sta_remove = mt76x02_sta_remove,
	.set_key = mt76x02_set_key,
	.conf_tx = mt76x02_conf_tx,
	.sw_scan_start = mt76x0_sw_scan,
	.sw_scan_complete = mt76x0_sw_scan_complete,
	.ampdu_action = mt76x02_ampdu_action,
	.sta_rate_tbl_update = mt76x02_sta_rate_tbl_update,
	.wake_tx_queue = mt76_wake_tx_queue,
};

static int mt76x0e_register_device(struct mt76x02_dev *dev)
{
	int err;

	mt76x0_chip_onoff(dev, true, false);
	if (!mt76x02_wait_for_mac(&dev->mt76))
		return -ETIMEDOUT;

	mt76x02_dma_disable(dev);
	err = mt76x0e_mcu_init(dev);
	if (err < 0)
		return err;

	err = mt76x02_dma_init(dev);
	if (err < 0)
		return err;

	err = mt76x0_init_hardware(dev);
	if (err < 0)
		return err;

	if (mt76_chip(&dev->mt76) == 0x7610) {
		u16 val;

		mt76_clear(dev, MT_COEXCFG0, BIT(0));

		val = mt76x02_eeprom_get(dev, MT_EE_NIC_CONF_0);
		if (!(val & MT_EE_NIC_CONF_0_PA_IO_CURRENT))
			mt76_set(dev, MT_XO_CTRL7, 0xc03);
	}

	mt76_clear(dev, 0x110, BIT(9));
	mt76_set(dev, MT_MAX_LEN_CFG, BIT(13));

	err = mt76x0_register_device(dev);
	if (err < 0)
		return err;

	set_bit(MT76_STATE_INITIALIZED, &dev->mt76.state);

	return 0;
}

static int
mt76x0e_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	static const struct mt76_driver_ops drv_ops = {
		.txwi_size = sizeof(struct mt76x02_txwi),
		.tx_prepare_skb = mt76x02_tx_prepare_skb,
		.tx_complete_skb = mt76x02_tx_complete_skb,
		.rx_skb = mt76x02_queue_rx_skb,
		.rx_poll_complete = mt76x02_rx_poll_complete,
	};
	struct mt76x02_dev *dev;
	int ret;

	ret = pcim_enable_device(pdev);
	if (ret)
		return ret;

	ret = pcim_iomap_regions(pdev, BIT(0), pci_name(pdev));
	if (ret)
		return ret;

	pci_set_master(pdev);

	ret = pci_set_dma_mask(pdev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	dev = mt76x0_alloc_device(&pdev->dev, &drv_ops, &mt76x0e_ops);
	if (!dev)
		return -ENOMEM;

	mt76_mmio_init(&dev->mt76, pcim_iomap_table(pdev)[0]);

	dev->mt76.rev = mt76_rr(dev, MT_ASIC_VERSION);
	dev_info(dev->mt76.dev, "ASIC revision: %08x\n", dev->mt76.rev);

	ret = devm_request_irq(dev->mt76.dev, pdev->irq, mt76x02_irq_handler,
			       IRQF_SHARED, KBUILD_MODNAME, dev);
	if (ret)
		goto error;

	ret = mt76x0e_register_device(dev);
	if (ret < 0)
		goto error;

	return 0;

error:
	ieee80211_free_hw(mt76_hw(dev));
	return ret;
}

static void mt76x0e_cleanup(struct mt76x02_dev *dev)
{
	clear_bit(MT76_STATE_INITIALIZED, &dev->mt76.state);
	mt76x0_chip_onoff(dev, false, false);
	mt76x0e_stop_hw(dev);
	mt76x02_dma_cleanup(dev);
	mt76x02_mcu_cleanup(dev);
}

static void
mt76x0e_remove(struct pci_dev *pdev)
{
	struct mt76_dev *mdev = pci_get_drvdata(pdev);
	struct mt76x02_dev *dev = container_of(mdev, struct mt76x02_dev, mt76);

	mt76_unregister_device(mdev);
	mt76x0e_cleanup(dev);
	ieee80211_free_hw(mdev->hw);
}

static const struct pci_device_id mt76x0e_device_table[] = {
	{ PCI_DEVICE(0x14c3, 0x7630) },
	{ PCI_DEVICE(0x14c3, 0x7650) },
	{ },
};

MODULE_DEVICE_TABLE(pci, mt76x0e_device_table);
MODULE_LICENSE("Dual BSD/GPL");

static struct pci_driver mt76x0e_driver = {
	.name		= KBUILD_MODNAME,
	.id_table	= mt76x0e_device_table,
	.probe		= mt76x0e_probe,
	.remove		= mt76x0e_remove,
};

module_pci_driver(mt76x0e_driver);

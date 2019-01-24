/*
 * Copyright (C) 2018 Stanislaw Gruszka <stf_xl@wp.pl>
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

#include <linux/module.h>
#include "mt76x02.h"

#define CCK_RATE(_idx, _rate) {					\
	.bitrate = _rate,					\
	.flags = IEEE80211_RATE_SHORT_PREAMBLE,			\
	.hw_value = (MT_PHY_TYPE_CCK << 8) | _idx,		\
	.hw_value_short = (MT_PHY_TYPE_CCK << 8) | (8 + _idx),	\
}

#define OFDM_RATE(_idx, _rate) {				\
	.bitrate = _rate,					\
	.hw_value = (MT_PHY_TYPE_OFDM << 8) | _idx,		\
	.hw_value_short = (MT_PHY_TYPE_OFDM << 8) | _idx,	\
}

struct ieee80211_rate mt76x02_rates[] = {
	CCK_RATE(0, 10),
	CCK_RATE(1, 20),
	CCK_RATE(2, 55),
	CCK_RATE(3, 110),
	OFDM_RATE(0, 60),
	OFDM_RATE(1, 90),
	OFDM_RATE(2, 120),
	OFDM_RATE(3, 180),
	OFDM_RATE(4, 240),
	OFDM_RATE(5, 360),
	OFDM_RATE(6, 480),
	OFDM_RATE(7, 540),
};
EXPORT_SYMBOL_GPL(mt76x02_rates);

static const struct ieee80211_iface_limit mt76x02_if_limits[] = {
	{
		.max = 1,
		.types = BIT(NL80211_IFTYPE_ADHOC)
	}, {
		.max = 8,
		.types = BIT(NL80211_IFTYPE_STATION) |
#ifdef CONFIG_MAC80211_MESH
			 BIT(NL80211_IFTYPE_MESH_POINT) |
#endif
			 BIT(NL80211_IFTYPE_AP)
	 },
};

static const struct ieee80211_iface_combination mt76x02_if_comb[] = {
	{
		.limits = mt76x02_if_limits,
		.n_limits = ARRAY_SIZE(mt76x02_if_limits),
		.max_interfaces = 8,
		.num_different_channels = 1,
		.beacon_int_infra_match = true,
		.radar_detect_widths = BIT(NL80211_CHAN_WIDTH_20_NOHT) |
				       BIT(NL80211_CHAN_WIDTH_20) |
				       BIT(NL80211_CHAN_WIDTH_40) |
				       BIT(NL80211_CHAN_WIDTH_80),
	}
};

void mt76x02_init_device(struct mt76x02_dev *dev)
{
	struct ieee80211_hw *hw = mt76_hw(dev);
	struct wiphy *wiphy = hw->wiphy;

	INIT_DELAYED_WORK(&dev->mac_work, mt76x02_mac_work);

	hw->queues = 4;
	hw->max_rates = 1;
	hw->max_report_rates = 7;
	hw->max_rate_tries = 1;
	hw->extra_tx_headroom = 2;

	if (mt76_is_usb(dev)) {
		hw->extra_tx_headroom += sizeof(struct mt76x02_txwi) +
					 MT_DMA_HDR_LEN;
		wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	} else {
		mt76x02_dfs_init_detector(dev);

		wiphy->reg_notifier = mt76x02_regd_notifier;
		wiphy->iface_combinations = mt76x02_if_comb;
		wiphy->n_iface_combinations = ARRAY_SIZE(mt76x02_if_comb);
		wiphy->interface_modes =
			BIT(NL80211_IFTYPE_STATION) |
			BIT(NL80211_IFTYPE_AP) |
#ifdef CONFIG_MAC80211_MESH
			BIT(NL80211_IFTYPE_MESH_POINT) |
#endif
			BIT(NL80211_IFTYPE_ADHOC);

		wiphy_ext_feature_set(wiphy, NL80211_EXT_FEATURE_VHT_IBSS);
	}

	hw->sta_data_size = sizeof(struct mt76x02_sta);
	hw->vif_data_size = sizeof(struct mt76x02_vif);

	ieee80211_hw_set(hw, SUPPORTS_HT_CCK_RATES);
	ieee80211_hw_set(hw, SUPPORTS_REORDERING_BUFFER);

	dev->mt76.global_wcid.idx = 255;
	dev->mt76.global_wcid.hw_key_idx = -1;
	dev->slottime = 9;

	if (is_mt76x2(dev)) {
		dev->mt76.sband_2g.sband.ht_cap.cap |=
				IEEE80211_HT_CAP_LDPC_CODING;
		dev->mt76.sband_5g.sband.ht_cap.cap |=
				IEEE80211_HT_CAP_LDPC_CODING;
		dev->mt76.chainmask = 0x202;
		dev->mt76.antenna_mask = 3;
	} else {
		dev->mt76.chainmask = 0x101;
		dev->mt76.antenna_mask = 1;
	}
}
EXPORT_SYMBOL_GPL(mt76x02_init_device);

void mt76x02_configure_filter(struct ieee80211_hw *hw,
			      unsigned int changed_flags,
			      unsigned int *total_flags, u64 multicast)
{
	struct mt76x02_dev *dev = hw->priv;
	u32 flags = 0;

#define MT76_FILTER(_flag, _hw) do { \
		flags |= *total_flags & FIF_##_flag;			\
		dev->mt76.rxfilter &= ~(_hw);				\
		dev->mt76.rxfilter |= !(flags & FIF_##_flag) * (_hw);	\
	} while (0)

	mutex_lock(&dev->mt76.mutex);

	dev->mt76.rxfilter &= ~MT_RX_FILTR_CFG_OTHER_BSS;

	MT76_FILTER(FCSFAIL, MT_RX_FILTR_CFG_CRC_ERR);
	MT76_FILTER(PLCPFAIL, MT_RX_FILTR_CFG_PHY_ERR);
	MT76_FILTER(CONTROL, MT_RX_FILTR_CFG_ACK |
			     MT_RX_FILTR_CFG_CTS |
			     MT_RX_FILTR_CFG_CFEND |
			     MT_RX_FILTR_CFG_CFACK |
			     MT_RX_FILTR_CFG_BA |
			     MT_RX_FILTR_CFG_CTRL_RSV);
	MT76_FILTER(PSPOLL, MT_RX_FILTR_CFG_PSPOLL);

	*total_flags = flags;
	mt76_wr(dev, MT_RX_FILTR_CFG, dev->mt76.rxfilter);

	mutex_unlock(&dev->mt76.mutex);
}
EXPORT_SYMBOL_GPL(mt76x02_configure_filter);

int mt76x02_sta_add(struct mt76_dev *mdev, struct ieee80211_vif *vif,
		    struct ieee80211_sta *sta)
{
	struct mt76x02_dev *dev = container_of(mdev, struct mt76x02_dev, mt76);
	struct mt76x02_sta *msta = (struct mt76x02_sta *)sta->drv_priv;
	struct mt76x02_vif *mvif = (struct mt76x02_vif *)vif->drv_priv;
	int idx = 0;

	idx = mt76_wcid_alloc(dev->mt76.wcid_mask, ARRAY_SIZE(dev->mt76.wcid));
	if (idx < 0)
		return -ENOSPC;

	msta->vif = mvif;
	msta->wcid.sta = 1;
	msta->wcid.idx = idx;
	msta->wcid.hw_key_idx = -1;
	mt76x02_mac_wcid_setup(dev, idx, mvif->idx, sta->addr);
	mt76x02_mac_wcid_set_drop(dev, idx, false);

	if (vif->type == NL80211_IFTYPE_AP)
		set_bit(MT_WCID_FLAG_CHECK_PS, &msta->wcid.flags);

	ewma_signal_init(&msta->rssi);

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_sta_add);

void mt76x02_sta_remove(struct mt76_dev *mdev, struct ieee80211_vif *vif,
			struct ieee80211_sta *sta)
{
	struct mt76x02_dev *dev = container_of(mdev, struct mt76x02_dev, mt76);
	struct mt76_wcid *wcid = (struct mt76_wcid *)sta->drv_priv;
	int idx = wcid->idx;

	mt76x02_mac_wcid_set_drop(dev, idx, true);
	mt76x02_mac_wcid_setup(dev, idx, 0, NULL);
}
EXPORT_SYMBOL_GPL(mt76x02_sta_remove);

void mt76x02_vif_init(struct mt76x02_dev *dev, struct ieee80211_vif *vif,
		      unsigned int idx)
{
	struct mt76x02_vif *mvif = (struct mt76x02_vif *)vif->drv_priv;
	struct mt76_txq *mtxq;

	mvif->idx = idx;
	mvif->group_wcid.idx = MT_VIF_WCID(idx);
	mvif->group_wcid.hw_key_idx = -1;
	mtxq = (struct mt76_txq *) vif->txq->drv_priv;
	mtxq->wcid = &mvif->group_wcid;

	mt76_txq_init(&dev->mt76, vif->txq);
}
EXPORT_SYMBOL_GPL(mt76x02_vif_init);

int
mt76x02_add_interface(struct ieee80211_hw *hw, struct ieee80211_vif *vif)
{
	struct mt76x02_dev *dev = hw->priv;
	unsigned int idx = 0;

	if (vif->addr[0] & BIT(1))
		idx = 1 + (((dev->mt76.macaddr[0] ^ vif->addr[0]) >> 2) & 7);

	/*
	 * Client mode typically only has one configurable BSSID register,
	 * which is used for bssidx=0. This is linked to the MAC address.
	 * Since mac80211 allows changing interface types, and we cannot
	 * force the use of the primary MAC address for a station mode
	 * interface, we need some other way of configuring a per-interface
	 * remote BSSID.
	 * The hardware provides an AP-Client feature, where bssidx 0-7 are
	 * used for AP mode and bssidx 8-15 for client mode.
	 * We shift the station interface bss index by 8 to force the
	 * hardware to recognize the BSSID.
	 * The resulting bssidx mismatch for unicast frames is ignored by hw.
	 */
	if (vif->type == NL80211_IFTYPE_STATION)
		idx += 8;

	mt76x02_vif_init(dev, vif, idx);
	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_add_interface);

void mt76x02_remove_interface(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif)
{
	struct mt76x02_dev *dev = hw->priv;

	mt76_txq_remove(&dev->mt76, vif->txq);
}
EXPORT_SYMBOL_GPL(mt76x02_remove_interface);

int mt76x02_ampdu_action(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			 struct ieee80211_ampdu_params *params)
{
	enum ieee80211_ampdu_mlme_action action = params->action;
	struct ieee80211_sta *sta = params->sta;
	struct mt76x02_dev *dev = hw->priv;
	struct mt76x02_sta *msta = (struct mt76x02_sta *) sta->drv_priv;
	struct ieee80211_txq *txq = sta->txq[params->tid];
	u16 tid = params->tid;
	u16 *ssn = &params->ssn;
	struct mt76_txq *mtxq;

	if (!txq)
		return -EINVAL;

	mtxq = (struct mt76_txq *)txq->drv_priv;

	switch (action) {
	case IEEE80211_AMPDU_RX_START:
		mt76_rx_aggr_start(&dev->mt76, &msta->wcid, tid,
				   *ssn, params->buf_size);
		mt76_set(dev, MT_WCID_ADDR(msta->wcid.idx) + 4, BIT(16 + tid));
		break;
	case IEEE80211_AMPDU_RX_STOP:
		mt76_rx_aggr_stop(&dev->mt76, &msta->wcid, tid);
		mt76_clear(dev, MT_WCID_ADDR(msta->wcid.idx) + 4,
			   BIT(16 + tid));
		break;
	case IEEE80211_AMPDU_TX_OPERATIONAL:
		mtxq->aggr = true;
		mtxq->send_bar = false;
		ieee80211_send_bar(vif, sta->addr, tid, mtxq->agg_ssn);
		break;
	case IEEE80211_AMPDU_TX_STOP_FLUSH:
	case IEEE80211_AMPDU_TX_STOP_FLUSH_CONT:
		mtxq->aggr = false;
		ieee80211_send_bar(vif, sta->addr, tid, mtxq->agg_ssn);
		break;
	case IEEE80211_AMPDU_TX_START:
		mtxq->agg_ssn = *ssn << 4;
		ieee80211_start_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	case IEEE80211_AMPDU_TX_STOP_CONT:
		mtxq->aggr = false;
		ieee80211_stop_tx_ba_cb_irqsafe(vif, sta->addr, tid);
		break;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_ampdu_action);

int mt76x02_set_key(struct ieee80211_hw *hw, enum set_key_cmd cmd,
		    struct ieee80211_vif *vif, struct ieee80211_sta *sta,
		    struct ieee80211_key_conf *key)
{
	struct mt76x02_dev *dev = hw->priv;
	struct mt76x02_vif *mvif = (struct mt76x02_vif *)vif->drv_priv;
	struct mt76x02_sta *msta;
	struct mt76_wcid *wcid;
	int idx = key->keyidx;
	int ret;

	/* fall back to sw encryption for unsupported ciphers */
	switch (key->cipher) {
	case WLAN_CIPHER_SUITE_WEP40:
	case WLAN_CIPHER_SUITE_WEP104:
	case WLAN_CIPHER_SUITE_TKIP:
	case WLAN_CIPHER_SUITE_CCMP:
		break;
	default:
		return -EOPNOTSUPP;
	}

	/*
	 * The hardware does not support per-STA RX GTK, fall back
	 * to software mode for these.
	 */
	if ((vif->type == NL80211_IFTYPE_ADHOC ||
	     vif->type == NL80211_IFTYPE_MESH_POINT) &&
	    (key->cipher == WLAN_CIPHER_SUITE_TKIP ||
	     key->cipher == WLAN_CIPHER_SUITE_CCMP) &&
	    !(key->flags & IEEE80211_KEY_FLAG_PAIRWISE))
		return -EOPNOTSUPP;

	msta = sta ? (struct mt76x02_sta *) sta->drv_priv : NULL;
	wcid = msta ? &msta->wcid : &mvif->group_wcid;

	if (cmd == SET_KEY) {
		key->hw_key_idx = wcid->idx;
		wcid->hw_key_idx = idx;
		if (key->flags & IEEE80211_KEY_FLAG_RX_MGMT) {
			key->flags |= IEEE80211_KEY_FLAG_SW_MGMT_TX;
			wcid->sw_iv = true;
		}
	} else {
		if (idx == wcid->hw_key_idx) {
			wcid->hw_key_idx = -1;
			wcid->sw_iv = true;
		}

		key = NULL;
	}
	mt76_wcid_key_setup(&dev->mt76, wcid, key);

	if (!msta) {
		if (key || wcid->hw_key_idx == idx) {
			ret = mt76x02_mac_wcid_set_key(dev, wcid->idx, key);
			if (ret)
				return ret;
		}

		return mt76x02_mac_shared_key_setup(dev, mvif->idx, idx, key);
	}

	return mt76x02_mac_wcid_set_key(dev, msta->wcid.idx, key);
}
EXPORT_SYMBOL_GPL(mt76x02_set_key);

int mt76x02_conf_tx(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		    u16 queue, const struct ieee80211_tx_queue_params *params)
{
	struct mt76x02_dev *dev = hw->priv;
	u8 cw_min = 5, cw_max = 10, qid;
	u32 val;

	qid = dev->mt76.q_tx[queue].hw_idx;

	if (params->cw_min)
		cw_min = fls(params->cw_min);
	if (params->cw_max)
		cw_max = fls(params->cw_max);

	val = FIELD_PREP(MT_EDCA_CFG_TXOP, params->txop) |
	      FIELD_PREP(MT_EDCA_CFG_AIFSN, params->aifs) |
	      FIELD_PREP(MT_EDCA_CFG_CWMIN, cw_min) |
	      FIELD_PREP(MT_EDCA_CFG_CWMAX, cw_max);
	mt76_wr(dev, MT_EDCA_CFG_AC(qid), val);

	val = mt76_rr(dev, MT_WMM_TXOP(qid));
	val &= ~(MT_WMM_TXOP_MASK << MT_WMM_TXOP_SHIFT(qid));
	val |= params->txop << MT_WMM_TXOP_SHIFT(qid);
	mt76_wr(dev, MT_WMM_TXOP(qid), val);

	val = mt76_rr(dev, MT_WMM_AIFSN);
	val &= ~(MT_WMM_AIFSN_MASK << MT_WMM_AIFSN_SHIFT(qid));
	val |= params->aifs << MT_WMM_AIFSN_SHIFT(qid);
	mt76_wr(dev, MT_WMM_AIFSN, val);

	val = mt76_rr(dev, MT_WMM_CWMIN);
	val &= ~(MT_WMM_CWMIN_MASK << MT_WMM_CWMIN_SHIFT(qid));
	val |= cw_min << MT_WMM_CWMIN_SHIFT(qid);
	mt76_wr(dev, MT_WMM_CWMIN, val);

	val = mt76_rr(dev, MT_WMM_CWMAX);
	val &= ~(MT_WMM_CWMAX_MASK << MT_WMM_CWMAX_SHIFT(qid));
	val |= cw_max << MT_WMM_CWMAX_SHIFT(qid);
	mt76_wr(dev, MT_WMM_CWMAX, val);

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_conf_tx);

void mt76x02_set_tx_ackto(struct mt76x02_dev *dev)
{
	u8 ackto, sifs, slottime = dev->slottime;

	/* As defined by IEEE 802.11-2007 17.3.8.6 */
	slottime += 3 * dev->coverage_class;
	mt76_rmw_field(dev, MT_BKOFF_SLOT_CFG,
		       MT_BKOFF_SLOT_CFG_SLOTTIME, slottime);

	sifs = mt76_get_field(dev, MT_XIFS_TIME_CFG,
			      MT_XIFS_TIME_CFG_OFDM_SIFS);

	ackto = slottime + sifs;
	mt76_rmw_field(dev, MT_TX_TIMEOUT_CFG,
		       MT_TX_TIMEOUT_CFG_ACKTO, ackto);
}
EXPORT_SYMBOL_GPL(mt76x02_set_tx_ackto);

void mt76x02_set_coverage_class(struct ieee80211_hw *hw,
				s16 coverage_class)
{
	struct mt76x02_dev *dev = hw->priv;

	mutex_lock(&dev->mt76.mutex);
	dev->coverage_class = coverage_class;
	mt76x02_set_tx_ackto(dev);
	mutex_unlock(&dev->mt76.mutex);
}
EXPORT_SYMBOL_GPL(mt76x02_set_coverage_class);

int mt76x02_set_rts_threshold(struct ieee80211_hw *hw, u32 val)
{
	struct mt76x02_dev *dev = hw->priv;

	if (val != ~0 && val > 0xffff)
		return -EINVAL;

	mutex_lock(&dev->mt76.mutex);
	mt76x02_mac_set_tx_protection(dev, val);
	mutex_unlock(&dev->mt76.mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_set_rts_threshold);

void mt76x02_sta_rate_tbl_update(struct ieee80211_hw *hw,
				struct ieee80211_vif *vif,
				struct ieee80211_sta *sta)
{
	struct mt76x02_dev *dev = hw->priv;
	struct mt76x02_sta *msta = (struct mt76x02_sta *) sta->drv_priv;
	struct ieee80211_sta_rates *rates = rcu_dereference(sta->rates);
	struct ieee80211_tx_rate rate = {};

	if (!rates)
		return;

	rate.idx = rates->rate[0].idx;
	rate.flags = rates->rate[0].flags;
	mt76x02_mac_wcid_set_rate(dev, &msta->wcid, &rate);
	msta->wcid.max_txpwr_adj = mt76x02_tx_get_max_txpwr_adj(dev, &rate);
}
EXPORT_SYMBOL_GPL(mt76x02_sta_rate_tbl_update);

int mt76x02_insert_hdr_pad(struct sk_buff *skb)
{
	int len = ieee80211_get_hdrlen_from_skb(skb);

	if (len % 4 == 0)
		return 0;

	skb_push(skb, 2);
	memmove(skb->data, skb->data + 2, len);

	skb->data[len] = 0;
	skb->data[len + 1] = 0;
	return 2;
}
EXPORT_SYMBOL_GPL(mt76x02_insert_hdr_pad);

void mt76x02_remove_hdr_pad(struct sk_buff *skb, int len)
{
	int hdrlen;

	if (!len)
		return;

	hdrlen = ieee80211_get_hdrlen_from_skb(skb);
	memmove(skb->data + len, skb->data, hdrlen);
	skb_pull(skb, len);
}
EXPORT_SYMBOL_GPL(mt76x02_remove_hdr_pad);

void mt76x02_sw_scan(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
		     const u8 *mac)
{
	struct mt76x02_dev *dev = hw->priv;

	if (mt76_is_mmio(dev))
		tasklet_disable(&dev->pre_tbtt_tasklet);
	set_bit(MT76_SCANNING, &dev->mt76.state);
}
EXPORT_SYMBOL_GPL(mt76x02_sw_scan);

void mt76x02_sw_scan_complete(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif)
{
	struct mt76x02_dev *dev = hw->priv;

	clear_bit(MT76_SCANNING, &dev->mt76.state);
	if (mt76_is_mmio(dev))
		tasklet_enable(&dev->pre_tbtt_tasklet);

	if (dev->cal.gain_init_done) {
		/* Restore AGC gain and resume calibration after scanning. */
		dev->cal.low_gain = -1;
		ieee80211_queue_delayed_work(hw, &dev->cal_work, 0);
	}
}
EXPORT_SYMBOL_GPL(mt76x02_sw_scan_complete);

int mt76x02_get_txpower(struct ieee80211_hw *hw,
			struct ieee80211_vif *vif, int *dbm)
{
	struct mt76x02_dev *dev = hw->priv;
	u8 nstreams = dev->mt76.chainmask & 0xf;

	*dbm = dev->mt76.txpower_cur / 2;

	/* convert from per-chain power to combined
	 * output on 2x2 devices
	 */
	if (nstreams > 1)
		*dbm += 3;

	return 0;
}
EXPORT_SYMBOL_GPL(mt76x02_get_txpower);

void mt76x02_sta_ps(struct mt76_dev *mdev, struct ieee80211_sta *sta,
		    bool ps)
{
	struct mt76x02_dev *dev = container_of(mdev, struct mt76x02_dev, mt76);
	struct mt76x02_sta *msta = (struct mt76x02_sta *)sta->drv_priv;
	int idx = msta->wcid.idx;

	mt76_stop_tx_queues(&dev->mt76, sta, true);
	mt76x02_mac_wcid_set_drop(dev, idx, ps);
}
EXPORT_SYMBOL_GPL(mt76x02_sta_ps);

const u16 mt76x02_beacon_offsets[16] = {
	/* 1024 byte per beacon */
	0xc000,
	0xc400,
	0xc800,
	0xcc00,
	0xd000,
	0xd400,
	0xd800,
	0xdc00,
	/* BSS idx 8-15 not used for beacons */
	0xc000,
	0xc000,
	0xc000,
	0xc000,
	0xc000,
	0xc000,
	0xc000,
	0xc000,
};

static void mt76x02_set_beacon_offsets(struct mt76x02_dev *dev)
{
	u16 val, base = MT_BEACON_BASE;
	u32 regs[4] = {};
	int i;

	for (i = 0; i < 16; i++) {
		val = mt76x02_beacon_offsets[i] - base;
		regs[i / 4] |= (val / 64) << (8 * (i % 4));
	}

	for (i = 0; i < 4; i++)
		mt76_wr(dev, MT_BCN_OFFSET(i), regs[i]);
}

void mt76x02_init_beacon_config(struct mt76x02_dev *dev)
{
	static const u8 null_addr[ETH_ALEN] = {};
	int i;

	mt76_wr(dev, MT_MAC_BSSID_DW0,
		get_unaligned_le32(dev->mt76.macaddr));
	mt76_wr(dev, MT_MAC_BSSID_DW1,
		get_unaligned_le16(dev->mt76.macaddr + 4) |
		FIELD_PREP(MT_MAC_BSSID_DW1_MBSS_MODE, 3) | /* 8 beacons */
		MT_MAC_BSSID_DW1_MBSS_LOCAL_BIT);

	/* Fire a pre-TBTT interrupt 8 ms before TBTT */
	mt76_rmw_field(dev, MT_INT_TIMER_CFG, MT_INT_TIMER_CFG_PRE_TBTT,
		       8 << 4);
	mt76_rmw_field(dev, MT_INT_TIMER_CFG, MT_INT_TIMER_CFG_GP_TIMER,
		       MT_DFS_GP_INTERVAL);
	mt76_wr(dev, MT_INT_TIMER_EN, 0);

	mt76_wr(dev, MT_BCN_BYPASS_MASK, 0xffff);

	for (i = 0; i < 8; i++) {
		mt76x02_mac_set_bssid(dev, i, null_addr);
		mt76x02_mac_set_beacon(dev, i, NULL);
	}
	mt76x02_set_beacon_offsets(dev);
}
EXPORT_SYMBOL_GPL(mt76x02_init_beacon_config);

void mt76x02_bss_info_changed(struct ieee80211_hw *hw,
			      struct ieee80211_vif *vif,
			      struct ieee80211_bss_conf *info,
			      u32 changed)
{
	struct mt76x02_vif *mvif = (struct mt76x02_vif *)vif->drv_priv;
	struct mt76x02_dev *dev = hw->priv;

	mutex_lock(&dev->mt76.mutex);

	if (changed & BSS_CHANGED_BSSID)
		mt76x02_mac_set_bssid(dev, mvif->idx, info->bssid);

	if (changed & BSS_CHANGED_BEACON_ENABLED) {
		tasklet_disable(&dev->pre_tbtt_tasklet);
		mt76x02_mac_set_beacon_enable(dev, mvif->idx,
					      info->enable_beacon);
		tasklet_enable(&dev->pre_tbtt_tasklet);
	}

	if (changed & BSS_CHANGED_BEACON_INT) {
		mt76_rmw_field(dev, MT_BEACON_TIME_CFG,
			       MT_BEACON_TIME_CFG_INTVAL,
			       info->beacon_int << 4);
		dev->beacon_int = info->beacon_int;
		dev->tbtt_count = 0;
	}

	if (changed & BSS_CHANGED_ERP_PREAMBLE)
		mt76x02_mac_set_short_preamble(dev, info->use_short_preamble);

	if (changed & BSS_CHANGED_ERP_SLOT) {
		int slottime = info->use_short_slot ? 9 : 20;

		dev->slottime = slottime;
		mt76x02_set_tx_ackto(dev);
	}

	mutex_unlock(&dev->mt76.mutex);
}
EXPORT_SYMBOL_GPL(mt76x02_bss_info_changed);

void mt76x02_config_mac_addr_list(struct mt76x02_dev *dev)
{
	struct ieee80211_hw *hw = mt76_hw(dev);
	struct wiphy *wiphy = hw->wiphy;
	int i;

	for (i = 0; i < ARRAY_SIZE(dev->macaddr_list); i++) {
		u8 *addr = dev->macaddr_list[i].addr;

		memcpy(addr, dev->mt76.macaddr, ETH_ALEN);

		if (!i)
			continue;

		addr[0] |= BIT(1);
		addr[0] ^= ((i - 1) << 2);
	}
	wiphy->addresses = dev->macaddr_list;
	wiphy->n_addresses = ARRAY_SIZE(dev->macaddr_list);
}
EXPORT_SYMBOL_GPL(mt76x02_config_mac_addr_list);

MODULE_LICENSE("Dual BSD/GPL");

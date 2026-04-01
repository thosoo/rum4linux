/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/etherdevice.h>
#include <linux/ratelimit.h>
#include <net/mac80211.h>
#include "rum4linux_hw.h"
#include "rum4linux_debug.h"
#include "rum4linux_tx.h"
#include "rum4linux_rx.h"
#include "rum4linux_eeprom.h"

static bool bind;
module_param(bind, bool, 0644);
MODULE_PARM_DESC(bind, "Actually bind to currently enumerated rum(4)-family IDs (default: false)");

static struct ieee80211_rate dwr_rates_2ghz[] = {
	{ .bitrate = 10,  .hw_value = 0 },
	{ .bitrate = 20,  .hw_value = 1 },
	{ .bitrate = 55,  .hw_value = 2 },
	{ .bitrate = 110, .hw_value = 3 },
};

#define DWR_CHAN(_idx, _freq) { .band = NL80211_BAND_2GHZ, .center_freq = (_freq), .hw_value = (_idx), .max_power = 20 }
static struct ieee80211_channel dwr_channels_2ghz[] = {
	DWR_CHAN(1, 2412), DWR_CHAN(2, 2417), DWR_CHAN(3, 2422),
	DWR_CHAN(4, 2427), DWR_CHAN(5, 2432), DWR_CHAN(6, 2437),
	DWR_CHAN(7, 2442), DWR_CHAN(8, 2447), DWR_CHAN(9, 2452),
	DWR_CHAN(10, 2457), DWR_CHAN(11, 2462), DWR_CHAN(12, 2467),
	DWR_CHAN(13, 2472), DWR_CHAN(14, 2484),
};
#undef DWR_CHAN

static struct ieee80211_supported_band dwr_band_2ghz = {
	.band = NL80211_BAND_2GHZ,
	.channels = dwr_channels_2ghz,
	.n_channels = ARRAY_SIZE(dwr_channels_2ghz),
	.bitrates = dwr_rates_2ghz,
	.n_bitrates = ARRAY_SIZE(dwr_rates_2ghz),
};

static void dwr_leave_run_state(struct dwr_dev *dwr, const char *reason);
static void dwr_enter_run_state(struct dwr_dev *dwr,
				struct ieee80211_bss_conf *info);
static void dwr_log_sta_rx_counters(struct dwr_dev *dwr, const char *reason);

static int dwr_detect_endpoints(struct dwr_dev *dwr)
{
	struct usb_host_interface *alts = dwr->usb.intf->cur_altsetting;
	int i;

	dwr->usb.bulk_in_ep = 0;
	dwr->usb.bulk_out_ep = 0;
	dwr->usb.intr_ep = 0;
	dwr->usb.bulk_in_maxp = 0;
	dwr->usb.bulk_out_maxp = 0;
	dwr->usb.intr_maxp = 0;

	dwr_info(&dwr->usb.intf->dev,
		 "probe: iface=%u alt=%u class=0x%02x eps=%u\n",
		 alts->desc.bInterfaceNumber,
		 alts->desc.bAlternateSetting,
		 alts->desc.bInterfaceClass,
		 alts->desc.bNumEndpoints);

	for (i = 0; i < alts->desc.bNumEndpoints; i++) {
		struct usb_endpoint_descriptor *ep = &alts->endpoint[i].desc;
		u8 addr = ep->bEndpointAddress;
		u16 maxp = usb_endpoint_maxp(ep);

		dwr_info(&dwr->usb.intf->dev,
			 "endpoint[%d]: addr=0x%02x attr=0x%02x maxp=%u interval=%u\n",
			 i, addr, ep->bmAttributes, maxp, ep->bInterval);

		if (usb_endpoint_is_bulk_in(ep) && !dwr->usb.bulk_in_ep) {
			dwr->usb.bulk_in_ep = addr;
			dwr->usb.bulk_in_maxp = maxp;
		} else if (usb_endpoint_is_bulk_out(ep) && !dwr->usb.bulk_out_ep) {
			dwr->usb.bulk_out_ep = addr;
			dwr->usb.bulk_out_maxp = maxp;
		} else if (usb_endpoint_is_int_in(ep) && !dwr->usb.intr_ep) {
			dwr->usb.intr_ep = addr;
			dwr->usb.intr_maxp = maxp;
		}
	}

	if (!dwr->usb.bulk_in_ep || !dwr->usb.bulk_out_ep) {
		dwr_err(&dwr->usb.intf->dev,
			"missing required bulk endpoints (bulk-in=0x%02x bulk-out=0x%02x)\n",
			dwr->usb.bulk_in_ep, dwr->usb.bulk_out_ep);
		return -ENODEV;
	}

	dwr_info(&dwr->usb.intf->dev,
		 "selected endpoints: bulk-in=0x%02x(%u) bulk-out=0x%02x(%u) intr=0x%02x(%u)\n",
		 dwr->usb.bulk_in_ep, dwr->usb.bulk_in_maxp,
		 dwr->usb.bulk_out_ep, dwr->usb.bulk_out_maxp,
		 dwr->usb.intr_ep, dwr->usb.intr_maxp);
	return 0;
}

static void dwr_reset_work(struct work_struct *work)
{
	/* TODO(openbsd-rum-port): device reset/reinit path after USB or MCU fault. */
}

static void dwr_link_tuner_workfn(struct work_struct *work)
{
	struct dwr_dev *dwr =
		container_of(to_delayed_work(work), struct dwr_dev, link_tuner_work);
	u16 fcs_err = 0, plcp_err = 0, physical_err = 0, false_cca = 0;
	s8 rssi;
	bool have_rssi;
	u8 low_bound, up_bound, next_vgc;

	if (!READ_ONCE(dwr->usb.running))
		return;

	if (dwr_read_rx_error_counters(dwr, &fcs_err, &plcp_err, &physical_err, &false_cca))
		goto reschedule;
	(void)fcs_err;
	(void)plcp_err;
	(void)physical_err;

	rssi = READ_ONCE(dwr->link_rssi_dbm);
	have_rssi = rssi != DWR_LINK_RSSI_INVALID_DBM;
	if (!have_rssi) {
		low_bound = dwr->bbp17_base - 0x04;
		up_bound = dwr->bbp17_base;
	} else if (rssi > -82) {
		low_bound = dwr->bbp17_base - 0x04;
		up_bound = dwr->bbp17_base + 0x20;
	} else if (rssi > -84) {
		low_bound = dwr->bbp17_base - 0x04;
		up_bound = dwr->bbp17_base;
	} else {
		low_bound = dwr->bbp17_base - 0x04;
		up_bound = dwr->bbp17_base - 0x04;
	}

	next_vgc = dwr->vgc_level;
	if (dwr->associated && have_rssi) {
		if (rssi > -35)
			next_vgc = 0x60;
		else if (rssi >= -58)
			next_vgc = up_bound;
		else if (rssi >= -66)
			next_vgc = low_bound + 0x10;
		else if (rssi >= -74)
			next_vgc = low_bound + 0x08;
	}

	if (next_vgc <= up_bound && false_cca > 512 && next_vgc < up_bound)
		next_vgc = min_t(u8, next_vgc + 4, up_bound);
	else if (false_cca < 100 && next_vgc > low_bound)
		next_vgc = max_t(u8, next_vgc - 4, low_bound);

	(void)dwr_set_vgc(dwr, next_vgc);
	/* TODO(openbsd-rum-port): FCS/plcp counters are collected for observability only; no additional control policy yet. */

reschedule:
	if (READ_ONCE(dwr->usb.running) && dwr->associated)
		schedule_delayed_work(&dwr->link_tuner_work, msecs_to_jiffies(2000));
}

static int dwr_mac_start(struct ieee80211_hw *hw)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);
	int ret;

	dwr_info(&dwr->usb.intf->dev, "mac80211 start\n");
	ret = dwr_hw_init(dwr);
	if (ret)
		return ret;
	ret = dwr_set_macaddr(dwr, dwr->mac_addr);
	if (ret)
		return ret;
	ret = dwr_set_rx_filter(dwr, dwr->filter_flags);
	if (ret)
		return ret;

	dwr->usb.running = true;
	ret = dwr_rx_start(dwr);
	if (ret) {
		dwr->usb.running = false;
		dwr_hw_stop(dwr);
		return ret;
	}
	return 0;
}

static void dwr_mac_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	dwr_info(&dwr->usb.intf->dev, "mac80211 stop suspend=%d\n", suspend);
	dwr->usb.running = false;
	dwr_leave_run_state(dwr, "stop");
	dwr_rx_stop(dwr);
	dwr_rx_log_summary(dwr, "mac_stop");
	dwr_log_channel_apply_summary(dwr, "mac_stop");
	dwr_log_sta_rx_counters(dwr, "mac_stop");
	dwr_tx_cancel_pending(dwr);
	dwr_hw_stop(dwr);
	cancel_work_sync(&dwr->reset_work);
}

static void dwr_mac_tx(struct ieee80211_hw *hw,
		       struct ieee80211_tx_control *control,
		       struct sk_buff *skb)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);
	bool ownership_transferred;
	int ret;

	ret = dwr_tx_submit_frame(dwr, skb, &ownership_transferred);
	if (ret && __ratelimit(&net_ratelimit_state))
		dwr_warn(&dwr->usb.intf->dev, "tx blocked len=%u err=%d\n", skb->len, ret);
	if (!ownership_transferred)
		ieee80211_free_txskb(hw, skb);
}

static int dwr_mac_config(struct ieee80211_hw *hw, u32 changed)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);
	struct ieee80211_conf *conf = &hw->conf;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL)
		return dwr_set_channel(dwr, conf->chandef.chan);
	return 0;
}

static void dwr_update_assoc_aid(struct dwr_dev *dwr, bool associated, u16 aid)
{
	dwr->associated = associated;
	dwr->aid = associated ? aid : 0;
}

static void dwr_leave_run_state(struct dwr_dev *dwr, const char *reason)
{
	int ret;

	dwr_update_assoc_aid(dwr, false, 0);
	WRITE_ONCE(dwr->link_rssi_dbm, DWR_LINK_RSSI_INVALID_DBM);
	cancel_delayed_work_sync(&dwr->link_tuner_work);
	ret = dwr_abort_tsf_sync(dwr);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "abort tsf sync failed (%s): %d\n", reason, ret);
	ret = dwr_clear_bssid(dwr);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "clear bssid failed (%s): %d\n", reason, ret);
}

static void dwr_enter_run_state(struct dwr_dev *dwr, struct ieee80211_bss_conf *info)
{
	struct ieee80211_channel *chan = dwr->hw->conf.chandef.chan;
	u8 short_retry = dwr->hw->conf.short_frame_max_tx_count;
	u8 long_retry = dwr->hw->conf.long_frame_max_tx_count;
	int ret;

	if (!short_retry)
		short_retry = 7;
	if (!long_retry)
		long_retry = 7;

	if (chan) {
		ret = dwr_set_channel(dwr, chan);
		if (ret)
			dwr_dbg(&dwr->usb.intf->dev, "run enter set channel failed: %d\n", ret);
	}

	ret = dwr_set_erp_timing(dwr, info->use_short_preamble,
				 info->use_short_slot ?
				 DWR_RT2573_SLOT_TIME_SHORT :
				 DWR_RT2573_SLOT_TIME_LONG,
				 DWR_RT2573_MAC_CSR8_SIFS_DEFAULT,
				 DWR_RT2573_MAC_CSR8_EIFS_DEFAULT);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "run enter erp timing failed: %d\n", ret);
	ret = dwr_set_retry_limits(dwr, short_retry, long_retry, true, 0, true);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "run enter retry limits failed: %d\n", ret);
	ret = dwr_set_basic_rates(dwr, info->basic_rates);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "run enter basic rates failed: %d\n", ret);
	ret = dwr_set_bssid(dwr, info->bssid);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "run enter bssid failed: %d\n", ret);
	ret = dwr_set_tsf_sync(dwr, true, info->beacon_int);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "run enter tsf sync failed: %d\n", ret);
	ret = dwr_set_vgc(dwr, dwr->bbp17_base);
	if (ret)
		dwr_dbg(&dwr->usb.intf->dev, "run enter set vgc failed: %d\n", ret);
	WRITE_ONCE(dwr->link_rssi_dbm, DWR_LINK_RSSI_INVALID_DBM);
	cancel_delayed_work_sync(&dwr->link_tuner_work);
	if (READ_ONCE(dwr->usb.running) && READ_ONCE(dwr->associated))
		schedule_delayed_work(&dwr->link_tuner_work, msecs_to_jiffies(2000));

	/* TODO(openbsd-rum-port): fake-join tx-rate initialization from if_rum.c has no direct mac80211 equivalent here. */
}

static void dwr_log_sta_rx_counters(struct dwr_dev *dwr, const char *reason)
{
	u16 fcs_err = 0, plcp_err = 0, false_cca = 0, physical_err = 0;
	int ret;

	ret = dwr_read_rx_error_counters(dwr, &fcs_err, &plcp_err,
					 &physical_err, &false_cca);
	if (ret) {
		dwr_dbg(&dwr->usb.intf->dev, "read rx counters failed (%s): %d\n", reason, ret);
		return;
	}

	dwr_info(&dwr->usb.intf->dev,
		 "rx hw counters (%s): fcs=%u plcp=%u physical=%u false_cca=%u\n",
		 reason, fcs_err, plcp_err, physical_err, false_cca);
}

static void dwr_mac_bss_info_changed(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *info,
				     u64 changed)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);
	int ret;

	dwr_dbg(&dwr->usb.intf->dev,
		"bss_info_changed: assoc=%d aid=%u changed=0x%llx\n",
		info->assoc, info->aid, changed);

	if (changed & BSS_CHANGED_BSSID) {
		if (is_valid_ether_addr(info->bssid)) {
			ret = dwr_set_bssid(dwr, info->bssid);
			if (ret)
				dwr_warn(&dwr->usb.intf->dev,
					 "set bssid %pM failed: %d\n",
					 info->bssid, ret);
		} else {
			ret = dwr_clear_bssid(dwr);
			if (ret)
				dwr_dbg(&dwr->usb.intf->dev,
					"clear bssid on invalid bss update failed: %d\n", ret);
		}
	}

	if (changed & BSS_CHANGED_ASSOC) {
		dwr_update_assoc_aid(dwr, info->assoc, info->aid);
		/*
		 * TODO(openbsd-rum-port): OpenBSD if_rum.c + if_rumreg.h
		 * expose no confirmed dedicated RT2573 hardware AID register/
		 * field in this station path; keep software AID state only.
		 */
		if (info->assoc)
			dwr_enter_run_state(dwr, info);
		else
			dwr_leave_run_state(dwr, "disassoc");
		/* TODO(openbsd-rum-port): program association-related timing/state registers once confirmed from if_rum.c. */
	}
	if (changed & BSS_CHANGED_BEACON_INT) {
		ret = dwr_set_tsf_sync(dwr, dwr->associated, info->beacon_int);
		if (ret)
			dwr_dbg(&dwr->usb.intf->dev, "set beacon interval failed: %d\n", ret);
	}
	if (changed & BSS_CHANGED_BASIC_RATES) {
		ret = dwr_set_basic_rates(dwr, info->basic_rates);
		if (ret)
			dwr_dbg(&dwr->usb.intf->dev, "set basic rates failed: %d\n", ret);
	}
	if (changed & (BSS_CHANGED_ERP_PREAMBLE | BSS_CHANGED_ERP_SLOT)) {
		ret = dwr_set_erp_timing(dwr, info->use_short_preamble,
					 info->use_short_slot ?
					 DWR_RT2573_SLOT_TIME_SHORT :
					 DWR_RT2573_SLOT_TIME_LONG,
					 DWR_RT2573_MAC_CSR8_SIFS_DEFAULT,
					 DWR_RT2573_MAC_CSR8_EIFS_DEFAULT);
		if (ret)
			dwr_dbg(&dwr->usb.intf->dev, "set erp timing failed: %d\n", ret);
	}
}

static int dwr_mac_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	if (vif->type != NL80211_IFTYPE_STATION)
		return -EOPNOTSUPP;
	if (dwr->vif_sta)
		return -EBUSY;

	dwr->vif_sta = vif;
	dwr->associated = false;
	dwr->aid = 0;
	dwr->bssid_valid = false;
	eth_zero_addr(dwr->bssid);
	return 0;
}

static void dwr_mac_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	if (dwr->vif_sta == vif)
		dwr->vif_sta = NULL;
	dwr_leave_run_state(dwr, "remove_interface");
}

static void dwr_mac_configure_filter(struct ieee80211_hw *hw,
				     unsigned int changed_flags,
				     unsigned int *total_flags,
				     u64 multicast)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	*total_flags &= FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC |
			FIF_CONTROL | FIF_OTHER_BSS | FIF_PROBE_REQ |
			FIF_FCSFAIL | FIF_PLCPFAIL | FIF_PSPOLL;
	dwr->filter_flags = *total_flags;
	if (!dwr->usb.running || !dwr->hw_state.hw_init_ok)
		return;
	if (dwr_set_rx_filter(dwr, *total_flags))
		dwr_dbg(&dwr->usb.intf->dev,
			"set rx filter failed flags=0x%x changed=0x%x\n",
			*total_flags, changed_flags);
}

static const struct ieee80211_ops dwr_mac_ops = {
	.tx = dwr_mac_tx,
	.start = dwr_mac_start,
	.stop = dwr_mac_stop,
	.add_interface = dwr_mac_add_interface,
	.remove_interface = dwr_mac_remove_interface,
	.config = dwr_mac_config,
	.bss_info_changed = dwr_mac_bss_info_changed,
	.configure_filter = dwr_mac_configure_filter,
	.wake_tx_queue = ieee80211_handle_wake_tx_queue,
};

static int dwr_usb_probe(struct usb_interface *intf,
			 const struct usb_device_id *id)
{
	struct ieee80211_hw *hw;
	struct dwr_dev *dwr;
	int ret;

	if (!bind) {
		dwr_info(&intf->dev,
			 "bind=0 refusing attach for %04x:%04x (set bind=1 to enable)\n",
			 id->idVendor, id->idProduct);
		return -ENODEV;
	}

	hw = ieee80211_alloc_hw(sizeof(*dwr), &dwr_mac_ops);
	if (!hw)
		return -ENOMEM;

	dwr = hw_to_dwr(hw);
	dwr->hw = hw;
	dwr->usb.udev = usb_get_dev(interface_to_usbdev(intf));
	dwr->usb.intf = intf;
	mutex_init(&dwr->usb.io_mutex);
	init_usb_anchor(&dwr->usb.tx_anchor);
	spin_lock_init(&dwr->tx_lock);
	INIT_WORK(&dwr->reset_work, dwr_reset_work);
	INIT_DELAYED_WORK(&dwr->link_tuner_work, dwr_link_tuner_workfn);
	dwr_rx_init_state(dwr);
	dwr->link_rssi_dbm = DWR_LINK_RSSI_INVALID_DBM;
	dwr->bbp17_base = 0x20;

	ret = dwr_detect_endpoints(dwr);
	if (ret)
		goto err_free_hw;

	SET_IEEE80211_DEV(hw, &intf->dev);
	hw->queues = 4;
	hw->max_rates = 1;
	hw->max_report_rates = 1;
	hw->extra_tx_headroom = 0;
	hw->wiphy->interface_modes = BIT(NL80211_IFTYPE_STATION);
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &dwr_band_2ghz;
	hw->flags = IEEE80211_HW_SIGNAL_DBM |
		    IEEE80211_HW_SUPPORTS_PS;

	ret = dwr_eeprom_parse(dwr);
	if (!ret && dwr->eeprom.valid) {
		ether_addr_copy(dwr->mac_addr, dwr->eeprom.mac_addr);
	} else {
		eth_random_addr(dwr->mac_addr);
		dwr_warn(&intf->dev,
			 "probe MAC fallback to random address (eeprom parse ret=%d)\n",
			 ret);
	}
	SET_IEEE80211_PERM_ADDR(hw, dwr->mac_addr);

	usb_set_intfdata(intf, dwr);

	ret = ieee80211_register_hw(hw);
	if (ret) {
		dwr_err(&intf->dev, "ieee80211_register_hw failed: %d\n", ret);
		goto err_clear_intf;
	}
	dwr->registered_hw = true;
	dwr_info(&intf->dev,
		 "registered rum4linux skeleton for %04x:%04x\n",
		 id->idVendor, id->idProduct);
	return 0;

err_clear_intf:
	usb_set_intfdata(intf, NULL);
err_free_hw:
	usb_put_dev(dwr->usb.udev);
	ieee80211_free_hw(hw);
	return ret;
}

static void dwr_usb_disconnect(struct usb_interface *intf)
{
	struct dwr_dev *dwr = usb_get_intfdata(intf);

	if (!dwr)
		return;

	usb_set_intfdata(intf, NULL);
	dwr->usb.running = false;
	dwr_leave_run_state(dwr, "disconnect");
	dwr_rx_stop(dwr);
	dwr_rx_log_summary(dwr, "disconnect");
	dwr_log_channel_apply_summary(dwr, "disconnect");
	dwr_log_sta_rx_counters(dwr, "disconnect");
	dwr_tx_cancel_pending(dwr);
	cancel_work_sync(&dwr->reset_work);
	if (dwr->registered_hw)
		ieee80211_unregister_hw(dwr->hw);
	usb_put_dev(dwr->usb.udev);
	ieee80211_free_hw(dwr->hw);
}

static const struct usb_device_id dwr_usb_ids[] = {
	{ USB_DEVICE(DWR_USB_VID, DWR_USB_PID) },
	{ }
};
MODULE_DEVICE_TABLE(usb, dwr_usb_ids);

static struct usb_driver dwr_usb_driver = {
	.name = "rum4linux",
	.id_table = dwr_usb_ids,
	.probe = dwr_usb_probe,
	.disconnect = dwr_usb_disconnect,
};

module_usb_driver(dwr_usb_driver);

MODULE_AUTHOR("OpenAI scaffold");
MODULE_DESCRIPTION("rum4linux: OpenBSD rum(4)-family Linux scaffold (early, conservative)");
MODULE_LICENSE("GPL");

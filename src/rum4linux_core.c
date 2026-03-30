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

static bool bind;
module_param(bind, bool, 0644);
MODULE_PARM_DESC(bind, "Actually bind to currently enumerated rum(4)-family IDs (default: false)");

static struct ieee80211_rate dwr_rates_2ghz[] = {
	{ .bitrate = 10,  .hw_value = 0 },
	{ .bitrate = 20,  .hw_value = 1 },
	{ .bitrate = 55,  .hw_value = 2 },
	{ .bitrate = 110, .hw_value = 3 },
	{ .bitrate = 60,  .hw_value = 4 },
	{ .bitrate = 90,  .hw_value = 5 },
	{ .bitrate = 120, .hw_value = 6 },
	{ .bitrate = 180, .hw_value = 7 },
	{ .bitrate = 240, .hw_value = 8 },
	{ .bitrate = 360, .hw_value = 9 },
	{ .bitrate = 480, .hw_value = 10 },
	{ .bitrate = 540, .hw_value = 11 },
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

static void dwr_rx_work(struct work_struct *work)
{
	/* TODO(openbsd-rum-port): parse RX descriptors and call ieee80211_rx_irqsafe(). */
}

static void dwr_reset_work(struct work_struct *work)
{
	/* TODO(openbsd-rum-port): device reset/reinit path after USB or MCU fault. */
}

static int dwr_mac_start(struct ieee80211_hw *hw)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);
	int ret;

	dwr_info(&dwr->usb.intf->dev, "mac80211 start\n");
	ret = dwr_hw_init(dwr);
	if (ret)
		return ret;

	dwr->usb.running = true;
	return 0;
}

static void dwr_mac_stop(struct ieee80211_hw *hw, bool suspend)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	dwr_info(&dwr->usb.intf->dev, "mac80211 stop suspend=%d\n", suspend);
	dwr_tx_cancel_pending(dwr);
	dwr_hw_stop(dwr);
	cancel_work_sync(&dwr->rx_work);
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

static void dwr_mac_bss_info_changed(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *info,
				     u64 changed)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	dwr_dbg(&dwr->usb.intf->dev,
		"bss_info_changed: assoc=%d aid=%u changed=0x%llx\n",
		info->assoc, info->aid, changed);
}

static int dwr_mac_add_interface(struct ieee80211_hw *hw,
				 struct ieee80211_vif *vif)
{
	return 0;
}

static void dwr_mac_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
}

static void dwr_mac_configure_filter(struct ieee80211_hw *hw,
				     unsigned int changed_flags,
				     unsigned int *total_flags,
				     u64 multicast)
{
	*total_flags &= FIF_ALLMULTI | FIF_BCN_PRBRESP_PROMISC |
			FIF_CONTROL | FIF_OTHER_BSS | FIF_PROBE_REQ;
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
	spin_lock_init(&dwr->tx_lock);
	INIT_WORK(&dwr->rx_work, dwr_rx_work);
	INIT_WORK(&dwr->reset_work, dwr_reset_work);

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

	eth_random_addr(dwr->mac_addr);
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
	dwr_tx_cancel_pending(dwr);
	cancel_work_sync(&dwr->rx_work);
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

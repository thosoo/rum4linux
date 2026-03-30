/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/etherdevice.h>
#include <net/mac80211.h>
#include "dwa111_rum_hw.h"
#include "dwa111_rum_debug.h"

static bool bind;
module_param(bind, bool, 0644);
MODULE_PARM_DESC(bind, "Actually bind to 07d1:3c06. Default: false");

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

static void dwr_rx_work(struct work_struct *work)
{
	/* TODO: pull completed RX URBs, decode descriptors, call ieee80211_rx_irqsafe() */
}

static void dwr_reset_work(struct work_struct *work)
{
	/* TODO: device reset / reinit path */
}

static int dwr_mac_start(struct ieee80211_hw *hw)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);
	int ret;

	dwr_info(&dwr->usb.intf->dev, "mac80211 start\n");
	ret = dwr_load_firmware(dwr);
	if (ret && ret != -EOPNOTSUPP)
		return ret;

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
	dwr_hw_stop(dwr);
	cancel_work_sync(&dwr->rx_work);
	cancel_work_sync(&dwr->reset_work);
}

static void dwr_mac_tx(struct ieee80211_hw *hw,
		       struct ieee80211_tx_control *control,
		       struct sk_buff *skb)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	/* TODO: map skb -> RT2571W TX descriptor -> USB bulk out */
	dwr_dbg(&dwr->usb.intf->dev, "drop TX len=%u (stub)\n", skb->len);
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

	if (!bind)
		return -ENODEV;

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
		usb_set_intfdata(intf, NULL);
		usb_put_dev(dwr->usb.udev);
		ieee80211_free_hw(hw);
		return ret;
	}
	dwr->registered_hw = true;
	dwr_info(&intf->dev, "registered skeleton driver for %04x:%04x\n",
		 id->idVendor, id->idProduct);
	return 0;
}

static void dwr_usb_disconnect(struct usb_interface *intf)
{
	struct dwr_dev *dwr = usb_get_intfdata(intf);

	if (!dwr)
		return;

	usb_set_intfdata(intf, NULL);
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
	.name = "dwa111_rum",
	.id_table = dwr_usb_ids,
	.probe = dwr_usb_probe,
	.disconnect = dwr_usb_disconnect,
};

module_usb_driver(dwr_usb_driver);

MODULE_AUTHOR("OpenAI scaffold");
MODULE_DESCRIPTION("DWA-111 RT2571W replacement driver scaffold inspired by OpenBSD rum(4)");
MODULE_LICENSE("GPL");

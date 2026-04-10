/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/usb/ch9.h>
#include <linux/etherdevice.h>
#include <linux/string.h>
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

static int dwr_leave_run_state(struct dwr_dev *dwr, const char *reason);
static int dwr_enter_run_state(struct dwr_dev *dwr,
			       struct ieee80211_bss_conf *info);
static int dwr_restore_started_state(struct dwr_dev *dwr);
static void dwr_log_sta_rx_counters(struct dwr_dev *dwr, const char *reason);
static void dwr_build_bss_conf_shadow(struct dwr_dev *dwr,
				      const struct ieee80211_bss_conf *src,
				      struct ieee80211_bss_conf *dst);
static void dwr_log_reset_summary(struct dwr_dev *dwr, const char *reason);
#define DWR_TX_WATCHDOG_PERIOD_MS 1000
#define DWR_TX_WATCHDOG_TIMEOUT_MS 5000
#define DWR_RESET_STORM_WINDOW_MS 10000
#define DWR_RESET_STORM_MAX_IN_WINDOW 4
#define DWR_RESET_STORM_COOLDOWN_MS 5000

static bool dwr_rf_rev_supported_narrow(u8 rf_rev)
{
	switch (rf_rev) {
	case DWR_RF_2528:
		return true;
	default:
		return false;
	}
}

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
	struct dwr_dev *dwr = container_of(work, struct dwr_dev, reset_work);
	struct ieee80211_bss_conf shadow = {};
	bool reenter_run;
	bool attempted = false;
	int ret;

	mutex_lock(&dwr->reset_mutex);
	if (test_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags) ||
	    !dwr->registered_hw || !READ_ONCE(dwr->usb.running))
		goto done;

	while (test_and_clear_bit(DWR_RESET_F_REQUESTED, &dwr->reset_flags)) {
		set_bit(DWR_RESET_F_IN_PROGRESS, &dwr->reset_flags);
		attempted = true;
		dwr->reset_last_stage = DWR_RESET_STAGE_IDLE;
		dwr->reset_last_reassoc = false;
		dwr->reset_last_replay_mode = DWR_RESET_REPLAY_NONE;
		dwr_warn(&dwr->usb.intf->dev, "reset: begin reason=%s err=%d\n",
			 dwr->reset_last_reason ? dwr->reset_last_reason : "unknown",
			 dwr->reset_last_err);

		reenter_run = dwr->associated && dwr->bssid_valid && dwr->bss_beacon_int;
		if (reenter_run)
			dwr_build_bss_conf_shadow(dwr, NULL, &shadow);

		dwr->reset_last_stage = DWR_RESET_STAGE_LEAVE_RUN;
		dwr_leave_run_state(dwr, "reset");
		WRITE_ONCE(dwr->usb.running, false);
		cancel_delayed_work_sync(&dwr->tx_watchdog_work);
		dwr->tx_cancel_reason = DWR_TX_CANCEL_RESET;
			dwr_tx_cancel_pending(dwr);
			dwr_rx_stop(dwr);

		dwr->reset_last_stage = DWR_RESET_STAGE_HW_STOP;
		dwr_hw_stop(dwr);
		dwr->reset_last_stage = DWR_RESET_STAGE_HW_INIT;
		ret = dwr_hw_init(dwr);
		if (ret)
			goto fail;
		dwr->reset_last_stage = DWR_RESET_STAGE_RESTORE_STARTED;
		ret = dwr_restore_started_state(dwr);
		if (ret)
			goto fail;
		dwr->reset_last_replay_mode = DWR_RESET_REPLAY_UNASSOC;

		WRITE_ONCE(dwr->usb.running, true);
		dwr->reset_last_stage = DWR_RESET_STAGE_RX_START;
		ret = dwr_rx_start(dwr);
		if (ret) {
			WRITE_ONCE(dwr->usb.running, false);
			goto fail;
		}

		if (reenter_run) {
			dwr->reset_last_stage = DWR_RESET_STAGE_REASSOC_REENTER;
			ret = dwr_enter_run_state(dwr, &shadow);
			if (ret)
				goto fail;
			dwr->reset_last_reassoc = true;
			dwr->reset_last_replay_mode = DWR_RESET_REPLAY_ASSOC_REENTER;
		}

		dwr->reset_success_count++;
		dwr_info(&dwr->usb.intf->dev, "reset: recovery complete\n");
		clear_bit(DWR_RESET_F_IN_PROGRESS, &dwr->reset_flags);
	}
	goto done;

fail:
	dwr->reset_failure_count++;
	dwr->reset_last_fail_stage = dwr->reset_last_stage;
	dwr_err(&dwr->usb.intf->dev,
		"reset: recovery failed stage=%u ret=%d\n",
		dwr->reset_last_stage, ret);
	WRITE_ONCE(dwr->usb.running, false);
	dwr_leave_run_state(dwr, "reset-failed");
	cancel_delayed_work_sync(&dwr->tx_watchdog_work);
	dwr->tx_cancel_reason = DWR_TX_CANCEL_RESET;
	dwr_tx_cancel_pending(dwr);
	dwr_rx_stop(dwr);
	dwr_hw_stop(dwr);
	clear_bit(DWR_RESET_F_IN_PROGRESS, &dwr->reset_flags);
	clear_bit(DWR_RESET_F_REQUESTED, &dwr->reset_flags);

done:
	if (attempted)
		dwr_log_reset_summary(dwr, "reset_work");
	mutex_unlock(&dwr->reset_mutex);
}

static void dwr_tx_watchdog_workfn(struct work_struct *work)
{
	struct dwr_dev *dwr =
		container_of(to_delayed_work(work), struct dwr_dev, tx_watchdog_work);
	unsigned long timeout = msecs_to_jiffies(DWR_TX_WATCHDOG_TIMEOUT_MS);
	int inflight;

	if (!READ_ONCE(dwr->usb.running) || test_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags))
		return;
	inflight = atomic_read(&dwr->tx_inflight);
	if (inflight <= 0) {
		dwr->tx_watchdog_clear_count++;
		return;
	}
	if (time_after(jiffies, dwr->tx_last_progress_jiffies + timeout)) {
		dwr->tx_watchdog_fire_count++;
		dwr_warn(&dwr->usb.intf->dev,
			 "tx watchdog timeout inflight=%d stalled_ms=%u\n",
			 inflight, jiffies_to_msecs(jiffies - dwr->tx_last_progress_jiffies));
		dwr_request_reset(dwr, "tx-watchdog", -ETIMEDOUT);
	}

	if (atomic_read(&dwr->tx_inflight) > 0 &&
	    !test_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags))
		schedule_delayed_work(&dwr->tx_watchdog_work,
				      msecs_to_jiffies(DWR_TX_WATCHDOG_PERIOD_MS));
}

void dwr_tx_progress(struct dwr_dev *dwr, bool inflight_nonzero)
{
	if (!dwr)
		return;
	dwr->tx_last_progress_jiffies = jiffies;
	if (!READ_ONCE(dwr->usb.running) || test_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags))
		return;
	if (inflight_nonzero) {
		dwr->tx_watchdog_arm_count++;
		schedule_delayed_work(&dwr->tx_watchdog_work,
				      msecs_to_jiffies(DWR_TX_WATCHDOG_PERIOD_MS));
	} else {
		cancel_delayed_work(&dwr->tx_watchdog_work);
		dwr->tx_watchdog_clear_count++;
	}
}

static void dwr_link_tuner_workfn(struct work_struct *work)
{
	struct dwr_dev *dwr =
		container_of(to_delayed_work(work), struct dwr_dev, link_tuner_work);
	u16 fcs_err = 0, plcp_err = 0, physical_err = 0, false_cca = 0;
	u16 tx_no_retry = 0, tx_one_retry = 0, tx_multi_retry = 0, tx_retry_fail = 0;
	s8 rssi;
	bool have_rssi;
	u8 low_bound, up_bound, next_vgc;

	if (!READ_ONCE(dwr->usb.running))
		return;

	if (dwr_read_rx_error_counters(dwr, &fcs_err, &plcp_err, &physical_err, &false_cca))
		goto reschedule;
	if (!dwr_read_tx_retry_counters(dwr, &tx_no_retry, &tx_one_retry,
					&tx_multi_retry, &tx_retry_fail)) {
		dwr->tx_retry_stats_read_ok_count++;
		dwr->tx_retry_no_retry_ok = tx_no_retry;
		dwr->tx_retry_one_retry_ok = tx_one_retry;
		dwr->tx_retry_multi_retry_ok = tx_multi_retry;
		dwr->tx_retry_fail = tx_retry_fail;
	} else {
		dwr->tx_retry_stats_read_fail_count++;
	}
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
	clear_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags);
	atomic_set(&dwr->tx_inflight, 0);
	clear_bit(DWR_TX_F_QUEUES_STOPPED, &dwr->tx_flags);
	dwr->tx_last_progress_jiffies = jiffies;
	dwr->reset_window_jiffies = 0;
	dwr->reset_window_count = 0;
	dwr->reset_cooldown_until = 0;
	ret = dwr_hw_init(dwr);
	if (ret)
		return ret;
	ret = dwr_restore_started_state(dwr);
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
	set_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags);
	dwr->usb.running = false;
	if (dwr_leave_run_state(dwr, "stop"))
		dwr_dbg(&dwr->usb.intf->dev, "run leave failed on stop\n");
	cancel_delayed_work_sync(&dwr->tx_watchdog_work);
	dwr_rx_stop(dwr);
	dwr_rx_log_summary(dwr, "mac_stop");
	dwr_log_channel_apply_summary(dwr, "mac_stop");
	dwr_log_sta_rx_counters(dwr, "mac_stop");
	dwr->tx_cancel_reason = DWR_TX_CANCEL_TEARDOWN;
	dwr_tx_cancel_pending(dwr);
	dwr_hw_stop(dwr);
	cancel_work_sync(&dwr->reset_work);
	clear_bit(DWR_RESET_F_REQUESTED, &dwr->reset_flags);
	clear_bit(DWR_RESET_F_IN_PROGRESS, &dwr->reset_flags);
	dwr_log_reset_summary(dwr, "mac_stop");
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
	int ret = 0;

	if (changed & IEEE80211_CONF_CHANGE_CHANNEL)
		ret = dwr_set_channel(dwr, conf->chandef.chan);
	if ((changed & IEEE80211_CONF_CHANGE_CHANNEL) && !dwr->associated && !ret)
		dwr->started_refresh_channel_count++;
	if (ret)
		return ret;

	if (READ_ONCE(dwr->usb.running) && !dwr->associated &&
	    (changed & IEEE80211_CONF_CHANGE_RETRY_LIMITS)) {
		dwr->started_refresh_count++;
		if (changed & IEEE80211_CONF_CHANGE_RETRY_LIMITS)
			dwr->started_refresh_retry_count++;
		ret = dwr_restore_started_state(dwr);
		if (ret) {
			dwr->started_refresh_fail_count++;
			dwr_warn(&dwr->usb.intf->dev,
				 "started-state refresh failed changed=0x%x err=%d\n",
				 changed, ret);
		} else {
			dwr_dbg(&dwr->usb.intf->dev,
				"started-state refresh applied changed=0x%x\n", changed);
		}
	}

	return ret;
}

static void dwr_update_assoc_aid(struct dwr_dev *dwr, bool associated, u16 aid)
{
	dwr->associated = associated;
	dwr->aid = associated ? aid : 0;
}

static void dwr_build_bss_conf_shadow(struct dwr_dev *dwr,
				      const struct ieee80211_bss_conf *src,
				      struct ieee80211_bss_conf *dst)
{
	memset(dst, 0, sizeof(*dst));
	if (src) {
		dst->beacon_int = src->beacon_int;
		dst->basic_rates = src->basic_rates;
		dst->use_short_preamble = src->use_short_preamble;
		dst->use_short_slot = src->use_short_slot;
		ether_addr_copy(dst->bssid, src->bssid);
		return;
	}

	dst->beacon_int = dwr->bss_beacon_int;
	dst->basic_rates = dwr->bss_basic_rates;
	dst->use_short_preamble = dwr->bss_use_short_preamble;
	dst->use_short_slot = dwr->bss_use_short_slot;
	if (dwr->bssid_valid)
		ether_addr_copy(dst->bssid, dwr->bssid);
}

void dwr_request_reset(struct dwr_dev *dwr, const char *reason, int err)
{
	unsigned long now = jiffies;
	unsigned long window = msecs_to_jiffies(DWR_RESET_STORM_WINDOW_MS);

	if (!dwr || !dwr->registered_hw || !READ_ONCE(dwr->usb.running) ||
	    test_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags))
		return;
	if (time_before(now, dwr->reset_cooldown_until)) {
		dwr->reset_suppressed_count++;
		if (__ratelimit(&net_ratelimit_state))
			dwr_warn(&dwr->usb.intf->dev,
				 "reset suppressed (cooldown) reason=%s err=%d\n",
				 reason, err);
		return;
	}
	if (!dwr->reset_window_jiffies ||
	    time_after(now, dwr->reset_window_jiffies + window)) {
		dwr->reset_window_jiffies = now;
		dwr->reset_window_count = 0;
	}
	dwr->reset_window_count++;
	if (dwr->reset_window_count > DWR_RESET_STORM_MAX_IN_WINDOW) {
		dwr->reset_cooldown_until = now + msecs_to_jiffies(DWR_RESET_STORM_COOLDOWN_MS);
		dwr->reset_suppressed_count++;
		dwr_warn(&dwr->usb.intf->dev,
			 "reset storm detected, entering cooldown ms=%u reason=%s\n",
			 DWR_RESET_STORM_COOLDOWN_MS, reason);
		return;
	}

	dwr->reset_last_reason = reason;
	dwr->reset_last_err = err;
	dwr->reset_req_total++;
	if (!strcmp(reason, "tx-submit"))
		dwr->reset_req_tx_submit++;
	else if (!strcmp(reason, "tx-complete"))
		dwr->reset_req_tx_complete++;
	else if (!strcmp(reason, "rx-complete"))
		dwr->reset_req_rx_complete++;
	set_bit(DWR_RESET_F_REQUESTED, &dwr->reset_flags);
	schedule_work(&dwr->reset_work);
}

static int dwr_restore_started_state(struct dwr_dev *dwr)
{
	struct ieee80211_channel *chan = dwr->hw->conf.chandef.chan;
	u8 short_retry = dwr->hw->conf.short_frame_max_tx_count ?: 7;
	u8 long_retry = dwr->hw->conf.long_frame_max_tx_count ?: 7;
	int ret;

	if (chan) {
		ret = dwr_set_channel(dwr, chan);
		if (ret)
			return ret;
	}
	ret = dwr_set_macaddr(dwr, dwr->mac_addr);
	if (ret)
		return ret;
	ret = dwr_set_rx_filter(dwr, dwr->filter_flags);
	if (ret)
		return ret;
	ret = dwr_set_rx_timing_defaults(dwr);
	if (ret)
		return ret;
	ret = dwr_set_erp_timing(dwr,
				 !!(dwr->hw->conf.flags & IEEE80211_CONF_SHORT_PREAMBLE),
				 dwr->bss_use_short_slot ?
				 DWR_RT2573_SLOT_TIME_SHORT :
				 DWR_RT2573_SLOT_TIME_LONG,
				 DWR_RT2573_MAC_CSR8_SIFS_DEFAULT,
				 DWR_RT2573_MAC_CSR8_EIFS_DEFAULT);
	if (ret)
		return ret;
	ret = dwr_set_retry_limits(dwr, short_retry, long_retry, true, 0, true);
	if (ret)
		return ret;
	ret = dwr_set_basic_rates(dwr, dwr->bss_basic_rates);
	if (ret)
		return ret;
	ret = dwr_abort_tsf_sync(dwr);
	if (ret)
		return ret;
	return dwr_clear_bssid(dwr);
}

static int dwr_leave_run_state(struct dwr_dev *dwr, const char *reason)
{
	int first_err = 0;
	int ret;

	dwr_update_assoc_aid(dwr, false, 0);
	WRITE_ONCE(dwr->link_rssi_dbm, DWR_LINK_RSSI_INVALID_DBM);
	cancel_delayed_work_sync(&dwr->link_tuner_work);
	ret = dwr_abort_tsf_sync(dwr);
	if (ret) {
		dwr_dbg(&dwr->usb.intf->dev, "abort tsf sync failed (%s): %d\n", reason, ret);
		if (!first_err)
			first_err = ret;
	}
	ret = dwr_clear_bssid(dwr);
	if (ret) {
		dwr_dbg(&dwr->usb.intf->dev, "clear bssid failed (%s): %d\n", reason, ret);
		if (!first_err)
			first_err = ret;
	}
	return first_err;
}

static int dwr_enter_run_state(struct dwr_dev *dwr, struct ieee80211_bss_conf *info)
{
	struct ieee80211_channel *chan = dwr->hw->conf.chandef.chan;
	u8 short_retry = dwr->hw->conf.short_frame_max_tx_count;
	u8 long_retry = dwr->hw->conf.long_frame_max_tx_count;
	bool have_valid_bssid = is_valid_ether_addr(info->bssid);
	bool enable_tsf = have_valid_bssid && info->beacon_int;
	int ret;

	if (!short_retry)
		short_retry = 7;
	if (!long_retry)
		long_retry = 7;

	if (chan) {
		ret = dwr_set_channel(dwr, chan);
		if (ret)
			return ret;
	}

	ret = dwr_set_erp_timing(dwr, info->use_short_preamble,
				 info->use_short_slot ?
				 DWR_RT2573_SLOT_TIME_SHORT :
				 DWR_RT2573_SLOT_TIME_LONG,
				 DWR_RT2573_MAC_CSR8_SIFS_DEFAULT,
				 DWR_RT2573_MAC_CSR8_EIFS_DEFAULT);
	if (ret)
		return ret;
	ret = dwr_set_retry_limits(dwr, short_retry, long_retry, true, 0, true);
	if (ret)
		return ret;
	ret = dwr_set_basic_rates(dwr, info->basic_rates);
	if (ret)
		return ret;
	if (have_valid_bssid) {
		ret = dwr_set_bssid(dwr, info->bssid);
		if (ret)
			return ret;
	} else {
		ret = dwr_clear_bssid(dwr);
		if (ret)
			return ret;
	}

	if (!enable_tsf) {
		ret = dwr_abort_tsf_sync(dwr);
		if (ret)
			return ret;
		/*
		 * OpenBSD rum_enable_tsf_sync() consumes station BSS interval.
		 * TODO(openbsd-rum-port): confirm whether zero beacon interval
		 * should be tolerated differently on RT2573.
		 */
	} else {
		ret = dwr_set_tsf_sync(dwr, true, info->beacon_int);
		if (ret)
			return ret;
	}
	ret = dwr_set_vgc(dwr, dwr->bbp17_base);
	if (ret)
		return ret;
	WRITE_ONCE(dwr->link_rssi_dbm, DWR_LINK_RSSI_INVALID_DBM);
	cancel_delayed_work_sync(&dwr->link_tuner_work);
	if (READ_ONCE(dwr->usb.running) && READ_ONCE(dwr->associated))
		schedule_delayed_work(&dwr->link_tuner_work, msecs_to_jiffies(2000));

	/* TODO(openbsd-rum-port): fake-join tx-rate initialization from if_rum.c has no direct mac80211 equivalent here. */
	return 0;
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

static void dwr_log_reset_summary(struct dwr_dev *dwr, const char *reason)
{
	dwr_info(&dwr->usb.intf->dev,
		 "reset summary (%s): req_total=%u req_tx_submit=%u req_tx_complete=%u req_rx_complete=%u ok=%u fail=%u suppressed=%u last_reason=%s last_err=%d last_stage=%u last_fail_stage=%u replay_mode=%u last_reassoc=%u refresh={ok:%u fail:%u chan:%u retry:%u filter:%u} tx_retry={read_ok:%u read_fail:%u no:%u one:%u multi:%u fail:%u} cooldown_active=%u in_progress=%u pending=%u blocked=%u\n",
		 reason,
		 dwr->reset_req_total,
		 dwr->reset_req_tx_submit,
		 dwr->reset_req_tx_complete,
		 dwr->reset_req_rx_complete,
		 dwr->reset_success_count,
		 dwr->reset_failure_count,
		 dwr->reset_suppressed_count,
		 dwr->reset_last_reason ? dwr->reset_last_reason : "none",
		 dwr->reset_last_err,
		 dwr->reset_last_stage,
		 dwr->reset_last_fail_stage,
		 dwr->reset_last_replay_mode,
		 dwr->reset_last_reassoc,
		 dwr->started_refresh_count,
		 dwr->started_refresh_fail_count,
		 dwr->started_refresh_channel_count,
		 dwr->started_refresh_retry_count,
		 dwr->started_refresh_filter_count,
		 dwr->tx_retry_stats_read_ok_count,
		 dwr->tx_retry_stats_read_fail_count,
		 dwr->tx_retry_no_retry_ok,
		 dwr->tx_retry_one_retry_ok,
		 dwr->tx_retry_multi_retry_ok,
		 dwr->tx_retry_fail,
		 time_before(jiffies, dwr->reset_cooldown_until),
		 !!test_bit(DWR_RESET_F_IN_PROGRESS, &dwr->reset_flags),
		 !!test_bit(DWR_RESET_F_REQUESTED, &dwr->reset_flags),
		 !!test_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags));
}

static void dwr_mac_bss_info_changed(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif,
				     struct ieee80211_bss_conf *info,
				     u64 changed)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);
	struct ieee80211_bss_conf shadow;
	bool assoc_transition = false;
	int ret;

	dwr_dbg(&dwr->usb.intf->dev,
		"bss_info_changed: assoc=%d aid=%u changed=0x%llx\n",
		info->assoc, info->aid, changed);
	dwr->bss_beacon_int = info->beacon_int;
	dwr->bss_basic_rates = info->basic_rates;
	dwr->bss_use_short_preamble = info->use_short_preamble;
	dwr->bss_use_short_slot = info->use_short_slot;

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
		assoc_transition = true;
		dwr_info(&dwr->usb.intf->dev,
			 "assoc transition: new_assoc=%d aid=%u bssid=%pM\n",
			 info->assoc, info->aid, info->bssid);
		dwr_update_assoc_aid(dwr, info->assoc, info->aid);
		/*
		 * TODO(openbsd-rum-port): OpenBSD if_rum.c + if_rumreg.h
		 * expose no confirmed dedicated RT2573 hardware AID register/
		 * field in this station path; keep software AID state only.
		 */
		if (info->assoc)
			ret = dwr_enter_run_state(dwr, info);
		else
			ret = dwr_leave_run_state(dwr, "disassoc");
		if (ret)
			dwr_warn(&dwr->usb.intf->dev,
				 "assoc transition programming failed assoc=%d err=%d\n",
				 info->assoc, ret);
		if (ret && info->assoc) {
			dwr_update_assoc_aid(dwr, false, 0);
			(void)dwr_leave_run_state(dwr, "assoc_failed");
		}
	}

	if (!assoc_transition && dwr->associated &&
	    (changed & (BSS_CHANGED_BSSID |
			BSS_CHANGED_BEACON_INT |
			BSS_CHANGED_BASIC_RATES |
			BSS_CHANGED_ERP_PREAMBLE |
			BSS_CHANGED_ERP_SLOT))) {
		dwr_dbg(&dwr->usb.intf->dev,
			"assoc runtime refresh changed=0x%llx bssid=%pM beacon_int=%u basic=0x%x short_slot=%u short_pre=%u\n",
			changed, info->bssid, info->beacon_int, info->basic_rates,
			info->use_short_slot, info->use_short_preamble);
		dwr_build_bss_conf_shadow(dwr, info, &shadow);
		ret = dwr_enter_run_state(dwr, &shadow);
		if (ret)
			dwr_warn(&dwr->usb.intf->dev,
				 "runtime run-state refresh failed changed=0x%llx err=%d\n",
				 changed, ret);
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
	dwr->bss_beacon_int = 0;
	dwr->bss_basic_rates = 0;
	dwr->bss_use_short_preamble = false;
	dwr->bss_use_short_slot = false;
	eth_zero_addr(dwr->bssid);
	return 0;
}

static void dwr_mac_remove_interface(struct ieee80211_hw *hw,
				     struct ieee80211_vif *vif)
{
	struct dwr_dev *dwr = hw_to_dwr(hw);

	if (dwr->vif_sta == vif)
		dwr->vif_sta = NULL;
	if (dwr_leave_run_state(dwr, "remove_interface"))
		dwr_dbg(&dwr->usb.intf->dev, "run leave failed on remove_interface\n");
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
	else if (!dwr->associated)
		dwr->started_refresh_filter_count++;
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
	mutex_init(&dwr->reset_mutex);
	init_usb_anchor(&dwr->usb.tx_anchor);
	spin_lock_init(&dwr->tx_lock);
	INIT_WORK(&dwr->reset_work, dwr_reset_work);
	INIT_DELAYED_WORK(&dwr->link_tuner_work, dwr_link_tuner_workfn);
	INIT_DELAYED_WORK(&dwr->tx_watchdog_work, dwr_tx_watchdog_workfn);
	dwr_rx_init_state(dwr);
	atomic_set(&dwr->tx_inflight, 0);
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
	hw->flags = IEEE80211_HW_SIGNAL_DBM;

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

	if (!dwr_rf_rev_supported_narrow(dwr->eeprom.rf_rev)) {
		dwr_warn(&intf->dev,
			 "unsupported rf_rev=%u for DWA-111 narrow target (RT2528 only); refusing attach for %04x:%04x\n",
			 dwr->eeprom.rf_rev, id->idVendor, id->idProduct);
		ret = -EOPNOTSUPP;
		goto err_free_hw;
	}
	hw->wiphy->bands[NL80211_BAND_2GHZ] = &dwr_band_2ghz;

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
	set_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags);
	dwr->usb.running = false;
	if (dwr_leave_run_state(dwr, "disconnect"))
		dwr_dbg(&dwr->usb.intf->dev, "run leave failed on disconnect\n");
	cancel_delayed_work_sync(&dwr->tx_watchdog_work);
	dwr_rx_stop(dwr);
	dwr_rx_log_summary(dwr, "disconnect");
	dwr_log_channel_apply_summary(dwr, "disconnect");
	dwr_log_sta_rx_counters(dwr, "disconnect");
	dwr->tx_cancel_reason = DWR_TX_CANCEL_TEARDOWN;
	dwr_tx_cancel_pending(dwr);
	cancel_work_sync(&dwr->reset_work);
	clear_bit(DWR_RESET_F_REQUESTED, &dwr->reset_flags);
	clear_bit(DWR_RESET_F_IN_PROGRESS, &dwr->reset_flags);
	dwr_log_reset_summary(dwr, "disconnect");
	if (dwr->registered_hw)
		ieee80211_unregister_hw(dwr->hw);
	usb_put_dev(dwr->usb.udev);
	ieee80211_free_hw(dwr->hw);
}

static const struct usb_device_id dwr_usb_ids[] = {
	/* Narrow target-first policy: D-Link DWA-111 (RT2571W + RT2528). */
	{ USB_DEVICE(0x07d1, 0x3c06) },
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

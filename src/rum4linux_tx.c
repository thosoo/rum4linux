/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/ieee80211.h>
#include <linux/etherdevice.h>
#include "rum4linux_tx.h"
#include "rum4linux_debug.h"

struct dwr_tx_desc_min {
	__le32 flags;
	__le16 wme;
	__le16 xflags;
	u8 plcp_signal;
	u8 plcp_service;
	u8 plcp_length_lo;
	u8 plcp_length_hi;
};

struct dwr_tx_urb_ctx {
	struct dwr_dev *dwr;
	struct sk_buff *skb;
	u8 *buf;
	s8 rate_idx;
	u8 rate_count;
	bool no_ack;
	bool report_tx_status;
};

#define DWR_TX_VALID BIT(1)
#define DWR_TX_IFS_SIFS BIT(6)
#define DWR_TX_NEED_ACK BIT(3)
#define DWR_TX_OFDM BIT(5)
#define DWR_TX_MORE_FRAG BIT(2)
#define DWR_TX_LONG_RETRY BIT(7)
#define DWR_TX_MAX_FRAME_LEN 4095
#define DWR_PLCP_LENGEXT BIT(7)
#define DWR_PLCP_SHORT_PREAMBLE BIT(3)
#define DWR_RUM_ACK_SIZE 14
#define DWR_TX_MAX_INFLIGHT_URBS 32

static void dwr_tx_report_failed(struct dwr_dev *dwr, struct sk_buff *skb, int rate_idx);
static bool dwr_tx_acquire_slot(struct dwr_dev *dwr);
static void dwr_tx_release_slot(struct dwr_dev *dwr);
static void dwr_tx_log_summary(struct dwr_dev *dwr, const char *reason);
static void dwr_tx_complete(struct urb *urb);

static u8 dwr_plcp_signal(u8 rate_500k)
{
	/* OpenBSD rum_plcp_signal() mapping for CCK+OFDM rates. */
	switch (rate_500k) {
	case 2:
		return 0x0;
	case 4:
		return 0x1;
	case 11:
		return 0x2;
	case 22:
		return 0x3;
	case 12:
		return 0xb;
	case 18:
		return 0xf;
	case 24:
		return 0xa;
	case 36:
		return 0xe;
	case 48:
		return 0x9;
	case 72:
		return 0xd;
	case 96:
		return 0x8;
	case 108:
		return 0xc;
	default:
		return 0xff;
	}
}

static int dwr_tx_signal_rate_500k_from_idx(int idx, u8 *signal,
					    u8 *rate_500k, bool *ofdm)
{
	switch (idx) {
	case 0:
		*rate_500k = 2;
		break;
	case 1:
		*rate_500k = 4;
		break;
	case 2:
		*rate_500k = 11;
		break;
	case 3:
		*rate_500k = 22;
		break;
	case 4:
		*rate_500k = 12;
		break;
	case 5:
		*rate_500k = 18;
		break;
	case 6:
		*rate_500k = 24;
		break;
	case 7:
		*rate_500k = 36;
		break;
	case 8:
		*rate_500k = 48;
		break;
	case 9:
		*rate_500k = 72;
		break;
	case 10:
		*rate_500k = 96;
		break;
	case 11:
		*rate_500k = 108;
		break;
	default:
		return -EOPNOTSUPP;
	}

	*ofdm = *rate_500k >= 12;
	*signal = dwr_plcp_signal(*rate_500k);
	if (*signal == 0xff)
		return -EINVAL;

	return 0;
}

static u8 dwr_ack_rate_500k(bool mode_11b_only, u8 tx_rate_500k)
{
	switch (tx_rate_500k) {
	case 2:
		return 2;
	case 4:
	case 11:
	case 22:
		return mode_11b_only ? 4 : tx_rate_500k;
	case 12:
	case 18:
		return 12;
	case 24:
	case 36:
		return 24;
	case 48:
	case 72:
	case 96:
	case 108:
		return 48;
	default:
		return 2;
	}
}

static u16 dwr_txtime_us(int len, u8 rate_500k, u32 conf_flags)
{
	u16 txtime;
	bool ofdm = rate_500k >= 12;

	if (ofdm) {
		txtime = (8 + 4 * len + 3 + rate_500k - 1) / rate_500k;
		txtime = 16 + 4 + 4 * txtime + 6;
	} else {
		txtime = (16 * len + rate_500k - 1) / rate_500k;
		if (rate_500k != 2 && (conf_flags & IEEE80211_CONF_SHORT_PREAMBLE))
			txtime += 72 + 24;
		else
			txtime += 144 + 48;
	}
	return txtime;
}

static int dwr_tx_build_desc(struct dwr_dev *dwr, struct sk_buff *skb,
			     struct dwr_tx_desc_min *desc, s8 rate_idx,
			     u32 extra_flags, bool no_ack,
			     bool skip_duration_update)
{
	struct ieee80211_hdr *hdr;
	u16 plcp_length;
	u16 dur;
	u8 signal;
	u8 rate_500k;
	u8 ack_rate_500k;
	u32 payload_len_crc;
	u16 wme;
	u32 flags;
	u32 remainder;
	bool ofdm = false;
	bool short_preamble;
	int ret;

	if (!skb || skb->len == 0 || skb->len > DWR_TX_MAX_FRAME_LEN)
		return -EMSGSIZE;

	memset(desc, 0, sizeof(*desc));

	ret = dwr_tx_signal_rate_500k_from_idx(rate_idx,
					       &signal, &rate_500k, &ofdm);
	if (ret)
		return ret;
	ack_rate_500k = dwr_ack_rate_500k(false, rate_500k);

	flags = DWR_TX_VALID | DWR_TX_IFS_SIFS | extra_flags | (skb->len << 16);
	if (ofdm)
		flags |= DWR_TX_OFDM;
	if (!no_ack)
		flags |= DWR_TX_NEED_ACK;
	desc->flags = cpu_to_le32(flags);

	/* OpenBSD rum_setup_tx_desc() default queue/wme words. */
	wme = (0) | (2 << 4) | (4 << 8) | (10 << 12);
	desc->wme = cpu_to_le16(wme);
	desc->xflags = cpu_to_le16(0);
	desc->plcp_signal = signal;
	desc->plcp_service = 4;
	short_preamble = !!(dwr->hw->conf.flags & IEEE80211_CONF_SHORT_PREAMBLE);

	payload_len_crc = skb->len + IEEE80211_FCS_LEN;
	if (ofdm) {
		/*
		 * OpenBSD rum_setup_tx_desc(): OFDM PLCP length is payload+FCS
		 * in a 12-bit field split as hi[11:6]/lo[5:0].
		 */
		plcp_length = payload_len_crc & 0x0fff;
		desc->plcp_length_hi = plcp_length >> 6;
		desc->plcp_length_lo = plcp_length & 0x3f;
	} else {
		/* OpenBSD CCK plcp length formula; len includes CRC. */
		plcp_length = (16 * payload_len_crc + rate_500k - 1) / rate_500k;
		if (rate_500k == 22) {
			/* OpenBSD rum_setup_tx_desc(): PLCP length extension for 11 Mbps CCK. */
			remainder = (16 * payload_len_crc) % 22;
			if (remainder && remainder < 7)
				desc->plcp_service |= DWR_PLCP_LENGEXT;
		}
		desc->plcp_length_lo = plcp_length & 0xff;
		desc->plcp_length_hi = plcp_length >> 8;
		if (rate_500k != 2 && short_preamble)
			desc->plcp_signal |= DWR_PLCP_SHORT_PREAMBLE;
	}

	if (skb->len >= sizeof(*hdr))
		hdr = (struct ieee80211_hdr *)skb->data;
	else
		hdr = NULL;
	if (!skip_duration_update && hdr && !is_multicast_ether_addr(hdr->addr1) &&
	    !no_ack) {
		dur = dwr_txtime_us(DWR_RUM_ACK_SIZE, ack_rate_500k,
				    dwr->hw->conf.flags) + DWR_RT2573_MAC_CSR8_SIFS_DEFAULT;
		hdr->duration_id = cpu_to_le16(dur);
	}
	return 0;
}

static int dwr_tx_submit_urb(struct dwr_dev *dwr, struct sk_buff *skb,
			     struct dwr_tx_desc_min *desc, s8 rate_idx,
			     u8 rate_count, bool no_ack, bool report_tx_status)
{
	struct dwr_tx_urb_ctx *ctx;
	struct urb *urb;
	u8 *buf;
	size_t xfer_len;
	size_t total;
	int ret;

	total = sizeof(*desc) + skb->len;
	xfer_len = roundup(total, 4);
	buf = kzalloc(xfer_len, GFP_ATOMIC);
	if (!buf) {
		dwr->tx_local_alloc_fail_count++;
		return -ENOMEM;
	}

	ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx) {
		kfree(buf);
		dwr->tx_local_alloc_fail_count++;
		return -ENOMEM;
	}

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		kfree(ctx);
		kfree(buf);
		dwr->tx_local_alloc_fail_count++;
		return -ENOMEM;
	}

	memcpy(buf, desc, sizeof(*desc));
	memcpy(buf + sizeof(*desc), skb->data, skb->len);

	ctx->dwr = dwr;
	ctx->skb = skb;
	ctx->buf = buf;
	ctx->rate_idx = rate_idx;
	ctx->rate_count = rate_count;
	ctx->no_ack = no_ack;
	ctx->report_tx_status = report_tx_status;

	usb_fill_bulk_urb(urb, dwr->usb.udev,
			  usb_sndbulkpipe(dwr->usb.udev, dwr->usb.bulk_out_ep),
			  buf, xfer_len, dwr_tx_complete, ctx);
	usb_anchor_urb(urb, &dwr->usb.tx_anchor);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		usb_unanchor_urb(urb);
		usb_free_urb(urb);
		kfree(ctx->buf);
		kfree(ctx);
		dwr->tx_submit_fail_count++;
		return ret;
	}

	usb_free_urb(urb);
	return 0;
}

static struct sk_buff *dwr_tx_build_protection_skb(struct dwr_dev *dwr,
						    struct sk_buff *data_skb,
						    bool use_rts)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(data_skb);
	struct sk_buff *skb;

	if (!dwr->vif_sta)
		return ERR_PTR(-ENOTSUPP);

	if (use_rts) {
		struct ieee80211_rts rts;

		memset(&rts, 0, sizeof(rts));
		ieee80211_rts_get(dwr->hw, dwr->vif_sta, data_skb->data, data_skb->len,
				  info, &rts);
		skb = dev_alloc_skb(sizeof(rts));
		if (!skb)
			return ERR_PTR(-ENOMEM);
		skb_put_data(skb, &rts, sizeof(rts));
		return skb;
	}

	{
		struct ieee80211_cts cts;

		memset(&cts, 0, sizeof(cts));
		ieee80211_ctstoself_get(dwr->hw, dwr->vif_sta, data_skb->data,
					data_skb->len, info, &cts);
		skb = dev_alloc_skb(sizeof(cts));
		if (!skb)
			return ERR_PTR(-ENOMEM);
		skb_put_data(skb, &cts, sizeof(cts));
		return skb;
	}
}

static void dwr_tx_report_failed(struct dwr_dev *dwr, struct sk_buff *skb, int rate_idx)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);

	ieee80211_tx_info_clear_status(info);
	info->status.rates[0].idx = rate_idx;
	info->status.rates[0].count = 0;
	info->status.rates[1].idx = -1;
	ieee80211_tx_status_irqsafe(dwr->hw, skb);
}

static bool dwr_tx_acquire_slot(struct dwr_dev *dwr)
{
	int old, new;

	for (;;) {
		old = atomic_read(&dwr->tx_inflight);
		if (old >= DWR_TX_MAX_INFLIGHT_URBS) {
			dwr->tx_reject_busy_count++;
			if (!test_and_set_bit(DWR_TX_F_QUEUES_STOPPED, &dwr->tx_flags)) {
				ieee80211_stop_queues(dwr->hw);
				dwr->tx_queue_stop_count++;
			}
			return false;
		}
		new = old + 1;
		if (atomic_cmpxchg(&dwr->tx_inflight, old, new) == old)
			break;
	}

	if ((u32)new > dwr->tx_inflight_high_wm)
		dwr->tx_inflight_high_wm = new;
	if (new >= DWR_TX_MAX_INFLIGHT_URBS &&
	    !test_and_set_bit(DWR_TX_F_QUEUES_STOPPED, &dwr->tx_flags)) {
		ieee80211_stop_queues(dwr->hw);
		dwr->tx_queue_stop_count++;
	}
	return true;
}

static void dwr_tx_release_slot(struct dwr_dev *dwr)
{
	int now;

	now = atomic_dec_if_positive(&dwr->tx_inflight);
	if (now < 0)
		return;
	if (now < DWR_TX_MAX_INFLIGHT_URBS - 1 &&
	    test_and_clear_bit(DWR_TX_F_QUEUES_STOPPED, &dwr->tx_flags)) {
		ieee80211_wake_queues(dwr->hw);
		dwr->tx_queue_wake_count++;
	}
}

static void dwr_tx_log_summary(struct dwr_dev *dwr, const char *reason)
{
	dwr_info(&dwr->usb.intf->dev,
		 "tx summary (%s): inflight=%d high_wm=%u qstop=%u qwake=%u reject_busy=%u prot_bypass=%u prot_reject=%u desc_fail=%u alloc_fail=%u submit_fail=%u complete_fail=%u reset_cancel=%u teardown_cancel=%u watchdog={arm:%u fire:%u clear:%u} queues_stopped=%u\n",
		 reason,
		 atomic_read(&dwr->tx_inflight),
		 dwr->tx_inflight_high_wm,
		 dwr->tx_queue_stop_count,
		 dwr->tx_queue_wake_count,
		 dwr->tx_reject_busy_count,
		 dwr->tx_protection_bypass_count,
		 dwr->tx_protection_reject_count,
		 dwr->tx_local_desc_fail_count,
		 dwr->tx_local_alloc_fail_count,
		 dwr->tx_submit_fail_count,
		 dwr->tx_complete_fail_count,
		 dwr->tx_reset_cancel_count,
		 dwr->tx_teardown_cancel_count,
		 dwr->tx_watchdog_arm_count,
		 dwr->tx_watchdog_fire_count,
		 dwr->tx_watchdog_clear_count,
		 !!test_bit(DWR_TX_F_QUEUES_STOPPED, &dwr->tx_flags));
}

static void dwr_tx_complete(struct urb *urb)
{
	struct dwr_tx_urb_ctx *ctx = urb->context;
	struct ieee80211_tx_info *info;

	if (!ctx)
		return;

	if (ctx->skb) {
		dwr_tx_release_slot(ctx->dwr);
		dwr_tx_progress(ctx->dwr, atomic_read(&ctx->dwr->tx_inflight) > 0);
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN) {
			if (ctx->dwr->tx_cancel_reason == DWR_TX_CANCEL_RESET) {
				ctx->dwr->tx_reset_cancel_count++;
				if (ctx->report_tx_status)
					dwr_tx_report_failed(ctx->dwr, ctx->skb, ctx->rate_idx);
				else
					ieee80211_free_txskb(ctx->dwr->hw, ctx->skb);
			} else {
				ctx->dwr->tx_teardown_cancel_count++;
				ieee80211_free_txskb(ctx->dwr->hw, ctx->skb);
			}
			goto out_free;
		}

		if (ctx->report_tx_status) {
			info = IEEE80211_SKB_CB(ctx->skb);
			ieee80211_tx_info_clear_status(info);
			info->status.rates[0].idx = ctx->rate_idx;
			info->status.rates[0].count = urb->status ? 0 : max_t(u8, ctx->rate_count, 1);
			info->status.rates[1].idx = -1;
			if (ctx->no_ack)
				info->flags |= IEEE80211_TX_STAT_NOACK_TRANSMITTED;
			/*
			 * OpenBSD rum(4) txeof() has only USB transfer completion and no
			 * host-visible per-frame ACK/retry report. Linux rt73usb uses a
			 * richer rt2x00 path that is not yet ported here.
			 * TODO(openbsd-rum-port): add hardware-backed TX result ingestion
			 * only when a confirmed RT2573 status source is wired in.
			 */
			ieee80211_tx_status_irqsafe(ctx->dwr->hw, ctx->skb);
		} else {
			ieee80211_free_txskb(ctx->dwr->hw, ctx->skb);
		}
		if (urb->status) {
			ctx->dwr->tx_complete_fail_count++;
			dwr_request_reset(ctx->dwr, "tx-complete", urb->status);
		}
	}

out_free:
	kfree(ctx->buf);
	kfree(ctx);
}

int dwr_tx_submit_frame(struct dwr_dev *dwr, struct sk_buff *skb,
			bool *ownership_transferred)
{
	struct dwr_tx_desc_min desc;
	struct dwr_tx_desc_min prot_desc;
	const struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct sk_buff *skb_prot = NULL;
	u16 fc;
	bool need_rts;
	bool need_cts;
	bool no_ack;
	int slots_needed;
	int slots_acquired = 0;
	int slots_submitted = 0;
	int ret;

	*ownership_transferred = false;
	if (!READ_ONCE(dwr->usb.running))
		return -ENETDOWN;
	if (skb->len < sizeof(struct ieee80211_hdr)) {
		ret = -EINVAL;
		goto fail_report;
	}

	fc = le16_to_cpu(((struct ieee80211_hdr *)skb->data)->frame_control);
	need_rts = !!(info->control.rates[0].flags & IEEE80211_TX_RC_USE_RTS_CTS);
	need_cts = !!(info->control.rates[0].flags & IEEE80211_TX_RC_USE_CTS_PROTECT);
	slots_needed = (need_rts || need_cts) ? 2 : 1;

	while (slots_acquired < slots_needed) {
		if (!dwr_tx_acquire_slot(dwr)) {
			ret = -EBUSY;
			goto fail_report;
		}
		slots_acquired++;
	}

	if (need_rts || need_cts) {
		if (!ieee80211_is_data(fc)) {
			dwr->tx_protection_reject_count++;
			dwr->tx_protection_reject_non_data_count++;
			ret = -EOPNOTSUPP;
			goto fail_report;
		}
		if (need_rts && need_cts) {
			dwr->tx_protection_reject_count++;
			ret = -EINVAL;
			goto fail_report;
		}

		skb_prot = dwr_tx_build_protection_skb(dwr, skb, need_rts);
		if (IS_ERR(skb_prot)) {
			dwr->tx_protection_reject_count++;
			dwr->tx_protection_reject_protected_data_count++;
			ret = PTR_ERR(skb_prot);
			skb_prot = NULL;
			goto fail_report;
		}
		ret = dwr_tx_build_desc(dwr, skb_prot, &prot_desc, 0,
					DWR_TX_MORE_FRAG, !need_rts, true);
		if (ret) {
			dwr->tx_protection_reject_count++;
			dwr->tx_protection_reject_protected_data_count++;
			goto fail_free_prot;
		}

		ret = dwr_tx_submit_urb(dwr, skb_prot, &prot_desc, 0, 1,
					!need_rts, false);
		if (ret) {
			dwr->tx_protection_reject_count++;
			dwr->tx_protection_reject_protected_data_count++;
			goto fail_free_prot;
		}
		slots_submitted++;
		skb_prot = NULL;
		dwr_tx_progress(dwr, true);
	}

	ret = dwr_tx_build_desc(dwr, skb, &desc, info->control.rates[0].idx,
				(need_rts || need_cts) ? DWR_TX_LONG_RETRY : 0,
				!!(info->flags & IEEE80211_TX_CTL_NO_ACK), false);
	if (ret) {
		goto fail_report;
	}

	no_ack = !!(info->flags & IEEE80211_TX_CTL_NO_ACK);
	ret = dwr_tx_submit_urb(dwr, skb, &desc, info->control.rates[0].idx,
				info->control.rates[0].count, no_ack, true);
	if (ret)
		goto fail_report;
	slots_submitted++;

	*ownership_transferred = true;
	dwr_tx_progress(dwr, true);
	return 0;

fail_free_prot:
	if (skb_prot)
		ieee80211_free_txskb(dwr->hw, skb_prot);
fail_report:
	while (slots_acquired > slots_submitted) {
		dwr_tx_release_slot(dwr);
		slots_acquired--;
	}
	if (ret == -ENOMEM)
		dwr->tx_local_alloc_fail_count++;
	else
		dwr->tx_local_desc_fail_count++;
	dwr_tx_report_failed(dwr, skb, info->control.rates[0].idx);
	*ownership_transferred = true;
	if (ret == -EIO || ret == -ESHUTDOWN || ret == -ENODEV || ret == -EPROTO ||
	    ret == -ETIME || ret == -ETIMEDOUT)
		dwr_request_reset(dwr, "tx-submit", ret);
	return ret;
}

void dwr_tx_cancel_pending(struct dwr_dev *dwr)
{
	usb_kill_anchored_urbs(&dwr->usb.tx_anchor);
	atomic_set(&dwr->tx_inflight, 0);
	dwr->tx_cancel_reason = DWR_TX_CANCEL_NONE;
	dwr_tx_progress(dwr, false);
	if (test_and_clear_bit(DWR_TX_F_QUEUES_STOPPED, &dwr->tx_flags)) {
		if (!test_bit(DWR_RESET_F_BLOCKED, &dwr->reset_flags)) {
			ieee80211_wake_queues(dwr->hw);
			dwr->tx_queue_wake_count++;
		}
	}
	dwr_tx_log_summary(dwr, "cancel_pending");
}

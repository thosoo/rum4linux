/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/ieee80211.h>
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
};

#define DWR_TX_VALID BIT(1)
#define DWR_TX_IFS_SIFS BIT(6)
#define DWR_TX_NEED_ACK BIT(3)
#define DWR_TX_OFDM BIT(5)
#define DWR_TX_MAX_FRAME_LEN 4095
#define DWR_PLCP_LENGEXT BIT(7)
#define DWR_PLCP_SHORT_PREAMBLE BIT(3)

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

static int dwr_tx_build_desc(struct dwr_dev *dwr, struct sk_buff *skb,
			     struct dwr_tx_desc_min *desc)
{
	u16 plcp_length;
	u8 signal;
	u8 rate_500k;
	u32 payload_len_crc;
	u16 wme;
	u32 flags;
	u32 remainder;
	bool ofdm = false;
	bool short_preamble;
	const struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	int ret;

	if (!skb || skb->len == 0 || skb->len > DWR_TX_MAX_FRAME_LEN)
		return -EMSGSIZE;

	memset(desc, 0, sizeof(*desc));

	ret = dwr_tx_signal_rate_500k_from_idx(info->control.rates[0].idx,
					       &signal, &rate_500k, &ofdm);
	if (ret)
		return ret;

	flags = DWR_TX_VALID | DWR_TX_IFS_SIFS | (skb->len << 16);
	if (ofdm)
		flags |= DWR_TX_OFDM;
	if (!(info->flags & IEEE80211_TX_CTL_NO_ACK))
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

	/* TODO(openbsd-rum-port): map ack-rate/duration/protection and multi-rate retry descriptor fields from if_rum.c. */
	return 0;
}

static void dwr_tx_complete(struct urb *urb)
{
	struct dwr_tx_urb_ctx *ctx = urb->context;
	struct ieee80211_tx_info *info;

	if (!ctx)
		return;

	if (ctx->skb) {
		if (urb->status == -ENOENT || urb->status == -ECONNRESET ||
		    urb->status == -ESHUTDOWN) {
			ieee80211_free_txskb(ctx->dwr->hw, ctx->skb);
			goto out_free;
		}

		info = IEEE80211_SKB_CB(ctx->skb);
		ieee80211_tx_info_clear_status(info);
		info->status.rates[0].idx = ctx->rate_idx;
		info->status.rates[0].count = max_t(u8, ctx->rate_count, 1);
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
	}

out_free:
	kfree(ctx->buf);
	kfree(ctx);
}

int dwr_tx_submit_frame(struct dwr_dev *dwr, struct sk_buff *skb,
			bool *ownership_transferred)
{
	struct dwr_tx_desc_min desc;
	const struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct dwr_tx_urb_ctx *ctx;
	struct urb *urb;
	u8 *buf;
	size_t xfer_len;
	int ret;
	size_t total;

	*ownership_transferred = false;
	if (!READ_ONCE(dwr->usb.running))
		return -ENETDOWN;

	ret = dwr_tx_build_desc(dwr, skb, &desc);
	if (ret)
		return ret;

	total = sizeof(desc) + skb->len;
	xfer_len = roundup(total, 4);
	buf = kzalloc(xfer_len, GFP_ATOMIC);
	if (!buf)
		return -ENOMEM;

	ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx) {
		kfree(buf);
		return -ENOMEM;
	}

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		kfree(ctx);
		kfree(buf);
		return -ENOMEM;
	}

	memcpy(buf, &desc, sizeof(desc));
	memcpy(buf + sizeof(desc), skb->data, skb->len);

	ctx->dwr = dwr;
	ctx->skb = skb;
	ctx->buf = buf;
	ctx->rate_idx = info->control.rates[0].idx;
	ctx->rate_count = info->control.rates[0].count;
	ctx->no_ack = !!(info->flags & IEEE80211_TX_CTL_NO_ACK);

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
		return ret;
	}

	*ownership_transferred = true;
	usb_free_urb(urb);
	return 0;
}

void dwr_tx_cancel_pending(struct dwr_dev *dwr)
{
	usb_kill_anchored_urbs(&dwr->usb.tx_anchor);
}

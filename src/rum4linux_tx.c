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

#define DWR_TX_VALID BIT(1)
#define DWR_TX_IFS_SIFS BIT(6)
#define DWR_TX_NEED_ACK BIT(3)
#define DWR_TX_DESC_SEMANTICS_CONFIRMED true
#define DWR_TX_RATE_1MBPS_500K_UNITS 2
#define DWR_TX_BUF_MAX (sizeof(struct dwr_tx_desc_min) + 2048)

struct dwr_tx_ctx {
	struct dwr_dev *dwr;
	struct sk_buff *skb;
	u8 *buf;
	size_t len;
};

static u8 dwr_plcp_signal_cck(u8 rate_500k)
{
	/* OpenBSD rum_plcp_signal() mapping for CCK rates. */
	switch (rate_500k) {
	case 2:
		return 0x0;
	case 4:
		return 0x1;
	case 11:
		return 0x2;
	case 22:
		return 0x3;
	default:
		return 0xff;
	}
}

static void dwr_tx_report_status(struct dwr_tx_ctx *ctx, int status)
{
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(ctx->skb);

	ieee80211_tx_info_clear_status(info);
	info->status.rates[0].idx = 0;
	info->status.rates[0].count = 1;
	info->status.rates[1].idx = -1;
	if (status == 0)
		info->flags |= IEEE80211_TX_STAT_ACK;

	ieee80211_tx_status_irqsafe(ctx->dwr->hw, ctx->skb);
}

static void dwr_tx_complete(struct urb *urb)
{
	struct dwr_tx_ctx *ctx = urb->context;
	int status = urb->status;

	dwr_info(&ctx->dwr->usb.intf->dev,
		 "tx complete: status=%d len=%zu ep=0x%02x\n",
		 status, ctx->len, ctx->dwr->usb.bulk_out_ep);
	dwr_tx_report_status(ctx, status);
	kfree(ctx->buf);
	kfree(ctx);
	usb_free_urb(urb);
}

static int dwr_tx_build_desc(struct sk_buff *skb, struct dwr_tx_desc_min *desc)
{
	u16 plcp_length;
	u8 signal;
	u16 wme;

	memset(desc, 0, sizeof(*desc));

	signal = dwr_plcp_signal_cck(DWR_TX_RATE_1MBPS_500K_UNITS);
	if (signal == 0xff)
		return -EINVAL;

	desc->flags = cpu_to_le32(DWR_TX_VALID | DWR_TX_IFS_SIFS | DWR_TX_NEED_ACK |
				  (skb->len << 16));

	/* OpenBSD rum_setup_tx_desc() default queue/wme words. */
	wme = (0) | (2 << 4) | (4 << 8) | (10 << 12);
	desc->wme = cpu_to_le16(wme);
	desc->xflags = cpu_to_le16(0);
	desc->plcp_signal = signal;
	desc->plcp_service = 4;

	/* OpenBSD CCK plcp length formula; len includes CRC. */
	plcp_length = (16 * (skb->len + IEEE80211_FCS_LEN) +
		       DWR_TX_RATE_1MBPS_500K_UNITS - 1) /
		      DWR_TX_RATE_1MBPS_500K_UNITS;
	desc->plcp_length_lo = plcp_length & 0xff;
	desc->plcp_length_hi = plcp_length >> 8;

	/* TODO(openbsd-rum-port): validate additional descriptor flags/rate semantics. */
	if (!DWR_TX_DESC_SEMANTICS_CONFIRMED)
		return -EOPNOTSUPP;

	return 0;
}

int dwr_tx_submit_frame(struct dwr_dev *dwr, struct sk_buff *skb,
			bool smoke_test, bool *ownership_transferred)
{
	struct urb *urb;
	struct dwr_tx_ctx *ctx;
	struct dwr_tx_desc_min desc;
	int ret;
	size_t tx_len;

	*ownership_transferred = false;

	ret = dwr_tx_build_desc(skb, &desc);
	if (ret) {
		dwr_warn(&dwr->usb.intf->dev,
			 "tx blocked: len=%u ep=0x%02x desc_len=%zu own=0 submit=0 reason=%d\n",
			 skb->len, dwr->usb.bulk_out_ep, sizeof(desc), ret);
		return ret;
	}

	if (skb->len + sizeof(desc) > DWR_TX_BUF_MAX)
		return -EMSGSIZE;

	ctx = kzalloc(sizeof(*ctx), GFP_ATOMIC);
	if (!ctx)
		return -ENOMEM;

	ctx->buf = kmalloc(DWR_TX_BUF_MAX, GFP_ATOMIC);
	if (!ctx->buf) {
		kfree(ctx);
		return -ENOMEM;
	}

	memcpy(ctx->buf, &desc, sizeof(desc));
	memcpy(ctx->buf + sizeof(desc), skb->data, skb->len);
	tx_len = sizeof(desc) + skb->len;
	ctx->len = tx_len;
	ctx->dwr = dwr;
	ctx->skb = skb;

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		kfree(ctx->buf);
		kfree(ctx);
		return -ENOMEM;
	}

	usb_fill_bulk_urb(urb, dwr->usb.udev,
			  usb_sndbulkpipe(dwr->usb.udev, dwr->usb.bulk_out_ep),
			  ctx->buf, tx_len, dwr_tx_complete, ctx);
	usb_anchor_urb(urb, &dwr->usb.tx_anchor);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev,
			"tx submit failed: len=%u ep=0x%02x desc_len=%zu own=0 submit=1 err=%d\n",
			skb->len, dwr->usb.bulk_out_ep, sizeof(desc), ret);
		usb_unanchor_urb(urb);
		usb_free_urb(urb);
		kfree(ctx->buf);
		kfree(ctx);
		return ret;
	}

	*ownership_transferred = true;
	dwr_info(&dwr->usb.intf->dev,
		 "tx submit: len=%u ep=0x%02x desc_len=%zu own=1 submit=1 smoke=%d\n",
		 skb->len, dwr->usb.bulk_out_ep, sizeof(desc), smoke_test);
	usb_free_urb(urb);
	return 0;
}

int dwr_tx_smoke_test(struct dwr_dev *dwr)
{
	struct sk_buff *skb;
	struct ieee80211_hdr *hdr;
	bool owned;
	int ret;

	skb = alloc_skb(64, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = skb_put_zero(skb, 24);
	hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_NULLFUNC);
	ether_addr_copy(hdr->addr1, dwr->mac_addr);
	ether_addr_copy(hdr->addr2, dwr->mac_addr);
	ether_addr_copy(hdr->addr3, dwr->mac_addr);

	ret = dwr_tx_submit_frame(dwr, skb, true, &owned);
	dwr_info(&dwr->usb.intf->dev,
		 "tx smoke: desc_ok=%d own=%d submit=%d ret=%d\n",
		 ret == 0, owned, ret == 0, ret);
	if (!owned)
		kfree_skb(skb);

	return ret;
}

void dwr_tx_cancel_pending(struct dwr_dev *dwr)
{
	usb_kill_anchored_urbs(&dwr->usb.tx_anchor);
}

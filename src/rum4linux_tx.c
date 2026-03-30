/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/ieee80211.h>
#include "rum4linux_tx.h"
#include "dwa111_rum_debug.h"

/*
 * Partially confirmed from OpenBSD if_rumreg.h rum_tx_desc layout.
 * TODO(openbsd-rum-port): validate complete descriptor semantics before enabling TX submission.
 */
struct dwr_tx_desc_min {
	__le32 flags;
	__le16 wme;
	__le16 xflags;
	u8 plcp_signal;
	u8 plcp_service;
	u8 plcp_length_lo;
	u8 plcp_length_hi;
};

#define DWR_TX_DESC_SEMANTICS_CONFIRMED false
#define DWR_TX_BUF_MAX (sizeof(struct dwr_tx_desc_min) + 2048)

struct dwr_tx_ctx {
	struct dwr_dev *dwr;
	u8 *buf;
	size_t len;
};

static void dwr_tx_complete(struct urb *urb)
{
	struct dwr_tx_ctx *ctx = urb->context;
	struct dwr_dev *dwr = ctx->dwr;

	dwr_info(&dwr->usb.intf->dev,
		 "tx complete: status=%d len=%zu ep=0x%02x\n",
		 urb->status, ctx->len, dwr->usb.bulk_out_ep);
	kfree(ctx->buf);
	kfree(ctx);
	usb_free_urb(urb);
}

static int dwr_tx_build_desc(struct dwr_dev *dwr, struct sk_buff *skb,
			     struct dwr_tx_desc_min *desc)
{
	memset(desc, 0, sizeof(*desc));
	desc->plcp_length_lo = skb->len & 0xff;
	desc->plcp_length_hi = (skb->len >> 8) & 0xff;

	/* TODO(openbsd-rum-port): set flags/rate/ack bits once descriptor semantics are fully confirmed. */
	if (!DWR_TX_DESC_SEMANTICS_CONFIRMED)
		return -EOPNOTSUPP;

	return 0;
}

int dwr_tx_submit_frame(struct dwr_dev *dwr, struct sk_buff *skb, bool smoke_test)
{
	struct urb *urb;
	struct dwr_tx_ctx *ctx;
	struct dwr_tx_desc_min desc;
	int ret;
	size_t tx_len;

	ret = dwr_tx_build_desc(dwr, skb, &desc);
	if (ret) {
		dwr_warn(&dwr->usb.intf->dev,
			 "tx blocked: len=%u ep=0x%02x desc_len=%zu attempted=%d reason=descriptor-unconfirmed\n",
			 skb->len, dwr->usb.bulk_out_ep, sizeof(desc), !smoke_test);
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

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		kfree(ctx->buf);
		kfree(ctx);
		return -ENOMEM;
	}

	usb_fill_bulk_urb(urb, dwr->usb.udev,
			  usb_sndbulkpipe(dwr->usb.udev, dwr->usb.bulk_out_ep),
			  ctx->buf, tx_len, dwr_tx_complete, ctx);

	ret = usb_submit_urb(urb, GFP_ATOMIC);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev,
			"tx submit failed: len=%u ep=0x%02x desc_len=%zu attempted=1 err=%d\n",
			skb->len, dwr->usb.bulk_out_ep, sizeof(desc), ret);
		usb_free_urb(urb);
		kfree(ctx->buf);
		kfree(ctx);
		return ret;
	}

	dwr_info(&dwr->usb.intf->dev,
		 "tx submit: len=%u ep=0x%02x desc_len=%zu attempted=1 smoke=%d\n",
		 skb->len, dwr->usb.bulk_out_ep, sizeof(desc), smoke_test);
	return 0;
}

int dwr_tx_smoke_test(struct dwr_dev *dwr)
{
	struct sk_buff *skb;
	struct ieee80211_hdr *hdr;

	skb = alloc_skb(64, GFP_KERNEL);
	if (!skb)
		return -ENOMEM;

	hdr = skb_put_zero(skb, 24);
	hdr->frame_control = cpu_to_le16(IEEE80211_FTYPE_DATA | IEEE80211_STYPE_NULLFUNC);
	ether_addr_copy(hdr->addr1, dwr->mac_addr);
	ether_addr_copy(hdr->addr2, dwr->mac_addr);
	ether_addr_copy(hdr->addr3, dwr->mac_addr);

	if (!DWR_TX_DESC_SEMANTICS_CONFIRMED) {
		dwr_warn(&dwr->usb.intf->dev,
			 "tx smoke test requested but descriptor semantics are unconfirmed\n");
		kfree_skb(skb);
		return -EOPNOTSUPP;
	}

	if (dwr_tx_submit_frame(dwr, skb, true)) {
		kfree_skb(skb);
		return -EIO;
	}

	kfree_skb(skb);
	return 0;
}

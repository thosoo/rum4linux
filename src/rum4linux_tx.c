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
#define DWR_TX_DESC_SEMANTICS_CONFIRMED false
#define DWR_TX_RATE_1MBPS_500K_UNITS 2

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

	/* TODO(openbsd-rum-port): validate complete TX descriptor semantics before enabling TX submission. */
	if (!DWR_TX_DESC_SEMANTICS_CONFIRMED)
		return -EOPNOTSUPP;

	return 0;
}

int dwr_tx_submit_frame(struct dwr_dev *dwr, struct sk_buff *skb,
			bool *ownership_transferred)
{
	struct dwr_tx_desc_min desc;
	int ret;

	*ownership_transferred = false;

	ret = dwr_tx_build_desc(skb, &desc);
	if (ret) {
		dwr_warn(&dwr->usb.intf->dev,
			 "tx blocked: len=%u ep=0x%02x desc_len=%zu submit=0 reason=%d\n",
			 skb->len, dwr->usb.bulk_out_ep, sizeof(desc), ret);
		return ret;
	}

	/* Defensive backstop: TX runtime is intentionally disabled until descriptors are confirmed. */
	dwr_warn(&dwr->usb.intf->dev,
		 "tx blocked: len=%u ep=0x%02x desc_len=%zu submit=0 reason=%d\n",
		 skb->len, dwr->usb.bulk_out_ep, sizeof(desc), -EOPNOTSUPP);
	return -EOPNOTSUPP;
}

void dwr_tx_cancel_pending(struct dwr_dev *dwr)
{
	(void)dwr;
	/* No-op while TX URB submission remains intentionally disabled. */
}

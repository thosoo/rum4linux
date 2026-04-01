/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/usb.h>
#include <linux/slab.h>
#include <linux/ieee80211.h>
#include <asm/unaligned.h>
#include "rum4linux_hw.h"
#include "rum4linux_rx.h"
#include "rum4linux_debug.h"

#ifndef RX_FLAG_FAILED_PLCP_CRC
#define RX_FLAG_FAILED_PLCP_CRC 0
/* TODO(openbsd-rum-port): require explicit PLCP failure flag once minimum kernel baseline is fixed. */
#endif

/*
 * Conservative RT2573/rum-style receive parsing notes:
 * - Linux rt73usb processes RX descriptor words 0/1 for byte-count, CRC, and PHY fields.
 * - OpenBSD rum(4) also treats USB RX payload as descriptor + frame with metadata.
 * - Exact bit semantics differ across chips/firmware variants; keep uncertain fields guarded.
 */
#define DWR_RX_DESC_LEN 24
#define DWR_RXD_W0_DROP BIT(1)
#define DWR_RXD_W0_CRC_ERROR BIT(6)
#define DWR_RXD_W0_OFDM BIT(7)
#define DWR_RXD_W0_DATABYTE_COUNT_MASK GENMASK(27, 16)
#define DWR_RXD_W1_SIGNAL_MASK GENMASK(7, 0)
#define DWR_RXD_W1_RSSI_MASK GENMASK(15, 8)
#define DWR_RXD_W1_FRAME_OFFSET_MASK GENMASK(30, 24)

static void dwr_rx_complete(struct urb *urb);

static bool dwr_rx_has_non_crc_drop_error(u32 word0)
{
	bool drop = !!(word0 & DWR_RXD_W0_DROP);
	bool crc = !!(word0 & DWR_RXD_W0_CRC_ERROR);

	/*
	 * OpenBSD if_rumreg.h names this bit RT2573_RX_DROP but does not
	 * define a narrower PLCP/PHY cause, and if_rum.c does not classify it.
	 * Keep CRC as the only explicit class, and treat DROP as a broader
	 * non-CRC receive-error class in this narrow path.
	 */
	return drop && !crc;
}

static int dwr_rx_rate_idx(bool ofdm, u8 signal)
{
	if (!ofdm) {
		switch (signal) {
		case 10:
			return 0;  /* 1 Mbps */
		case 20:
			return 1;  /* 2 Mbps */
		case 55:
			return 2;  /* 5.5 Mbps */
		case 110:
			return 3;  /* 11 Mbps */
		default:
			return -EINVAL;
		}
	}

	/*
	 * TODO(openbsd-rum-port): validate complete OFDM PLCP signal mapping across all rum(4)-family revisions.
	 * These values align with common rt2x00/rt73 OFDM signal coding.
	 */
	switch (signal) {
	case 0xb:
		return 4;  /* 6 Mbps */
	case 0xf:
		return 5;  /* 9 Mbps */
	case 0xa:
		return 6;  /* 12 Mbps */
	case 0xe:
		return 7;  /* 18 Mbps */
	case 0x9:
		return 8;  /* 24 Mbps */
	case 0xd:
		return 9;  /* 36 Mbps */
	case 0x8:
		return 10; /* 48 Mbps */
	case 0xc:
		return 11; /* 54 Mbps */
	default:
		return -EINVAL;
	}
}

static int dwr_rx_submit_slot(struct dwr_dev *dwr, struct dwr_rx_slot *slot,
			      gfp_t gfp, bool resubmit)
{
	int ret;

	usb_fill_bulk_urb(slot->urb, dwr->usb.udev,
			 usb_rcvbulkpipe(dwr->usb.udev, dwr->usb.bulk_in_ep),
			 slot->buf, DWR_RX_BUF_SIZE, dwr_rx_complete, slot);

	ret = usb_submit_urb(slot->urb, gfp);
	if (ret) {
		dwr_warn(&dwr->usb.intf->dev,
			 "rx urb[%u] submit failed ep=0x%02x err=%d resubmit=%d\n",
			 slot->index, dwr->usb.bulk_in_ep, ret, resubmit);
		return ret;
	}

	if (resubmit)
		atomic_inc(&dwr->rx.stats.urbs_resubmitted);
	else
		atomic_inc(&dwr->rx.stats.urbs_submitted);

	return 0;
}

static bool dwr_rx_parse_desc(struct dwr_dev *dwr, struct dwr_rx_slot *slot,
			      int actual_len, int *frame_off, int *frame_len,
			      struct ieee80211_rx_status *status)
{
	u32 word0, word1;
	u16 data_len;
	u8 signal;
	s8 rssi;
	u8 frame_offset;
	int rate_idx;
	bool ofdm;
	bool crc_error;
	bool non_crc_drop_error;
	bool allow_fcs_fail;
	bool allow_plcp_fail;
	int min_total;

	if (actual_len < DWR_RX_DESC_LEN) {
		atomic_inc(&dwr->rx.stats.drop_short);
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] short packet len=%d < desc=%u\n",
			slot->index, actual_len, DWR_RX_DESC_LEN);
		return false;
	}

	word0 = get_unaligned_le32(&slot->buf[0]);
	word1 = get_unaligned_le32(&slot->buf[4]);
	data_len = FIELD_GET(DWR_RXD_W0_DATABYTE_COUNT_MASK, word0);
	signal = FIELD_GET(DWR_RXD_W1_SIGNAL_MASK, word1);
	rssi = FIELD_GET(DWR_RXD_W1_RSSI_MASK, word1);
	frame_offset = FIELD_GET(DWR_RXD_W1_FRAME_OFFSET_MASK, word1);
	ofdm = !!(word0 & DWR_RXD_W0_OFDM);
	crc_error = !!(word0 & DWR_RXD_W0_CRC_ERROR);
	non_crc_drop_error = dwr_rx_has_non_crc_drop_error(word0);
	allow_fcs_fail = !!(READ_ONCE(dwr->filter_flags) & FIF_FCSFAIL);
	allow_plcp_fail = !!(READ_ONCE(dwr->filter_flags) & FIF_PLCPFAIL);

	/* TODO(openbsd-rum-port): confirm frame_offset semantics for all rum(4)-family revisions. */
	min_total = DWR_RX_DESC_LEN + frame_offset + data_len;
	if (!data_len || min_total > actual_len || min_total > DWR_RX_BUF_SIZE) {
		atomic_inc(&dwr->rx.stats.drop_bad_desc);
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] bad desc len=%d data_len=%u frame_off=%u min_total=%d\n",
			slot->index, actual_len, data_len, frame_offset, min_total);
		return false;
	}

	if (crc_error && !allow_fcs_fail) {
		atomic_inc(&dwr->rx.stats.drop_failed_fcs_filtered);
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] drop failed-FCS by filter policy signal=%u ofdm=%u data_len=%u\n",
			slot->index, signal, ofdm, data_len);
		return false;
	}

	/*
	 * RT2573_RX_DROP is source-confirmed as a broad drop bit, but not
	 * source-confirmed as a pure PLCP or PHY indicator in if_rum.c/reg.h.
	 * We therefore gate only non-CRC DROP frames with FIF_PLCPFAIL as the
	 * closest mac80211 failure-policy class for now.
	 * TODO(openbsd-rum-port): confirm full RT2573 RX_DROP cause taxonomy.
	 */
	if (non_crc_drop_error && !allow_plcp_fail) {
		atomic_inc(&dwr->rx.stats.drop_failed_plcp_filtered);
		atomic_inc(&dwr->rx.stats.drop_bad_desc);
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] drop non-CRC descriptor-drop by filter policy drop=%u crc=%u ofdm=%u signal=%u rssi=%d data_len=%u\n",
			slot->index,
			!!(word0 & DWR_RXD_W0_DROP), crc_error,
				ofdm,
				signal, rssi, data_len);
		return false;
	}

	rate_idx = dwr_rx_rate_idx(ofdm, signal);
	if (rate_idx < 0) {
		atomic_inc(&dwr->rx.stats.drop_bad_desc);
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] unsupported signal=%u ofdm=%u\n",
			slot->index, signal, ofdm);
		return false;
	}

	*frame_off = DWR_RX_DESC_LEN + frame_offset;
	/* rt73/rum path reports byte count including FCS; strip it before mac80211 delivery. */
	if (data_len < IEEE80211_FCS_LEN) {
		atomic_inc(&dwr->rx.stats.drop_bad_desc);
		return false;
	}
	*frame_len = data_len - IEEE80211_FCS_LEN;

	memset(status, 0, sizeof(*status));
	status->band = NL80211_BAND_2GHZ;
	status->signal = rssi + dwr->eeprom.rssi_2ghz_corr;
	WRITE_ONCE(dwr->link_rssi_dbm, status->signal);
	status->rate_idx = rate_idx;
	status->freq = ieee80211_channel_to_frequency(dwr->hw_state.current_channel,
						      NL80211_BAND_2GHZ);
	if (crc_error)
		status->flag |= RX_FLAG_FAILED_FCS_CRC;
	if (non_crc_drop_error)
		status->flag |= RX_FLAG_FAILED_PLCP_CRC;

	dwr_dbg(&dwr->usb.intf->dev,
		"rx urb[%u] desc ok word0=0x%08x word1=0x%08x data_len=%u frame_off=%u ofdm=%u signal=%u rssi=%d\n",
		slot->index, word0, word1, data_len, frame_offset,
		ofdm, signal, rssi);
	return true;
}

static void dwr_rx_deliver_frame(struct dwr_dev *dwr, struct dwr_rx_slot *slot,
				 int actual_len)
{
	struct ieee80211_rx_status status;
	struct sk_buff *skb;
	int frame_off;
	int frame_len;

	if (!dwr_rx_parse_desc(dwr, slot, actual_len, &frame_off, &frame_len, &status))
		return;

	if (frame_off < 0 || frame_len <= 0 || frame_off + frame_len > actual_len) {
		atomic_inc(&dwr->rx.stats.drop_bad_desc);
		return;
	}

	skb = dev_alloc_skb(frame_len + 64);
	if (!skb) {
		atomic_inc(&dwr->rx.stats.drop_bad_desc);
		return;
	}

	skb_reserve(skb, 2);
	skb_put_data(skb, &slot->buf[frame_off], frame_len);
	memcpy(IEEE80211_SKB_RXCB(skb), &status, sizeof(status));
	if (status.flag & RX_FLAG_FAILED_FCS_CRC) {
		atomic_inc(&dwr->rx.stats.delivered_failed_fcs);
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] delivering failed-FCS frame len=%d\n",
			slot->index, frame_len);
	}
	if (status.flag & RX_FLAG_FAILED_PLCP_CRC) {
		atomic_inc(&dwr->rx.stats.delivered_failed_plcp);
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] delivering non-CRC descriptor-drop frame len=%d\n",
			slot->index, frame_len);
	}
	ieee80211_rx_irqsafe(dwr->hw, skb);
	atomic_inc(&dwr->rx.stats.delivered);
}

static void dwr_rx_complete(struct urb *urb)
{
	struct dwr_rx_slot *slot = urb->context;
	struct dwr_dev *dwr;
	int status;

	if (!slot)
		return;
	dwr = slot->dwr;
	status = urb->status;
	atomic_inc(&dwr->rx.stats.urbs_completed);

	if (!READ_ONCE(dwr->usb.running))
		return;

	if (status == 0) {
		dwr_rx_deliver_frame(dwr, slot, urb->actual_length);
	} else if (status == -ENOENT || status == -ECONNRESET || status == -ESHUTDOWN) {
		return;
	} else {
		dwr_dbg(&dwr->usb.intf->dev,
			"rx urb[%u] completion error=%d len=%d\n",
			slot->index, status, urb->actual_length);
	}

	if (READ_ONCE(dwr->usb.running))
		dwr_rx_submit_slot(dwr, slot, GFP_ATOMIC, true);
}

void dwr_rx_init_state(struct dwr_dev *dwr)
{
	int i;

	memset(&dwr->rx, 0, sizeof(dwr->rx));
	atomic_set(&dwr->rx.stats.urbs_submitted, 0);
	atomic_set(&dwr->rx.stats.urbs_completed, 0);
	atomic_set(&dwr->rx.stats.urbs_resubmitted, 0);
	atomic_set(&dwr->rx.stats.drop_short, 0);
	atomic_set(&dwr->rx.stats.drop_bad_desc, 0);
	atomic_set(&dwr->rx.stats.drop_failed_fcs_filtered, 0);
	atomic_set(&dwr->rx.stats.drop_failed_plcp_filtered, 0);
	atomic_set(&dwr->rx.stats.delivered_failed_fcs, 0);
	atomic_set(&dwr->rx.stats.delivered_failed_plcp, 0);
	atomic_set(&dwr->rx.stats.delivered, 0);
	for (i = 0; i < DWR_RX_URB_COUNT; i++) {
		dwr->rx.slots[i].index = i;
		dwr->rx.slots[i].dwr = dwr;
	}
}

int dwr_rx_start(struct dwr_dev *dwr)
{
	int i;
	int ret;

	if (dwr->rx.started)
		return 0;

	for (i = 0; i < DWR_RX_URB_COUNT; i++) {
		struct dwr_rx_slot *slot = &dwr->rx.slots[i];

		slot->buf = kzalloc(DWR_RX_BUF_SIZE, GFP_KERNEL);
		if (!slot->buf) {
			ret = -ENOMEM;
			goto err_stop;
		}

		slot->urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!slot->urb) {
			ret = -ENOMEM;
			goto err_stop;
		}

		ret = dwr_rx_submit_slot(dwr, slot, GFP_KERNEL, false);
		if (ret)
			goto err_stop;
	}

	dwr->rx.started = true;
	dwr_info(&dwr->usb.intf->dev,
		 "rx start: urbs=%u buf=%u bulk-in=0x%02x\n",
		 DWR_RX_URB_COUNT, DWR_RX_BUF_SIZE, dwr->usb.bulk_in_ep);
	return 0;

err_stop:
	dwr_rx_stop(dwr);
	return ret;
}

void dwr_rx_stop(struct dwr_dev *dwr)
{
	int i;

	for (i = 0; i < DWR_RX_URB_COUNT; i++) {
		struct dwr_rx_slot *slot = &dwr->rx.slots[i];

		if (slot->urb)
			usb_kill_urb(slot->urb);
	}

	for (i = 0; i < DWR_RX_URB_COUNT; i++) {
		struct dwr_rx_slot *slot = &dwr->rx.slots[i];

		usb_free_urb(slot->urb);
		slot->urb = NULL;
		kfree(slot->buf);
		slot->buf = NULL;
	}

	dwr->rx.started = false;
}

void dwr_rx_log_summary(struct dwr_dev *dwr, const char *reason)
{
	dwr_info(&dwr->usb.intf->dev,
		 "rx summary (%s): submitted=%d completed=%d resubmitted=%d drop_short=%d drop_bad_desc=%d drop_fcs_filtered=%d drop_plcp_filtered=%d delivered=%d delivered_fcs_fail=%d delivered_plcp_fail=%d\n",
		 reason,
		 atomic_read(&dwr->rx.stats.urbs_submitted),
		 atomic_read(&dwr->rx.stats.urbs_completed),
		 atomic_read(&dwr->rx.stats.urbs_resubmitted),
		 atomic_read(&dwr->rx.stats.drop_short),
		 atomic_read(&dwr->rx.stats.drop_bad_desc),
		 atomic_read(&dwr->rx.stats.drop_failed_fcs_filtered),
		 atomic_read(&dwr->rx.stats.drop_failed_plcp_filtered),
		 atomic_read(&dwr->rx.stats.delivered),
		 atomic_read(&dwr->rx.stats.delivered_failed_fcs),
		 atomic_read(&dwr->rx.stats.delivered_failed_plcp));
}

/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RUM4LINUX_RX_H
#define _RUM4LINUX_RX_H

#include <linux/atomic.h>
#include <linux/usb.h>
#include <linux/types.h>

#define DWR_RX_URB_COUNT 4
#define DWR_RX_BUF_SIZE 4096

struct dwr_dev;

struct dwr_rx_stats {
	atomic_t urbs_submitted;
	atomic_t urbs_completed;
	atomic_t urbs_resubmitted;
	atomic_t drop_short;
	atomic_t drop_bad_desc;
	atomic_t drop_failed_fcs_filtered;
	atomic_t drop_failed_plcp_filtered;
	atomic_t delivered_failed_fcs;
	atomic_t delivered_failed_plcp;
	atomic_t delivered;
};

struct dwr_rx_slot {
	struct urb *urb;
	u8 *buf;
	unsigned int index;
	struct dwr_dev *dwr;
};

struct dwr_rx_state {
	bool started;
	struct dwr_rx_slot slots[DWR_RX_URB_COUNT];
	struct dwr_rx_stats stats;
};

void dwr_rx_init_state(struct dwr_dev *dwr);
int dwr_rx_start(struct dwr_dev *dwr);
void dwr_rx_stop(struct dwr_dev *dwr);
void dwr_rx_log_summary(struct dwr_dev *dwr, const char *reason);

#endif

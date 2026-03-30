/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RUM4LINUX_TX_H
#define _RUM4LINUX_TX_H

#include <linux/skbuff.h>
#include "dwa111_rum_hw.h"

int dwr_tx_submit_frame(struct dwr_dev *dwr, struct sk_buff *skb,
			bool smoke_test, bool *ownership_transferred);
int dwr_tx_smoke_test(struct dwr_dev *dwr);
void dwr_tx_cancel_pending(struct dwr_dev *dwr);

#endif

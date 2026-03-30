/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RUM4LINUX_BBP_H
#define _RUM4LINUX_BBP_H

#include "rum4linux_hw.h"

int dwr_bbp_write(struct dwr_dev *dwr, u8 reg, u8 val);
int dwr_bbp_read(struct dwr_dev *dwr, u8 reg, u8 *val);
int dwr_bbp_init(struct dwr_dev *dwr);

#endif

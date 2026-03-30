/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DWA111_RUM_DEBUG_H
#define _DWA111_RUM_DEBUG_H

#include <linux/device.h>

#define dwr_info(dev, fmt, ...) dev_info((dev), "dwa111_rum: " fmt, ##__VA_ARGS__)
#define dwr_warn(dev, fmt, ...) dev_warn((dev), "dwa111_rum: " fmt, ##__VA_ARGS__)
#define dwr_err(dev, fmt, ...)  dev_err((dev),  "dwa111_rum: " fmt, ##__VA_ARGS__)
#define dwr_dbg(dev, fmt, ...)  dev_dbg((dev),  "dwa111_rum: " fmt, ##__VA_ARGS__)

#endif

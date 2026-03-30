/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DWA111_RUM_HW_H
#define _DWA111_RUM_HW_H

#include <linux/usb.h>
#include <linux/mutex.h>
#include <net/mac80211.h>

#define DWR_USB_VID 0x07d1
#define DWR_USB_PID 0x3c06

#define DWR_USB_REQ_MAC_CSR   0x09
#define DWR_USB_REQ_EEPROM    0x07
#define DWR_USB_TIMEOUT_MS    1000

struct dwr_usb_state {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct mutex io_mutex;
	bool firmware_loaded;
	bool running;
};

struct dwr_dev {
	struct ieee80211_hw *hw;
	struct dwr_usb_state usb;
	spinlock_t tx_lock;
	struct work_struct rx_work;
	struct work_struct reset_work;
	u8 mac_addr[ETH_ALEN];
	bool registered_hw;
};

static inline struct dwr_dev *hw_to_dwr(struct ieee80211_hw *hw)
{
	return hw->priv;
}

int dwr_read_reg(struct dwr_dev *dwr, u16 reg, u32 *val);
int dwr_write_reg(struct dwr_dev *dwr, u16 reg, u32 val);
int dwr_read_eeprom(struct dwr_dev *dwr, u16 off, void *buf, size_t len);
int dwr_load_firmware(struct dwr_dev *dwr);
int dwr_hw_init(struct dwr_dev *dwr);
void dwr_hw_stop(struct dwr_dev *dwr);
int dwr_set_channel(struct dwr_dev *dwr, struct ieee80211_channel *chan);

#endif

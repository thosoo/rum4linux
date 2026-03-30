/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include "dwa111_rum_hw.h"
#include "dwa111_rum_debug.h"

#define DWR_FW_NAME "rum-rt2573"

static int dwr_ctrl_in(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
		       void *buf, u16 len)
{
	int ret;

	mutex_lock(&dwr->usb.io_mutex);
	ret = usb_control_msg(dwr->usb.udev,
		usb_rcvctrlpipe(dwr->usb.udev, 0),
		req,
		USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		value, index, buf, len, DWR_USB_TIMEOUT_MS);
	mutex_unlock(&dwr->usb.io_mutex);

	return ret < 0 ? ret : 0;
}

static int dwr_ctrl_out(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
			void *buf, u16 len)
{
	int ret;

	mutex_lock(&dwr->usb.io_mutex);
	ret = usb_control_msg(dwr->usb.udev,
		usb_sndctrlpipe(dwr->usb.udev, 0),
		req,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		value, index, buf, len, DWR_USB_TIMEOUT_MS);
	mutex_unlock(&dwr->usb.io_mutex);

	return ret < 0 ? ret : 0;
}

int dwr_read_reg(struct dwr_dev *dwr, u16 reg, u32 *val)
{
	__le32 tmp = 0;
	int ret;

	ret = dwr_ctrl_in(dwr, DWR_USB_REQ_MAC_CSR, 0, reg, &tmp, sizeof(tmp));
	if (ret)
		return ret;
	*val = le32_to_cpu(tmp);
	return 0;
}

int dwr_write_reg(struct dwr_dev *dwr, u16 reg, u32 val)
{
	__le32 tmp = cpu_to_le32(val);

	return dwr_ctrl_out(dwr, DWR_USB_REQ_MAC_CSR, 0, reg, &tmp, sizeof(tmp));
}

int dwr_read_eeprom(struct dwr_dev *dwr, u16 off, void *buf, size_t len)
{
	if (len > U16_MAX)
		return -EINVAL;
	return dwr_ctrl_in(dwr, DWR_USB_REQ_EEPROM, 0, off, buf, (u16)len);
}

int dwr_load_firmware(struct dwr_dev *dwr)
{
	const struct firmware *fw;
	int ret;

	/*
	 * TODO: Port the OpenBSD rum(4) firmware path here:
	 *  - request_firmware(DWR_FW_NAME)
	 *  - upload image in device-specific chunk sizes
	 *  - trigger MCU / 8051 handoff
	 *  - wait for post-firmware reset/re-enumeration semantics
	 */
	ret = request_firmware(&fw, DWR_FW_NAME, &dwr->usb.intf->dev);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "firmware %s not found: %d\n",
			DWR_FW_NAME, ret);
		return ret;
	}

	dwr_info(&dwr->usb.intf->dev,
		 "firmware placeholder loaded (%zu bytes), upload path TODO\n",
		 fw->size);

	release_firmware(fw);
	msleep(250);
	dwr->usb.firmware_loaded = true;
	return -EOPNOTSUPP;
}

int dwr_hw_init(struct dwr_dev *dwr)
{
	u32 mac_csr0;
	int ret;
	u8 eeprom[32] = {0};

	/*
	 * Intended order, mirroring the BSD driver conceptually:
	 *  1. verify MAC_CSR0 readability
	 *  2. read enough EEPROM to obtain MAC and RF/BBP config
	 *  3. apply BBP defaults
	 *  4. program RF/channel state
	 *  5. enable RX/TX engines
	 */
	ret = dwr_read_reg(dwr, 0x0000, &mac_csr0);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "MAC_CSR0 read failed: %d\n", ret);
		return ret;
	}
	ret = dwr_read_eeprom(dwr, 0x0000, eeprom, sizeof(eeprom));
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "EEPROM read failed: %d\n", ret);
		return ret;
	}

	if (!is_valid_ether_addr(eeprom))
		eth_random_addr(dwr->mac_addr);
	else
		ether_addr_copy(dwr->mac_addr, eeprom);

	dwr_info(&dwr->usb.intf->dev,
		 "stub hw_init ok, MAC %pM, MAC_CSR0=0x%08x\n",
		 dwr->mac_addr, mac_csr0);
	return 0;
}

void dwr_hw_stop(struct dwr_dev *dwr)
{
	dwr->usb.running = false;
}

int dwr_set_channel(struct dwr_dev *dwr, struct ieee80211_channel *chan)
{
	if (!chan)
		return -EINVAL;

	dwr_info(&dwr->usb.intf->dev,
		 "stub set_channel: center_freq=%d hw_value=%d\n",
		 chan->center_freq, chan->hw_value);
	return 0;
}

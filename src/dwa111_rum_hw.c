/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include "dwa111_rum_hw.h"
#include "dwa111_rum_debug.h"
#include "dwa111_rum_eeprom.h"
#include "dwa111_rum_fw.h"
#include "rum4linux_bbp.h"
#include "rum4linux_rf.h"

static int dwr_ctrl_in_once(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
			    void *buf, u16 len)
{
	return usb_control_msg(dwr->usb.udev,
		usb_rcvctrlpipe(dwr->usb.udev, 0),
		req,
		USB_DIR_IN | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		value, index, buf, len, DWR_USB_TIMEOUT_MS);
}

static int dwr_ctrl_out_once(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
			     void *buf, u16 len)
{
	return usb_control_msg(dwr->usb.udev,
		usb_sndctrlpipe(dwr->usb.udev, 0),
		req,
		USB_DIR_OUT | USB_TYPE_VENDOR | USB_RECIP_DEVICE,
		value, index, buf, len, DWR_USB_TIMEOUT_MS);
}

int dwr_ctrl_vendor_in(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
		      void *buf, u16 len)
{
	int ret = -EIO;
	int attempt;

	mutex_lock(&dwr->usb.io_mutex);
	for (attempt = 1; attempt <= DWR_USB_IO_RETRIES; attempt++) {
		ret = dwr_ctrl_in_once(dwr, req, value, index, buf, len);
		if (ret == len) {
			ret = 0;
			break;
		}
		if (ret < 0)
			dwr_dbg(&dwr->usb.intf->dev,
				"ctrl_in req=0x%02x idx=0x%04x try=%d err=%d\n",
				req, index, attempt, ret);
		else
			dwr_warn(&dwr->usb.intf->dev,
				 "ctrl_in short req=0x%02x idx=0x%04x try=%d got=%d expected=%u\n",
				 req, index, attempt, ret, len);
		usleep_range(attempt * 1000, attempt * 2000);
	}
	mutex_unlock(&dwr->usb.io_mutex);

	return ret;
}

int dwr_ctrl_vendor_out(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
		       void *buf, u16 len)
{
	int ret = -EIO;
	int attempt;

	mutex_lock(&dwr->usb.io_mutex);
	for (attempt = 1; attempt <= DWR_USB_IO_RETRIES; attempt++) {
		ret = dwr_ctrl_out_once(dwr, req, value, index, buf, len);
		if (ret == len) {
			ret = 0;
			break;
		}
		if (ret < 0)
			dwr_dbg(&dwr->usb.intf->dev,
				"ctrl_out req=0x%02x idx=0x%04x try=%d err=%d\n",
				req, index, attempt, ret);
		else
			dwr_warn(&dwr->usb.intf->dev,
				 "ctrl_out short req=0x%02x idx=0x%04x try=%d got=%d expected=%u\n",
				 req, index, attempt, ret, len);
		usleep_range(attempt * 1000, attempt * 2000);
	}
	mutex_unlock(&dwr->usb.io_mutex);

	return ret;
}

int dwr_read_reg(struct dwr_dev *dwr, u16 reg, u32 *val)
{
	__le32 tmp = 0;
	int ret;

	ret = dwr_ctrl_vendor_in(dwr, DWR_USB_REQ_READ_MAC, 0, reg,
				 &tmp, sizeof(tmp));
	if (ret)
		return ret;
	*val = le32_to_cpu(tmp);
	return 0;
}

int dwr_write_reg(struct dwr_dev *dwr, u16 reg, u32 val)
{
	__le32 tmp = cpu_to_le32(val);

	return dwr_ctrl_vendor_out(dwr, DWR_USB_REQ_WRITE_MAC, 0, reg,
				   &tmp, sizeof(tmp));
}

int dwr_read_eeprom(struct dwr_dev *dwr, u16 off, void *buf, size_t len)
{
	if (len > U16_MAX)
		return -EINVAL;

	return dwr_ctrl_vendor_in(dwr, DWR_USB_REQ_READ_EEPROM, 0, off,
				  buf, (u16)len);
}

static int dwr_verify_mac_regs(struct dwr_dev *dwr)
{
	int ret;

	ret = dwr_read_reg(dwr, DWR_MAC_CSR0, &dwr->hw_state.mac_csr0_before_fw);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "MAC_CSR0 pre-fw read failed: %d\n", ret);
		return ret;
	}

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR0, &dwr->hw_state.txrx_csr0_before_fw);
	if (ret)
		dwr_warn(&dwr->usb.intf->dev, "TXRX_CSR0 pre-fw read failed: %d\n", ret);

	if (!dwr->hw_state.mac_csr0_before_fw ||
	    dwr->hw_state.mac_csr0_before_fw == 0xffffffff) {
		dwr_err(&dwr->usb.intf->dev,
			"MAC_CSR0 pre-fw invalid 0x%08x (compare with rt73usb failure traces)\n",
			dwr->hw_state.mac_csr0_before_fw);
		return -ENODEV;
	}

	return 0;
}

static int dwr_soft_reset(struct dwr_dev *dwr)
{
	int ret;

	ret = dwr_write_reg(dwr, DWR_MAC_CSR1, DWR_RESET_ASIC | DWR_RESET_BBP);
	if (ret)
		return ret;
	ret = dwr_write_reg(dwr, DWR_MAC_CSR1, 0);
	if (ret)
		return ret;
	ret = dwr_write_reg(dwr, DWR_MAC_CSR1, DWR_HOST_READY);
	if (ret)
		return ret;
	ret = dwr_write_reg(dwr, DWR_MAC_CSR1, 0);
	if (ret)
		return ret;

	/* TODO(openbsd-rum-port): validate exact reset ordering against all revisions. */
	return 0;
}

static int dwr_wait_bbprf_awake(struct dwr_dev *dwr)
{
	u32 val;
	int i;
	int ret;

	for (i = 0; i < 200; i++) {
		ret = dwr_read_reg(dwr, DWR_MAC_CSR12, &val);
		if (!ret && (val & DWR_BBPRF_AWAKE))
			return 0;
		(void)dwr_write_reg(dwr, DWR_MAC_CSR12, DWR_FORCE_WAKEUP);
		usleep_range(1000, 2000);
	}

	dwr_err(&dwr->usb.intf->dev, "timeout waiting for BBP/RF wakeup\n");
	return -ETIMEDOUT;
}

static void dwr_log_init_summary(struct dwr_dev *dwr)
{
	dwr_info(&dwr->usb.intf->dev,
		 "init summary: module=rum4linux eeprom_valid=%d mac=%pM rf_rev=%u fw_uploaded=%d bbp_init=%d rf_init=%d chan=%u sanity=%d\n",
		 dwr->hw_state.eeprom_valid, dwr->mac_addr, dwr->eeprom.rf_rev,
		 dwr->hw_state.fw_uploaded, dwr->hw_state.bbp_init_ok,
		 dwr->hw_state.rf_init_ok, dwr->hw_state.current_channel,
		 dwr->hw_state.post_fw_sanity_ok);
}

int dwr_hw_init(struct dwr_dev *dwr)
{
	u32 mac_csr0;
	int ret;
	u8 default_chan = 1;

	memset(&dwr->hw_state, 0, sizeof(dwr->hw_state));
	dwr->hw_state.fw_required = true;

	if (dwr->hw->conf.chandef.chan && dwr->hw->conf.chandef.chan->hw_value)
		default_chan = dwr->hw->conf.chandef.chan->hw_value;

	ret = dwr_verify_mac_regs(dwr);
	if (ret)
		return ret;

	ret = dwr_soft_reset(dwr);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "soft reset failed: %d\n", ret);
		return ret;
	}

	ret = dwr_eeprom_parse(dwr);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "eeprom parse failed: %d\n", ret);
		return ret;
	}

	dwr->hw_state.eeprom_valid = dwr->eeprom.valid;
	ether_addr_copy(dwr->mac_addr, dwr->eeprom.mac_addr);
	SET_IEEE80211_PERM_ADDR(dwr->hw, dwr->mac_addr);

	ret = dwr_fw_upload(dwr);
	if (ret) {
		dwr_warn(&dwr->usb.intf->dev,
			 "firmware step incomplete/failed (%d), stopping init early\n",
			 ret);
		dwr_log_init_summary(dwr);
		return ret;
	}

	ret = dwr_wait_bbprf_awake(dwr);
	if (ret)
		return ret;

	ret = dwr_bbp_init(dwr);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "bbp init failed: %d\n", ret);
		dwr_log_init_summary(dwr);
		return ret;
	}

	ret = dwr_rf_set_channel_2ghz(dwr, default_chan);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev,
			"rf/channel init failed for channel %u: %d\n", default_chan, ret);
		dwr_log_init_summary(dwr);
		return ret;
	}

	ret = dwr_read_reg(dwr, DWR_MAC_CSR0, &mac_csr0);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "MAC_CSR0 post-init read failed: %d\n", ret);
		return ret;
	}
	dwr->hw_state.mac_csr0_after_fw = mac_csr0;
	dwr->hw_state.post_fw_sanity_ok = true;
	dwr->hw_state.hw_init_ok = true;

	dwr_log_init_summary(dwr);
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
	if (chan->band != NL80211_BAND_2GHZ)
		return -EOPNOTSUPP;

	if (!dwr->hw_state.hw_init_ok) {
		dwr_info(&dwr->usb.intf->dev,
			 "set_channel deferred until init completes (chan=%d)\n",
			 chan->hw_value);
		return 0;
	}

	return dwr_rf_set_channel_2ghz(dwr, chan->hw_value);
}

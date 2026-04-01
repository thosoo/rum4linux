/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/string.h>
#include "rum4linux_hw.h"
#include "rum4linux_debug.h"
#include "rum4linux_eeprom.h"
#include "rum4linux_fw.h"
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

struct dwr_sanity_capture {
	bool has_mac_csr0;
	bool has_phy_csr4;
	bool has_bbp0;
	bool has_bbp3;
	u32 mac_csr0;
	u32 phy_csr4;
	u8 bbp0;
	u8 bbp3;
};

static void dwr_set_channel_apply_origin(struct dwr_dev *dwr, u8 origin)
{
	if (origin >= DWR_CHAN_APPLY_ORIGIN_MAX)
		origin = DWR_CHAN_APPLY_ORIGIN_UNKNOWN;
	dwr->hw_state.last_channel_apply_origin = origin;
}

static void dwr_record_channel_apply_failure_origin(struct dwr_dev *dwr, u8 origin)
{
	dwr_set_channel_apply_origin(dwr, origin);
	dwr->hw_state.channel_apply_origin_count[origin]++;
}

static int dwr_post_channel_sanity(struct dwr_dev *dwr, bool recovery,
				   struct dwr_sanity_capture *capture)
{
	u32 mac_csr0;
	u32 phy_csr4;
	u8 bbp0;
	u8 bbp3;
	int ret;

	dwr->hw_state.post_chan_sanity_attempted = true;
	if (capture)
		memset(capture, 0, sizeof(*capture));

	ret = dwr_read_reg(dwr, DWR_MAC_CSR0, &mac_csr0);
	if (ret)
		dwr_set_channel_apply_origin(dwr,
			recovery ? DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_MAC_CSR0 :
				   DWR_CHAN_APPLY_ORIGIN_SANITY_READ_MAC_CSR0);
	if (ret)
		return ret;
	if (capture) {
		capture->has_mac_csr0 = true;
		capture->mac_csr0 = mac_csr0;
	}
	ret = dwr_read_reg(dwr, DWR_PHY_CSR4, &phy_csr4);
	if (ret)
		dwr_set_channel_apply_origin(dwr,
			recovery ? DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_PHY_CSR4 :
				   DWR_CHAN_APPLY_ORIGIN_SANITY_READ_PHY_CSR4);
	if (ret)
		return ret;
	if (capture) {
		capture->has_phy_csr4 = true;
		capture->phy_csr4 = phy_csr4;
	}
	ret = dwr_bbp_read(dwr, 0, &bbp0);
	if (ret)
		dwr_set_channel_apply_origin(dwr,
			recovery ? DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_BBP0 :
				   DWR_CHAN_APPLY_ORIGIN_SANITY_READ_BBP0);
	if (ret)
		return ret;
	if (capture) {
		capture->has_bbp0 = true;
		capture->bbp0 = bbp0;
	}
	ret = dwr_bbp_read(dwr, 3, &bbp3);
	if (ret)
		dwr_set_channel_apply_origin(dwr,
			recovery ? DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_BBP3 :
				   DWR_CHAN_APPLY_ORIGIN_SANITY_READ_BBP3);
	if (ret)
		return ret;
	if (capture) {
		capture->has_bbp3 = true;
		capture->bbp3 = bbp3;
	}

	dwr_info(&dwr->usb.intf->dev,
		 "post-chan sanity: mac_csr0=0x%08x phy_csr4=0x%08x bbp0=0x%02x bbp3=0x%02x\n",
		 mac_csr0, phy_csr4, bbp0, bbp3);

	if (!mac_csr0 || mac_csr0 == 0xffffffff || bbp0 == 0x00 || bbp0 == 0xff ||
	    bbp3 == 0x00 || bbp3 == 0xff) {
		dwr_set_channel_apply_origin(dwr,
			recovery ? DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_PATTERN_INVALID :
				   DWR_CHAN_APPLY_ORIGIN_SANITY_PATTERN_INVALID);
		return -EIO;
	}

	dwr->hw_state.mac_csr0_after_fw = mac_csr0;
	return 0;
}

static const char *dwr_channel_apply_stage_name(u8 stage)
{
	switch (stage) {
	case DWR_CHAN_APPLY_STAGE_BBP_PROFILE:
		return "bbp_profile";
	case DWR_CHAN_APPLY_STAGE_RF_SET:
		return "rf_set";
	case DWR_CHAN_APPLY_STAGE_POST_SANITY:
		return "post_sanity";
	case DWR_CHAN_APPLY_STAGE_RECOVERY_BBP_INIT:
		return "recovery_bbp_init";
	case DWR_CHAN_APPLY_STAGE_RECOVERY_BBP_PROFILE:
		return "recovery_bbp_profile";
	case DWR_CHAN_APPLY_STAGE_RECOVERY_RF_SET:
		return "recovery_rf_set";
	case DWR_CHAN_APPLY_STAGE_RECOVERY_POST_SANITY:
		return "recovery_post_sanity";
	default:
		return "none";
	}
}

static const char *dwr_channel_apply_origin_name(u8 origin)
{
	switch (origin) {
	case DWR_CHAN_APPLY_ORIGIN_NONE:
		return "none";
	case DWR_CHAN_APPLY_ORIGIN_BBP_PROFILE:
		return "bbp_profile";
	case DWR_CHAN_APPLY_ORIGIN_RF_SET:
		return "rf_set";
	case DWR_CHAN_APPLY_ORIGIN_SANITY_READ_MAC_CSR0:
		return "sanity_read_mac_csr0";
	case DWR_CHAN_APPLY_ORIGIN_SANITY_READ_PHY_CSR4:
		return "sanity_read_phy_csr4";
	case DWR_CHAN_APPLY_ORIGIN_SANITY_READ_BBP0:
		return "sanity_read_bbp0";
	case DWR_CHAN_APPLY_ORIGIN_SANITY_READ_BBP3:
		return "sanity_read_bbp3";
	case DWR_CHAN_APPLY_ORIGIN_SANITY_PATTERN_INVALID:
		return "sanity_pattern_invalid";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_INIT:
		return "recovery_bbp_init";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_PROFILE:
		return "recovery_bbp_profile";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_RF_SET:
		return "recovery_rf_set";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_MAC_CSR0:
		return "recovery_sanity_read_mac_csr0";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_PHY_CSR4:
		return "recovery_sanity_read_phy_csr4";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_BBP0:
		return "recovery_sanity_read_bbp0";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_BBP3:
		return "recovery_sanity_read_bbp3";
	case DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_PATTERN_INVALID:
		return "recovery_sanity_pattern_invalid";
	default:
		return "unknown";
	}
}

static u8 dwr_classify_channel_apply_error(u8 stage, int err)
{
	if (!err)
		return DWR_CHAN_APPLY_ERRCLASS_NONE;
	if (err == -EINVAL)
		return DWR_CHAN_APPLY_ERRCLASS_INVALID;
	if (err == -EOPNOTSUPP)
		return DWR_CHAN_APPLY_ERRCLASS_UNSUPPORTED;
	if (err == -ETIMEDOUT)
		return DWR_CHAN_APPLY_ERRCLASS_TIMEOUT;
	if (err == -EIO &&
	    (stage == DWR_CHAN_APPLY_STAGE_POST_SANITY ||
	     stage == DWR_CHAN_APPLY_STAGE_RECOVERY_POST_SANITY))
		return DWR_CHAN_APPLY_ERRCLASS_SANITY;
	if (err < 0)
		return DWR_CHAN_APPLY_ERRCLASS_IO;
	return DWR_CHAN_APPLY_ERRCLASS_UNKNOWN;
}

static const char *dwr_channel_apply_errclass_name(u8 errclass)
{
	switch (errclass) {
	case DWR_CHAN_APPLY_ERRCLASS_NONE:
		return "none";
	case DWR_CHAN_APPLY_ERRCLASS_INVALID:
		return "invalid";
	case DWR_CHAN_APPLY_ERRCLASS_UNSUPPORTED:
		return "unsupported";
	case DWR_CHAN_APPLY_ERRCLASS_TIMEOUT:
		return "timeout";
	case DWR_CHAN_APPLY_ERRCLASS_IO:
		return "io";
	case DWR_CHAN_APPLY_ERRCLASS_SANITY:
		return "sanity";
	default:
		return "unknown";
	}
}

static u8 dwr_channel_apply_failure_sanity_delta_state(bool prev_has, u32 prev_val,
						       bool curr_has, u32 curr_val)
{
	if (!prev_has || !curr_has)
		return DWR_CHAN_APPLY_DELTA_SANITY_MISSING;
	if (prev_val == curr_val)
		return DWR_CHAN_APPLY_DELTA_SANITY_SAME;
	return DWR_CHAN_APPLY_DELTA_SANITY_CHANGED;
}

static const char *dwr_channel_apply_sanity_delta_name(u8 delta)
{
	switch (delta) {
	case DWR_CHAN_APPLY_DELTA_SANITY_MISSING:
		return "missing_before_or_now";
	case DWR_CHAN_APPLY_DELTA_SANITY_SAME:
		return "same";
	case DWR_CHAN_APPLY_DELTA_SANITY_CHANGED:
		return "changed";
	default:
		return "unknown";
	}
}

static void dwr_record_channel_apply_error(struct dwr_dev *dwr, int err)
{
	u8 errclass = dwr_classify_channel_apply_error(dwr->hw_state.last_channel_apply_stage,
						       err);

	dwr->hw_state.last_channel_apply_err = err;
	dwr->hw_state.last_channel_apply_errclass = errclass;
	switch (errclass) {
	case DWR_CHAN_APPLY_ERRCLASS_INVALID:
		dwr->hw_state.channel_apply_errclass_invalid_count++;
		break;
	case DWR_CHAN_APPLY_ERRCLASS_UNSUPPORTED:
		dwr->hw_state.channel_apply_errclass_unsupported_count++;
		break;
	case DWR_CHAN_APPLY_ERRCLASS_TIMEOUT:
		dwr->hw_state.channel_apply_errclass_timeout_count++;
		break;
	case DWR_CHAN_APPLY_ERRCLASS_IO:
		dwr->hw_state.channel_apply_errclass_io_count++;
		break;
	case DWR_CHAN_APPLY_ERRCLASS_SANITY:
		dwr->hw_state.channel_apply_errclass_sanity_count++;
		break;
	case DWR_CHAN_APPLY_ERRCLASS_UNKNOWN:
		dwr->hw_state.channel_apply_errclass_unknown_count++;
		break;
	default:
		break;
	}
}

static void dwr_record_channel_apply_failure_snapshot(struct dwr_dev *dwr,
						      bool runtime, u8 chan,
						      const struct dwr_sanity_capture *capture)
{
	struct dwr_hw_state *hs = &dwr->hw_state;
	bool prev_valid = hs->last_channel_apply_failure_snapshot_valid;
	bool prev_has_mac_csr0 = hs->last_channel_apply_failure_has_mac_csr0;
	bool prev_has_phy_csr4 = hs->last_channel_apply_failure_has_phy_csr4;
	bool prev_has_bbp0 = hs->last_channel_apply_failure_has_bbp0;
	bool prev_has_bbp3 = hs->last_channel_apply_failure_has_bbp3;
	u32 prev_mac_csr0 = hs->last_channel_apply_failure_mac_csr0;
	u32 prev_phy_csr4 = hs->last_channel_apply_failure_phy_csr4;
	u8 prev_bbp0 = hs->last_channel_apply_failure_bbp0;
	u8 prev_bbp3 = hs->last_channel_apply_failure_bbp3;

	hs->last_channel_apply_failure_delta_valid = true;
	hs->last_channel_apply_failure_delta_prev_valid = prev_valid;
	hs->last_channel_apply_failure_delta_runtime_same = prev_valid &&
		(hs->last_channel_apply_failure_was_runtime == runtime);
	hs->last_channel_apply_failure_delta_channel_same = prev_valid &&
		(hs->last_channel_apply_failure_channel == chan);
	hs->last_channel_apply_failure_delta_stage_same = prev_valid &&
		(hs->last_channel_apply_failure_stage == hs->last_channel_apply_stage);
	hs->last_channel_apply_failure_delta_errclass_same = prev_valid &&
		(hs->last_channel_apply_failure_errclass == hs->last_channel_apply_errclass);
	hs->last_channel_apply_failure_delta_origin_same = prev_valid &&
		(hs->last_channel_apply_failure_origin == hs->last_channel_apply_origin);
	hs->last_channel_apply_failure_delta_errno_same = prev_valid &&
		(hs->last_channel_apply_failure_err == hs->last_channel_apply_err);
	hs->last_channel_apply_failure_delta_mac_csr0_state =
		dwr_channel_apply_failure_sanity_delta_state(prev_has_mac_csr0, prev_mac_csr0,
							     capture && capture->has_mac_csr0,
							     capture ? capture->mac_csr0 : 0);
	hs->last_channel_apply_failure_delta_phy_csr4_state =
		dwr_channel_apply_failure_sanity_delta_state(prev_has_phy_csr4, prev_phy_csr4,
							     capture && capture->has_phy_csr4,
							     capture ? capture->phy_csr4 : 0);
	hs->last_channel_apply_failure_delta_bbp0_state =
		dwr_channel_apply_failure_sanity_delta_state(prev_has_bbp0, prev_bbp0,
							     capture && capture->has_bbp0,
							     capture ? capture->bbp0 : 0);
	hs->last_channel_apply_failure_delta_bbp3_state =
		dwr_channel_apply_failure_sanity_delta_state(prev_has_bbp3, prev_bbp3,
							     capture && capture->has_bbp3,
							     capture ? capture->bbp3 : 0);

	dwr->hw_state.last_channel_apply_failure_snapshot_valid = true;
	dwr->hw_state.last_channel_apply_failure_was_runtime = runtime;
	dwr->hw_state.last_channel_apply_failure_channel = chan;
	dwr->hw_state.last_channel_apply_failure_stage = dwr->hw_state.last_channel_apply_stage;
	dwr->hw_state.last_channel_apply_failure_errclass = dwr->hw_state.last_channel_apply_errclass;
	dwr->hw_state.last_channel_apply_failure_origin = dwr->hw_state.last_channel_apply_origin;
	dwr->hw_state.last_channel_apply_failure_err = dwr->hw_state.last_channel_apply_err;

	dwr->hw_state.last_channel_apply_failure_has_mac_csr0 = capture && capture->has_mac_csr0;
	dwr->hw_state.last_channel_apply_failure_has_phy_csr4 = capture && capture->has_phy_csr4;
	dwr->hw_state.last_channel_apply_failure_has_bbp0 = capture && capture->has_bbp0;
	dwr->hw_state.last_channel_apply_failure_has_bbp3 = capture && capture->has_bbp3;
	if (capture && capture->has_mac_csr0)
		dwr->hw_state.last_channel_apply_failure_mac_csr0 = capture->mac_csr0;
	if (capture && capture->has_phy_csr4)
		dwr->hw_state.last_channel_apply_failure_phy_csr4 = capture->phy_csr4;
	if (capture && capture->has_bbp0)
		dwr->hw_state.last_channel_apply_failure_bbp0 = capture->bbp0;
	if (capture && capture->has_bbp3)
		dwr->hw_state.last_channel_apply_failure_bbp3 = capture->bbp3;
}

static int dwr_recover_channel_2ghz_once(struct dwr_dev *dwr, u8 chan,
					 bool runtime, const char *reason)
{
	int ret;
	struct dwr_sanity_capture capture;

	dwr->hw_state.recovery_attempted = true;
	dwr->hw_state.channel_recovery_attempt_count++;
	dwr_warn(&dwr->usb.intf->dev,
		 "channel apply (%s) failed, attempting one bounded recovery (chan=%u)\n",
		 reason, chan);

	dwr->hw_state.last_channel_apply_was_runtime = runtime;
	dwr->hw_state.last_channel_apply_channel = chan;
	dwr->hw_state.last_channel_apply_stage = DWR_CHAN_APPLY_STAGE_RECOVERY_BBP_INIT;
	dwr_set_channel_apply_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_INIT);
	ret = dwr_bbp_init(dwr);
	if (ret) {
		dwr->hw_state.channel_recovery_failure_count++;
		dwr_record_channel_apply_failure_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_INIT);
		dwr_record_channel_apply_error(dwr, ret);
		dwr_record_channel_apply_failure_snapshot(dwr, runtime, chan, NULL);
		return ret;
	}
	dwr->hw_state.last_channel_apply_stage = DWR_CHAN_APPLY_STAGE_RECOVERY_BBP_PROFILE;
	dwr_set_channel_apply_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_PROFILE);
	ret = dwr_apply_2ghz_bbp_profile(dwr);
	if (ret) {
		dwr->hw_state.channel_recovery_failure_count++;
		dwr_record_channel_apply_failure_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_PROFILE);
		dwr_record_channel_apply_error(dwr, ret);
		dwr_record_channel_apply_failure_snapshot(dwr, runtime, chan, NULL);
		return ret;
	}
	dwr->hw_state.last_channel_apply_stage = DWR_CHAN_APPLY_STAGE_RECOVERY_RF_SET;
	dwr_set_channel_apply_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RECOVERY_RF_SET);
	ret = dwr_rf_set_channel_2ghz(dwr, chan);
	if (ret) {
		dwr->hw_state.channel_recovery_failure_count++;
		dwr_record_channel_apply_failure_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RECOVERY_RF_SET);
		dwr_record_channel_apply_error(dwr, ret);
		dwr_record_channel_apply_failure_snapshot(dwr, runtime, chan, NULL);
		return ret;
	}
	dwr->hw_state.last_channel_apply_stage = DWR_CHAN_APPLY_STAGE_RECOVERY_POST_SANITY;
	ret = dwr_post_channel_sanity(dwr, true, &capture);
	if (!ret) {
		dwr->hw_state.recovery_succeeded = true;
		dwr->hw_state.channel_recovery_success_count++;
		dwr->hw_state.last_channel_apply_err = 0;
		dwr->hw_state.last_channel_apply_errclass = DWR_CHAN_APPLY_ERRCLASS_NONE;
		dwr_set_channel_apply_origin(dwr, DWR_CHAN_APPLY_ORIGIN_NONE);
	}
	if (ret) {
		dwr->hw_state.channel_recovery_failure_count++;
		dwr_record_channel_apply_failure_origin(dwr, dwr->hw_state.last_channel_apply_origin);
		dwr_record_channel_apply_error(dwr, ret);
		dwr_record_channel_apply_failure_snapshot(dwr, runtime, chan, &capture);
	}
	return ret;
}

static int dwr_apply_2ghz_rt2528_channel(struct dwr_dev *dwr, u8 chan,
					 const char *reason)
{
	bool runtime = !strcmp(reason, "runtime");
	int ret;

	if (runtime)
		dwr->hw_state.runtime_channel_apply_count++;
	else
		dwr->hw_state.init_channel_apply_count++;
	dwr->hw_state.last_channel_apply_was_runtime = runtime;
	dwr->hw_state.last_channel_apply_channel = chan;
	dwr->hw_state.last_channel_apply_stage = DWR_CHAN_APPLY_STAGE_BBP_PROFILE;
	dwr->hw_state.last_channel_apply_err = 0;
	dwr->hw_state.last_channel_apply_errclass = DWR_CHAN_APPLY_ERRCLASS_NONE;
	dwr_set_channel_apply_origin(dwr, DWR_CHAN_APPLY_ORIGIN_BBP_PROFILE);

	ret = dwr_apply_2ghz_bbp_profile(dwr);
	if (ret)
		goto fail_before_recovery;
	dwr->hw_state.last_channel_apply_stage = DWR_CHAN_APPLY_STAGE_RF_SET;
	dwr_set_channel_apply_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RF_SET);
	ret = dwr_rf_set_channel_2ghz(dwr, chan);
	if (ret)
		goto fail_before_recovery;
	dwr->hw_state.last_channel_apply_stage = DWR_CHAN_APPLY_STAGE_POST_SANITY;
	{
		struct dwr_sanity_capture capture;

		ret = dwr_post_channel_sanity(dwr, false, &capture);
		if (!ret) {
			dwr->hw_state.channel_apply_first_pass_success_count++;
			dwr_set_channel_apply_origin(dwr, DWR_CHAN_APPLY_ORIGIN_NONE);
			dwr_dbg(&dwr->usb.intf->dev,
				"channel apply (%s) first-pass success chan=%u\n",
				reason, chan);
			return 0;
		}
		dwr->hw_state.channel_apply_failure_count++;
		dwr_record_channel_apply_failure_origin(dwr, dwr->hw_state.last_channel_apply_origin);
		dwr_record_channel_apply_error(dwr, ret);
		dwr_record_channel_apply_failure_snapshot(dwr, runtime, chan, &capture);
		goto log_first_pass_failure;
	}

fail_before_recovery:
	dwr->hw_state.channel_apply_failure_count++;
	if (dwr->hw_state.last_channel_apply_stage == DWR_CHAN_APPLY_STAGE_BBP_PROFILE)
		dwr_record_channel_apply_failure_origin(dwr, DWR_CHAN_APPLY_ORIGIN_BBP_PROFILE);
	else if (dwr->hw_state.last_channel_apply_stage == DWR_CHAN_APPLY_STAGE_RF_SET)
		dwr_record_channel_apply_failure_origin(dwr, DWR_CHAN_APPLY_ORIGIN_RF_SET);
	dwr_record_channel_apply_error(dwr, ret);
	dwr_record_channel_apply_failure_snapshot(dwr, runtime, chan, NULL);

log_first_pass_failure:
	dwr_warn(&dwr->usb.intf->dev,
		 "channel apply (%s) first-pass failure chan=%u stage=%s err=%d class=%s origin=%s; recovery=1\n",
		 reason, chan,
		 dwr_channel_apply_stage_name(dwr->hw_state.last_channel_apply_stage),
		 ret,
		 dwr_channel_apply_errclass_name(dwr->hw_state.last_channel_apply_errclass),
		 dwr_channel_apply_origin_name(dwr->hw_state.last_channel_apply_origin));

	ret = dwr_recover_channel_2ghz_once(dwr, chan, runtime, reason);
	if (ret)
		dwr_err(&dwr->usb.intf->dev,
			"channel apply (%s) failed after bounded recovery chan=%u stage=%s err=%d class=%s origin=%s\n",
			reason, chan,
			dwr_channel_apply_stage_name(dwr->hw_state.last_channel_apply_stage),
		 ret,
		 dwr_channel_apply_errclass_name(dwr->hw_state.last_channel_apply_errclass),
		 dwr_channel_apply_origin_name(dwr->hw_state.last_channel_apply_origin));
	else
		dwr_info(&dwr->usb.intf->dev,
			 "channel apply (%s) recovered chan=%u stage=%s rec_attempt=1 rec_ok=1\n",
			 reason, chan,
			 dwr_channel_apply_stage_name(dwr->hw_state.last_channel_apply_stage));

	return ret;
}

void dwr_log_channel_apply_summary(struct dwr_dev *dwr, const char *reason)
{
	dwr_info(&dwr->usb.intf->dev,
		 "channel apply summary (%s): init=%u runtime=%u first_pass_ok=%u first_pass_fail=%u rec_attempt=%u rec_ok=%u rec_fail=%u class_invalid=%u class_unsupported=%u class_timeout=%u class_io=%u class_sanity=%u class_unknown=%u origin_bbp=%u origin_rf=%u origin_sanity_mac=%u origin_sanity_phy=%u origin_sanity_bbp0=%u origin_sanity_bbp3=%u origin_sanity_pattern=%u origin_rec_bbp_init=%u origin_rec_bbp=%u origin_rec_rf=%u origin_rec_sanity_mac=%u origin_rec_sanity_phy=%u origin_rec_sanity_bbp0=%u origin_rec_sanity_bbp3=%u origin_rec_sanity_pattern=%u origin_unknown=%u last_runtime=%u last_chan=%u last_stage=%s last_err=%d last_class=%s last_origin=%s fail_valid=%u fail_runtime=%u fail_chan=%u fail_stage=%s fail_err=%d fail_class=%s fail_origin=%s fail_has={mac:%u phy:%u bbp0:%u bbp3:%u} fail_vals={mac:0x%08x phy:0x%08x bbp0:0x%02x bbp3:0x%02x} fail_delta={valid:%u prev_valid:%u runtime:%s chan:%s stage:%s class:%s origin:%s err:%s sanity:{mac:%s phy:%s bbp0:%s bbp3:%s}}\n",
		 reason,
		 dwr->hw_state.init_channel_apply_count,
		 dwr->hw_state.runtime_channel_apply_count,
		 dwr->hw_state.channel_apply_first_pass_success_count,
		 dwr->hw_state.channel_apply_failure_count,
		 dwr->hw_state.channel_recovery_attempt_count,
		 dwr->hw_state.channel_recovery_success_count,
		 dwr->hw_state.channel_recovery_failure_count,
		 dwr->hw_state.channel_apply_errclass_invalid_count,
		 dwr->hw_state.channel_apply_errclass_unsupported_count,
		 dwr->hw_state.channel_apply_errclass_timeout_count,
		 dwr->hw_state.channel_apply_errclass_io_count,
		 dwr->hw_state.channel_apply_errclass_sanity_count,
		 dwr->hw_state.channel_apply_errclass_unknown_count,
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_BBP_PROFILE],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RF_SET],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_SANITY_READ_MAC_CSR0],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_SANITY_READ_PHY_CSR4],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_SANITY_READ_BBP0],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_SANITY_READ_BBP3],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_SANITY_PATTERN_INVALID],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_INIT],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_BBP_PROFILE],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_RF_SET],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_MAC_CSR0],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_PHY_CSR4],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_BBP0],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_READ_BBP3],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_RECOVERY_SANITY_PATTERN_INVALID],
		 dwr->hw_state.channel_apply_origin_count[DWR_CHAN_APPLY_ORIGIN_UNKNOWN],
		 dwr->hw_state.last_channel_apply_was_runtime,
		 dwr->hw_state.last_channel_apply_channel,
		 dwr_channel_apply_stage_name(dwr->hw_state.last_channel_apply_stage),
		 dwr->hw_state.last_channel_apply_err,
		 dwr_channel_apply_errclass_name(dwr->hw_state.last_channel_apply_errclass),
		 dwr_channel_apply_origin_name(dwr->hw_state.last_channel_apply_origin),
		 dwr->hw_state.last_channel_apply_failure_snapshot_valid,
		 dwr->hw_state.last_channel_apply_failure_was_runtime,
		 dwr->hw_state.last_channel_apply_failure_channel,
		 dwr_channel_apply_stage_name(dwr->hw_state.last_channel_apply_failure_stage),
		 dwr->hw_state.last_channel_apply_failure_err,
		 dwr_channel_apply_errclass_name(dwr->hw_state.last_channel_apply_failure_errclass),
		 dwr_channel_apply_origin_name(dwr->hw_state.last_channel_apply_failure_origin),
		 dwr->hw_state.last_channel_apply_failure_has_mac_csr0,
		 dwr->hw_state.last_channel_apply_failure_has_phy_csr4,
		 dwr->hw_state.last_channel_apply_failure_has_bbp0,
		 dwr->hw_state.last_channel_apply_failure_has_bbp3,
		 dwr->hw_state.last_channel_apply_failure_mac_csr0,
		 dwr->hw_state.last_channel_apply_failure_phy_csr4,
		 dwr->hw_state.last_channel_apply_failure_bbp0,
		 dwr->hw_state.last_channel_apply_failure_bbp3,
		 dwr->hw_state.last_channel_apply_failure_delta_valid,
		 dwr->hw_state.last_channel_apply_failure_delta_prev_valid,
		 dwr->hw_state.last_channel_apply_failure_delta_runtime_same ? "same" : "different",
		 dwr->hw_state.last_channel_apply_failure_delta_channel_same ? "same" : "different",
		 dwr->hw_state.last_channel_apply_failure_delta_stage_same ? "same" : "different",
		 dwr->hw_state.last_channel_apply_failure_delta_errclass_same ? "same" : "different",
		 dwr->hw_state.last_channel_apply_failure_delta_origin_same ? "same" : "different",
		 dwr->hw_state.last_channel_apply_failure_delta_errno_same ? "same" : "different",
		 dwr_channel_apply_sanity_delta_name(dwr->hw_state.last_channel_apply_failure_delta_mac_csr0_state),
		 dwr_channel_apply_sanity_delta_name(dwr->hw_state.last_channel_apply_failure_delta_phy_csr4_state),
		 dwr_channel_apply_sanity_delta_name(dwr->hw_state.last_channel_apply_failure_delta_bbp0_state),
		 dwr_channel_apply_sanity_delta_name(dwr->hw_state.last_channel_apply_failure_delta_bbp3_state));
}

static void dwr_log_init_summary(struct dwr_dev *dwr)
{
	dwr_info(&dwr->usb.intf->dev,
		 "init summary: module=rum4linux eeprom=%d fw=%d bbp=%d rf=%d calib=%d chan=%u sanity=%d sanity_try=%d recover_try=%d recover_ok=%d mac=%pM rf_rev=%u\n",
		 dwr->hw_state.eeprom_valid, dwr->hw_state.fw_uploaded,
		 dwr->hw_state.bbp_init_ok, dwr->hw_state.rf_init_ok,
		 dwr->hw_state.calibration_applied, dwr->hw_state.current_channel,
		 dwr->hw_state.post_fw_sanity_ok, dwr->hw_state.post_chan_sanity_attempted,
		 dwr->hw_state.recovery_attempted, dwr->hw_state.recovery_succeeded,
		 dwr->mac_addr, dwr->eeprom.rf_rev);
	dwr_log_channel_apply_summary(dwr, "init");
}

int dwr_apply_2ghz_bbp_profile(struct dwr_dev *dwr)
{
	u8 bbp17 = 0x20, bbp35 = 0x50, bbp96 = 0x48;
	u8 bbp97 = 0x48, bbp98 = 0x48, bbp104 = 0x2c;
	u32 phy_csr0;
	int ret;

	/* OpenBSD if_rum.c: rum_select_band() 2.4GHz BBP/PA profile. */
	if (dwr->eeprom.ext_2ghz_lna) {
		bbp17 += 0x10;
		bbp96 += 0x10;
		bbp104 += 0x10;
	}

	ret = dwr_bbp_write(dwr, 17, bbp17);
	if (ret)
		return ret;
	ret = dwr_bbp_write(dwr, 96, bbp96);
	if (ret)
		return ret;
	ret = dwr_bbp_write(dwr, 104, bbp104);
	if (ret)
		return ret;

	if (dwr->eeprom.ext_2ghz_lna) {
		ret = dwr_bbp_write(dwr, 75, 0x80);
		if (ret)
			return ret;
		ret = dwr_bbp_write(dwr, 86, 0x80);
		if (ret)
			return ret;
		ret = dwr_bbp_write(dwr, 88, 0x80);
		if (ret)
			return ret;
	}

	ret = dwr_bbp_write(dwr, 35, bbp35);
	if (ret)
		return ret;
	ret = dwr_bbp_write(dwr, 97, bbp97);
	if (ret)
		return ret;
	ret = dwr_bbp_write(dwr, 98, bbp98);
	if (ret)
		return ret;

	ret = dwr_read_reg(dwr, DWR_PHY_CSR0, &phy_csr0);
	if (ret)
		return ret;
	phy_csr0 &= ~(DWR_PHY_CSR0_PA_PE_2GHZ | DWR_PHY_CSR0_PA_PE_5GHZ);
	phy_csr0 |= DWR_PHY_CSR0_PA_PE_2GHZ;
	ret = dwr_write_reg(dwr, DWR_PHY_CSR0, phy_csr0);
	if (ret)
		return ret;

	dwr->bbp17_base = bbp17;
	return 0;
}

int dwr_hw_init(struct dwr_dev *dwr)
{
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

	ret = dwr_apply_2ghz_rt2528_channel(dwr, default_chan, "init");
	if (ret) {
		dwr_err(&dwr->usb.intf->dev,
			"rf/channel init failed for channel %u: %d\n", default_chan, ret);
		dwr_log_init_summary(dwr);
		return ret;
	}

	ret = dwr_set_rx_timing_defaults(dwr);
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "rx timing defaults init failed: %d\n", ret);
		dwr_log_init_summary(dwr);
		return ret;
	}
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
	int ret;

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

	return dwr_apply_2ghz_rt2528_channel(dwr, chan->hw_value, "runtime");
}

int dwr_set_macaddr(struct dwr_dev *dwr, const u8 *addr)
{
	u32 low;
	u32 high;
	int ret;

	if (!is_valid_ether_addr(addr))
		return -EINVAL;

	low = addr[0] | (addr[1] << 8) | (addr[2] << 16) | (addr[3] << 24);
	high = addr[4] | (addr[5] << 8) | (0xff << 16);

	ret = dwr_write_reg(dwr, DWR_MAC_CSR2, low);
	if (ret)
		return ret;
	ret = dwr_write_reg(dwr, DWR_MAC_CSR3, high);
	if (ret)
		return ret;

	return 0;
}

int dwr_set_vgc(struct dwr_dev *dwr, u8 vgc_level)
{
	int ret;

	if (dwr->vgc_level == vgc_level)
		return 0;

	ret = dwr_bbp_write(dwr, 17, vgc_level);
	if (ret)
		return ret;
	dwr->vgc_level = vgc_level;
	return 0;
}

int dwr_set_bssid(struct dwr_dev *dwr, const u8 *bssid)
{
	u32 low;
	u32 high;
	int ret;

	if (!bssid)
		return -EINVAL;

	/*
	 * OpenBSD rum(4) programs BSSID into RT2573 MAC_CSR4/5.
	 * MAC_CSR4 gets bytes 0..3, MAC_CSR5 gets bytes 4..5 in low bits.
	 */
	low = bssid[0] | (bssid[1] << 8) | (bssid[2] << 16) | (bssid[3] << 24);
	high = bssid[4] | (bssid[5] << 8) | (DWR_BSSID_ONE_MODE << 16);

	ret = dwr_write_reg(dwr, DWR_MAC_CSR4, low);
	if (ret)
		return ret;
	ret = dwr_write_reg(dwr, DWR_MAC_CSR5, high);
	if (ret)
		return ret;

	ether_addr_copy(dwr->bssid, bssid);
	dwr->bssid_valid = true;
	return 0;
}

int dwr_clear_bssid(struct dwr_dev *dwr)
{
	int ret;

	ret = dwr_write_reg(dwr, DWR_MAC_CSR4, 0);
	if (ret)
		return ret;
	ret = dwr_write_reg(dwr, DWR_MAC_CSR5, 0);
	if (ret)
		return ret;

	eth_zero_addr(dwr->bssid);
	dwr->bssid_valid = false;
	return 0;
}

int dwr_set_rx_filter(struct dwr_dev *dwr, unsigned int filter_flags)
{
	u32 reg;
	int ret;

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR0, &reg);
	if (ret)
		return ret;

	reg |= DWR_TXRX_CSR0_DROP_VER_ERROR;
	if (filter_flags & FIF_FCSFAIL)
		reg &= ~DWR_TXRX_CSR0_DROP_CRC_ERROR;
	else
		reg |= DWR_TXRX_CSR0_DROP_CRC_ERROR;
	if (filter_flags & FIF_PLCPFAIL)
		reg &= ~DWR_TXRX_CSR0_DROP_PHY_ERROR;
	else
		reg |= DWR_TXRX_CSR0_DROP_PHY_ERROR;
	if (filter_flags & (FIF_CONTROL | FIF_PSPOLL))
		reg &= ~DWR_TXRX_CSR0_DROP_CTL;
	else
		reg |= DWR_TXRX_CSR0_DROP_CTL;
	if (filter_flags & FIF_CONTROL)
		reg &= ~DWR_TXRX_CSR0_DROP_ACKCTS;
	else
		reg |= DWR_TXRX_CSR0_DROP_ACKCTS;
	/* Station-only conservative policy: keep DROP_NOT_TO_ME and DROP_TODS separate as rt73usb does. */
	if (filter_flags & FIF_OTHER_BSS)
		reg &= ~DWR_TXRX_CSR0_DROP_NOT_TO_ME;
	else
		reg |= DWR_TXRX_CSR0_DROP_NOT_TO_ME;
	if (filter_flags & FIF_OTHER_BSS)
		reg &= ~DWR_TXRX_CSR0_DROP_TODS;
	else
		reg |= DWR_TXRX_CSR0_DROP_TODS;
	if (filter_flags & FIF_ALLMULTI)
		reg &= ~DWR_TXRX_CSR0_DROP_MULTICAST;
	else
		reg |= DWR_TXRX_CSR0_DROP_MULTICAST;

	reg &= ~DWR_TXRX_CSR0_DROP_BROADCAST;

	ret = dwr_write_reg(dwr, DWR_TXRX_CSR0, reg);
	if (!ret)
		dwr->filter_flags = filter_flags;
	return ret;
}

int dwr_set_basic_rates(struct dwr_dev *dwr, u32 basic_rates)
{
	/*
	 * Current narrow TX path is CCK-only (idx 0..3) until OFDM TX
	 * descriptor/status semantics are source-confirmed.
	 */
	/* TODO(openbsd-rum-port): verify full rate-mask mapping across bands and OFDM TX support. */
	return dwr_write_reg(dwr, DWR_TXRX_CSR5, basic_rates & 0x000f);
}

int dwr_set_tsf_sync(struct dwr_dev *dwr, bool enable, u16 beacon_int)
{
	u32 reg;
	u32 timestamp_comp;
	int ret;

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR9, &reg);
	if (ret)
		return ret;

	/*
	 * OpenBSD rum_enable_tsf_sync() preserves TXRX_CSR9[31:24] and
	 * programs TSF mode/interval in the low 24 bits for STA mode.
	 */
	timestamp_comp = reg & DWR_TXRX_CSR9_TIMESTAMP_COMP_MASK;
	reg = timestamp_comp;

	if (enable) {
		reg |= ((u32)beacon_int * 16) & DWR_TXRX_CSR9_BEACON_INTERVAL_MASK;
		reg |= DWR_TXRX_CSR9_TSF_TICKING | DWR_TXRX_CSR9_ENABLE_TBTT;
		reg |= FIELD_PREP(DWR_TXRX_CSR9_TSF_MODE_MASK, 1);
	}

	return dwr_write_reg(dwr, DWR_TXRX_CSR9, reg);
}

int dwr_abort_tsf_sync(struct dwr_dev *dwr)
{
	u32 reg;
	int ret;

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR9, &reg);
	if (ret)
		return ret;

	/* OpenBSD rum_task(): abort TSF synchronization by clearing low 24 bits. */
	reg &= ~0x00ffffff;
	return dwr_write_reg(dwr, DWR_TXRX_CSR9, reg);
}

int dwr_set_mrr(struct dwr_dev *dwr, bool enable, bool cck_fallback)
{
	u32 reg;
	int ret;

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR4, &reg);
	if (ret)
		return ret;

	if (enable)
		reg |= DWR_TXRX_CSR4_MRR_ENABLE;
	else
		reg &= ~DWR_TXRX_CSR4_MRR_ENABLE;
	if (cck_fallback)
		reg |= DWR_TXRX_CSR4_MRR_CCK_FALLBACK;
	else
		reg &= ~DWR_TXRX_CSR4_MRR_CCK_FALLBACK;

	return dwr_write_reg(dwr, DWR_TXRX_CSR4, reg);
}

int dwr_set_retry_limits(struct dwr_dev *dwr, u8 short_retry, u8 long_retry,
			bool ofdm_rate_down, u8 ofdm_rate_step,
			bool ofdm_fallback_cck)
{
	u32 reg;
	int ret;

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR4, &reg);
	if (ret)
		return ret;

	if (ofdm_rate_down)
		reg |= DWR_TXRX_CSR4_OFDM_TX_RATE_DOWN;
	else
		reg &= ~DWR_TXRX_CSR4_OFDM_TX_RATE_DOWN;
	reg &= ~DWR_TXRX_CSR4_OFDM_TX_RATE_STEP_MASK;
	reg |= FIELD_PREP(DWR_TXRX_CSR4_OFDM_TX_RATE_STEP_MASK,
			  ofdm_rate_step & 0x3);
	if (ofdm_fallback_cck)
		reg |= DWR_TXRX_CSR4_OFDM_TX_FALLBACK_CCK;
	else
		reg &= ~DWR_TXRX_CSR4_OFDM_TX_FALLBACK_CCK;
	reg &= ~(DWR_TXRX_CSR4_LONG_RETRY_LIMIT_MASK |
		 DWR_TXRX_CSR4_SHORT_RETRY_LIMIT_MASK);
	reg |= FIELD_PREP(DWR_TXRX_CSR4_LONG_RETRY_LIMIT_MASK,
			  long_retry & 0xf);
	reg |= FIELD_PREP(DWR_TXRX_CSR4_SHORT_RETRY_LIMIT_MASK,
			  short_retry & 0xf);

	return dwr_write_reg(dwr, DWR_TXRX_CSR4, reg);
}

int dwr_set_erp_timing(struct dwr_dev *dwr, bool short_preamble,
		      u8 slot_time, u8 sifs, u16 eifs)
{
	u32 reg4, mac8, mac9;
	int ret;

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR4, &reg4);
	if (ret)
		return ret;
	if (short_preamble)
		reg4 |= DWR_TXRX_CSR4_SHORT_PREAMBLE;
	else
		reg4 &= ~DWR_TXRX_CSR4_SHORT_PREAMBLE;
	reg4 |= DWR_TXRX_CSR4_AUTORESPOND_ENABLE;
	ret = dwr_write_reg(dwr, DWR_TXRX_CSR4, reg4);
	if (ret)
		return ret;

	ret = dwr_read_reg(dwr, DWR_MAC_CSR9, &mac9);
	if (ret)
		return ret;
	mac9 &= ~DWR_MAC_CSR9_SLOT_TIME_MASK;
	mac9 |= FIELD_PREP(DWR_MAC_CSR9_SLOT_TIME_MASK, slot_time);
	ret = dwr_write_reg(dwr, DWR_MAC_CSR9, mac9);
	if (ret)
		return ret;

	ret = dwr_read_reg(dwr, DWR_MAC_CSR8, &mac8);
	if (ret)
		return ret;
	mac8 &= ~(DWR_MAC_CSR8_SIFS_MASK |
		  DWR_MAC_CSR8_SIFS_AFTER_RX_OFDM_MASK |
		  DWR_MAC_CSR8_EIFS_MASK);
	mac8 |= FIELD_PREP(DWR_MAC_CSR8_SIFS_MASK, sifs);
	/*
	 * OpenBSD if_rumreg.h RT2573_DEF_MAC sets MAC_CSR8 to 0x016c030a:
	 * SIFS=0x0a, SIFS_after_RX_OFDM=0x03, EIFS=0x016c.
	 */
	mac8 |= FIELD_PREP(DWR_MAC_CSR8_SIFS_AFTER_RX_OFDM_MASK,
			   DWR_RT2573_MAC_CSR8_SIFS_OFDM_DEFAULT);
	mac8 |= FIELD_PREP(DWR_MAC_CSR8_EIFS_MASK, eifs);
	return dwr_write_reg(dwr, DWR_MAC_CSR8, mac8);
}

int dwr_set_rx_timing_defaults(struct dwr_dev *dwr)
{
	u32 reg;
	int ret;

	ret = dwr_read_reg(dwr, DWR_TXRX_CSR0, &reg);
	if (ret)
		return ret;

	reg &= ~(DWR_TXRX_CSR0_RX_ACK_TIMEOUT_MASK | DWR_TXRX_CSR0_TSF_OFFSET_MASK);
	/*
	 * OpenBSD if_rumreg.h RT2573_DEF_MAC sets TXRX_CSR0=0x025fb032:
	 * RX_ACK_TIMEOUT=0x32 and TSF_OFFSET=24.
	 */
	reg |= FIELD_PREP(DWR_TXRX_CSR0_RX_ACK_TIMEOUT_MASK,
			  DWR_RT2573_TXRX_CSR0_ACK_TIMEOUT_DEFAULT);
	reg |= FIELD_PREP(DWR_TXRX_CSR0_TSF_OFFSET_MASK,
			  DWR_RT2573_TXRX_CSR0_TSF_OFFSET_DEFAULT);
	return dwr_write_reg(dwr, DWR_TXRX_CSR0, reg);
}

int dwr_read_rx_error_counters(struct dwr_dev *dwr, u16 *fcs_err,
			       u16 *plcp_err, u16 *physical_err,
			       u16 *false_cca)
{
	u32 sta0, sta1;
	int ret;

	ret = dwr_read_reg(dwr, DWR_STA_CSR0, &sta0);
	if (ret)
		return ret;
	ret = dwr_read_reg(dwr, DWR_STA_CSR1, &sta1);
	if (ret)
		return ret;

	if (fcs_err)
		*fcs_err = FIELD_GET(DWR_STA_CSR0_FCS_ERROR_MASK, sta0);
	if (plcp_err)
		*plcp_err = FIELD_GET(DWR_STA_CSR0_PLCP_ERROR_MASK, sta0);
	if (physical_err)
		*physical_err = FIELD_GET(DWR_STA_CSR1_PHYSICAL_ERROR_MASK, sta1);
	if (false_cca)
		*false_cca = FIELD_GET(DWR_STA_CSR1_FALSE_CCA_MASK, sta1);

	return 0;
}

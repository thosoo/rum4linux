/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/firmware.h>
#include <linux/delay.h>
#include <asm/unaligned.h>
#include "rum4linux_fw.h"
#include "rum4linux_debug.h"

static const char * const dwr_fw_candidates[] = {
	"rum-rt2573",
	"rt73.bin", /* TODO(openbsd-rum-port): verify whether this fallback applies across rum(4)-family devices. */
};

static int dwr_fw_upload_chunks(struct dwr_dev *dwr, const u8 *data, size_t len)
{
	u32 word;
	u16 reg = DWR_MCU_CODE_BASE;
	size_t off;
	size_t chunks = len / sizeof(word);
	int ret;

	if (len % sizeof(word)) {
		dwr_err(&dwr->usb.intf->dev,
			"firmware size %zu is not 4-byte aligned; upload sequence is unconfirmed\n",
			len);
		/* TODO(openbsd-rum-port): confirm final partial word handling. */
		return -EINVAL;
	}

	for (off = 0; off < len; off += sizeof(word), reg += sizeof(word)) {
		word = get_unaligned_le32(data + off);
		ret = dwr_write_reg(dwr, reg, word);
		if (ret) {
			dwr_err(&dwr->usb.intf->dev,
				"firmware chunk write failed at off=%zu reg=0x%04x: %d\n",
				off, reg, ret);
			return ret;
		}
	}

	dwr_info(&dwr->usb.intf->dev,
		 "firmware upload wrote %zu chunks (%zu bytes) starting at 0x%04x\n",
		 chunks, len, DWR_MCU_CODE_BASE);
	return 0;
}

static int dwr_fw_handoff(struct dwr_dev *dwr)
{
	int ret;

	ret = dwr_ctrl_vendor_out(dwr, DWR_USB_REQ_MCU_CNTL,
				  DWR_MCU_RUN, 0, NULL, 0);
	if (ret)
		dwr_err(&dwr->usb.intf->dev,
			"firmware handoff (MCU_RUN) failed: %d\n", ret);
	return ret;
}

static int dwr_fw_wait_ready(struct dwr_dev *dwr)
{
	u32 mac_csr0;
	int i;
	int ret;

	for (i = 0; i < 200; i++) {
		ret = dwr_read_reg(dwr, DWR_MAC_CSR0, &mac_csr0);
		if (!ret && mac_csr0 != 0 && mac_csr0 != 0xffffffff) {
			dwr_info(&dwr->usb.intf->dev,
				 "firmware post-handoff ready after %d ms (MAC_CSR0=0x%08x)\n",
				 i, mac_csr0);
			return 0;
		}
		usleep_range(1000, 2000);
	}

	/* TODO(openbsd-rum-port): verify whether this device requires USB re-enumeration here. */
	dwr_err(&dwr->usb.intf->dev,
		"firmware handoff did not reach ready state (MAC_CSR0 unreadable/invalid)\n");
	return -ETIMEDOUT;
}

int dwr_fw_upload(struct dwr_dev *dwr)
{
	const struct firmware *fw;
	const char *chosen = NULL;
	int ret;
	int i;

	if (dwr->hw_state.fw_uploaded)
		return 0;

	for (i = 0; i < ARRAY_SIZE(dwr_fw_candidates); i++) {
		ret = request_firmware(&fw, dwr_fw_candidates[i], &dwr->usb.intf->dev);
		if (!ret) {
			chosen = dwr_fw_candidates[i];
			break;
		}
		dwr_dbg(&dwr->usb.intf->dev, "firmware candidate %s not available: %d\n",
			dwr_fw_candidates[i], ret);
	}

	if (!chosen) {
		dwr_err(&dwr->usb.intf->dev,
			"no supported firmware file found (tried rum-rt2573, rt73.bin)\n");
		return -ENOENT;
	}

	dwr_info(&dwr->usb.intf->dev, "firmware selected: %s (%zu bytes)\n",
		 chosen, fw->size);

	ret = dwr_fw_upload_chunks(dwr, fw->data, fw->size);
	if (ret)
		goto out_release;

	ret = dwr_fw_handoff(dwr);
	if (ret)
		goto out_release;

	ret = dwr_fw_wait_ready(dwr);
	if (ret)
		goto out_release;

	dwr->hw_state.fw_uploaded = true;
	ret = 0;
out_release:
	if (ret)
		dwr_err(&dwr->usb.intf->dev,
			"firmware upload path failed at step=%s err=%d\n",
			dwr->hw_state.fw_uploaded ? "post-ready" : "upload/handoff", ret);
	release_firmware(fw);
	return ret;
}

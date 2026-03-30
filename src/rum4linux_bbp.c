/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/delay.h>
#include "rum4linux_bbp.h"
#include "rum4linux_debug.h"

struct dwr_bbp_init_ent {
	u8 reg;
	u8 val;
};

/* OpenBSD if_rumreg.h RT2573_DEF_BBP defaults. */
static const struct dwr_bbp_init_ent dwr_def_bbp[] = {
	{ 3, 0x80 }, { 15, 0x30 }, { 17, 0x20 }, { 21, 0xc8 },
	{ 22, 0x38 }, { 23, 0x06 }, { 24, 0xfe }, { 25, 0x0a },
	{ 26, 0x0d }, { 32, 0x0b }, { 34, 0x12 }, { 37, 0x07 },
	{ 39, 0xf8 }, { 41, 0x60 }, { 53, 0x10 }, { 54, 0x18 },
	{ 60, 0x10 }, { 61, 0x04 }, { 62, 0x04 }, { 75, 0xfe },
	{ 86, 0xfe }, { 88, 0xfe }, { 90, 0x0f }, { 99, 0x00 },
	{ 102, 0x16 }, { 107, 0x04 },
};

int dwr_bbp_write(struct dwr_dev *dwr, u8 reg, u8 val)
{
	u32 csr;
	int i;
	int ret;

	for (i = 0; i < 5; i++) {
		ret = dwr_read_reg(dwr, DWR_PHY_CSR3, &csr);
		if (ret)
			return ret;
		if (!(csr & DWR_BBP_BUSY))
			break;
		udelay(10);
	}
	if (i == 5)
		return -EBUSY;

	csr = DWR_BBP_BUSY | ((reg & 0x7f) << 8) | val;
	return dwr_write_reg(dwr, DWR_PHY_CSR3, csr);
}

int dwr_bbp_read(struct dwr_dev *dwr, u8 reg, u8 *val)
{
	u32 csr;
	int i;
	int ret;

	for (i = 0; i < 5; i++) {
		ret = dwr_read_reg(dwr, DWR_PHY_CSR3, &csr);
		if (ret)
			return ret;
		if (!(csr & DWR_BBP_BUSY))
			break;
		udelay(10);
	}
	if (i == 5)
		return -EBUSY;

	csr = DWR_BBP_BUSY | DWR_BBP_READ | (reg << 8);
	ret = dwr_write_reg(dwr, DWR_PHY_CSR3, csr);
	if (ret)
		return ret;

	for (i = 0; i < 100; i++) {
		ret = dwr_read_reg(dwr, DWR_PHY_CSR3, &csr);
		if (ret)
			return ret;
		if (!(csr & DWR_BBP_BUSY)) {
			*val = csr & 0xff;
			return 0;
		}
		udelay(10);
	}

	return -ETIMEDOUT;
}

int dwr_bbp_init(struct dwr_dev *dwr)
{
	u8 bbp0;
	u8 readback;
	int i;
	int ret;

	for (i = 0; i < 100; i++) {
		ret = dwr_bbp_read(dwr, 0, &bbp0);
		if (!ret && bbp0 != 0x00 && bbp0 != 0xff)
			break;
		usleep_range(1000, 2000);
	}
	if (i == 100) {
		dwr_err(&dwr->usb.intf->dev, "bbp init: timeout waiting for BBP ready\n");
		return -ETIMEDOUT;
	}

	for (i = 0; i < ARRAY_SIZE(dwr_def_bbp); i++) {
		ret = dwr_bbp_write(dwr, dwr_def_bbp[i].reg, dwr_def_bbp[i].val);
		if (ret) {
			dwr_err(&dwr->usb.intf->dev,
				"bbp init: default write reg=%u val=0x%02x failed: %d\n",
				dwr_def_bbp[i].reg, dwr_def_bbp[i].val, ret);
			return ret;
		}
	}

	for (i = 0; i < ARRAY_SIZE(dwr->eeprom.bbp_prom); i++) {
		u8 reg = dwr->eeprom.bbp_prom[i].reg;
		u8 val = dwr->eeprom.bbp_prom[i].val;

		if (reg == 0x00 || reg == 0xff)
			continue;

		ret = dwr_bbp_write(dwr, reg, val);
		if (ret) {
			dwr_err(&dwr->usb.intf->dev,
				"bbp init: eeprom override idx=%d reg=%u val=0x%02x failed: %d\n",
				i, reg, val, ret);
			return ret;
		}

		ret = dwr_bbp_read(dwr, reg, &readback);
		if (ret)
			return ret;
		dwr_info(&dwr->usb.intf->dev,
			 "bbp override: idx=%d reg=%u val=0x%02x readback=0x%02x\n",
			 i, reg, val, readback);
	}

	dwr->hw_state.bbp_init_ok = true;
	return 0;
}

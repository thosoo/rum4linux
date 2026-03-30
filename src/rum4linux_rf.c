/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/delay.h>
#include "rum4linux_rf.h"
#include "dwa111_rum_debug.h"

struct dwr_rf_chan_plan {
	u8 chan;
	u32 r1;
	u32 r2;
	u32 r3;
	u32 r4;
	bool confirmed;
};

/*
 * TODO(openbsd-rum-port): confirm RT2528-specific RF constants for channels 1-14.
 * OpenBSD rum(4) exports RF tables for RT5225/RT5226 families. This scaffold keeps
 * the table structure and channel gate for RT2528 but does not claim constants yet.
 */
static const struct dwr_rf_chan_plan dwr_rf2528_2ghz[] = {
	{ 1, 0, 0, 0, 0, false }, { 2, 0, 0, 0, 0, false },
	{ 3, 0, 0, 0, 0, false }, { 4, 0, 0, 0, 0, false },
	{ 5, 0, 0, 0, 0, false }, { 6, 0, 0, 0, 0, false },
	{ 7, 0, 0, 0, 0, false }, { 8, 0, 0, 0, 0, false },
	{ 9, 0, 0, 0, 0, false }, { 10, 0, 0, 0, 0, false },
	{ 11, 0, 0, 0, 0, false }, { 12, 0, 0, 0, 0, false },
	{ 13, 0, 0, 0, 0, false }, { 14, 0, 0, 0, 0, false },
};

static int dwr_rf_write(struct dwr_dev *dwr, u8 reg, u32 val)
{
	u32 csr;
	int i;
	int ret;

	for (i = 0; i < 5; i++) {
		ret = dwr_read_reg(dwr, DWR_PHY_CSR4, &csr);
		if (ret)
			return ret;
		if (!(csr & DWR_RF_BUSY))
			break;
		udelay(10);
	}
	if (i == 5)
		return -EBUSY;

	csr = DWR_RF_BUSY | DWR_RF_20BIT | ((val & 0xfffff) << 2) | (reg & 0x3);
	ret = dwr_write_reg(dwr, DWR_PHY_CSR4, csr);
	if (!ret)
		dwr_dbg(&dwr->usb.intf->dev,
			"rf write reg=%u val=0x%05x csr=0x%08x\n", reg & 0x3,
			val & 0xfffff, csr);
	return ret;
}

int dwr_rf_set_channel_2ghz(struct dwr_dev *dwr, u8 chan)
{
	const struct dwr_rf_chan_plan *plan = NULL;
	u32 verify;
	int ret;
	int i;

	if (dwr->eeprom.rf_rev != DWR_RF_2528) {
		dwr_err(&dwr->usb.intf->dev,
			"rf init: unsupported rf_rev=%u for RT2528-first path\n",
			dwr->eeprom.rf_rev);
		return -EOPNOTSUPP;
	}

	if (chan < 1 || chan > 14) {
		dwr_err(&dwr->usb.intf->dev, "rf init: invalid 2.4GHz channel %u\n", chan);
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(dwr_rf2528_2ghz); i++) {
		if (dwr_rf2528_2ghz[i].chan == chan) {
			plan = &dwr_rf2528_2ghz[i];
			break;
		}
	}
	if (!plan)
		return -EINVAL;

	if (!plan->confirmed) {
		dwr_err(&dwr->usb.intf->dev,
			"rf init: channel %u plan exists but RT2528 constants are not confirmed yet\n",
			chan);
		/* TODO(openbsd-rum-port): port confirmed RT2528 channel table constants. */
		return -EOPNOTSUPP;
	}

	ret = dwr_rf_write(dwr, DWR_RF1, plan->r1);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF2, plan->r2);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF3, plan->r3);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF4, plan->r4 | ((u32)dwr->eeprom.rffreq << 10));
	if (ret)
		return ret;

	ret = dwr_read_reg(dwr, DWR_PHY_CSR4, &verify);
	if (ret)
		return ret;

	dwr_info(&dwr->usb.intf->dev,
		 "rf init: channel=%u phy_csr4=0x%08x rffreq=%u\n",
		 chan, verify, dwr->eeprom.rffreq);
	dwr->hw_state.rf_init_ok = true;
	dwr->hw_state.current_channel = chan;
	return 0;
}

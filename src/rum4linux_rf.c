/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/delay.h>
#include "rum4linux_rf.h"
#include "rum4linux_bbp.h"
#include "rum4linux_debug.h"

struct dwr_rf_chan_plan {
	u8 chan;
	u32 r1;
	u32 r2;
	u32 r3;
	u32 r4;
};

#define DWR_BBP3_SMART_MODE BIT(0)
#define DWR_BBP94_DEFAULT   6

/*
 * Confidence source:
 * - Linux rt73usb rf_vals_bg_2528[] channel table (RT2528, 2.4GHz only).
 * - OpenBSD rum(4) style 3-phase RF write sequencing (RF3 bit2 toggle).
 */
static const struct dwr_rf_chan_plan dwr_rf2528_2ghz[] = {
	{ 1, 0x02c0c, 0x00786, 0x68255, 0xfea0b },
	{ 2, 0x02c0c, 0x00786, 0x68255, 0xfea1f },
	{ 3, 0x02c0c, 0x0078a, 0x68255, 0xfea0b },
	{ 4, 0x02c0c, 0x0078a, 0x68255, 0xfea1f },
	{ 5, 0x02c0c, 0x0078e, 0x68255, 0xfea0b },
	{ 6, 0x02c0c, 0x0078e, 0x68255, 0xfea1f },
	{ 7, 0x02c0c, 0x00792, 0x68255, 0xfea0b },
	{ 8, 0x02c0c, 0x00792, 0x68255, 0xfea1f },
	{ 9, 0x02c0c, 0x00796, 0x68255, 0xfea0b },
	{ 10, 0x02c0c, 0x00796, 0x68255, 0xfea1f },
	{ 11, 0x02c0c, 0x0079a, 0x68255, 0xfea0b },
	{ 12, 0x02c0c, 0x0079a, 0x68255, 0xfea1f },
	{ 13, 0x02c0c, 0x0079e, 0x68255, 0xfea0b },
	{ 14, 0x02c0c, 0x007a2, 0x68255, 0xfea13 },
};

struct dwr_chan_txpower {
	u8 rf_power;
	u8 bbp94;
	s8 eeprom_power;
};

static struct dwr_chan_txpower dwr_rf_txpower_for_chan(struct dwr_dev *dwr, u8 chan)
{
	s8 eep = dwr->eeprom.txpow_2ghz[chan - 1];
	struct dwr_chan_txpower txp = {
		.rf_power = 0,
		.bbp94 = DWR_BBP94_DEFAULT,
		.eeprom_power = eep,
	};

	/*
	 * Confidence source:
	 * - OpenBSD rum_set_chan(): per-channel signed EEPROM power drives RF3
	 *   (clamped to [0,31]) and adjusts BBP R94 around default 6.
	 * - Linux rt73usb_config_channel(): applies TXPOWER_TO_DEV(txpower) to RF3
	 *   and companion BBP R94 adjustment using the same default of 6.
	 */
	if (eep < 0) {
		if (eep >= -DWR_BBP94_DEFAULT)
			txp.bbp94 += eep;
		return txp;
	}
	if (eep <= 31) {
		txp.rf_power = eep;
		return txp;
	}
	txp.rf_power = 31;
	if (eep <= (31 + DWR_BBP94_DEFAULT))
		txp.bbp94 += eep - 31;
	return txp;
}

static int dwr_rf_set_chan_bbp(struct dwr_dev *dwr, u8 bbp94)
{
	u8 bbp3;
	int ret;

	ret = dwr_bbp_read(dwr, 3, &bbp3);
	if (ret)
		return ret;
	bbp3 &= ~DWR_BBP3_SMART_MODE;
	ret = dwr_bbp_write(dwr, 3, bbp3);
	if (ret)
		return ret;
	return dwr_bbp_write(dwr, 94, bbp94);
}

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
	u8 bbp3;
	struct dwr_chan_txpower txp;
	int ret;
	int i;
	u32 rf3;
	u32 rf4;

	if (dwr->eeprom.rf_rev != DWR_RF_2528) {
		dwr_err(&dwr->usb.intf->dev,
			"rf init: unsupported rf_rev=%u for RT2528 path\n",
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

	txp = dwr_rf_txpower_for_chan(dwr, chan);
	/* OpenBSD rum(4) uses power << 7 into RF3. */
	rf3 = (plan->r3 & ~(0x1f << 7)) | ((u32)txp.rf_power << 7);
	/* OpenBSD rum(4) uses rffreq << 10 in RF4 value domain. */
	rf4 = plan->r4 | ((u32)dwr->eeprom.rffreq << 10);

	ret = dwr_rf_write(dwr, DWR_RF1, plan->r1);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF2, plan->r2);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF3, rf3 & ~BIT(2));
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF4, rf4);
	if (ret)
		return ret;

	ret = dwr_rf_write(dwr, DWR_RF1, plan->r1);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF2, plan->r2);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF3, rf3 | BIT(2));
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF4, rf4);
	if (ret)
		return ret;

	ret = dwr_rf_write(dwr, DWR_RF1, plan->r1);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF2, plan->r2);
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF3, rf3 & ~BIT(2));
	if (ret)
		return ret;
	ret = dwr_rf_write(dwr, DWR_RF4, rf4);
	if (ret)
		return ret;

	udelay(10);

	ret = dwr_rf_set_chan_bbp(dwr, txp.bbp94);
	if (ret)
		return ret;

	/* No direct RF readback path; use PHY_CSR4 + BBP read as access sanity checks. */
	ret = dwr_read_reg(dwr, DWR_PHY_CSR4, &verify);
	if (ret)
		return ret;
	ret = dwr_bbp_read(dwr, 3, &bbp3);
	if (ret)
		return ret;

	dwr_info(&dwr->usb.intf->dev,
		 "rf init ok: rf=%u chan=%u txp_eep=%d txp_rf=%u bbp94=%u r1=0x%05x r2=0x%05x r3=0x%05x r4=0x%05x phy_csr4=0x%08x bbp3=0x%02x\n",
		 dwr->eeprom.rf_rev, chan, txp.eeprom_power, txp.rf_power, txp.bbp94,
		 plan->r1, plan->r2, rf3, rf4, verify, bbp3);
	dwr->hw_state.rf_init_ok = true;
	dwr->hw_state.current_channel = chan;
	dwr->hw_state.calibration_applied = true;
	return 0;
}

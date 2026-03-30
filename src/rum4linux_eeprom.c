/* SPDX-License-Identifier: GPL-2.0-only */
#include <linux/etherdevice.h>
#include <asm/unaligned.h>
#include "rum4linux_eeprom.h"
#include "rum4linux_debug.h"

/* OpenBSD if_rumreg.h EEPROM offsets. */
#define DWR_EEPROM_MACBBP            0x0000
#define DWR_EEPROM_ADDRESS           0x0004
#define DWR_EEPROM_ANTENNA           0x0020
#define DWR_EEPROM_CONFIG2           0x0022
#define DWR_EEPROM_BBP_BASE          0x0026
#define DWR_EEPROM_TXPOWER           0x0046
#define DWR_EEPROM_FREQ_OFFSET       0x005e
#define DWR_EEPROM_RSSI_2GHZ_OFFSET  0x009a
#define DWR_EEPROM_RSSI_5GHZ_OFFSET  0x009c

static bool dwr_is_uniform_buf(const u8 *buf, size_t len, u8 fill)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] != fill)
			return false;
	}
	return true;
}

int dwr_eeprom_read_word(struct dwr_dev *dwr, u16 off, u16 *val)
{
	__le16 tmp;
	int ret;

	ret = dwr_read_eeprom(dwr, off, &tmp, sizeof(tmp));
	if (ret)
		return ret;
	*val = le16_to_cpu(tmp);
	return 0;
}

int dwr_eeprom_read_block(struct dwr_dev *dwr, u16 off, void *buf, size_t len)
{
	return dwr_read_eeprom(dwr, off, buf, len);
}

void dwr_eeprom_dump(struct dwr_dev *dwr)
{
	struct dwr_eeprom_info *ee = &dwr->eeprom;

	dwr_info(&dwr->usb.intf->dev,
		 "eeprom: valid=%d mac=%pM macbbp=0x%04x rf=%u hw_radio=%u rx_ant=%u tx_ant=%u nb_ant=%u\n",
		 ee->valid, ee->mac_addr, ee->macbbp_rev, ee->rf_rev,
		 ee->hw_radio, ee->rx_ant, ee->tx_ant, ee->nb_ant);
	dwr_info(&dwr->usb.intf->dev,
		 "eeprom: ext_lna_2g=%u ext_lna_5g=%u rffreq=%u rssi_corr_2g=%d rssi_corr_5g=%d\n",
		 ee->ext_2ghz_lna, ee->ext_5ghz_lna, ee->rffreq,
		 ee->rssi_2ghz_corr, ee->rssi_5ghz_corr);
	dwr_info(&dwr->usb.intf->dev,
		 "eeprom: txpow2g[1..4]=%u,%u,%u,%u raw0=0x%04x raw1=0x%04x raw2=0x%04x raw3=0x%04x\n",
		 ee->txpow_2ghz[0], ee->txpow_2ghz[1], ee->txpow_2ghz[2], ee->txpow_2ghz[3],
		 ee->raw_words[0], ee->raw_words[1], ee->raw_words[2], ee->raw_words[3]);
}

int dwr_eeprom_parse(struct dwr_dev *dwr)
{
	struct dwr_eeprom_info *ee = &dwr->eeprom;
	u16 val;
	int ret;
	int i;

	memset(ee, 0, sizeof(*ee));

	ret = dwr_eeprom_read_block(dwr, 0, ee->raw, sizeof(ee->raw));
	if (ret) {
		dwr_err(&dwr->usb.intf->dev, "eeprom raw read failed: %d\n", ret);
		return ret;
	}

	if (dwr_is_uniform_buf(ee->raw, sizeof(ee->raw), 0x00) ||
	    dwr_is_uniform_buf(ee->raw, sizeof(ee->raw), 0xff)) {
		dwr_err(&dwr->usb.intf->dev,
			"eeprom looks invalid (all-0x00 or all-0xff)\n");
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(ee->raw_words); i++)
		ee->raw_words[i] = get_unaligned_le16(ee->raw + (i * sizeof(u16)));

	ret = dwr_eeprom_read_word(dwr, DWR_EEPROM_MACBBP, &ee->macbbp_rev);
	if (ret)
		return ret;

	ret = dwr_eeprom_read_block(dwr, DWR_EEPROM_ADDRESS, ee->mac_addr, ETH_ALEN);
	if (ret)
		return ret;
	if (!is_valid_ether_addr(ee->mac_addr)) {
		dwr_err(&dwr->usb.intf->dev, "invalid eeprom MAC %pM\n", ee->mac_addr);
		return -EINVAL;
	}

	ret = dwr_eeprom_read_word(dwr, DWR_EEPROM_ANTENNA, &val);
	if (ret)
		return ret;
	ee->rf_rev = (val >> 11) & 0x1f;
	ee->hw_radio = (val >> 10) & 0x1;
	ee->rx_ant = (val >> 4) & 0x3;
	ee->tx_ant = (val >> 2) & 0x3;
	ee->nb_ant = val & 0x3;

	ret = dwr_eeprom_read_word(dwr, DWR_EEPROM_CONFIG2, &val);
	if (ret)
		return ret;
	ee->ext_5ghz_lna = (val >> 6) & 0x1;
	ee->ext_2ghz_lna = (val >> 4) & 0x1;

	ret = dwr_eeprom_read_word(dwr, DWR_EEPROM_RSSI_2GHZ_OFFSET, &val);
	if (ret)
		return ret;
	if ((val & 0xff) != 0xff)
		ee->rssi_2ghz_corr = (s8)(val & 0xff);

	ret = dwr_eeprom_read_word(dwr, DWR_EEPROM_RSSI_5GHZ_OFFSET, &val);
	if (ret)
		return ret;
	if ((val & 0xff) != 0xff)
		ee->rssi_5ghz_corr = (s8)(val & 0xff);

	ret = dwr_eeprom_read_word(dwr, DWR_EEPROM_FREQ_OFFSET, &val);
	if (ret)
		return ret;
	if ((val & 0xff) != 0xff)
		ee->rffreq = val & 0xff;

	ret = dwr_eeprom_read_block(dwr, DWR_EEPROM_TXPOWER,
				    ee->txpow_2ghz,
				    DWR_EEPROM_TXPOWER_CHANS_2G);
	if (ret)
		return ret;

	ret = dwr_eeprom_read_block(dwr, DWR_EEPROM_BBP_BASE,
				    ee->bbp_prom,
				    sizeof(ee->bbp_prom));
	if (ret)
		return ret;

	ee->valid = true;
	dwr_eeprom_dump(dwr);
	return 0;
}

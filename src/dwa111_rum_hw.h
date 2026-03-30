/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _DWA111_RUM_HW_H
#define _DWA111_RUM_HW_H

#include <linux/usb.h>
#include <linux/mutex.h>
#include <net/mac80211.h>

#define DWR_USB_VID 0x07d1
#define DWR_USB_PID 0x3c06

/* OpenBSD rum(4)-style USB vendor requests for RT2573. */
#define DWR_USB_REQ_MCU_CNTL       0x01
#define DWR_USB_REQ_WRITE_MAC      0x02
#define DWR_USB_REQ_READ_MAC       0x03
#define DWR_USB_REQ_READ_MULTI_MAC 0x07
#define DWR_USB_REQ_READ_EEPROM    0x09
#define DWR_USB_TIMEOUT_MS         1000
#define DWR_USB_IO_RETRIES         3

/* TODO(openbsd-rum-port): confirm complete register map. */
#define DWR_MCU_CODE_BASE      0x0800
#define DWR_MAC_CSR0           0x3000
#define DWR_MAC_CSR1           0x3004
#define DWR_MAC_CSR9           0x3024
#define DWR_MAC_CSR10          0x3028
#define DWR_MAC_CSR12          0x3030
#define DWR_TXRX_CSR0          0x3040
#define DWR_TXRX_CSR1          0x3044
#define DWR_TXRX_CSR2          0x3048
#define DWR_TXRX_CSR3          0x304c
#define DWR_PHY_CSR0           0x3080
#define DWR_PHY_CSR3           0x308c
#define DWR_PHY_CSR4           0x3090
#define DWR_INT_SOURCE_CSR     0x3080 /* TODO(openbsd-rum-port): verify interrupt source register. */
#define DWR_INT_MASK_CSR       0x3084 /* TODO(openbsd-rum-port): verify interrupt mask register. */

#define DWR_RESET_ASIC         BIT(0)
#define DWR_RESET_BBP          BIT(1)
#define DWR_HOST_READY         BIT(2)
#define DWR_MCU_RUN            BIT(3)
#define DWR_BBPRF_AWAKE        BIT(3)
#define DWR_FORCE_WAKEUP       BIT(2)
#define DWR_BBP_READ           BIT(15)
#define DWR_BBP_BUSY           BIT(16)
#define DWR_RF_20BIT           (20U << 24)
#define DWR_RF_BUSY            BIT(31)

#define DWR_RF_5226            1
#define DWR_RF_2528            2
#define DWR_RF_5225            3
#define DWR_RF_2527            4

#define DWR_RF1                0
#define DWR_RF2                2
#define DWR_RF3                1
#define DWR_RF4                3

#define DWR_EEPROM_RAW_CACHE_LEN     256
#define DWR_EEPROM_TXPOWER_CHANS_2G  14
#define DWR_EEPROM_BBP_PROM_ENTRIES  16

struct dwr_eeprom_bbp_word {
	u8 reg;
	u8 val;
};

struct dwr_eeprom_info {
	bool valid;
	u8 mac_addr[ETH_ALEN];
	u16 macbbp_rev;
	u8 rf_rev;
	u8 hw_radio;
	u8 rx_ant;
	u8 tx_ant;
	u8 nb_ant;
	u8 ext_2ghz_lna;
	u8 ext_5ghz_lna;
	u8 rffreq;
	s8 rssi_2ghz_corr;
	s8 rssi_5ghz_corr;
	u8 txpow_2ghz[DWR_EEPROM_TXPOWER_CHANS_2G];
	struct dwr_eeprom_bbp_word bbp_prom[DWR_EEPROM_BBP_PROM_ENTRIES];

	/* TODO(openbsd-rum-port): map additional EEPROM words once confirmed. */
	u16 raw_words[16];
	u8 raw[DWR_EEPROM_RAW_CACHE_LEN];
};

struct dwr_hw_state {
	bool eeprom_valid;
	bool fw_required;
	bool fw_uploaded;
	bool bbp_init_ok;
	bool rf_init_ok;
	bool post_fw_sanity_ok;
	bool calibration_applied;
	bool post_chan_sanity_attempted;
	bool recovery_attempted;
	bool recovery_succeeded;
	bool hw_init_ok;
	u8 current_channel;
	u32 mac_csr0_before_fw;
	u32 mac_csr0_after_fw;
	u32 txrx_csr0_before_fw;
};

struct dwr_usb_state {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct mutex io_mutex;
	bool running;

	u8 bulk_in_ep;
	u8 bulk_out_ep;
	u8 intr_ep;
	u16 bulk_in_maxp;
	u16 bulk_out_maxp;
	u16 intr_maxp;
};

struct dwr_dev {
	struct ieee80211_hw *hw;
	struct dwr_usb_state usb;
	spinlock_t tx_lock;
	struct work_struct rx_work;
	struct work_struct reset_work;
	u8 mac_addr[ETH_ALEN];
	bool registered_hw;

	struct dwr_eeprom_info eeprom;
	struct dwr_hw_state hw_state;
};

static inline struct dwr_dev *hw_to_dwr(struct ieee80211_hw *hw)
{
	return hw->priv;
}

int dwr_ctrl_vendor_in(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
		      void *buf, u16 len);
int dwr_ctrl_vendor_out(struct dwr_dev *dwr, u8 req, u16 value, u16 index,
		       void *buf, u16 len);
int dwr_read_reg(struct dwr_dev *dwr, u16 reg, u32 *val);
int dwr_write_reg(struct dwr_dev *dwr, u16 reg, u32 val);
int dwr_read_eeprom(struct dwr_dev *dwr, u16 off, void *buf, size_t len);
int dwr_hw_init(struct dwr_dev *dwr);
void dwr_hw_stop(struct dwr_dev *dwr);
int dwr_set_channel(struct dwr_dev *dwr, struct ieee80211_channel *chan);

#endif

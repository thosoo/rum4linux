/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RUM4LINUX_HW_H
#define _RUM4LINUX_HW_H

#include <linux/usb.h>
#include <linux/mutex.h>
#include <linux/bitfield.h>
#include <net/mac80211.h>
#include "rum4linux_rx.h"

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
#define DWR_MAC_CSR2           0x3008
#define DWR_MAC_CSR3           0x300c
#define DWR_MAC_CSR4           0x3010
#define DWR_MAC_CSR5           0x3014
#define DWR_MAC_CSR8           0x3020
#define DWR_MAC_CSR9           0x3024
#define DWR_MAC_CSR10          0x3028
#define DWR_MAC_CSR12          0x3030
#define DWR_TXRX_CSR0          0x3040
#define DWR_TXRX_CSR1          0x3044
#define DWR_TXRX_CSR2          0x3048
#define DWR_TXRX_CSR3          0x304c
#define DWR_TXRX_CSR4          0x3050
#define DWR_TXRX_CSR5          0x3054
#define DWR_TXRX_CSR9          0x3064
#define DWR_TXRX_CSR10         0x3068
#define DWR_STA_CSR0           0x30c0
#define DWR_STA_CSR1           0x30c4
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
#define DWR_TXRX_CSR0_RX_ACK_TIMEOUT_MASK GENMASK(8, 0)
#define DWR_TXRX_CSR0_TSF_OFFSET_MASK GENMASK(14, 9)
#define DWR_TXRX_CSR0_DROP_CRC_ERROR BIT(17)
#define DWR_TXRX_CSR0_DROP_PHY_ERROR BIT(18)
#define DWR_TXRX_CSR0_DROP_CTL BIT(19)
#define DWR_TXRX_CSR0_DROP_NOT_TO_ME BIT(20)
#define DWR_TXRX_CSR0_DROP_TODS BIT(21)
#define DWR_TXRX_CSR0_DROP_VER_ERROR BIT(22)
#define DWR_TXRX_CSR0_DROP_MULTICAST BIT(23)
#define DWR_TXRX_CSR0_DROP_BROADCAST BIT(24)
#define DWR_TXRX_CSR0_DROP_ACKCTS BIT(25)
#define DWR_TXRX_CSR4_AUTORESPOND_ENABLE BIT(17)
#define DWR_TXRX_CSR4_SHORT_PREAMBLE BIT(18)
#define DWR_TXRX_CSR4_MRR_ENABLE BIT(19)
#define DWR_TXRX_CSR4_OFDM_TX_RATE_DOWN DWR_TXRX_CSR4_MRR_ENABLE
#define DWR_TXRX_CSR4_OFDM_TX_RATE_STEP_MASK GENMASK(21, 20)
#define DWR_TXRX_CSR4_MRR_CCK_FALLBACK BIT(22)
#define DWR_TXRX_CSR4_OFDM_TX_FALLBACK_CCK DWR_TXRX_CSR4_MRR_CCK_FALLBACK
#define DWR_TXRX_CSR4_LONG_RETRY_LIMIT_MASK GENMASK(27, 24)
#define DWR_TXRX_CSR4_SHORT_RETRY_LIMIT_MASK GENMASK(31, 28)
#define DWR_TXRX_CSR9_BEACON_INTERVAL_MASK GENMASK(15, 0)
#define DWR_TXRX_CSR9_TSF_TICKING BIT(16)
#define DWR_TXRX_CSR9_TSF_MODE_MASK GENMASK(18, 17)
#define DWR_TXRX_CSR9_ENABLE_TBTT BIT(19)
#define DWR_TXRX_CSR9_GENERATE_BEACON BIT(20)
#define DWR_TXRX_CSR9_TIMESTAMP_COMP_MASK GENMASK(31, 24)
#define DWR_MAC_CSR8_SIFS_MASK GENMASK(7, 0)
#define DWR_MAC_CSR8_SIFS_AFTER_RX_OFDM_MASK GENMASK(15, 8)
#define DWR_MAC_CSR8_EIFS_MASK GENMASK(31, 16)
#define DWR_MAC_CSR9_SLOT_TIME_MASK GENMASK(7, 0)
#define DWR_BSSID_ONE_MODE 3
#define DWR_STA_CSR0_FCS_ERROR_MASK GENMASK(15, 0)
#define DWR_STA_CSR0_PLCP_ERROR_MASK GENMASK(31, 16)
#define DWR_STA_CSR1_PHYSICAL_ERROR_MASK GENMASK(15, 0)
#define DWR_STA_CSR1_FALSE_CCA_MASK GENMASK(31, 16)
#define DWR_PHY_CSR0_PA_PE_2GHZ BIT(16)
#define DWR_PHY_CSR0_PA_PE_5GHZ BIT(17)

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
#define DWR_LINK_RSSI_INVALID_DBM    (-128)
#define DWR_CHAN_APPLY_STAGE_NONE                  0
#define DWR_CHAN_APPLY_STAGE_BBP_PROFILE           1
#define DWR_CHAN_APPLY_STAGE_RF_SET                2
#define DWR_CHAN_APPLY_STAGE_POST_SANITY           3
#define DWR_CHAN_APPLY_STAGE_RECOVERY_BBP_INIT     4
#define DWR_CHAN_APPLY_STAGE_RECOVERY_BBP_PROFILE  5
#define DWR_CHAN_APPLY_STAGE_RECOVERY_RF_SET       6
#define DWR_CHAN_APPLY_STAGE_RECOVERY_POST_SANITY  7
#define DWR_CHAN_APPLY_ERRCLASS_NONE               0
#define DWR_CHAN_APPLY_ERRCLASS_INVALID            1
#define DWR_CHAN_APPLY_ERRCLASS_UNSUPPORTED        2
#define DWR_CHAN_APPLY_ERRCLASS_TIMEOUT            3
#define DWR_CHAN_APPLY_ERRCLASS_IO                 4
#define DWR_CHAN_APPLY_ERRCLASS_SANITY             5
#define DWR_CHAN_APPLY_ERRCLASS_UNKNOWN            6

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
	u32 init_channel_apply_count;
	u32 runtime_channel_apply_count;
	u32 channel_apply_first_pass_success_count;
	u32 channel_apply_failure_count;
	u32 channel_recovery_attempt_count;
	u32 channel_recovery_success_count;
	u32 channel_recovery_failure_count;
	u32 channel_apply_errclass_invalid_count;
	u32 channel_apply_errclass_unsupported_count;
	u32 channel_apply_errclass_timeout_count;
	u32 channel_apply_errclass_io_count;
	u32 channel_apply_errclass_sanity_count;
	u32 channel_apply_errclass_unknown_count;
	bool last_channel_apply_was_runtime;
	u8 last_channel_apply_stage;
	u8 last_channel_apply_channel;
	u8 last_channel_apply_errclass;
	int last_channel_apply_err;
};

struct dwr_usb_state {
	struct usb_device *udev;
	struct usb_interface *intf;
	struct mutex io_mutex;
	bool running;
	struct usb_anchor tx_anchor;

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
	struct work_struct reset_work;
	struct delayed_work link_tuner_work;
	struct dwr_rx_state rx;
	u8 mac_addr[ETH_ALEN];
	u8 bssid[ETH_ALEN];
	bool bssid_valid;
	bool associated;
	u16 aid;
	s8 link_rssi_dbm;
	u8 bbp17_base;
	u8 vgc_level;
	struct ieee80211_vif *vif_sta;
	unsigned int filter_flags;
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
int dwr_apply_2ghz_bbp_profile(struct dwr_dev *dwr);
int dwr_set_macaddr(struct dwr_dev *dwr, const u8 *addr);
int dwr_set_vgc(struct dwr_dev *dwr, u8 vgc_level);
int dwr_set_bssid(struct dwr_dev *dwr, const u8 *bssid);
int dwr_clear_bssid(struct dwr_dev *dwr);
int dwr_set_rx_filter(struct dwr_dev *dwr, unsigned int filter_flags);
int dwr_set_basic_rates(struct dwr_dev *dwr, u32 basic_rates);
int dwr_set_tsf_sync(struct dwr_dev *dwr, bool enable, u16 beacon_int);
int dwr_abort_tsf_sync(struct dwr_dev *dwr);
int dwr_set_mrr(struct dwr_dev *dwr, bool enable, bool cck_fallback);
int dwr_set_retry_limits(struct dwr_dev *dwr, u8 short_retry, u8 long_retry,
			bool ofdm_rate_down, u8 ofdm_rate_step,
			bool ofdm_fallback_cck);
int dwr_set_erp_timing(struct dwr_dev *dwr, bool short_preamble,
		      u8 slot_time, u8 sifs, u16 eifs);
int dwr_set_rx_timing_defaults(struct dwr_dev *dwr);
int dwr_read_rx_error_counters(struct dwr_dev *dwr, u16 *fcs_err,
			       u16 *plcp_err, u16 *physical_err,
			       u16 *false_cca);
void dwr_log_channel_apply_summary(struct dwr_dev *dwr, const char *reason);

#endif

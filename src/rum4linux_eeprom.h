/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RUM4LINUX_EEPROM_H
#define _RUM4LINUX_EEPROM_H

#include "rum4linux_hw.h"

int dwr_eeprom_read_word(struct dwr_dev *dwr, u16 off, u16 *val);
int dwr_eeprom_read_block(struct dwr_dev *dwr, u16 off, void *buf, size_t len);
int dwr_eeprom_parse(struct dwr_dev *dwr);
void dwr_eeprom_dump(struct dwr_dev *dwr);

#endif

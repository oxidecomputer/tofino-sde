/*******************************************************************************
 * Copyright (c) 2015-2022 Barefoot Networks, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * $Id: $
 *
 ******************************************************************************/

#ifndef _BF_NEWPORT_TLV_EEPROM_H
#define _BF_NEWPORT_TLV_EEPROM_H

#include <bf_pltfm_types/bf_pltfm_types.h>

#ifdef __cplusplus
extern "C" {
#endif

int bf_pltfm_get_tlv_eeprom_max_read_batch_size();

bf_pltfm_status_t bf_pltfm_read_tlv_eeprom(uint16_t offset,
                                           uint8_t *buffer,
                                           uint8_t length);

#ifdef __cplusplus
}
#endif /* C++ */

#endif /* _BF_NEWPORT_TLV_EEPROM_H */

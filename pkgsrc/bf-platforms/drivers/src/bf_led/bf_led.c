/*******************************************************************************
 * Copyright (c) 2015-2020 Barefoot Networks, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * $Id: $
 *
 ******************************************************************************/

#include <bf_pltfm_types/bf_pltfm_types.h>
#include <bf_led/bf_led.h>
#include <bf_pltfm_led.h>

int bf_port_led_set(int chip_id,
                    bf_pltfm_port_info_t *port_info,
                    bf_led_condition_t led_cond) {
  return (bf_pltfm_port_led_set(chip_id, port_info, led_cond));
}

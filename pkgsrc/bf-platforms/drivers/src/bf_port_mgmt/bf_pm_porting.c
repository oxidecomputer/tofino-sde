/*******************************************************************************
 * Copyright (c) 2015-2020 Barefoot Networks, Inc.
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * $Id: $
 *
 ******************************************************************************/

// Module includes
#include <bf_switchd/bf_switchd.h>
#include <tofino/bf_pal/bf_pal_types.h>
#include <tofino/bf_pal/bf_pal_pltfm_porting.h>
#include <bf_port_mgmt/bf_port_mgmt_intf.h>
#include <bf_pltfm_ext_phy.h>
#include <bf_bd_cfg/bf_bd_cfg_intf.h>
#include <port_mgr/bf_port_if.h>
#include <port_mgr/bf_fsm_if.h>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include <bf_pltfm.h>
// Local header includes
#include "bf_pm_priv.h"

// Until we integrate it with SDK
bf_status_t bf_pm_encoding_mode_get(bf_port_speed_t speed,
                                    uint32_t n_lanes,
                                    bf_pltfm_encoding_type_t *enc_mode) {
  if (!enc_mode) return BF_INVALID_ARG;

  *enc_mode = BF_PLTFM_ENCODING_NRZ;

  switch (speed) {
    case BF_SPEED_400G:
      *enc_mode = BF_PLTFM_ENCODING_PAM4;
      return BF_SUCCESS;
      break;
    case BF_SPEED_200G:
      if (n_lanes == 8) {
        *enc_mode = BF_PLTFM_ENCODING_NRZ;
        return BF_SUCCESS;
      } else if (n_lanes == 4) {
        *enc_mode = BF_PLTFM_ENCODING_PAM4;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_100G:
      if (n_lanes == 2) {
        *enc_mode = BF_PLTFM_ENCODING_PAM4;
        return BF_SUCCESS;
      } else if (n_lanes == 4) {
        *enc_mode = BF_PLTFM_ENCODING_NRZ;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_50G:
      if (n_lanes == 1) {
        *enc_mode = BF_PLTFM_ENCODING_PAM4;
        return BF_SUCCESS;
      } else if (n_lanes == 2) {
        *enc_mode = BF_PLTFM_ENCODING_NRZ;
        return BF_SUCCESS;
      }
      break;
    case BF_SPEED_40G_R2:
    case BF_SPEED_40G:
    case BF_SPEED_25G:
    case BF_SPEED_10G:
    case BF_SPEED_1G:
      *enc_mode = BF_PLTFM_ENCODING_NRZ;
      return BF_SUCCESS;
    case BF_SPEED_NONE:
      return BF_SUCCESS;
    default:
      // Invalid speed
      break;
  }
  return BF_INVALID_ARG;
}

bf_status_t bf_pm_pre_port_add_cfg_set(bf_pal_front_port_handle_t *port_hdl,
                                       bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;
  bf_dev_id_t dev_id_of_port = 0;
  int ch, i;
  bf_dev_port_t dev_port;
  bf_pltfm_encoding_type_t encoding = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;
  if (!port_cfg) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  // Handle speed mismatch -
  // If there is a mismatch between configured speed and platform supported,
  // then number of channels will vary. For eg, with 100G configuration,
  // num of channels should be four in both board-map and port-configuration.
  // If platform supports only 25G, then board-map will have one channel.
  // So bail-out from port-add.
  if (!bf_pm_intf_is_device_family_tofino3(dev_id)) {
    for (i = 0; i < port_cfg->num_lanes; i++) {
      ch = port_info.chnl_id + i;
      if (!bf_bd_is_this_channel_valid(port_info.conn_id, ch)) {
        LOG_ERROR(
            "%s: Unsupported speed dev : %d : front port : %d/%d "
            "port-cfg-nlanes:%d "
            "invalid channel:%d ",
            __func__,
            dev_id,
            port_info.conn_id,
            port_info.chnl_id,
            port_cfg->num_lanes,
            ch);

        return BF_INVALID_ARG;
      }
    }
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl, &dev_id_of_port, &dev_port);
    dev_id = dev_id_of_port;

    bf_pm_encoding_mode_get(
        port_cfg->speed_cfg, port_cfg->num_lanes, &encoding);
    bf_pm_intf_add(&port_info, dev_id, dev_port, port_cfg, encoding);
    return BF_SUCCESS;
  }

  qsfp_fsm_update_cfg(port_info.conn_id,
                      port_info.chnl_id,
                      port_cfg->speed_cfg,
                      port_cfg->num_lanes,
                      BF_PLTFM_ENCODING_NRZ);
  return BF_SUCCESS;
}

static bf_status_t bf_pm_port_fc_mac_set(bf_pal_front_port_handle_t *port_hdl,
                                         bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_dev_port_t dev_port;
  bf_dev_id_t dev_id = 0;
  bf_dev_id_t dev_id_of_port = 0;
  bf_pltfm_port_info_t port_info;
  uint8_t mac_addr[6] = {0};
  bf_status_t sts;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  if (platform_port_mac_addr_get(&port_info, mac_addr) != BF_PLTFM_SUCCESS) {
    LOG_ERROR("Unable to get the mac address for front port %d/%d",
              port_info.conn_id,
              port_info.chnl_id);
    return BF_INVALID_ARG;
  }

  bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  dev_id = dev_id_of_port;

  sts = bf_port_flow_control_frame_src_mac_address_set(
      dev_id, dev_port, mac_addr);
  if (sts != BF_SUCCESS) {
    LOG_ERROR("Unable to set the flow-control mac address for front port %d/%d",
              port_info.conn_id,
              port_info.chnl_id);
  }

  // Currently there is nothing to be done so just return
  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_post_port_add_cfg_set(bf_pal_front_port_handle_t *port_hdl,
                                        bf_pal_front_port_cb_cfg_t *port_cfg) {
  bool is_present;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;
  bf_dev_id_t dev_id_of_port = 0;
  bf_dev_port_t dev_port;
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  bf_pm_port_fc_mac_set(port_hdl, port_cfg);

  // If the port has a retimer, disable Link-training
  //
  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;
  bf_pm_port_front_panel_port_to_dev_port_get(
      port_hdl, &dev_id_of_port, &dev_port);
  dev_id = dev_id_of_port;

  if (!bf_bd_is_this_port_internal(port_info.conn_id, port_info.chnl_id)) {
    /* LED set to disabled state on port-add */
    sts = bf_port_led_set(dev_id, &port_info, BF_LED_POST_PORT_DIS);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR(
          "Unable to set led on port add for dev : %d : front port : %d/%d : "
          "%s (%d)",
          dev_id,
          port_info.conn_id,
          port_info.chnl_id,
          bf_pltfm_err_str(sts),
          sts);
    }
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    return BF_SUCCESS;
  }

  bf_bd_cfg_port_has_rtmr(&port_info, &is_present);
  if (is_present) {
    bf_port_lt_disable_set(dev_id, dev_port, true);
  }

  if (bf_bd_is_this_port_internal(port_info.conn_id, port_info.chnl_id)) {
    bf_pm_pltfm_front_port_ready_for_bringup(dev_id, port_hdl, true);
  }

  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_pre_port_delete_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;
  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;
  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_intf_del(&port_info, port_cfg);
    return BF_SUCCESS;
  }
  // Currently there is nothing to be done so just return
  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_post_port_delete_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  // Turn off the LED
  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  if (bf_bd_is_this_port_internal(port_info.conn_id, port_info.chnl_id)) {
    // nothing todo
    return BF_SUCCESS;
  }

  sts = bf_port_led_set(dev_id, &port_info, BF_LED_POST_PORT_DEL);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR(
        "Unable to turn off led for dev : %d : front port : %d/%d : %s (%d)",
        dev_id,
        port_info.conn_id,
        port_info.chnl_id,
        bf_pltfm_err_str(sts),
        sts);
  }

  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_pre_port_enable_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;
  bf_dev_id_t dev_id_of_port = 0;
  bool is_present = false;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  // Change the LED to milder blue
  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  if (bf_bd_is_this_port_internal(port_info.conn_id, port_info.chnl_id)) {
    if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
      bf_pm_intf_enable(&port_info, port_cfg);
    }
    bf_pm_pltfm_front_port_eligible_for_autoneg(dev_id, port_hdl, true);
    return BF_SUCCESS;
  }

  sts = bf_port_led_set(dev_id, &port_info, BF_LED_PRE_PORT_EN);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR(
        "Unable to set led on port enable for dev : %d : front port : %d/%d : "
        "%s (%d)",
        dev_id,
        port_info.conn_id,
        port_info.chnl_id,
        bf_pltfm_err_str(sts),
        sts);
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_intf_enable(&port_info, port_cfg);
    return BF_PLTFM_SUCCESS;
  }

  pltfm_pm_port_qsfp_is_present(&port_info, &is_present);
  if (is_present) {
    int ln;
    bool is_optic = false;
    bf_dev_port_t dev_port;

    bf_pm_port_front_panel_port_to_dev_port_get(
        port_hdl, &dev_id_of_port, &dev_port);
    dev_id = dev_id_of_port;

    is_optic = bf_qsfp_is_optical(port_info.conn_id);
    if (is_optic) {
      bf_port_is_optical_xcvr_set(dev_id, dev_port, true);
    } else {
      bf_port_is_optical_xcvr_set(dev_id, dev_port, false);
    }

    if (bf_qsfp_is_luxtera_optic(port_info.conn_id)) {
      // no need for special-case o retimer ports
      bf_bd_cfg_port_has_rtmr(&port_info, &is_present);
      for (ln = 0; ln < port_cfg->num_lanes; ln++) {
        if (is_present) {
          sts = bf_serdes_tx_loop_bandwidth_set(
              dev_id, dev_port, ln, BF_SDS_TOF_TX_LOOP_BANDWIDTH_DEFAULT);
        } else {
          sts = bf_serdes_tx_loop_bandwidth_set(
              dev_id, dev_port, ln, BF_SDS_TOF_TX_LOOP_BANDWIDTH_9MHZ);
        }
      }
    } else {
      for (ln = 0; ln < port_cfg->num_lanes; ln++) {
        sts = bf_serdes_tx_loop_bandwidth_set(
            dev_id, dev_port, ln, BF_SDS_TOF_TX_LOOP_BANDWIDTH_DEFAULT);
      }
    }
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR(
          "Unable to set Tx Loop bandwidth for front port %d/%d : "
          "%s (%d)",
          port_info.conn_id,
          port_info.chnl_id,
          bf_pltfm_err_str(sts),
          sts);
    }
    LOG_DEBUG("QSFP: %2d : ch[%d] : dev_port=%3d : is %s",
              port_info.conn_id,
              port_info.chnl_id,
              dev_port,
              is_optic ? "OPTICAL" : "COPPER");
  }
  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_post_port_enable_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_pltfm_ext_phy_cfg_t rtmr_cfg;
  bool is_present = false;
  int num_lanes, log_lane;
  bool is_optic = false;
  bf_dev_id_t dev_id = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;
  if (!port_cfg) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;
  num_lanes = port_cfg->num_lanes;

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    return BF_PLTFM_SUCCESS;
  }

  // Turn ON the QSFP lasers if present
  pltfm_pm_port_qsfp_is_present(&port_info, &is_present);
  if (is_present) {
    is_optic = bf_qsfp_is_optical(port_info.conn_id);
  }
  // Set Retimer mode
  bf_bd_cfg_port_has_rtmr(&port_info, &is_present);
  if (is_present) {
    if (!bf_pltfm_pm_is_ha_mode()) {
      rtmr_cfg.speed_cfg = port_cfg->speed_cfg;
      rtmr_cfg.is_an_on = port_cfg->is_an_on;
      rtmr_cfg.is_optic = is_optic;

      for (log_lane = 0; log_lane < num_lanes; log_lane++) {
        bf_pltfm_port_info_t temp_port_info = port_info;

        temp_port_info.chnl_id += log_lane;
        sts = bf_pltfm_ext_phy_conn_eq_set(&temp_port_info, &rtmr_cfg);
        if (sts != BF_PLTFM_SUCCESS) {
          LOG_ERROR(
              "Unable to set retimer tx eq values : front port : %d/%d : %s "
              "(%d)",
              temp_port_info.conn_id,
              temp_port_info.chnl_id,
              bf_pltfm_err_str(sts),
              sts);
        }
      }
      if ((sts = bf_pltfm_ext_phy_set_mode(&port_info, &rtmr_cfg))) {
        LOG_ERROR("Error(%d) setting rtmr mode.", sts);
      } else {
        LOG_DEBUG("RTMR: %2d/%d: Spd=%d : Optic=%d : AN=%d",
                  port_info.conn_id,
                  port_info.chnl_id,
                  rtmr_cfg.speed_cfg,
                  rtmr_cfg.is_optic,
                  rtmr_cfg.is_an_on);
      }
    }
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_pre_port_disable_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_port_info_t port_info;
  bool is_present = false, is_optic = false;
  int num_lanes, log_lane;
  bf_dev_id_t dev_id = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  num_lanes = port_cfg->num_lanes;

  // Turn OFF the QSFP lasers if present
  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    return BF_PLTFM_SUCCESS;
  }

  pltfm_pm_port_qsfp_is_present(&port_info, &is_present);
  if (is_present) {
    is_optic = bf_qsfp_is_optical(port_info.conn_id);

    if (is_optic) {
      for (log_lane = 0; log_lane < num_lanes; log_lane++) {
        // test
        if (!bf_pltfm_pm_is_ha_mode()) {
          qsfp_fsm_ch_disable(
              0 /*fixme*/, port_info.conn_id, port_info.chnl_id + log_lane);
        } else {
          qsfp_state_ha_enable_config_delete(port_info.conn_id,
                                             port_info.chnl_id + log_lane);
        }
      }
    }
  }
  return BF_SUCCESS;
}

bf_status_t bf_pm_post_port_disable_cfg_set(
    bf_pal_front_port_handle_t *port_hdl,
    bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;
  bool is_present = false, port_enb = false;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  // Change the LED to Orange
  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  if (bf_bd_is_this_port_internal(port_info.conn_id, port_info.chnl_id)) {
    if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
      bf_pm_intf_disable(&port_info, port_cfg);
    }
    return BF_PLTFM_SUCCESS;
  }

  sts = bf_pm_port_is_enabled(dev_id, port_hdl, &port_enb);
  if (sts != BF_SUCCESS) return sts;

  if (!port_enb) {
    sts = bf_port_led_set(dev_id, &port_info, BF_LED_POST_PORT_DIS);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR(
          "Unable to set led on port disable for dev : %d : front port : %d/%d "
          ": "
          "%s (%d)",
          dev_id,
          port_info.conn_id,
          port_info.chnl_id,
          bf_pltfm_err_str(sts),
          sts);
    }
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_intf_disable(&port_info, port_cfg);
    return BF_PLTFM_SUCCESS;
  }

  // Run Retimer deletion steps
  bf_bd_cfg_port_has_rtmr(&port_info, &is_present);
  if (is_present) {
    if (!bf_pltfm_pm_is_ha_mode()) {
      bf_pltfm_ext_phy_cfg_t rtmr_cfg;

      rtmr_cfg.speed_cfg = port_cfg->speed_cfg;
      rtmr_cfg.is_an_on = port_cfg->is_an_on;
      if ((sts = bf_pltfm_ext_phy_del_mode(&port_info, &rtmr_cfg))) {
        LOG_ERROR("Error(%d) running rtmr post deletion steps.", sts);
      }
    }
  }

  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_link_up_actions(bf_pal_front_port_handle_t *port_hdl,
                                       bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  // Change the LED to Green
  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  if (bf_bd_is_this_port_internal(port_info.conn_id, port_info.chnl_id)) {
    // nothing todo
    return BF_SUCCESS;
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_intf_set_link_state(&port_info, true);
  }

  sts = bf_port_led_set(dev_id, &port_info, BF_LED_PORT_LINK_UP);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_DEBUG(
        "Unable to set led on port up event for dev : %d : front port : %d/%d "
        ": %s (%d)",
        dev_id,
        port_info.conn_id,
        port_info.chnl_id,
        bf_pltfm_err_str(sts),
        sts);
  }

  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_port_link_down_actions(bf_pal_front_port_handle_t *port_hdl,
                                         bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;

  // Change the LED to milder blue
  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  if (bf_bd_is_this_port_internal(port_info.conn_id, port_info.chnl_id)) {
    // nothing todo
    return BF_SUCCESS;
  }

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_intf_set_link_state(&port_info, false);
  }

  sts = bf_port_led_set(dev_id, &port_info, BF_LED_PORT_LINK_DOWN);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR(
        "Unable to set led on port down event for dev : %d : front port : "
        "%d/%d : %s (%d)",
        dev_id,
        port_info.conn_id,
        port_info.chnl_id,
        bf_pltfm_err_str(sts),
        sts);
  }

  (void)port_cfg;
  return BF_SUCCESS;
}

bf_status_t bf_pm_pltfm_port_media_type_get(
    bf_pal_front_port_handle_t *port_hdl, bf_media_type_t *media_type) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;
  if (!media_type) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  sts = bf_pltfm_pm_media_type_get(&port_info, media_type);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR(
        "Unable to get the media type for dev : %d : front port : %d/%d : %s "
        "(%d)",
        dev_id,
        port_info.conn_id,
        port_info.chnl_id,
        bf_pltfm_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_pltfm_port_presence_get(
    bf_pal_front_port_handle_t *port_hdl, bool *presence) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_pltfm_port_info_t port_info;
  bf_dev_id_t dev_id = 0;

  // Safety Checks
  if (!port_hdl) return BF_INVALID_ARG;
  if (!presence) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;

  sts = bf_pltfm_pm_presence_get(&port_info, presence);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR(
        "Unable to get the presence bit for dev : %d : front port : %d/%d : %s "
        "(%d)",
        dev_id,
        port_info.conn_id,
        port_info.chnl_id,
        bf_pltfm_err_str(sts),
        sts);
    return sts;
  }

  return BF_SUCCESS;
}

static bf_status_t bf_pm_qsfp_mgmt_cb(bf_dev_id_t dev_id,
                                      bf_dev_port_t dev_port,
                                      bf_fsm_st *next_state,
                                      uint32_t *next_state_wait_ms) {
  bf_pal_front_port_handle_t port_hdl;
  int num_lanes;
  bf_status_t rc;
  int lane;
  bool is_optical = false;
  bf_pltfm_port_info_t port_info;

  rc = bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (rc) {
    LOG_ERROR("Error (%d) Mapping dev_port (%d) to conn_id", rc, dev_port);
    return BF_INVALID_ARG;
  }
  // Check type, this fn only handles optical QSFPs
  port_info.conn_id = port_hdl.conn_id;
  port_info.chnl_id = port_hdl.chnl_id;

  is_optical = bf_qsfp_is_optical(port_info.conn_id);

  LOG_DEBUG("QSFP: %2d : ch[%d] : dev_port=%3d : is %s",
            port_info.conn_id,
            port_info.chnl_id,
            dev_port,
            is_optical ? "OPTICAL" : "COPPER");

  if (!is_optical) {
    bf_port_is_optical_xcvr_set(dev_id, dev_port, false);
    return BF_SUCCESS;
  }

  bf_port_is_optical_xcvr_set(dev_id, dev_port, true);
  bf_port_optical_xcvr_ready_set(dev_id, dev_port, false);

  rc = bf_pm_pltfm_front_port_num_lanes_get(dev_id, &port_hdl, &num_lanes);
  if (rc) {
    LOG_ERROR("Error (%d) Getting num_lanes for dev_port (%d)", rc, dev_port);
    return BF_INVALID_ARG;
  }
  for (lane = 0; lane < num_lanes; lane++) {
    LOG_DEBUG("QSFP: %2d : ch[%d] Enable (dev_port=%d, ln=%d)",
              port_hdl.conn_id,
              port_hdl.chnl_id + lane,
              dev_port,
              lane);
    if (!bf_pltfm_pm_is_ha_mode()) {
      qsfp_fsm_ch_enable(dev_id, port_hdl.conn_id, port_hdl.chnl_id + lane);
    } else {
      qsfp_state_ha_enable_config_set(port_hdl.conn_id,
                                      port_hdl.chnl_id + lane);
    }
  }
  return BF_SUCCESS;
}

bf_status_t bf_pltfm_pm_rtmr_lane_reset_cb(bf_dev_id_t dev_id,
                                           bf_dev_port_t dev_port,
                                           bf_fsm_st *next_state,
                                           uint32_t *next_state_wait_ms) {
  bf_pal_front_port_handle_t port_hdl;
  int num_lanes;
  bf_status_t rc;
  bool is_optical = false, is_rtmr = false;
  bf_pltfm_port_info_t port_info;
  rc = bf_pm_port_dev_port_to_front_panel_port_get(dev_id, dev_port, &port_hdl);
  if (rc) {
    LOG_ERROR("Error (%d) Mapping dev_port (%d) to conn_id", rc, dev_port);
    return BF_INVALID_ARG;
  }
  // Check type, this fn only deals with optical QSFPs on retimer ports
  port_info.conn_id = port_hdl.conn_id;
  port_info.chnl_id = port_hdl.chnl_id;
  bf_bd_cfg_port_has_rtmr(&port_info, &is_rtmr);
  // Nothing to do here if not a retimer port
  if (!is_rtmr) {
    return BF_SUCCESS;
  }
  is_optical = bf_qsfp_is_optical(port_info.conn_id);
  // nothing to do here (either) if not optical
  if (!is_optical) {
    return BF_SUCCESS;
  }
  LOG_DEBUG(
      "RTMR: %2d : ch[%d] : dev_port=%3d : is OPTICAL. Reset rtmr lane(s)",
      port_info.conn_id,
      port_info.chnl_id,
      dev_port);
  rc = bf_pm_pltfm_front_port_num_lanes_get(dev_id, &port_hdl, &num_lanes);
  if (rc) {
    LOG_ERROR("Error (%d) Getting num_lanes for dev_port (%d)", rc, dev_port);
    return BF_INVALID_ARG;
  }
  rc = bf_pltfm_ext_phy_lane_reset(&port_info, num_lanes);
  return rc;
}

int in_rtmr_init = 0;

bf_status_t bf_pm_cold_init(bf_dev_init_mode_t warm_init_mode) {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;
  bf_dev_id_t dev_id = 0;
  bf_fsm_t fsm;
  bool warm_init_mode_cfg;

  if (warm_init_mode != BF_DEV_INIT_COLD) {
    bf_pm_intf_obj_reset();
    warm_init_mode_cfg = true;
  } else
    warm_init_mode_cfg = false;

  // Inits LEDs
  bf_pltfm_platform_port_init(dev_id, warm_init_mode_cfg);

  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_intf_init();

    sts = qsfp_scan(dev_id);
    if (sts != BF_PLTFM_SUCCESS) {
      LOG_ERROR("Unable to do warm init for dev : %d : %s (%d)",
                dev_id,
                bf_pltfm_err_str(sts),
                sts);
      return sts;
    }

    return BF_PLTFM_SUCCESS;
  }

  sts = qsfp_scan(dev_id);
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR("Unable to do warm init for dev : %d : %s (%d)",
              dev_id,
              bf_pltfm_err_str(sts),
              sts);
    return sts;
  }

  // Initialize retimers
  in_rtmr_init = 1;

  if (!bf_pltfm_pm_is_ha_mode()) {
    sts = bf_pltfm_ext_phy_init();
    if (sts) {
      LOG_ERROR("Error (%d) while initializing retimers.", sts);
      in_rtmr_init = 0;
      return sts;
    }
  }
  in_rtmr_init = 0;

  bf_status_t rc;

  LOG_DEBUG("%s:%d Bind QSFP Mgmt cb to BF_FSM_ST_CFG_SERDES in default FSM",
            __func__,
            __LINE__);

  fsm = bf_get_default_fsm(dev_id, false /*only non-AN s/m*/);
  rc = bf_fsm_bind_cb_to_state(
      (bf_fsm_state_desc_t *)fsm, BF_FSM_ST_CFG_SERDES, bf_pm_qsfp_mgmt_cb);
  if (rc) {
    LOG_ERROR(
        "Error (%d) Binding QSFP Mgmt callback to BF_FSM_ST_CFG_SERDES failed",
        rc);
  }

  // bind callback for retimer lane-reset
  rc = bf_fsm_bind_cb_to_state((bf_fsm_state_desc_t *)fsm,
                               BF_FSM_ST_WAIT_SIGNAL_OK,
                               bf_pltfm_pm_rtmr_lane_reset_cb);
  if (rc) {
    LOG_ERROR("Error (%d) Binding RTMR callback.", rc);
  }

  // bind to tx-only FSM as well
  LOG_DEBUG("%s:%d Bind QSFP Mgmt cb to BF_FSM_ST_CFG_SERDES in tx-mode FSM",
            __func__,
            __LINE__);
  fsm = bf_get_fsm_for_tx_mode();
  rc = bf_fsm_bind_cb_to_state(
      (bf_fsm_state_desc_t *)fsm, BF_FSM_ST_CFG_SERDES, bf_pm_qsfp_mgmt_cb);
  if (rc) {
    LOG_ERROR(
        "Error (%d) Binding QSFP Mgmt callback to BF_FSM_ST_CFG_SERDES failed",
        rc);
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_safe_to_call_in_notify() {
  bf_pltfm_status_t sts = BF_PLTFM_SUCCESS;

  // Safety Checks

  /* Since QSFP scan polling will scan QSFPs and make calls into the SDE,
     start doing it only after being notified by the SDE that it is safe
     to do so */

  sts = bf_pltfm_pm_qsfp_scan_poll_start();
  if (sts != BF_PLTFM_SUCCESS) {
    LOG_ERROR("Unable to start qsfp scan polling");
    return sts;
  }

  return BF_SUCCESS;
}

bf_status_t bf_pm_prbs_cfg_set(bf_pal_front_port_handle_t *port_hdl,
                               bf_pal_front_port_cb_cfg_t *port_cfg) {
  bf_port_speed_t speed;

  // Set up retimer if needed
  if ((port_cfg->speed_cfg == BF_SPEED_10G) ||
      (port_cfg->speed_cfg == BF_SPEED_40G)) {
    speed = BF_SPEED_10G;
  } else if ((port_cfg->speed_cfg == BF_SPEED_25G) ||
             (port_cfg->speed_cfg == BF_SPEED_50G) ||
             (port_cfg->speed_cfg == BF_SPEED_100G) ||
             (port_cfg->speed_cfg == BF_SPEED_40G_R2)) {
    speed = BF_SPEED_25G;
  } else {
    LOG_ERROR("Failed to configure retimer for prbs. Unsupported speed: %d.",
              port_cfg->speed_cfg);
    return BF_INVALID_ARG;
  }
  bf_pltfm_ext_phy_init_speed(port_hdl->conn_id, speed);

  return BF_SUCCESS;
}

bf_status_t bf_pm_ha_mode_enable() {
  bf_pltfm_pm_ha_mode_set();
  return BF_SUCCESS;
}

bf_status_t bf_pm_ha_mode_disable() {
  bf_pltfm_pm_ha_mode_clear();
  return BF_SUCCESS;
}

// At the end of warm-init/fast-reconfig, this will be called to move
// the fsm state.
bf_status_t bf_pm_ha_link_state_notify(bf_dev_id_t dev_id,
                                       bf_pal_front_port_handle_t *port_hdl,
                                       bool link_up) {
  bf_pltfm_port_info_t port_info;

  if (!port_hdl) return BF_INVALID_ARG;

  port_info.conn_id = port_hdl->conn_id;
  port_info.chnl_id = port_hdl->chnl_id;
  if (!bf_pm_intf_is_device_family_tofino(dev_id)) {
    bf_pm_intf_ha_link_state_notiy(&port_info, link_up);
    return BF_SUCCESS;
  }
  return BF_SUCCESS;
}

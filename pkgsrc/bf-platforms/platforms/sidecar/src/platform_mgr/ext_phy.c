#include <stdint.h>
#include <stdio.h>
#include <bf_types/bf_types.h>
#include <bf_pltfm_types/bf_pltfm_types.h>
#include <bf_pltfm_ext_phy.h>

bf_pltfm_status_t
bf_pltfm_ext_phy_init(void)
{
	return (BF_PLTFM_SUCCESS);
}

/* set the retimer/external phy into a specific mode
 *
 * @return
 *  BF_PLTFM_SUCCESS onsuccess and other values on failure
 */
bf_pltfm_status_t
bf_pltfm_ext_phy_set_mode(bf_pltfm_port_info_t *port_info,
    bf_pltfm_ext_phy_cfg_t *port_cfg)
{
	return (BF_PLTFM_SUCCESS);
}

/* reset the mode of a retimer/external phy
 *
 * @return
 *  BF_PLTFM_SUCCESS onsuccess and other values on failure
 */
bf_pltfm_status_t
bf_pltfm_ext_phy_del_mode(bf_pltfm_port_info_t *port_info,
    bf_pltfm_ext_phy_cfg_t *port_cfg)
{
	return (BF_PLTFM_SUCCESS);
}

/* reset a lane of a retimer/external phy
 *
 * @return
 *  BF_PLTFM_SUCCESS onsuccess and other values on failure
 */
bf_pltfm_status_t
bf_pltfm_ext_phy_lane_reset(bf_pltfm_port_info_t *port_info,
    uint32_t num_lanes)
{
	return (BF_PLTFM_SUCCESS);
}

/* configure the retimer/external phy for a specific initial speed
 *
 * @return
 *  BF_PLTFM_SUCCESS onsuccess and other values on failure
 */
bf_pltfm_status_t
bf_pltfm_ext_phy_init_speed(uint8_t port_num, bf_port_speed_t port_speed)
{
	return (BF_PLTFM_SUCCESS);
}

/* apply specific equalization settings to a retimer/external phy
 *
 * @return
 *  BF_PLTFM_SUCCESS onsuccess and other values on failure
 */
bf_pltfm_status_t
bf_pltfm_ext_phy_conn_eq_set( bf_pltfm_port_info_t *port_info,
    bf_pltfm_ext_phy_cfg_t *port_cfg)
{
	return (BF_PLTFM_SUCCESS);
}

/* apply a pre-set configuration to the retime/external phyr based on the type i
 * of qsfp module
 *
 * @return
 *  BF_PLTFM_SUCCESS onsuccess and other values on failure
 */
bf_pltfm_status_t
bf_pltfm_platform_ext_phy_config_set( bf_dev_id_t dev_id, uint32_t conn,
    bf_pltfm_qsfp_type_t qsfp_type)
{
	return (BF_PLTFM_SUCCESS);
}

/* configure the prbs pattern in external phy
 *
 * paramxi
 *   port_info
 *   direction:  0:ingress, 1:egress
 *   prbs_mode:  0:PRBS9, 1: PRBS15, 2:PRBS23, 3:PRBS31
 * @return
 *  BF_PLTFM_SUCCESS onsuccess and other values on failure
 */
bf_pltfm_status_t bf_pltfm_platform_ext_phy_prbs_set(
    bf_pltfm_port_info_t *port_info, uint8_t direction, uint16_t prbs_mode)
{
	return (BF_PLTFM_SUCCESS);
}

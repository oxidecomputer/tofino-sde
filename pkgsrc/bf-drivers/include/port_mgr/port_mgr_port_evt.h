/*******************************************************************************
 *  Copyright (C) 2024 Intel Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing,
 *  software distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions
 *  and limitations under the License.
 *
 *
 *  SPDX-License-Identifier: Apache-2.0
 ******************************************************************************/


#ifndef PORT_MGR_PORT_EVT_H_INCLUDED
#define PORT_MGR_PORT_EVT_H_INCLUDED

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  PORT_MGR_PORT_EVT_NONE = 0,
  PORT_MGR_PORT_EVT_UP,
  PORT_MGR_PORT_EVT_DOWN,
  PORT_MGR_PORT_EVT_SPEED_SET,
  PORT_MGR_PORT_FSM_TRANSITION,
  // TODO: tofino-sde#2 PORT_MGR_PORT_PRESENCE_UPDATE,
} port_mgr_port_event_t;

typedef void (*port_mgr_port_callback_t)(bf_dev_id_t chip,
                                         bf_dev_port_t port,
                                         port_mgr_port_event_t reason,
					 uint32_t event_data,
                                         void *userdata);

bf_status_t port_mgr_register_port_cb(bf_dev_id_t dev_id,
                                      port_mgr_port_callback_t fn,
                                      void *userdata);
#ifdef __cplusplus
}
#endif /* C++ */

#endif  // PORT_MGR_PORT_EVT_H_INCLUDED

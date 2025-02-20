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


#include "bf_rt_dev_table_impl.hpp"
#include <bf_rt_common/bf_rt_init_impl.hpp>
#include <bf_pm/bf_pm_fsm_common.h>
#include <bf_pm/bf_pm_intf.h>
#include "../bf_rt_port/bf_rt_port_mgr_intf.hpp"
#include "bf_rt_dev_table_data_impl.hpp"

namespace bfrt {

bf_status_t BfRtDevTable::tableDefaultEntryReset(
    const BfRtSession &session,
    const bf_rt_target_t &dev_tgt,
    const uint64_t & /*flags*/) const {
  bf_status_t status = BF_SUCCESS;
  auto *pipeMgr = PipeMgrIntf::getInstance(session);

  status = pipeMgr->pipeMgrFlowLrnTimeoutSet(
      session.sessHandleGet(), dev_tgt.dev_id, 500);
  if (status != BF_SUCCESS) {
    LOG_TRACE("%s:%d %s: Error in setting flow learning timeout",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return status;
  }

  status = pipeMgr->pipeMgrFlowLrnIntrModeSet(
      session.sessHandleGet(), dev_tgt.dev_id, false);
  if (status != BF_SUCCESS) {
    LOG_TRACE("%s:%d %s: Error in setting flow learning interrupt processing",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return status;
  }

  status = pipeMgr->pipeMgrInactiveNodeDeleteSet(
      session.sessHandleGet(), dev_tgt.dev_id, false);
  if (status != BF_SUCCESS) {
    LOG_TRACE("%s:%d %s: Error in setting inactive node delete functionality",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return status;
  }

  status = pipeMgr->pipeMgrSelectorMbrOrderSet(
      session.sessHandleGet(), dev_tgt.dev_id, false);
  if (status != BF_SUCCESS) {
    LOG_TRACE(
        "%s:%d %s: Error in setting Selector table member order functionality",
        __func__,
        __LINE__,
        table_name_get().c_str());
    return status;
  }
  // No default value for LRT_DR_TIMEOUT_MSEC

  return status;
}

bf_status_t BfRtDevTable::tableClear(const BfRtSession &session,
                                     const bf_rt_target_t &dev_tgt,
                                     const uint64_t &flags) const {
  return this->tableDefaultEntryReset(session, dev_tgt, flags);
}

bf_status_t BfRtDevTable::tableDefaultEntrySet(
    const BfRtSession &session,
    const bf_rt_target_t &dev_tgt,
    const uint64_t & /*flags*/,
    const BfRtTableData &data) const {
  const BfRtDevTableData *dev_data =
      static_cast<const BfRtDevTableData *>(&data);
  auto *pipeMgr = PipeMgrIntf::getInstance(session);
  bf_status_t status = BF_SUCCESS;

  const std::unordered_map<bf_rt_id_t, bool> &boolData =
      dev_data->getBoolFieldDataMap();
  const std::unordered_map<bf_rt_id_t, uint32_t> &u32Data =
      dev_data->getU32FieldDataMap();

  if (boolData.find(FLOW_LEARN_INTR_MODE) != boolData.end()) {
    status =
        pipeMgr->pipeMgrFlowLrnIntrModeSet(session.sessHandleGet(),
                                           dev_tgt.dev_id,
                                           boolData.at(FLOW_LEARN_INTR_MODE));
    if (status != BF_SUCCESS) {
      LOG_TRACE("%s:%d %s: Error in setting flow learning interrupt processing",
                __func__,
                __LINE__,
                table_name_get().c_str());
      return status;
    }
  }

  if (boolData.find(INACTIVE_NODE_DELETE) != boolData.end()) {
    status = pipeMgr->pipeMgrInactiveNodeDeleteSet(
        session.sessHandleGet(),
        dev_tgt.dev_id,
        boolData.at(INACTIVE_NODE_DELETE));
    if (status != BF_SUCCESS) {
      LOG_TRACE("%s:%d %s: Error in setting inactive node delete functionality",
                __func__,
                __LINE__,
                table_name_get().c_str());
      return status;
    }
  }

  if (boolData.find(SELECTOR_MEMBER_ORDER) != boolData.end()) {
    status =
        pipeMgr->pipeMgrSelectorMbrOrderSet(session.sessHandleGet(),
                                            dev_tgt.dev_id,
                                            boolData.at(SELECTOR_MEMBER_ORDER));
    if (status != BF_SUCCESS) {
      LOG_TRACE(
          "%s:%d %s: Error in setting Selector table member order "
          "functionality",
          __func__,
          __LINE__,
          table_name_get().c_str());
      return status;
    }
  }

  if (!u32Data.empty()) {
    if (u32Data.find(FLOW_LEARN_TIMEOUT_USEC) != u32Data.end()) {
      status = pipeMgr->pipeMgrFlowLrnTimeoutSet(
          session.sessHandleGet(),
          dev_tgt.dev_id,
          u32Data.at(FLOW_LEARN_TIMEOUT_USEC));
      if (status != BF_SUCCESS) {
        LOG_TRACE("%s:%d %s: Error in setting flow learning timeout",
                  __func__,
                  __LINE__,
                  table_name_get().c_str());
        return status;
      }
    }
    if (u32Data.find(LRT_DR_TIMEOUT_USEC) != u32Data.end()) {
      status = pipeMgr->pipeMgrLrtDrTimeoutSet(dev_tgt.dev_id,
                                               u32Data.at(LRT_DR_TIMEOUT_USEC));
      if (status != BF_SUCCESS) {
        LOG_TRACE(
            "%s:%d %s: Error in setting LRT descriptor ring timer timeout",
            __func__,
            __LINE__,
            table_name_get().c_str());
        return status;
      }
    }
  }

  return status;
}

bf_status_t BfRtDevTable::tableDefaultEntryGet(const BfRtSession &session,
                                               const bf_rt_target_t &dev_tgt,
                                               const uint64_t & /*flags*/,
                                               BfRtTableData *data) const {
  BfRtDevTableData *dev_data = static_cast<BfRtDevTableData *>(data);
  const std::set<bf_rt_id_t> &activeDataFields = dev_data->getActiveFields();
  auto *portMgr = PortMgrIntf::getInstance();
  auto *pipeMgr = PipeMgrIntf::getInstance(session);
  bf_status_t status = BF_SUCCESS;
  if (!activeDataFields.empty()) {
    for (const auto &id : activeDataFields) {
      uint32_t uint32_arg = 0;
      uint8_t uint8_arg = 0;
      bool bool_arg = false;
      switch (id) {
        case (NUM_PIPES):
          status = portMgr->portMgrNumPipesGet(dev_tgt.dev_id, &uint32_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE("%s:%d %s: Error in getting num_pipes",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return status;
          }
          status =
              dev_data->setValue(NUM_PIPES, static_cast<uint64_t>(uint32_arg));
          break;
        case (NUM_STAGES):
          status =
              pipeMgr->pipeMgrGetNumActiveStages(dev_tgt.dev_id, &uint8_arg);
          if (status != BF_SUCCESS) {
            LOG_ERROR("%s:%d %s: Error in getting device active stages count",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return status;
          }
          status =
              dev_data->setValue(NUM_STAGES, static_cast<uint64_t>(uint8_arg));
          break;
        case (NUM_MAX_PORTS):
          status = portMgr->portMgrMaxPortsGet(dev_tgt.dev_id, &uint32_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE("%s:%d %s: Error in getting maximum ports number",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(NUM_MAX_PORTS,
                                      static_cast<uint64_t>(uint32_arg));
          break;
        case (NUM_FRONT_PORTS):
          status =
              portMgr->portMgrNumFrontPortsGet(dev_tgt.dev_id, &uint32_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE("%s:%d %s: Error in getting maximum front ports number",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(NUM_FRONT_PORTS,
                                      static_cast<uint64_t>(uint32_arg));
          break;
        case (PCIE_CPU_PORT): {
          int pcie_cpu_port = bf_pcie_cpu_port_get(dev_tgt.dev_id);
          if (pcie_cpu_port == -1) {
            LOG_TRACE("%s:%d %s: Error in getting pcie cpu port",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return BF_UNEXPECTED;
          }
          status = dev_data->setValue(PCIE_CPU_PORT,
                                      static_cast<uint64_t>(pcie_cpu_port));
        } break;
        case (ETH_CPU_PORT_LIST): {
          int cpu_port = bf_eth_cpu_port_get(dev_tgt.dev_id);
          if (cpu_port == -1) {
            LOG_TRACE("%s:%d %s: Error in getting cpu port",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return BF_UNEXPECTED;
          }
          std::vector<uint32_t> vect;
          /* Add all channels of cpu port */
          const auto max_cpu = bf_eth_max_cpu_port_get(dev_tgt.dev_id);
          for (int port = cpu_port; port <= max_cpu && status == BF_SUCCESS;) {
            vect.push_back(static_cast<uint32_t>(port));
            status = bf_eth_get_next_cpu_port(dev_tgt.dev_id, &port);
          };

          status = dev_data->setValue(ETH_CPU_PORT_LIST, vect);
        } break;
        case (INTERNAL_PORT_LIST):
        /* Fallthrough */
        case (EXTERNAL_PORT_LIST): {
          std::vector<uint32_t> vect;
          status = this->getPortList(dev_tgt.dev_id, id, vect);
          if (status != BF_SUCCESS) {
            LOG_TRACE("%s:%d %s: Error in getting port list",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(id, vect);
        } break;
        case (RECIRC_PORT_LIST): {
          const int MAX_RECIRC_PORTS = 30;
          uint32_t recirc_ports[MAX_RECIRC_PORTS];
          uint32_t max_ports =
              portMgr->portMgrRecircDevPortsGet(dev_tgt.dev_id, recirc_ports);
          if (max_ports == 0) {
            LOG_TRACE("%s:%d %s: Error in getting recirc port range",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return status;
          }
          std::vector<uint32_t> vect;
          for (uint32_t i = 0; i < max_ports && i < MAX_RECIRC_PORTS; i++)
            vect.push_back(recirc_ports[i]);

          status = dev_data->setValue(RECIRC_PORT_LIST, vect);
        } break;

        case (FLOW_LEARN_TIMEOUT_USEC):
          status =
              pipeMgr->pipeMgrFlowLrnTimeoutGet(dev_tgt.dev_id, &uint32_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE("%s:%d %s: Error in getting flow learning timeout",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(FLOW_LEARN_TIMEOUT_USEC,
                                      static_cast<uint64_t>(uint32_arg));
          break;
        case (FLOW_LEARN_INTR_MODE):
          status = pipeMgr->pipeMgrFlowLrnIntrModeGet(
              session.sessHandleGet(), dev_tgt.dev_id, &bool_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE(
                "%s:%d %s: Error in getting flow learning interrupt processing",
                __func__,
                __LINE__,
                table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(FLOW_LEARN_INTR_MODE, bool_arg);
          break;
        case (INACTIVE_NODE_DELETE):
          status = pipeMgr->pipeMgrInactiveNodeDeleteGet(
              session.sessHandleGet(), dev_tgt.dev_id, &bool_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE(
                "%s:%d %s: Error in getting flow learning interrupt processing",
                __func__,
                __LINE__,
                table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(INACTIVE_NODE_DELETE, bool_arg);
          break;
        case (SELECTOR_MEMBER_ORDER):
          status = pipeMgr->pipeMgrSelectorMbrOrderGet(
              session.sessHandleGet(), dev_tgt.dev_id, &bool_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE(
                "%s:%d %s: Error in getting Selector table member order value",
                __func__,
                __LINE__,
                table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(SELECTOR_MEMBER_ORDER, bool_arg);
          break;
        case (LRT_DR_TIMEOUT_USEC):
          status = pipeMgr->pipeMgrLrtDrTimeoutGet(dev_tgt.dev_id, &uint32_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE(
                "%s:%d %s: Error in getting LRT descriptor ring timer timeout",
                __func__,
                __LINE__,
                table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(LRT_DR_TIMEOUT_USEC,
                                      static_cast<uint64_t>(uint32_arg));
          break;
        case (INTR_BASED_LINK_MONITORING):
          status =
              portMgr->portMgrIntrLinkMonitoringGet(dev_tgt.dev_id, &bool_arg);
          if (status != BF_SUCCESS) {
            LOG_TRACE(
                "%s:%d %s: Error in getting interrupt based link monitoring",
                __func__,
                __LINE__,
                table_name_get().c_str());
            return status;
          }
          status = dev_data->setValue(INTR_BASED_LINK_MONITORING, bool_arg);
          break;
        case (SKU): {
          bf_dev_type_t sku = bf_drv_get_dev_type(dev_tgt.dev_id);
          if (sku == BF_DEV_UNKNOWN) {
            LOG_ERROR("%s:%d %s: Error in getting device sku type",
                      __func__,
                      __LINE__,
                      table_name_get().c_str());
            return BF_INVALID_ARG;
          }
          std::string s = pipe_mgr_dev_type2str(sku);
          status = dev_data->setValue(SKU, s);
        } break;
      }
      if (status != BF_SUCCESS) {
        LOG_TRACE("%s:%d %s: Error in setting data object",
                  __func__,
                  __LINE__,
                  table_name_get().c_str());
        return status;
      }
    }
  }

  return status;
}

bf_status_t BfRtDevTable::getPortList(bf_dev_id_t dev_id,
                                      bf_rt_id_t data_field,
                                      std::vector<uint32_t> &vect) const {
  auto *portMgr = PortMgrIntf::getInstance();
  bf_dev_port_t first_port, next_port;
  bf_status_t status = portMgr->portMgrPortGetFirst(dev_id, &first_port);

  if (BF_SUCCESS != status) {
    LOG_TRACE("%s:%d %s: Error in getting first dev_port",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return status;
  }

  /* Find all channels of the cpu port so they can be excluded from the list. */
  std::vector<bf_dev_port_t> cpu_ports;
  const bf_dev_port_t max_cpu = bf_eth_max_cpu_port_get(dev_id);
  const bf_dev_port_t first_cpu_port = bf_eth_cpu_port_get(dev_id);
  if (first_cpu_port == -1) {
    LOG_TRACE("%s:%d %s: Error in getting cpu port",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return BF_UNEXPECTED;
  }
  for (bf_dev_port_t port = first_cpu_port; port <= max_cpu;) {
    cpu_ports.push_back(port);
    status = bf_eth_get_next_cpu_port(dev_id, &port);
    if (status != BF_SUCCESS) break;
  };

  status = BF_SUCCESS;
  while (status == BF_SUCCESS) {
    // Do not add cpu ports to the list
    bool is_cpu_port = false;
    for (const auto &cpu_port : cpu_ports) {
      if (first_port == cpu_port) {
        is_cpu_port = true;
        break;
      }
    }
    if (!is_cpu_port) {
      bool is_internal;
      status =
          portMgr->portMgrPortIsInternalGet(dev_id, first_port, &is_internal);
      if (status != BF_SUCCESS) {
        LOG_TRACE("%s:%d %s: Error in getting is port internal info",
                  __func__,
                  __LINE__,
                  table_name_get().c_str());
        return status;
      }
      if (is_internal && data_field == INTERNAL_PORT_LIST)
        vect.push_back(first_port);
      else if (!is_internal && data_field == EXTERNAL_PORT_LIST)
        vect.push_back(first_port);
    }

    status = portMgr->portMgrPortGetNext(dev_id, first_port, &next_port);
    if (BF_OBJECT_NOT_FOUND == status) {
      // This means that we have successfully read all the ports in the system
      return BF_SUCCESS;
    } else if (BF_SUCCESS != status) {
      LOG_TRACE("%s:%d %s: Error in getting next dev_port %d",
                __func__,
                __LINE__,
                table_name_get().c_str(),
                first_port);
      break;
    }

    first_port = next_port;
  }

  return status;
}

bf_status_t BfRtDevTable::dataReset(const std::vector<bf_rt_id_t> &fields,
                                    BfRtTableData *data) const {
  if (data == nullptr) {
    LOG_ERROR("%s:%d %s Error : Failed to reset data",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return BF_OBJECT_NOT_FOUND;
  }
  return (static_cast<BfRtDevTableData *>(data))->reset(fields);
}

bf_status_t BfRtDevTable::dataReset(BfRtTableData *data) const {
  std::vector<bf_rt_id_t> emptyFields;
  return this->dataReset(emptyFields, data);
}

bf_status_t BfRtDevTable::dataAllocate(
    std::unique_ptr<BfRtTableData> *data_ret) const {
  std::vector<bf_rt_id_t> fields;

  *data_ret =
      std::unique_ptr<BfRtTableData>(new BfRtDevTableData(this, fields));
  if (*data_ret == nullptr) {
    LOG_TRACE("%s:%d %s Error : Failed to allocate data",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return BF_NO_SYS_RESOURCES;
  }

  return BF_SUCCESS;
}

bf_status_t BfRtDevTable::dataAllocate(
    const std::vector<bf_rt_id_t> &fields,
    std::unique_ptr<BfRtTableData> *data_ret) const {
  *data_ret =
      std::unique_ptr<BfRtTableData>(new BfRtDevTableData(this, fields));

  if (*data_ret == nullptr) {
    LOG_TRACE("%s:%d %s Error : Failed to allocate data",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return BF_NO_SYS_RESOURCES;
  }

  return BF_SUCCESS;
}

const std::map<const std::string, bf_dev_init_mode_t>
    BfRtDevWarmInitTable::str_to_init_mode_map{
        {"INIT_COLD", BF_DEV_INIT_COLD},
        {"WARM_INIT_FAST_RECFG", BF_DEV_WARM_INIT_FAST_RECFG},
        {"WARM_INIT_HITLESS", BF_DEV_WARM_INIT_HITLESS}};

const std::map<bf_dev_init_mode_t, std::string>
    BfRtDevWarmInitTable::init_mode_to_str_map{
        {BF_DEV_INIT_COLD, "INIT_COLD"},
        {BF_DEV_WARM_INIT_FAST_RECFG, "WARM_INIT_FAST_RECFG"},
        {BF_DEV_WARM_INIT_HITLESS, "WARM_INIT_HITLESS"}};

bf_status_t BfRtDevWarmInitTable::tableDefaultEntrySet(
    const BfRtSession & /*session*/,
    const bf_rt_target_t &dev_tgt,
    const uint64_t & /*flags*/,
    const BfRtTableData &data) const {
  const BfRtDevWarmInitTableData *dev_data =
      static_cast<const BfRtDevWarmInitTableData *>(&data);
  bf_status_t status = BF_SUCCESS;
  bf_rt_id_t act_id;

  dev_data->actionIdGet(&act_id);

  switch (act_id) {
    case (WARM_INIT_BEGIN): {
      bf_device_profile_t device_profile = {0};

      status = dev_data->containerToProfile(dev_tgt.dev_id, &device_profile);
      if (status != BF_SUCCESS) {
        LOG_TRACE(
            "%s:%d %s : Error : Failed to create device profile from passed "
            "data",
            __func__,
            __LINE__,
            table_name_get().c_str());
        return status;
      }

      bf_dev_init_mode_t init_mode =
          str_to_init_mode_map.at(dev_data->getInitMode());
      // TODO check if it's a valid data?

      status = bf_pal_device_warm_init_begin(dev_tgt.dev_id,
                                             init_mode,
                                             BF_DEV_SERDES_UPD_NONE,
                                             dev_data->getUpgradeAgents());
      if (status != BF_SUCCESS) {
        bf_pal_warm_init_error_set(dev_tgt.dev_id, true);
        LOG_TRACE("%s:%d %s : Error : bf_pal_device_warm_init_begin() failed",
                  __func__,
                  __LINE__,
                  table_name_get().c_str());
        return status;
      }

      status = bf_pal_device_add(dev_tgt.dev_id, &device_profile);
      if (status != BF_SUCCESS) {
        bf_pal_warm_init_error_set(dev_tgt.dev_id, true);
        LOG_TRACE("%s:%d %s : Error : failed to add a device",
                  __func__,
                  __LINE__,
                  table_name_get().c_str());
        return status;
      }
      bf_pal_warm_init_error_set(dev_tgt.dev_id, false);
    } break;
    case (WARM_INIT_COMPLETED):
      status = bf_pal_device_warm_init_end(dev_tgt.dev_id);
      if (status != BF_SUCCESS) {
        bf_pal_warm_init_error_set(dev_tgt.dev_id, true);
        LOG_TRACE("%s:%d %s : Error : failed to bf_pal_device_warm_init_end()",
                  __func__,
                  __LINE__,
                  table_name_get().c_str());
        return status;
      }
      bf_pal_warm_init_error_set(dev_tgt.dev_id, false);
      break;
    default:
      break;
  }

  return status;
}

bf_status_t BfRtDevWarmInitTable::profileToContainer(
    bf_dev_id_t dev_id, BfRtTableData *data) const {
  BfRtDevWarmInitTableData *dev_data =
      static_cast<BfRtDevWarmInitTableData *>(data);

  // Get the device from the device mgr
  std::vector<std::reference_wrapper<const std::string>> p4_names;
  auto bf_status =
      BfRtDevMgr::getInstance().bfRtInfoP4NamesGet(dev_id, p4_names);

  std::vector<std::unique_ptr<BfRtTableData>> inner_data_vec;
  // For each program
  for (const auto &p4_name : p4_names) {
    const BfRtInfo *bfrt_info;
    bf_status =
        BfRtDevMgr::getInstance().bfRtInfoGet(dev_id, p4_name, &bfrt_info);
    if (bf_status != BF_SUCCESS) {
      return bf_status;
    }
    // get context
    std::vector<std::reference_wrapper<const std::string>> context_file_name_v;
    bf_status = bfrt_info->contextFilePathGet(&context_file_name_v);
    // and binary names
    std::vector<std::reference_wrapper<const std::string>> binary_file_name_v;
    bf_status = bfrt_info->binaryFilePathGet(&binary_file_name_v);
    // and the bf-rt json path
    std::vector<std::reference_wrapper<const std::string>> bfrt_info_path_vec;
    bf_status = bfrt_info->bfRtInfoFilePathGet(&bfrt_info_path_vec);

    ///
    std::unique_ptr<BfRtTableData> inner_data;
    bf_status = this->dataAllocateContainer(
        P4_PROGRAMS, WARM_INIT_COMPLETED, &inner_data);
    if (bf_status != BF_SUCCESS) return bf_status;

    BfRtDevP4ProgramsTableData *inner_data_p =
        dynamic_cast<BfRtDevP4ProgramsTableData *>(inner_data.get());

    if (inner_data_p == nullptr) return BF_UNEXPECTED;

    bf_status = inner_data_p->setValue(P4_NAME, p4_name);
    if (bf_status != BF_SUCCESS) return bf_status;

    ///
    bf_status =
        inner_data->setValue(BFRT_JSON_FILE, bfrt_info_path_vec.back().get());
    if (bf_status != BF_SUCCESS) return bf_status;

    PipelineProfInfoVec pipe_info;
    bf_status = bfrt_info->bfRtInfoPipelineInfoGet(&pipe_info);
    if (bf_status != BF_SUCCESS) {
      LOG_ERROR(
          "ERROR: %s:%d bfRtInfoPipelineInfoGet() failed", __func__, __LINE__);
      return bf_status;
    }

    std::vector<std::unique_ptr<BfRtTableData>> inner_pipe_data_vec;
    uint32_t num_pipelines = pipe_info.size();
    for (uint32_t i = 0; i < num_pipelines; i++) {
      std::unique_ptr<BfRtTableData> inner_pipe_data;
      bf_status = this->dataAllocateContainer(
          P4_PIPELINES, WARM_INIT_COMPLETED, &inner_pipe_data);
      if (bf_status != BF_SUCCESS) return bf_status;

      bf_status =
          inner_pipe_data->setValue(PIPELINE_NAME, pipe_info[i].first.get());
      if (bf_status != BF_SUCCESS) return bf_status;

      bf_status = inner_pipe_data->setValue(RUNTIME_CTX_FILE,
                                            context_file_name_v[i].get());
      if (bf_status != BF_SUCCESS) return bf_status;

      bf_status =
          inner_pipe_data->setValue(BINARY_PATH, binary_file_name_v[i].get());
      if (bf_status != BF_SUCCESS) return bf_status;

      bf_status =
          inner_pipe_data->setValue(PIPE_SCOPE, pipe_info[i].second.get());
      if (bf_status != BF_SUCCESS) return bf_status;

      inner_pipe_data_vec.push_back(std::move(inner_pipe_data));
    }
    bf_status =
        inner_data->setValue(P4_PIPELINES, std::move(inner_pipe_data_vec));
    if (bf_status != BF_SUCCESS) return bf_status;

    inner_data_vec.push_back(std::move(inner_data));
  }
  bf_status = dev_data->setValue(P4_PROGRAMS, std::move(inner_data_vec));

  return bf_status;
}

bf_status_t BfRtDevWarmInitTable::tableDefaultEntryGet(
    const BfRtSession & /*session*/,
    const bf_rt_target_t &dev_tgt,
    const uint64_t & /*flags*/,
    BfRtTableData *data) const {
  BfRtDevWarmInitTableData *dev_data =
      static_cast<BfRtDevWarmInitTableData *>(data);
  bf_status_t status = BF_SUCCESS;

  // Action needs to be non-zero before setActiveFields is called. With out it,
  // active_fields_ are not set properly.
  dev_data->actionIdSet(WARM_INIT_COMPLETED);

  std::vector<bf_rt_id_t> emptyFields;
  dev_data->setActiveFields(emptyFields);

  // ERROR data field
  bool error = false;
  status = bf_pal_warm_init_error_get(dev_tgt.dev_id, &error);
  if (status != BF_SUCCESS) {
    LOG_ERROR(
        "ERROR: %s:%d bf_pal_warm_init_error_get() failed", __func__, __LINE__);
    return status;
  }
  dev_data->setValue(ERROR, error);

  // INIT_MODE data field
  bf_dev_init_mode_t warm_init_mode;

  status = bf_device_init_mode_get(dev_tgt.dev_id, &warm_init_mode);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Unable to get the device mode for dev %d, sts %s (%d)",
              dev_tgt.dev_id,
              bf_err_str(status),
              status);
  }

  const std::string mode_string = init_mode_to_str_map.at(warm_init_mode);
  status = dev_data->setValue(INIT_MODE, (mode_string));

  // P4_PROGRAMS data field
  status = profileToContainer(dev_tgt.dev_id, dev_data);
  if (status != BF_SUCCESS) {
    LOG_TRACE("%s:%d %s: Error in setting data object",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return status;
  }

  return status;
}

bf_status_t BfRtDevWarmInitTable::tableDefaultEntryReset(
    const BfRtSession & /*session*/,
    const bf_rt_target_t &dev_tgt,
    const uint64_t & /*flags*/) const {
  return bf_pal_pltfm_reset_config(dev_tgt.dev_id);
}

bf_status_t BfRtDevWarmInitTable::tableClear(const BfRtSession &session,
                                             const bf_rt_target_t &dev_tgt,
                                             const uint64_t &flags) const {
  return this->tableDefaultEntryReset(session, dev_tgt, flags);
}

bf_status_t BfRtDevWarmInitTable::dataAllocate(
    std::unique_ptr<BfRtTableData> *data_ret) const {
  std::vector<bf_rt_id_t> fields;

  return this->dataAllocate_internal(fields, 0, data_ret);
}

bf_status_t BfRtDevWarmInitTable::dataAllocate(
    const bf_rt_id_t &action_id,
    std::unique_ptr<BfRtTableData> *data_ret) const {
  std::vector<bf_rt_id_t> fields;

  return this->dataAllocate_internal(fields, action_id, data_ret);
}

bf_status_t BfRtDevWarmInitTable::dataAllocate(
    const std::vector<bf_rt_id_t> &fields,
    std::unique_ptr<BfRtTableData> *data_ret) const {
  return this->dataAllocate_internal(fields, 0, data_ret);
}

bf_status_t BfRtDevWarmInitTable::dataAllocate(
    const std::vector<bf_rt_id_t> &fields,
    const bf_rt_id_t &action_id,
    std::unique_ptr<BfRtTableData> *data_ret) const {
  return this->dataAllocate_internal(fields, action_id, data_ret);
}

bf_status_t BfRtDevWarmInitTable::dataAllocate_internal(
    const std::vector<bf_rt_id_t> &fields,
    const bf_rt_id_t &action_id,
    std::unique_ptr<BfRtTableData> *data_ret) const {
  *data_ret = std::unique_ptr<BfRtTableData>(
      new BfRtDevWarmInitTableData(this, action_id, fields));

  if (*data_ret == nullptr) {
    LOG_ERROR("%s:%d %s Error : Failed to allocate data",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return BF_NO_SYS_RESOURCES;
  }

  return BF_SUCCESS;
}

bf_status_t BfRtDevWarmInitTable::dataAllocateContainer(
    const bf_rt_id_t &container_id,
    const bf_rt_id_t &action_id,
    std::unique_ptr<BfRtTableData> *data_ret) const {
  if (container_id == P4_PROGRAMS) {
    *data_ret = std::unique_ptr<BfRtTableData>(
        new BfRtDevP4ProgramsTableData(this, action_id, container_id));
  } else if (container_id == P4_PIPELINES) {
    *data_ret = std::unique_ptr<BfRtTableData>(
        new BfRtDevP4PipelinesTableData(this, action_id, container_id));
  } else {
    LOG_ERROR("%s:%d %s Error : Failed to allocate data",
              __func__,
              __LINE__,
              table_name_get().c_str());
    return BF_NO_SYS_RESOURCES;
  }

  if (*data_ret == nullptr) {
    return BF_NO_SYS_RESOURCES;
  }
  return BF_SUCCESS;
}

}  // namespace bfrt

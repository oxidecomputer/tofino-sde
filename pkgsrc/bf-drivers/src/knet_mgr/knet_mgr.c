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

#include <inttypes.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#ifndef __sun
#include <linux/if_ether.h>
#endif
#include <stdlib.h>
#include <unistd.h>
#include <knet_mgr/bf_knet_if.h>
#include "knet_mgr_log.h"

#ifdef __sun

#undef NDEBUG
#include <assert.h>

#define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))

bool bf_knet_module_is_inited() {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_cpuif_ndev_add(const char *UNUSED(cpuif_netdev_name),
                                   char *UNUSED(cpuif_knetdev_name),
                                   bf_knet_cpuif_t *UNUSED(knet_cpuif_id)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_cpuif_list_get(bf_knet_cpuif_list_t *const UNUSED(cpuif_list),
                                   bf_knet_count_t *const UNUSED(cpuif_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_cpuif_ndev_delete(const bf_knet_cpuif_t UNUSED(knet_cpuif_id)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_hostif_kndev_add(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                     bf_knet_hostif_knetdev_t *UNUSED(hostif_knetdev)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_hostif_list_get(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                    bf_knet_hostif_list_t *const UNUSED(hostif_list),
				    bf_knet_count_t *const UNUSED(hostif_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_hostif_kndev_delete(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                        const bf_knet_hostif_t UNUSED(knet_hostif_id)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_rx_filter_add(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                 bf_knet_rx_filter_t *UNUSED(rx_filter)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_rx_filter_delete(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                    const bf_knet_filter_t UNUSED(filter_id)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_rx_filter_get(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                 const bf_knet_filter_t UNUSED(filter_id),
                                 bf_knet_rx_filter_t *UNUSED(rx_filter),
                                 bf_knet_count_t UNUSED(rx_mutation_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_rx_filter_list_get(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                      bf_knet_filter_t *const UNUSED(filter_list),
                                      bf_knet_count_t *const UNUSED(filter_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_tx_action_add(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                 const bf_knet_hostif_t UNUSED(knet_hostif_id),
                                 bf_knet_tx_action_t *const UNUSED(tx_action)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_tx_action_delete(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                    const bf_knet_hostif_t UNUSED(knet_hostif_id)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_get_cpuif_cnt(bf_knet_count_t *const UNUSED(obj_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_get_hostif_cnt(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                  bf_knet_count_t *const UNUSED(obj_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_get_rx_filter_cnt(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                     bf_knet_count_t *const UNUSED(obj_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_get_rx_mutation_cnt(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                       const bf_knet_filter_t UNUSED(filter_id),
                                       bf_knet_count_t *const UNUSED(obj_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_get_tx_mutation_cnt(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                       const bf_knet_hostif_t UNUSED(hostif_id),
                                       bf_knet_count_t *const UNUSED(obj_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

bf_status_t bf_knet_tx_action_get(const bf_knet_cpuif_t UNUSED(knet_cpuif_id),
                                 const bf_knet_hostif_t UNUSED(knet_hostif_id),
                                 bf_knet_tx_action_t *UNUSED(tx_action),
                                 bf_knet_count_t UNUSED(tx_mutation_count)) {
	assert(0);
	return BF_NOT_SUPPORTED;
}

#else //__sun

static iline bf_status_t bf_knet_status_to_status(
    bf_knet_status_t knet_status) {
  bf_status_t status = BF_SUCCESS;

  switch (knet_status) {
    case BF_KNET_E_NONE:
      status = BF_SUCCESS;
      break;

    case BF_KNET_E_RESOURCE:
      status = BF_NO_SYS_RESOURCES;
      break;

    case BF_KNET_E_PARAM:
    case BF_KNET_E_MEM_ACCESS:
    case BF_KNET_E_NAME_INVALID:
    case BF_KNET_E_MSG_TYPE:
    case BF_KNET_E_OBJ_TYPE:
      status = BF_INVALID_ARG;
      break;

    case BF_KNET_E_NOT_FOUND:
    case BF_KNET_E_CPUIF_NOT_FOUND:
    case BF_KNET_E_HOSTIF_NOT_FOUND:
    case BF_KNET_E_RX_FILTER_NOT_FOUND:
    case BF_KNET_E_TX_ACTION_NOT_FOUND:
      status = BF_OBJECT_NOT_FOUND;
      break;

    default:
      status = BF_UNEXPECTED;
      break;
  }
  return status;
}

bool bf_knet_module_is_inited() {
#ifdef __sun
	assert(0);
	return BF_NOT_SUPPORTED;
#else
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t mod_info;
  bf_status_t status = BF_SUCCESS;

  memset(&mod_info, 0, sizeof(mod_info));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  mod_info.hdr.type = BF_KNET_M_IS_MOD_INIT;
  mod_info.hdr.len = sizeof(mod_info);

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&mod_info);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);

  if (sockfd < 0) {
    LOG_ERROR("Failed to Create to socket:%s at %s,  line %d\n",
              strerror(errno),
              __FILE__,
              __LINE__);
    return false;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    if (errno != ENODEV) {
      LOG_ERROR("IOCTL on %s Failed:%s at %s, line %d\n",
                cpuifreq.ifr_name,
                strerror(errno),
                __FILE__,
                __LINE__);
    }
    close(sockfd);
    return false;
  }

  status = bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Failed to fetch KNET init status, sts %s (%d)",
              bf_err_str(status),
              status);
    close(sockfd);
    return false;
  }

  close(sockfd);
  return true;
#endif
}

bf_status_t bf_knet_cpuif_ndev_add(const char *cpuif_netdev_name,
                                   char *cpuif_knetdev_name,
                                   bf_knet_cpuif_t *knet_cpuif_id) {
#ifdef __sun
	assert(0);
	return BF_NOT_SUPPORTED;
#else
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  if (!cpuif_netdev_name || !cpuif_knetdev_name || !knet_cpuif_id) {
    LOG_ERROR(
        "cpuif_netdev add failed. One or more input paramters are "
        "invalid(null)  at %s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_CPUIF_NDEV_ADD;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  memcpy(cpuif_config.cpuif_ndev_add.cpuif_netdev_name,
         cpuif_netdev_name,
         IFNAMSIZ);

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);

  if (sockfd < 0) {
    LOG_ERROR("Failed to Create to socket:%s at %s,  line %d\n",
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }
  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("IOCTL on %s Failed:%s at %s, line %d\n",
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  if (((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status != BF_KNET_E_NONE)
    goto ret;

  *knet_cpuif_id =
      ((bf_knet_msg_cpuif_ndev_add_t *)cpuifreq.ifr_data)->knet_cpuif_id;
  memcpy(
      cpuif_knetdev_name,
      ((bf_knet_msg_cpuif_ndev_add_t *)cpuifreq.ifr_data)->cpuif_knetdev_name,
      IFNAMSIZ);

ret:
  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
#endif // __sun
}

bf_status_t bf_knet_cpuif_list_get(bf_knet_cpuif_list_t *const cpuif_list,
                                   bf_knet_count_t *const cpuif_count) {
#ifdef __sun
	assert(0);
	return BF_NOT_SUPPORTED;
#else
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  if (!cpuif_list) {
    LOG_ERROR(
        "Cpuif list get failed. Input paramter cpuif_list is invalid(NULL)  at "
        "%s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_CPUIF_LIST_GET;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  cpuif_config.cpuif_list_get.cpuif_list = cpuif_list;
  cpuif_config.cpuif_list_get.size = *cpuif_count;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR(
        "Cpuif List Get failed"
        ". Failed to "
        "create socket:%s at %s, line %d\n",
        strerror(errno),
        __FILE__,
        __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR(
        "Cpuif List Get failed"
        ". IOCTL on %s "
        "Failed:%s at %s, line %d\n",
        cpuifreq.ifr_name,
        strerror(errno),
        __FILE__,
        __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  *cpuif_count = ((bf_knet_msg_cpuif_list_get_t *)(cpuifreq.ifr_data))->size;
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
#endif __sun
}

bf_status_t bf_knet_cpuif_ndev_delete(const bf_knet_cpuif_t knet_cpuif_id) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_CPUIF_NDEV_DELETE;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Failed to Create to socket:%s at %s,  line %d\n",
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("IOCTL on %s Failed:%s at %s, line %d\n",
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_hostif_kndev_add(const bf_knet_cpuif_t knet_cpuif_id,
                                     bf_knet_hostif_knetdev_t *hostif_knetdev) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  if (!hostif_knetdev) {
    LOG_ERROR(
        "Hostif knetdev add failed. Input paramters hostif_knetdev is "
        "invalid(null)  at %s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_HOSTIF_KNDEV_ADD;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  memcpy(&cpuif_config.hostif_kndev_add.hostif_knetdev,
         hostif_knetdev,
         sizeof(*hostif_knetdev));
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("hostif_knetdev add failed for %s on cpuif_id %016" PRIx64
              ". Failed to create "
              "socket:%s at %s, line %d\n",
              hostif_knetdev->name,
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("hostif_knetdev add failed for %s on cpuif_id %016" PRIx64
              ". IOCTL on %s "
              "Failed:%s at %s, line %d\n",
              hostif_knetdev->name,
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  if (((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status != BF_KNET_E_NONE)
    goto ret;

  hostif_knetdev->knet_hostif_id =
      cpuif_config.hostif_kndev_add.hostif_knetdev.knet_hostif_id;
ret:
  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_hostif_list_get(const bf_knet_cpuif_t knet_cpuif_id,
                                    bf_knet_hostif_list_t *const hostif_list,
                                    bf_knet_count_t *const hostif_count) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  if (!hostif_list) {
    LOG_ERROR(
        "hostif list get failed. Input paramter hostif_list is invalid(NULL)  "
        "at "
        "%s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_HOSTIF_LIST_GET;
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  cpuif_config.hostif_list_get.hostif_list = hostif_list;
  cpuif_config.hostif_list_get.size = *hostif_count;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR(
        "Hostif List Get failed"
        ". Failed to "
        "create socket:%s at %s, line %d\n",
        strerror(errno),
        __FILE__,
        __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR(
        "Hostif List Get failed"
        ". IOCTL on %s "
        "Failed:%s at %s, line %d\n",
        cpuifreq.ifr_name,
        strerror(errno),
        __FILE__,
        __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  *hostif_count = ((bf_knet_msg_hostif_list_get_t *)(cpuifreq.ifr_data))->size;
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_hostif_kndev_delete(const bf_knet_cpuif_t knet_cpuif_id,
                                        const bf_knet_hostif_t knet_hostif_id) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_HOSTIF_KNDEV_DELETE;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.hdr.id.hostif_id = knet_hostif_id;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("hostif_knetdev delete failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". "
              "Failed to create socket:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("hostif_knetdev delete failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". IOCTL "
              "on %s Failed:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

static int cmp_mut(const void *a, const void *b) {
  bf_knet_packet_mutation_t *mutation1 = (bf_knet_packet_mutation_t *)a;
  bf_knet_packet_mutation_t *mutation2 = (bf_knet_packet_mutation_t *)b;

  if (mutation1->mutation_type == mutation2->mutation_type) {
    return (mutation1->offset - mutation2->offset);
  } else {
    if (mutation1->mutation_type == BF_KNET_RX_MUT_STRIP)
      return -1;
    else
      return 1;
  }
}

bf_status_t bf_knet_rx_filter_add(const bf_knet_cpuif_t knet_cpuif_id,
                                  bf_knet_rx_filter_t *rx_filter) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;
  bf_knet_packet_mutation_t *mutation;
  bf_knet_action_count_t count;

  if (!rx_filter) {
    LOG_ERROR(
        "Failed to add rx filter.Input paramter rx_filter is invalid(null)  at "
        "%s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_RX_FILTER_ADD;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  memcpy(&cpuif_config.rx_filter_add.rx_filter, rx_filter, sizeof(*rx_filter));
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;

  mutation = cpuif_config.rx_filter_add.rx_filter.action.pkt_mutation;
  count = cpuif_config.rx_filter_add.rx_filter.action.count;

  // TODO:Is this ok??
  /* Sorting is done simply to help improve the LKM performance. The chances of
  reallocation reduces
  if all strip actions are performed before insert operations. This assumes that
  the STRIP and insert offset+len
  regions are non overlapping */
  qsort(mutation, count, sizeof(bf_knet_packet_mutation_t), cmp_mut);

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Rx Filter Add failed on cpuif_id %016" PRIx64
              ". Failed to create socket:%s at "
              "%s, line %d\n",
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("Rx Filter Add failed on cpuif_id %016" PRIx64
              ". IOCTL on %s Failed:%s at %s, "
              "line %d\n",
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  if (((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status != BF_KNET_E_NONE)
    goto ret;

  rx_filter->spec.filter_id =
      cpuif_config.rx_filter_add.rx_filter.spec.filter_id;
ret:
  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_rx_filter_delete(const bf_knet_cpuif_t knet_cpuif_id,
                                     const bf_knet_filter_t filter_id) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_RX_FILTER_DELETE;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.hdr.id.filter_id = filter_id;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Rx Filter Delete failed for filter_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". Failed to "
              "create socket:%s at %s, line %d\n",
              filter_id,
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("Rx Filter Delete failed for filter id %016" PRIx64
              " on cpuifid %016" PRIx64
              ". IOCTL on %s "
              "Failed:%s at %s, line %d\n",
              filter_id,
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_rx_filter_get(const bf_knet_cpuif_t knet_cpuif_id,
                                  const bf_knet_filter_t filter_id,
                                  bf_knet_rx_filter_t *rx_filter,
                                  bf_knet_count_t rx_mutation_count) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  if (!rx_filter) {
    LOG_ERROR(
        "Rx filter get failed. Input paramter rx_filter is invalid(NULL)  at "
        "%s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  /* We copy the entire rx_filter passed by user to our ioctl request. This
  basically
  copies over the address of pkt_mutation  where the kernel copies over the
  filter mutations */
  memcpy(&cpuif_config.rx_filter_get.rx_filter, rx_filter, sizeof(*rx_filter));
  cpuif_config.hdr.type = BF_KNET_M_RX_FILTER_GET;
  cpuif_config.hdr.len = sizeof(cpuif_config);

  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.hdr.id.filter_id = filter_id;
  cpuif_config.rx_filter_get.mutation_count = rx_mutation_count;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Rx Filter Get failed for filter_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". Failed to "
              "create socket:%s at %s, line %d\n",
              filter_id,
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("Rx Filter Get failed for filter id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". IOCTL on %s "
              "Failed:%s at %s, line %d\n",
              filter_id,
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  memcpy(rx_filter, &cpuif_config.rx_filter_get.rx_filter, sizeof(*rx_filter));
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_rx_filter_list_get(const bf_knet_cpuif_t knet_cpuif_id,
                                       bf_knet_filter_t *const filter_list,
                                       bf_knet_count_t *const filter_count) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  if (!filter_list) {
    LOG_ERROR(
        "Rx filter list get failed. Input paramter filter_list is "
        "invalid(NULL)  at "
        "%s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_RX_FILTER_LIST_GET;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  cpuif_config.rx_filter_list_get.filter_list = filter_list;

  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.rx_filter_list_get.size = *filter_count;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Rx Filter List Get failed for knet_cpuif_id %016" PRIx64
              ". Failed to "
              "create socket:%s at %s, line %d\n",
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("Rx Filter List Get failed for knet_cpuif_id %016" PRIx64
              ". IOCTL on %s "
              "Failed:%s at %s, line %d\n",
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  *filter_count =
      ((bf_knet_msg_rx_filter_list_get_t *)(cpuifreq.ifr_data))->size;
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_tx_action_add(const bf_knet_cpuif_t knet_cpuif_id,
                                  const bf_knet_hostif_t knet_hostif_id,
                                  bf_knet_tx_action_t *const tx_action) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;
  bf_knet_packet_mutation_t *mutation;
  bf_knet_action_count_t count;

  if (!tx_action) {
    LOG_ERROR(
        "Tx action add failed. Input paramter tx_action is invalid(NULL)  at "
        "%s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_TX_ACTION_ADD;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  memcpy(&cpuif_config.tx_action_add.tx_action, tx_action, sizeof(*tx_action));
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.hdr.id.hostif_id = knet_hostif_id;

  mutation = cpuif_config.tx_action_add.tx_action.pkt_mutation;
  count = cpuif_config.tx_action_add.tx_action.count;

  // TODO:Is this ok??
  /* Sorting is done simply to help improve the LKM performance. The chances of
  reallocation reduces
  if all strip actions are performed before insert operations. This assumes that
  the STRIP and insert offset+len
  regions are non overlapping */
  qsort(mutation, count, sizeof(bf_knet_packet_mutation_t), cmp_mut);

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Tx Action Add failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". Failed to "
              "create socket:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("Tx Action add failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". IOCTL on %s "
              "Failed:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  if (((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status != BF_KNET_E_NONE)
    goto ret;

ret:
  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

bf_status_t bf_knet_tx_action_delete(const bf_knet_cpuif_t knet_cpuif_id,
                                     const bf_knet_hostif_t knet_hostif_id) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  cpuif_config.hdr.type = BF_KNET_M_TX_ACTION_DELETE;
  cpuif_config.hdr.len = sizeof(cpuif_config);
  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.hdr.id.hostif_id = knet_hostif_id;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Tx Action Delete failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". Failed to "
              "create socket:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("Tx Action Delete failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". IOCTL on "
              "%s Failed:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

static bf_status_t bf_knet_obj_cnt_get(const bf_knet_cpuif_t knet_cpuif_id,
                                       const bf_knet_obj_type_t type,
                                       const bf_knet_hostif_t hostif_id,
                                       const bf_knet_filter_t filter_id,
                                       bf_knet_count_t *const obj_count) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t get_msg;
  bf_status_t status = BF_SUCCESS;

  memset(&get_msg, 0, sizeof(get_msg));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  get_msg.hdr.type = BF_KNET_M_OBJ_CNT_GET;
  get_msg.hdr.len = sizeof(get_msg);

  get_msg.hdr.knet_cpuif_id = knet_cpuif_id;
  if (type == BF_KNET_O_RX_MUTATION) {
    get_msg.hdr.id.filter_id = filter_id;
  } else {
    get_msg.hdr.id.hostif_id = hostif_id;
  }
  get_msg.obj_cnt_get.type = type;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&get_msg);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);

  if (sockfd < 0) {
    LOG_ERROR("Failed to Create to socket:%s at %s,  line %d\n",
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("IOCTL on %s Failed:%s at %s, line %d\n",
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  status = bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
  if (status != BF_SUCCESS) {
    LOG_ERROR(
        "Object Count get failed, sts %s (%d)", bf_err_str(status), status);
    close(sockfd);
    return status;
  }

  close(sockfd);
  *obj_count = ((bf_knet_msg_obj_cnt_get_t *)(cpuifreq.ifr_data))->obj_count;
  return status;
}

bf_status_t bf_knet_get_cpuif_cnt(bf_knet_count_t *const obj_count) {
  bf_status_t status;

  status = bf_knet_obj_cnt_get(0, BF_KNET_O_CPUIF, 0, 0, obj_count);
  if (status != BF_SUCCESS) {
    LOG_ERROR(
        "Failed to get Cpuif count, sts %s (%d)", bf_err_str(status), status);
  }
  return status;
}

bf_status_t bf_knet_get_hostif_cnt(const bf_knet_cpuif_t knet_cpuif_id,
                                   bf_knet_count_t *const obj_count) {
  bf_status_t status;

  status =
      bf_knet_obj_cnt_get(knet_cpuif_id, BF_KNET_O_HOSTIF, 0, 0, obj_count);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Failed to get hostif count, for cpuif_id %016" PRIx64
              " sts %s (%d)",
              knet_cpuif_id,
              bf_err_str(status),
              status);
  }
  return status;
}

bf_status_t bf_knet_get_rx_filter_cnt(const bf_knet_cpuif_t knet_cpuif_id,
                                      bf_knet_count_t *const obj_count) {
  bf_status_t status;

  status =
      bf_knet_obj_cnt_get(knet_cpuif_id, BF_KNET_O_RX_FILTER, 0, 0, obj_count);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Failed to get rx filter count, for cpuif_id %016" PRIx64
              " sts %s (%d)",
              knet_cpuif_id,
              bf_err_str(status),
              status);
  }
  return status;
}

bf_status_t bf_knet_get_rx_mutation_cnt(const bf_knet_cpuif_t knet_cpuif_id,
                                        const bf_knet_filter_t filter_id,
                                        bf_knet_count_t *const obj_count) {
  bf_status_t status;

  status = bf_knet_obj_cnt_get(
      knet_cpuif_id, BF_KNET_O_RX_MUTATION, 0, filter_id, obj_count);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Failed to get rx filter mutation count, for cpuif_id %016" PRIx64
              ", filter %016" PRIx64
              " "
              "sts %s (%d)",
              knet_cpuif_id,
              filter_id,
              bf_err_str(status),
              status);
  }
  return status;
}

bf_status_t bf_knet_get_tx_mutation_cnt(const bf_knet_cpuif_t knet_cpuif_id,
                                        const bf_knet_hostif_t hostif_id,
                                        bf_knet_count_t *const obj_count) {
  bf_status_t status;

  status = bf_knet_obj_cnt_get(
      knet_cpuif_id, BF_KNET_O_TX_MUTATION, hostif_id, 0, obj_count);
  if (status != BF_SUCCESS) {
    LOG_ERROR("Failed to get tx mutation count, for cpuif_id %016" PRIx64
              ", hostif %016" PRIx64
              " sts %s "
              "(%d)",
              knet_cpuif_id,
              hostif_id,
              bf_err_str(status),
              status);
  }
  return status;
}

bf_status_t bf_knet_tx_action_get(const bf_knet_cpuif_t knet_cpuif_id,
                                  const bf_knet_hostif_t knet_hostif_id,
                                  bf_knet_tx_action_t *tx_action,
                                  bf_knet_count_t tx_mutation_count) {
  int sockfd;
  struct ifreq cpuifreq;
  bf_knet_msg_t cpuif_config;

  if (!tx_action) {
    LOG_ERROR(
        "Tx Action get failed. Input paramter tx_action is invalid(NULL)  at "
        "%s, line %d\n",
        __FILE__,
        __LINE__);
    return BF_INVALID_ARG;
  }

  memset(&cpuif_config, 0, sizeof(cpuif_config));
  memset(&cpuifreq, 0, sizeof(cpuifreq));
  memcpy(&cpuif_config.tx_action_get.tx_action, tx_action, sizeof(*tx_action));
  cpuif_config.hdr.type = BF_KNET_M_TX_ACTION_GET;
  cpuif_config.hdr.len = sizeof(cpuif_config);

  cpuif_config.hdr.knet_cpuif_id = knet_cpuif_id;
  cpuif_config.hdr.id.hostif_id = knet_hostif_id;
  cpuif_config.tx_action_get.mutation_count = tx_mutation_count;

  strcpy(cpuifreq.ifr_name, "bf_knet");
  cpuifreq.ifr_data = (void *)(&cpuif_config);

  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
  if (sockfd < 0) {
    LOG_ERROR("Tx Action Get failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". Failed to "
              "create socket:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              strerror(errno),
              __FILE__,
              __LINE__);
    return BF_UNEXPECTED;
  }

  if (ioctl(sockfd, SIOCBFKNETCMD, &cpuifreq) < 0) {
    LOG_ERROR("Tx Action Get failed for hostif_id %016" PRIx64
              " on cpuif_id %016" PRIx64
              ". IOCTL on %s "
              "Failed:%s at %s, line %d\n",
              knet_hostif_id,
              knet_cpuif_id,
              cpuifreq.ifr_name,
              strerror(errno),
              __FILE__,
              __LINE__);
    close(sockfd);
    return BF_UNEXPECTED;
  }

  close(sockfd);
  memcpy(tx_action, &cpuif_config.tx_action_get.tx_action, sizeof(*tx_action));
  return bf_knet_status_to_status(
      ((bf_knet_msg_t *)(cpuifreq.ifr_data))->hdr.status);
}

#endif // __sun

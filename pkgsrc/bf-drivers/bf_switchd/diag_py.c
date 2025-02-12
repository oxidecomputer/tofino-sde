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

/* clang-format off */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <stdbool.h>
#include <inttypes.h>  //strlen

#include <pthread.h>
#ifndef __sun
int pthread_setname_np(pthread_t *thread, const char *name);
#endif

#include <bf_types/bf_types.h>

/* Compile out the diagnostic python interface by default */
#undef DIAG_PYTHON_SUPPORT_CREDO
#undef DIAG_PYTHON_SUPPORT_ALPHAWAVE

/* un-comment to open diag serdes TCP port */
//#define DIAG_PYTHON_SUPPORT_CREDO
#ifdef DIAG_PYTHON_SUPPORT_CREDO
void start_credo_py_server_thread(bool *local_only);
#endif  // DIAG_PYTHON_SUPPORT_CREDO

/* un-comment to open AW serdes UDP port */
//#define DIAG_PYTHON_SUPPORT_ALPHAWAVE
#ifdef DIAG_PYTHON_SUPPORT_ALPHAWAVE
void start_aw_py_server_thread(bool *local_only);
#endif // DIAG_PYTHON_SUPPORT_ALPHAWAVE

/**************************************************************
* diagnostic_python_interface
*
* (optionally) create the diagnostic interface used to test
* serdes using external python modules.
**************************************************************/
void diagnostic_python_interface(bool *local_only) {

#ifdef DIAG_PYTHON_SUPPORT_CREDO
  start_credo_py_server_thread(local_only);
#endif //DIAG_PYTHON_SUPPORT_CREDO

#ifdef DIAG_PYTHON_SUPPORT_ALPHAWAVE
  start_aw_py_server_thread(local_only);
#endif // DIAG_PYTHON_SUPPORT_ALPHAWAVE

  (void)local_only; // in case neither enabled
}

#ifdef DIAG_PYTHON_SUPPORT_CREDO
#include "credo_py.c"

pthread_t credo_py_thread;
pthread_attr_t credo_py_attr;
const char *credo_py_thread_nm = "Credo Python Server";
extern void start_server(bool is_local_only);

void *credo_py_thread_fn(void *local_only) {
  bool local = *((bool *)local_only);

  credo_py_start_server(local);
  return NULL;
}

void start_credo_py_server_thread(bool *local_only) {
  int ret;

  pthread_attr_init(&credo_py_attr);
  if ((ret = pthread_create(
           &credo_py_thread, &credo_py_attr, credo_py_thread_fn, local_only)) !=
      0) {
    printf("ERROR: thread creation failed for credo python service\n");
    return;
  }
  pthread_setname_np((pthread_t *)credo_py_thread, credo_py_thread_nm);
  printf("bf_switchd: Credo python server thread initialized..");
}
#endif  // DIAG_PYTHON_SUPPORT_CREDO



#ifdef DIAG_PYTHON_SUPPORT_ALPHAWAVE
#include "aw_py.c"
pthread_t aw_py_thread;
pthread_attr_t aw_py_attr;
const char *aw_py_thread_nm = "Alphawave Python Server";
extern void aw_py_start_server(bool is_local_only);

void *aw_py_thread_fn(void *local_only) {
  bool local = *((bool *)local_only);
  aw_py_start_server(local);
  return NULL;
}

void start_aw_py_server_thread(bool *local_only) {
  int ret;

  pthread_attr_init(&aw_py_attr);
  if ((ret = pthread_create(
           &aw_py_thread, &aw_py_attr, aw_py_thread_fn, local_only)) !=
      0) {
    printf("ERROR: thread creation failed for aw python service\n");
    return;
  }
  pthread_setname_np((pthread_t *)aw_py_thread, aw_py_thread_nm);
  printf("bf_switchd: Alphawave python server thread initialized..");
}
#else
void evb_mss_reset(uint32_t macro_base_addr) {
  (void)macro_base_addr;
}
void evb_write_csr(uint32_t addr, uint32_t data) {
  (void)addr;
  (void)data;
}
void evb_read_csr(uint32_t addr, uint32_t *data) {
  (void)addr;
  (void)data;
}

#endif // DIAG_PYTHON_SUPPORT_ALPHAWAVE


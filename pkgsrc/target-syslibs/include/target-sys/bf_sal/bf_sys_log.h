/*******************************************************************************
 * Copyright(c) 2021 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this software except as stipulated in the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ******************************************************************************/

/*!
 * @file bf_sys_log.h
 * @date
 *
 */

#ifndef _BF_SYS_LOG_H_
#define _BF_SYS_LOG_H_

/* Allow the use in C++ code.  */
#ifdef __cplusplus
extern "C" {
#endif

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif /* __KERNEL */

/**
 * @addtogroup bf_sal-log
 * @{
 */

#include <target-sys/log_common.h>

/*!
 * Macros BF Lo Destination flag bit masks
 */
#define BF_LOG_DEST_NONE 0
#define BF_LOG_DEST_STDOUT (1 << 0)
#define BF_LOG_DEST_STDERR (1 << 1)
#define BF_LOG_DEST_FILE (1 << 2)
#define BF_LOG_DEST_SYSLOG (1 << 3)
#define BF_LOG_DEST_MAX (4) /* match it to the maximum */

/**
 * initialize bf_sys_log subsystem
 *
 * @param arg1
 *  implementation specific parameter
 * @param arg2
 *  implementation specific parameter
 * @param arg3
 *  implementation specific parameter
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_log_init(void *arg1, void *arg2, void *arg3);

/**
 * close bf_sys_log subsystem
 *
 * @param none
 *
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_log_close(void);

/**
 * log function (generic)
 *
 * @param module
 *  log module (or facility)
 * @param bf_level
 *  log level
 * @param format
 *  formatted string
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_log(int module, int bf_level, const char *format, ...);

/**
 * set log level
 *  if log level is equal or more than set trace level, then log messages
 *  go to trace buffer as wel
 *
 * @param module
 *  log module (or facility)
 * @param bf_level
 *  log level
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_log_level_set(int module, int output, int bf_level);

/* @} */

/**
 * @addtogroup bf_sal-tracing
 * @{
 */

/**
 * trace function (generic)
 *
 * @param module
 *  trace module (or facility)
 * @param bf_level
 *  trace level
 * @param format
 *  formatted string
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_trace(int module, int bf_level, const char *format, ...);

/*!
 * trace ring buffer APIs
 */

/**
 * set trace level
 *   if log level is equal or more than set trace level, then log messages
 *   go to trace buffer as well
 * @param module
 *  trace module (or facility)
 * @param bf_level
 *  trace level
 * @return
 *  0 on Sucess, -1 on error
 */
void bf_sys_trace_level_set(int module, int bf_level);

/**
 * get trace ring buffer
 *
 * @param buf
 *  buffer to read data into
 * @param size
 *  size of buf
 * @param size
 *  size of data actually read into buf
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_trace_get(uint8_t *buf, size_t size, size_t *len_written);

/**
 * reset trace ring buffer
 *
 * @param  none
 *
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_trace_reset(void);

/**
 * trace plus log function (generic)
 *
 * @param module
 *  trace module (or facility)
 * @param bf_level
 *  trace level
 * @param format
 *  formatted string
 * @return
 *  0 on Sucess, -1 on error
 */
#ifdef BF_SYS_LOG_FORMAT_CHECK
int bf_sys_log_and_trace(bf_log_modules_t module,
    bf_log_levels_t level, const char *format, ...)
    __attribute__((format(printf, 3, 4)));
#else
int bf_sys_log_and_trace(bf_log_modules_t module,
    bf_log_levels_t level, const char *format, ...);
#endif

/**
 * Check if a log level is enabled or disabled.
 *
 * @param module
 *  trace module (or facility)
 * @param bf_level
 *  trace level
 * @return
 *  Less than zero on error
 *  Zero if the module's log level is disabled
 *  One if the moduel's log level is enabled
 */
int bf_sys_log_is_log_enabled(int module, int level);

/**
 * set syslog level
 *
 * @param bf_level
 *  log level
 * @return
 *  0 on Sucess, -1 on error
 */
int bf_sys_syslog_level_set(int level);

/* @} */

#ifdef __cplusplus
}
#endif /* C++ */

#endif /* _BF_SYS_LOG_H_ */

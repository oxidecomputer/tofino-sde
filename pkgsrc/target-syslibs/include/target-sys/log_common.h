#ifndef _LOG_COMMON_H_
#define _LOG_COMMON_H_

typedef enum bf_log_levels {
	BF_LOG_NONE,
	BF_LOG_CRIT,
	BF_LOG_ERR,
	BF_LOG_WARN,
	BF_LOG_INFO,
	BF_LOG_DBG
} bf_log_levels_t;

#define BF_LOG_MAX (BF_LOG_DBG) /* keep this the last */

typedef enum bf_log_modules {
	BF_MOD_START =  0,
	BF_MOD_SYS,
	BF_MOD_UTIL,
	BF_MOD_LLD,
	BF_MOD_PIPE,
	BF_MOD_TM,
	BF_MOD_MC,
	BF_MOD_PKT,
	BF_MOD_DVM,
	BF_MOD_PORT,
	BF_MOD_AVAGO,
	BF_MOD_DRU,
	BF_MOD_MAP,
	BF_MOD_SWITCHAPI,
	BF_MOD_SAI,
	BF_MOD_PI,
	BF_MOD_PLTFM,
	BF_MOD_PAL,
	BF_MOD_PM,
	BF_MOD_KNET,
	BF_MOD_BFRT,
	BF_MOD_P4RT,
	BF_MOD_SWITCHD,
	BF_MOD_MAX
} bf_log_modules_t;

/*
 * Optional callback provided by the control plane binary linked with this
 * library.
 */
extern int bf_sys_log_callback(bf_log_modules_t module,
	bf_log_levels_t level, const char *msg);

#endif /* _LOG_COMMON_H_ */

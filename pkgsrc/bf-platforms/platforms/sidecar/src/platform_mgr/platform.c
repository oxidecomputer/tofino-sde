#ifdef __linux__
    #define _GNU_SOURCE
#endif

#include <ctype.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <bf_types/bf_types.h>
#include <bf_pltfm_types/bf_pltfm_types.h>
#include <bf_switchd/bf_switchd.h>
#include <bf_pltfm_qsfp.h>

extern int platform_board_init();

// typedef struct pltfm_mgr_info_s { pthread_t tclserver_t_id; } pltfm_mgr_info_t;

/* platform specific initialization for the given platform
 * this function is called before Barefoot switch Asic intiailization
 * @return
 *   BF_PLTFM_SUCCESS on success and other values on error
 *   **This is a good place for driving "Test_core_tap_l" pin of Tofino-B0
 *   * low and hold low forever. However, this could be done anytime before this
 *   * as well (e.g. in some platform startup script).
 */
bf_status_t
bf_pltfm_platform_init(bf_switchd_context_t *switchd_ctx)
{
	int err = BF_PLTFM_SUCCESS;
	char *file_name;
	size_t revision_len = 0;
	char *revision_lower = NULL;
	assert(BF_PLTFM_SUCCESS == 0);

	if (switchd_ctx->sidecar_revision == NULL) {
		fprintf(stderr, "Sidecar board revision must be set");
		return BF_PLTFM_INVALID_ARG;
	}

	/*
	 * Take the lowercase value of the revision, which we use to construct the
	 * filename of the board-map.
	 */
	revision_len = strlen(switchd_ctx->sidecar_revision);
	revision_lower = calloc(revision_len + 1, 1);
	if (revision_lower == NULL) {
		return BF_PLTFM_OTHER;
	}
	for (size_t i = 0; i < revision_len; i++) {
		revision_lower[i] = tolower(switchd_ctx->sidecar_revision[i]);
	}

	if (asprintf(&file_name, "%s/share/platforms/board-maps/oxide/sidecar_rev_%s.csv",
		switchd_ctx->install_dir, revision_lower) == -1) {
		assert(file_name == NULL);
		return BF_PLTFM_OTHER;
	}

	err = platform_board_init(file_name);
	free(file_name);

	if (err == BF_PLTFM_SUCCESS) {
		if (bf_pltfm_qsfp_init(NULL)) {
			LOG_ERROR("Error in qsfp init \n");
			err |= BF_PLTFM_COMM_FAILED;
		}
	}

	return (err);
}

void
bf_pltfm_platform_exit(void *arg)
{
	(void) arg;
}


/* platform specific port subsystem initialization
 * this function i scalled after the Barefoot switch ASIC is initialized
 * and before the ports are initialized.
 * e.g., this function can implement any platforms specific operation
 * that may involve Barefoot switch Asic's support.
 */
bf_pltfm_status_t
bf_pltfm_platform_port_init(bf_dev_id_t dev_id, bool warm_init)
{
	return (BF_PLTFM_SUCCESS);
}

/* platform specific query
 * all real hardware platform should stub this to return true
 * @return
 *   BF_PLTFM_SUCCESS on success and other values on error
 */
bool
platform_is_hw(void)
{
	struct stat buf;

	return (stat("/dev/tofino", &buf) == 0);
}

bf_pltfm_status_t
bf_pltfm_device_type_get(bf_dev_id_t dev_id, bool *is_sw_model)
{
	*is_sw_model = false;
	return (BF_PLTFM_SUCCESS);
}

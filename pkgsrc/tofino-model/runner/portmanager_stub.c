/*
 * Stub implementation of portmanager for illumos.
 * Real network interface support would require DLPI implementation.
 */

#include "portmanager.h"
#include <stdio.h>

static bfm_packet_handler_vector_f packet_handler_vector;
static int port_count = 0;

void bfm_packet_handler_vector_set(bfm_packet_handler_vector_f fn) {
    packet_handler_vector = fn;
}

bfm_error_t bfm_port_init(int count) {
    port_count = count;
    fprintf(stderr, "portmanager: stub init (no real interface support on illumos)\n");
    return BFM_E_NONE;
}

bfm_error_t bfm_port_start_pkt_processing(void) {
    return BFM_E_NONE;
}

bfm_error_t bfm_port_finish(void) {
    return BFM_E_NONE;
}

bfm_error_t bfm_port_interface_add(const char *ifname, uint32_t port_num,
                                   const char *sw_name, int dump_pcap) {
    (void)ifname;
    (void)port_num;
    (void)sw_name;
    (void)dump_pcap;
    fprintf(stderr, "portmanager: interface add not supported on illumos\n");
    return BFM_E_NOT_SUPPORTED;
}

bfm_error_t bfm_port_interface_remove(const char *ifname) {
    (void)ifname;
    return BFM_E_NOT_SUPPORTED;
}

bfm_error_t bfm_port_packet_emit(uint32_t port_num, uint16_t queue_id,
                                  uint8_t *data, int len) {
    (void)port_num;
    (void)queue_id;
    (void)data;
    (void)len;
    return BFM_E_NOT_SUPPORTED;
}

void bfm_set_pcap_outdir(const char *outdir_name) {
    (void)outdir_name;
}

int bfm_get_port_count(void) {
    return port_count;
}

bool bfm_is_if_up(int port) {
    (void)port;
    return false;
}

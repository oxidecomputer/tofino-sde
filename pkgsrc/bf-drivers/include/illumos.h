#ifndef _HELIOS_H
#define _HELIOS_H

#include <sys/types.h>

#ifdef __sun
typedef uint32_t u_int32_t;
#endif

inline uint64_t
bswap_64(uint64_t value)
{
	__asm__("bswapq %0" : "+r" (value));
	return (value);
}

inline uint32_t
bswap_32(uint32_t x)
{
	__asm__("bswap %0" : "+r" (x));
	return (x);
}

inline uint16_t
bswap_16(uint16_t x)
{
	return (x >> 8 | ((x & 0xff) << 8));
}

#define CLOCK_MONOTONIC_RAW CLOCK_MONOTONIC

/*
 * All of the following should go into the helios tree with the tofino driver
 */

typedef uint64_t phys_addr_t;

#define BF_TBUS_MSIX_INDICES_MAX   3
#define BF_TBUS_MSIX_INDICES_MIN   1  

typedef struct {
	phys_addr_t phy_addr;
	void *dma_addr;
	size_t size;
} bf_dma_bus_map_t;

typedef struct {
	int cnt;
	int indices[BF_TBUS_MSIX_INDICES_MAX];
} bf_tbus_msix_indices_t;

enum bf_intr_mode {
	BF_INTR_MODE_NONE = 0,
	BF_INTR_MODE_LEGACY,
	BF_INTR_MODE_MSI,
	BF_INTR_MODE_MSIX,
};

typedef struct bf_intr_mode_s {
	enum bf_intr_mode intr_mode;
} bf_intr_mode_t;

#define TOC_IOC_PREFIX  0x1d1c
#define TOF_IOC(x) ((TOC_IOC_PREFIX << 16) | x)

/* Update truss */
#define BF_IOCMAPDMAADDR	TOF_IOC(0x0001)
#define BF_IOCUNMAPDMAADDR	TOF_IOC(0x0002)
#define BF_TBUS_MSIX_INDEX	TOF_IOC(0x0003)
#define BF_GET_INTR_MODE	TOF_IOC(0x0004)
#define BF_PKT_INIT		TOF_IOC(0x1000)
#define BF_GET_PCI_DEVID	TOF_IOC(0x1001)

#endif // _HELIOS_H

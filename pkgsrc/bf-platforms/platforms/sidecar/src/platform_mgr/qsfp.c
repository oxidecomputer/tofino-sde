#include <stdint.h>
#include <stdio.h>
#include <strings.h>
#include <bf_types/bf_types.h>
#include <bf_pltfm_bd_cfg.h>
#include <bf_bd_cfg/bf_bd_cfg_intf.h>
#include <bf_qsfp/bf_qsfp.h>
#include <bf_qsfp/sff.h>

static uint8_t num_ports;
#define MAX_QSFP 64
#define CPU_PORT (MAX_QSFP + 1)

#define QSFP_RESET 93
// from SFF-8636_R2.10a spec
#define QSFP_MEMORY_LEN 256
static uint8_t qsfp_initial_memory[QSFP_MEMORY_LEN] = {
	// byte 0
	0x0d,	// ID as QSFP+
	0x00,
	0x05,	// Single page / data-not-ready

	// byte 03
	// 19 bytes of interrupt flags
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00,

	// byte 22
	// 12 bytes of device monitors
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,

	// byte 34
	// 48 bytes of channel monitors
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

	// byte 82
	// 4 bytes reserved 
	0x00, 0x00, 0x00, 0x00,

	// byte 86
	// 12 bytes of control
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00,

	// byte 98
	// 2 bytes reserved
	0x00, 0x00,

	// byte 100
	// 7 bytes mask and channel masks
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

	// byte 107
	// 21 bytes reserved
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00,

	// Upper page data
	// byte 128
	0x0d, // QSFP+
	0x00,
	0x23, // "non-separable"
	
	// byte 131
	// 8 bytes of 'compliance'
	0x00, // all 40g except active
	0x00, // no SONET
	0x00, // no SAS, SATA
	0x00, // all gigabit eth
	0x00, 0x00, 0x00, 0x00, // no fiber channel

	// byte 139
	0x00, // unspecified encoding
	0x00, // bit rate unspecified
	0x00, // extended RateSelect

	// byte 142
	// optical length limits
	0x00, 0x00, 0x00, 0x00,

	// byte 146
	0x01, // copper length limit
	0xb0, // passive copper

	// byte 148
	// 16 bytes of vendor name
	'O', 'x', 'i', 'd', 'e', ' ', ' ', ' ',
	' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',

	// byte 164
	0x00, // infiniband
	0x00, 0x00, 0x00, // vendor OUI

	// byte 168
	// 16 bytes of 'part number'
	'f', 'a', 'k', 'e', ' ', 'c', 'a', 'b',
	'l', 'e', ' ', ' ', ' ', ' ', ' ', ' ',

	// byte 184
	'0', '1', // revision

	// byte 186
	0x00, 0x00,	// attenuation
	0x00, 0x00,	// tolerance
	0x00,		// max temp
	0x09, 		// checksum

	// byte 192
	// options
	0x00, 0x00, 0x00, 0x00,

	// byte 196
	// 16 bytes of 'serial number'
	'1', '2', '3', '4', '5', '6', '7', '8',
	' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',

	// byte 212
	// date code (3/3/22)
	'2', '2', '0', '3', '0', '3', 'a', 'b',

	// byte 220
	0x00,	// diagnostic monitoring
	0x00,	// extended options
	0x00, 	// checksum of 192-222

	// byte 223
	// 31 bytes of vendor data
	0x91, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static uint8_t *qsfp_memory;

static uint8_t *
qsfp_memory_addr(int module, int offset)
{
	if (module < 1 || module > num_ports) {
		return (NULL);
	}

	if (offset >= QSFP_MEMORY_LEN)
		return (NULL);

	return (qsfp_memory + QSFP_MEMORY_LEN * (module - 1) + offset);
}	

/* initialize the entire qsfp subsystem
 * it is presently invoked with arg = NULL
 * @return
 *   0 on success and -1 on error
 */
int
bf_pltfm_qsfp_init(void *arg)
{
	num_ports = platform_num_ports_get();
	// The actual number of QSFP ports is one fewer than the board map
	// would indicate, as one of the mapped ports is for the internal
	// CPU port.
	if (bf_bd_is_this_port_internal(CPU_PORT, 0)) {
		num_ports = num_ports - 1;
		printf("num_ports reduced to %d\n", num_ports);
	}
	bf_qsfp_set_num(num_ports);

	qsfp_memory = (uint8_t *)malloc(QSFP_MEMORY_LEN * num_ports);
	if (qsfp_memory == NULL) {
		return (-1);
	}

	for (int i = 1; i <= num_ports; i++)
		bcopy(qsfp_initial_memory, qsfp_memory_addr(i, 0), QSFP_MEMORY_LEN);

	return (0);
}

/** detect a qsfp module by attempting to read its memory offste zero
 *
 *  @param module
 *   module (1 based)
 *  @return
 *   true if present and false if not present
 */
bool
bf_pltfm_detect_qsfp(unsigned int module)
{
	return (module >= 1 && module <= MAX_QSFP);
}

/** read a qsfp module memory
 *
 *  @param module
 *   module (1 based)
 *  @param offset
 *   offset into qsfp memory space
 *  @param len
 *   len num of bytes to read
 *  @param buf
 *   buf to read into
 *  @return
 *   0 on success and -1 in error
 */
int
bf_pltfm_qsfp_read_module(unsigned int module, int offset, int len, uint8_t *buf)
{
	uint8_t *addr;

	addr = qsfp_memory_addr(module, offset);
	if (addr == NULL) {
		return (-1);
	}

	bcopy(addr, buf, len);
	return (0);
}

/** write a qsfp module memory
 *
 *  @param module
 *   module (1 based)
 *  @param offset
 *   offset into qsfp memory space
 *  @param len
 *   len num of bytes to write
 *  @param buf
 *   buf to read from
 *  @return
 *   0 on success and -1 in error
 */
int
bf_pltfm_qsfp_write_module(unsigned int module, int offset, int len, uint8_t *buf)
{
	uint8_t *addr;

	addr = qsfp_memory_addr(module, offset);
	if (addr == NULL) {
		return (-1);
	}

	bcopy(buf, addr, len);
	return (0);
}

/* read a qsfp  module register
 *  @param module
 *   module (1 based)
 *  @param page
 *   page that the register belongs to
 *  @param offset
 *   offset into qsfp memory space
 *  @param val
 *   val to read into
 *  @return
 *   0 on success and -1 in error
 */
int
bf_pltfm_qsfp_read_reg(unsigned int module, uint8_t page, int offset, uint8_t *val)
{
	return (-1);
}

/* write a qsfp  module register
 *  @param module
 *   module (1 based)
 *  @param page
 *   page that the register belongs to
 *  @param offset
 *   offset into qsfp memory space
 *  @param val
 *   val to write
 *  @return
 *   0 on success and -1 in error
 */
int
bf_pltfm_qsfp_write_reg(unsigned int module, uint8_t page, int offset, uint8_t val)
{
	return (-1);
}

/** get qsfp presence mask status
 *
 *  @param port_1_32_pres
 *   mask for lower 32 ports (1-32) 0: present, 1:not-present
 *  @param port_33_64_pres
 *   mask for upper 32 ports (33-64) 0: present, 1:not-present
 *  @param port_cpu_pres
 *   mask for cu port presence
 *  @return
 *   0 on success and -1 in error
 */
int
bf_pltfm_qsfp_get_presence_mask(uint32_t *port_1_32_pres, uint32_t *port_33_64_pres,
    uint32_t *port_cpu_pres)
{
	*port_1_32_pres = 0x00000000;  // ports 1-32 are present
	*port_33_64_pres = 0x00000000; // ports 33-64 are also present
	*port_cpu_pres = 0xffffffff;

	return (0);
}

/** get qsfp interrupt status
 *
 *  @param port_1_32_ints
 *   interrupt from lower 32 ports (1-32) 0: int-present, 1:not-present
 *  @param port_33_64_ints
 *   interrupt from upper 32 ports (33-64) 0: int-present, 1:not-present
 *  @param port_cpu_ints
 *   mask for cpu port interrupt
 */
void
bf_pltfm_qsfp_get_int_mask(uint32_t *port_1_32_ints, uint32_t *port_33_64_ints,
    uint32_t *port_cpu_ints)
{
	*port_1_32_ints = 0xffffffff;
	*port_33_64_ints = 0xffffffff;
	*port_cpu_ints = 0x0;
}


/** get qsfp lpmode status
 *
 *  @param port_1_32_lpmode_
 *   lpmode of lower 32 ports (1-32) 0: no-lpmod 1: lpmode
 *  @param port_33_64_ints
 *   lpmode of upper 32 ports (33-64) 0: no-lpmod 1: lpmode
 *  @param port_cpu_ints
 *   lpmode of cpu port
 *  @return
 *   0 on success and -1 in error
 */
int
bf_pltfm_qsfp_get_lpmode_mask(uint32_t *port_1_32_lpm, uint32_t *port_33_64_lpm,
    uint32_t *port_cpu_lpm)
{
	*port_1_32_lpm = 0;
	*port_33_64_lpm = 0;
	*port_cpu_lpm = 0;
	return (0);
}

/** set qsfp lpmode (hardware pins)
 *
 *  @param port
 *   port
 *  @param lpmode
 *   true : set lpmode, false : set no-lpmode
 *  @return
 *   0: success, -1: failure
 */
int
bf_pltfm_qsfp_set_lpmode(int port, bool lpmode)
{
	return (0);
}

/** reset qsfp module (hardware pins)
 *
 *  @param module
 *   module (1 based)
 *  @param reg
 *   syscpld register to write
 *  @param val
 *   value to write
 *  @return
 *   0 on success and -1 in error
 */
int
bf_pltfm_qsfp_module_reset(int module, bool reset)
{
	return (0);
}

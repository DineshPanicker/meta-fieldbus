/* modbus_uapi.h -- the ABI between the modbus serdev driver and userspace.
 *
 * One read() on /dev/modbus0 returns exactly one struct modbus_record:
 * a complete RTU frame whose CRC the driver has already checked, plus the
 * kernel timestamp taken when the t3.5 silence timer closed the frame.
 *
 * Plain C, kernel __u types, natural alignment, no __attribute__((packed)).
 * The kernel driver and every userspace master include this one file, so
 * the two sides cannot disagree about the layout.
 *
 *   offset  0  __u64 ts_ns       8 bytes
 *   offset  8  __u32 len         4 bytes
 *   offset 12  __u8  data[256] 256 bytes
 *   offset 268 (4 bytes tail padding, so sizeof is a multiple of 8)
 *   size     272
 *
 * Because of that padding the driver must zero the whole record (memset or
 * = {}) before filling it and copying it to userspace; otherwise the 4
 * padding bytes leak whatever was on the kernel stack.
 */
#ifndef MODBUS_UAPI_H
#define MODBUS_UAPI_H

#include <linux/types.h>

#define MODBUS_MAX_ADU 256

struct modbus_record {
	__u64 ts_ns;                 /* ktime_get_ns() when t3.5 expired */
	__u32 len;                   /* valid bytes in data[]            */
	__u8  data[MODBUS_MAX_ADU];  /* one complete RTU frame, CRC included */
};

#endif /* MODBUS_UAPI_H */

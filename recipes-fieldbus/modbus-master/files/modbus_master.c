/* modbus_master.c -- 100 Hz SCHED_DEADLINE Modbus RTU scan-cycle master.
 * No external library dependencies: GPIO27 cycle marker driven via the
 * kernel sysfs GPIO interface (/sys/class/gpio). This keeps the recipe
 * free of libgpiod/meta-oe entirely.
 *
 * STATUS: written and code-reviewed; not yet run on hardware (bus solder
 * joint failed before the first exchange). Validate once repaired.
 *
 * Scan cycle:
 *   wake at absolute period boundary -> raise GPIO27 (cycle marker) ->
 *   write 0x03 read request to /dev/modbus0 -> block on read() for the
 *   validated response -> lower GPIO27 -> sleep to next boundary.
 *
 * The GPIO27 marker is the master-side timing signal for the logic
 * analyzer: successive rising edges give the true scan period; the high
 * time gives request->response round-trip latency.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_NS   (10L * 1000L * 1000L)   /* 10 ms -> 100 Hz */
#define N_CYCLES    6000                    /* 1 minute at 100 Hz */
#define CYCLE_GPIO  "27"

/* SCHED_DEADLINE is not exposed by glibc; declare the ABI ourselves. */
struct sched_attr {
	__u32 size, sched_policy;
	__u64 sched_flags;
	__s32 sched_nice;
	__u32 sched_priority;
	__u64 sched_runtime, sched_deadline, sched_period;
};
#ifndef SCHED_DEADLINE
#define SCHED_DEADLINE 6
#endif

static int sched_setattr_(pid_t pid, const struct sched_attr *a,
			  unsigned int flags)
{
	return syscall(SYS_sched_setattr, pid, a, flags);
}

static uint16_t modbus_crc16(const uint8_t *buf, size_t len)
{
	uint16_t crc = 0xFFFF;
	for (size_t i = 0; i < len; i++) {
		crc ^= buf[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
	}
	return crc;
}

/* Must match struct modbus_record in the kernel driver. */
struct modbus_record {
	uint64_t ts_ns;
	uint16_t len;
	uint8_t  data[256];
} __attribute__((packed));

/* --- sysfs GPIO helpers (legacy /sys/class/gpio interface) --- */

static int gpio_write_file(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	ssize_t n = write(fd, val, strlen(val));
	close(fd);
	return (n < 0) ? -1 : 0;
}

/* Export GPIO27, set direction out. Returns an open fd to its value file
 * (kept open for the whole run so each toggle is a single fast write). */
static int gpio_setup(void)
{
	/* Export (ignore EBUSY if already exported from a previous run). */
	gpio_write_file("/sys/class/gpio/export", CYCLE_GPIO);
	/* Give udev a moment to create the node + set permissions. */
	usleep(100000);

	if (gpio_write_file("/sys/class/gpio/gpio" CYCLE_GPIO "/direction",
			    "out") < 0) {
		perror("gpio direction");
		return -1;
	}

	int fd = open("/sys/class/gpio/gpio" CYCLE_GPIO "/value", O_WRONLY);
	if (fd < 0)
		perror("gpio value open");
	return fd;
}

static inline void gpio_set(int fd, int high)
{
	/* pwrite to offset 0 each time; single-byte value file. */
	(void)pwrite(fd, high ? "1" : "0", 1, 0);
}

static void gpio_teardown(int fd)
{
	if (fd >= 0)
		close(fd);
	gpio_write_file("/sys/class/gpio/unexport", CYCLE_GPIO);
}

int main(void)
{
	/* Lock all memory: no page faults in the RT loop. */
	if (mlockall(MCL_CURRENT | MCL_FUTURE)) {
		perror("mlockall");
		return 1;
	}

	struct sched_attr attr = {0};
	attr.size           = sizeof(attr);
	attr.sched_policy   = SCHED_DEADLINE;
	attr.sched_runtime  =  3 * 1000 * 1000;   /* 3 ms CPU budget / period  */
	attr.sched_deadline =  8 * 1000 * 1000;   /* must finish within 8 ms   */
	attr.sched_period   = 10 * 1000 * 1000;   /* released every 10 ms      */
	if (sched_setattr_(0, &attr, 0)) {
		perror("sched_setattr (need root; RT throttling off?)");
		return 1;
	}

	int fd = open("/dev/modbus0", O_RDWR);
	if (fd < 0) { perror("open /dev/modbus0"); return 1; }

	int gpio_fd = gpio_setup();
	if (gpio_fd < 0) { close(fd); return 1; }

	/* Fixed request: read 4 holding registers from address 0.
	 * 01 03 00 00 00 04 <crc-lo> <crc-hi> */
	uint8_t reqbuf[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x04, 0, 0};
	uint16_t crc = modbus_crc16(reqbuf, 6);
	reqbuf[6] = crc & 0xFF;
	reqbuf[7] = crc >> 8;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	int read_errors = 0;

	for (int i = 0; i < N_CYCLES; i++) {
		/* Absolute-time wakeup: compute next boundary, sleep to it.
		 * Absolute (not relative) prevents drift accumulation. */
		next.tv_nsec += PERIOD_NS;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec  += 1;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		gpio_set(gpio_fd, 1);                  /* T_cycle_start */

		if (write(fd, reqbuf, sizeof(reqbuf)) != (ssize_t)sizeof(reqbuf))
			perror("write");

		struct modbus_record rec;
		ssize_t n = read(fd, &rec, sizeof(rec));  /* blocks on kfifo */
		if (n < 0)
			read_errors++;

		gpio_set(gpio_fd, 0);                  /* T_cycle_end */
	}

	printf("done: %d cycles, %d read errors\n", N_CYCLES, read_errors);

	gpio_teardown(gpio_fd);
	close(fd);
	return 0;
}

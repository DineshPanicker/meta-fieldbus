/* modbus_master.c -- 100 Hz SCHED_DEADLINE Modbus RTU scan-cycle master.
 * libgpiod v2 API (for libgpiod >= 2.0, e.g. 2.2.2 from meta-oe walnascar).
 *
 * STATUS: written and code-reviewed; not yet run on hardware (bus solder
 * joint failed before the first exchange). Validate once repaired.
 *
 * Replicates a classical PLC scan cycle:
 *   wake at absolute period boundary -> raise GPIO27 (cycle marker) ->
 *   write 0x03 read request to /dev/modbus0 -> block on read() for the
 *   validated response frame -> lower GPIO27 -> sleep to next boundary.
 *
 * The GPIO27 marker is the master-side timing signal for the logic
 * analyzer: successive rising edges measure the true scan period, and the
 * high-time measures request->response round-trip latency.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <gpiod.h>
#include <linux/sched.h>
#include <linux/types.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_NS (10L * 1000L * 1000L) /* 10 ms -> 100 Hz */
#define N_CYCLES 6000					/* 1 minute at 100 Hz */
#define CYCLE_GPIO 27

/* SCHED_DEADLINE is not exposed by glibc; declare the ABI ourselves. */
struct sched_attr
{
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
	for (size_t i = 0; i < len; i++)
	{
		crc ^= buf[i];
		for (int b = 0; b < 8; b++)
			crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
	}
	return crc;
}

/* Must match struct modbus_record in the kernel driver. */
struct modbus_record
{
	uint64_t ts_ns;
	uint16_t len;
	uint8_t data[256];
} __attribute__((packed));

int main(void)
{
	/* Lock all memory: no page faults in the RT loop. */
	if (mlockall(MCL_CURRENT | MCL_FUTURE))
	{
		perror("mlockall");
		return 1;
	}

	struct sched_attr attr = {0};
	attr.size = sizeof(attr);
	attr.sched_policy = SCHED_DEADLINE;
	attr.sched_runtime = 3 * 1000 * 1000;  /* 3 ms CPU budget / period  */
	attr.sched_deadline = 8 * 1000 * 1000; /* must finish within 8 ms   */
	attr.sched_period = 10 * 1000 * 1000;  /* released every 10 ms      */
	if (sched_setattr_(0, &attr, 0))
	{
		perror("sched_setattr (need root; RT throttling off?)");
		return 1;
	}

	int fd = open("/dev/modbus0", O_RDWR);
	if (fd < 0)
	{
		perror("open /dev/modbus0");
		return 1;
	}

	/* --- libgpiod v2: acquire GPIO27 as an output line --- */
	struct gpiod_chip *chip = gpiod_chip_open("/dev/gpiochip0");
	if (!chip)
	{
		perror("gpiod_chip_open");
		return 1;
	}

	struct gpiod_line_settings *settings = gpiod_line_settings_new();
	gpiod_line_settings_set_direction(settings,
									  GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings,
										 GPIOD_LINE_VALUE_INACTIVE);

	struct gpiod_line_config *line_cfg = gpiod_line_config_new();
	unsigned int offset = CYCLE_GPIO;
	gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);

	struct gpiod_request_config *req_cfg = gpiod_request_config_new();
	gpiod_request_config_set_consumer(req_cfg, "modbus-cycle");

	struct gpiod_line_request *req =
		gpiod_chip_request_lines(chip, req_cfg, line_cfg);
	if (!req)
	{
		perror("gpiod_chip_request_lines");
		return 1;
	}

	/* Fixed request: read 4 holding registers from address 0.
	 * 01 03 00 00 00 04 <crc-lo> <crc-hi> */
	uint8_t reqbuf[8] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x04, 0, 0};
	uint16_t crc = modbus_crc16(reqbuf, 6);
	reqbuf[6] = crc & 0xFF;
	reqbuf[7] = crc >> 8;

	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	int read_errors = 0;

	for (int i = 0; i < N_CYCLES; i++)
	{
		/* Absolute-time wakeup: compute next boundary, sleep to it.
		 * Absolute (not relative) sleep prevents drift accumulation. */
		next.tv_nsec += PERIOD_NS;
		while (next.tv_nsec >= 1000000000L)
		{
			next.tv_nsec -= 1000000000L;
			next.tv_sec += 1;
		}
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);

		gpiod_line_request_set_value(req, CYCLE_GPIO,
									 GPIOD_LINE_VALUE_ACTIVE); /* start */

		if (write(fd, reqbuf, sizeof(reqbuf)) != (ssize_t)sizeof(reqbuf))
			perror("write");

		struct modbus_record rec;
		ssize_t n = read(fd, &rec, sizeof(rec)); /* blocks on kfifo */
		if (n < 0)
			read_errors++;

		gpiod_line_request_set_value(req, CYCLE_GPIO,
									 GPIOD_LINE_VALUE_INACTIVE); /* end */
	}

	printf("done: %d cycles, %d read errors\n", N_CYCLES, read_errors);

	gpiod_line_request_release(req);
	gpiod_request_config_free(req_cfg);
	gpiod_line_config_free(line_cfg);
	gpiod_line_settings_free(settings);
	gpiod_chip_close(chip);
	close(fd);
	return 0;
}
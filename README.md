# meta-fieldbus

Yocto meta-layer for a deterministic Modbus RTU fieldbus on PREEMPT_RT.

## What this builds
- PREEMPT_RT 6.12 kernel (raspberry Pi 3B+)
- In-kernel serdev Modbus RTU driver (/dev/modbus0)
  - hrtimer-based 1.75ms inter-frame silence (fixed, per spec >19200 baud)
  - CRC16 validation, kfifo buffering, sysfs stats
- Device tree overlay binding uart0 to the driver
- SCHED_DEADLINE 100Hz scan-cycle master (modbus_master)

## Hardware
- Raspberry Pi 3B+ (master)
- STM32 Nucleo-F446RE (bare-metal slave, USART1)
- RS485 auto-direction modules x2
- 24MHz logic analyzer for wire-level jitter measurement

## Layer dependencies
- poky (walnascar)
- meta-raspberrypi (walnascar)

## Build
See docs/ for full setup instructions.

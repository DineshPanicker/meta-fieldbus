# Resume point

## Done + hardware-validated
- Kernel driver (4a-4d): serdev bind, hrtimer 1.75ms t3.5, CRC16, kfifo,
  blocking read, sysfs frames_ok/crc_errors. Loopback-tested: good frame
  increments frames_ok, bad CRC increments crc_errors.

## Written, not yet run on hardware
- STM32 slave (modbus_slave/): function 0x03, USART1 PA9/PA10, PA8 marker.
- Master (recipes-fieldbus/): SCHED_DEADLINE 100Hz, sysfs-GPIO27 marker.
  Expected wire response: 01 03 08 00 64 00 C8 01 2C 01 90 90 08

## Blocked / next actions
1. RE-SOLDER the RS485 bus joint (this is what stopped integration).
2. Flash STM32 slave, load driver, run first real exchange, watch frames_ok.
3. Compile master: pseudo bug (host kernel 6.17) blocks Yocto do_install.
   Cross-compile needs a sysroot with libc + (if libgpiod version) gpiod.h,
   OR use the sysfs-GPIO master (zero deps) OR compile natively on the Pi.
4. Measurement campaign: logic analyzer, idle/stress-ng/isolcpus, histograms.

## Known host issue
pseudo fails do_install (chown: Invalid argument) on host kernel 6.17.
Real fix = containerized build. Workaround = SCP compiled artifacts.

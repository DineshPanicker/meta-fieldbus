# meta-fieldbus

A **deterministic Modbus RTU fieldbus on PREEMPT_RT**, built end to end: a Linux
kernel serdev driver doing spec-compliant RTU frame detection, a Yocto meta-layer
that bakes it into a self-contained image, a bare-metal STM32 slave, and a
SCHED_DEADLINE scan-cycle master — measured at the wire with a logic analyzer.

**Status: working on hardware.** The Pi master and STM32 slave exchange validated
frames over real RS485 — correct register values, zero CRC errors. The driver,
overlay, and master are packaged into the image and the driver auto-loads at boot.

---

## What this builds

- **PREEMPT_RT 6.12 kernel** for the Raspberry Pi 3B+, built from source with Yocto
- **In-kernel serdev Modbus RTU driver** exposing `/dev/modbus0`
  - hrtimer-based 1.75 ms inter-frame silence (t3.5, fixed per spec above 19200 baud)
  - CRC16 validation, kfifo buffering, blocking `read()`, sysfs statistics
- **Device-tree overlay** binding `uart0` to the driver via a private compatible string
- **SCHED_DEADLINE 100 Hz scan-cycle master** (`modbus_master`) with a sysfs-GPIO
  cycle marker for logic-analyzer measurement
- **Bare-metal STM32 slave** (separate CubeIDE project under `modbus_slave/`)

## Hardware

| Component | Role |
|---|---|
| Raspberry Pi 3B+ | Modbus RTU master (Linux, PREEMPT_RT) |
| STM32 Nucleo-F446RE | Bare-metal Modbus slave (USART1, PA9/PA10) |
| 2 × RS485-to-TTL auto-direction modules | Bus transceivers |
| 24 MHz 8-channel logic analyzer | Wire-level jitter measurement |
| USB-TTL adapter (CP2102/CH340) | Serial console for early boot |

## Layer dependencies

- poky (**scarthgap**, LTS)
- meta-raspberrypi (**scarthgap**)

Build host: Debian 13 (trixie), kernel 6.12. (Host kernel matters — see
TROUBLESHOOTING.md, the pseudo/host-kernel section.)

---

## 1. Host setup (Debian)

```bash
sudo apt update
sudo apt install -y gawk wget git diffstat unzip texinfo gcc build-essential \
    chrpath socat cpio python3 python3-pip python3-pexpect xz-utils \
    debianutils iputils-ping python3-git python3-jinja2 python3-subunit \
    zstd lz4 file locales libacl1
sudo locale-gen en_US.UTF-8

# Yocto needs /bin/sh = bash, not dash:
sudo dpkg-reconfigure dash        # answer NO

# Locale must be exported before sourcing the build env:
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
```

## 2. Clone and assemble the tree

```bash
mkdir -p ~/fieldbus && cd ~/fieldbus
git clone -b scarthgap git://git.yoctoproject.org/poky
git clone -b scarthgap https://github.com/agherzan/meta-raspberrypi
git clone https://github.com/DineshPanicker/meta-fieldbus
```

## 3. Init the build environment and add layers

```bash
cd ~/fieldbus
source poky/oe-init-build-env build_fieldbus
bitbake-layers add-layer ../meta-raspberrypi
bitbake-layers add-layer ../meta-fieldbus
bitbake-layers show-layers        # confirm core, raspberrypi, meta-fieldbus
```

## 4. Configure `conf/local.conf`

Append:

```
MACHINE = "raspberrypi3-64"
PREFERRED_VERSION_linux-raspberrypi = "6.12%"
ENABLE_UART = "1"
RPI_EXTRA_CONFIG = "dtoverlay=disable-bt\ndtoverlay=modbus"
IMAGE_BOOT_FILES:append = " modbus.dtbo;overlays/modbus.dtbo"
LICENSE_FLAGS_ACCEPTED += "synaptics-killswitch"
IMAGE_INSTALL:append = " linux-firmware-rpidistro-bcm43455 wireless-regdb wpa-supplicant kernel-modules modbus-drv modbus-master"
CMDLINE:append = " dwc_otg.fiq_enable=0 dwc_otg.fiq_fsm_enable=0 dwc_otg.microframe_schedule=0"
EXTRA_IMAGE_FEATURES = "ssh-server-openssh allow-root-login"
BB_NUMBER_THREADS = "6"
PARALLEL_MAKE = "-j 6"
```

WiFi credentials live in `recipes-core/images/core-image-minimal.bbappend`
(`setup_wifi`) and are **gitignored** — set your own SSID/PSK there.

## 5. Verify PREEMPT_RT applies (before the full image)

```bash
cd ~/fieldbus/build_fieldbus
bitbake linux-raspberrypi -c cleansstate && bitbake linux-raspberrypi
grep -E "^CONFIG_PREEMPT_RT" tmp/work/*/linux-raspberrypi/*/linux-*-build/.config
# must print: CONFIG_PREEMPT_RT=y
```

## 6. Build the image

```bash
bitbake core-image-minimal
```

---

## 7. Flash the SD card (bmaptool)

Identify the card first — **flash the whole device, never a partition**, and confirm
you have the right one:

```bash
lsblk                              # find the SD card (e.g. /dev/sdc)
lsblk -o NAME,SIZE,LABEL,MOUNTPOINTS /dev/sdc
```

Unmount any auto-mounted partitions (the Debian desktop auto-mounts on insert):

```bash
udisksctl unmount -b /dev/sdc1 2>/dev/null
udisksctl unmount -b /dev/sdc2 2>/dev/null
lsblk /dev/sdc                     # confirm nothing mounted
```

Flash:

```bash
cd ~/fieldbus/build_fieldbus/tmp/deploy/images/raspberrypi3-64
sudo bmaptool copy core-image-minimal-raspberrypi3-64.rootfs.wic.bz2 /dev/sdc
sync
```

If `bmaptool` is missing: `sudo apt install -y bmap-tools`.
Plain `dd` fallback:

```bash
bzcat core-image-minimal-raspberrypi3-64.rootfs.wic.bz2 | sudo dd of=/dev/sdc bs=4M status=progress
sync
```

---

## 8. Serial console (picocom) — for early boot / no-network debugging

The Pi's kernel console is on the UART (`ttyAMA0`). Wire a USB-TTL adapter:

```
CP2102 RXD  ->  Pi GPIO14 / TXD (pin 8)
CP2102 TXD  ->  Pi GPIO15 / RXD (pin 10)
CP2102 GND  ->  Pi GND (pin 6)      # do NOT connect VCC
```

Connect:

```bash
sudo picocom -b 115200 /dev/ttyUSB0
```

Exit: `Ctrl+A` then `Ctrl+X`. Find the device with `ls /dev/ttyUSB*` or
`dmesg | grep ttyUSB`. To drop the `sudo`: `sudo usermod -a -G dialout $USER`
then log out/in.

Note: once the serdev driver claims `ttyAMA0` (via the `modbus@0` overlay node),
there is no serial login prompt — that UART belongs to the fieldbus. picocom is
for watching the boot log; **SSH over WiFi is the working console.**

---

## 9. First boot & SSH

Find the Pi on the network (Huawei routers use `192.168.8.x`):

```bash
sudo nmap -sn 192.168.8.0/24 | grep -B2 -i raspberry
```

SSH in as **root** (password `root`, or empty if using debug-tweaks):

```bash
ssh root@192.168.8.10
```

If login is refused, patch the card's shadow/sshd_config directly (see
TROUBLESHOOTING.md). To kill the SSH reverse-DNS lag on some networks, the image
already sets `UseDNS no`.

---

## 10. Verify the stack after boot

```bash
uname -a                                    # PREEMPT_RT, 6.12.93-v8
lsmod | grep modbus                         # modbus_drv auto-loaded from image
ls -l /dev/modbus0                          # char device, major 10, present at boot
dmesg | grep modbus                         # "ready on ..., t3.5=1.75ms"
ls /proc/device-tree/soc/serial@7e201000/modbus@0/   # overlay applied
which modbus_master                         # master binary in the image
cat /sys/class/misc/modbus0/frames_ok       # 0
```

---

## 11. Hardware connections (Pi <-> RS485 <-> STM32)

**Pi 3B+ -> Module 1**

| Pi pin | Module 1 TTL |
|---|---|
| Pin 1 (3.3V) | VCC |
| Pin 8 (GPIO14/TXD) | TXD |
| Pin 10 (GPIO15/RXD) | RXD |
| Pin 6 (GND) | GND |

**Nucleo-F446RE -> Module 2**

| Nucleo pin | Module 2 TTL |
|---|---|
| 3V3 | VCC |
| PA9 (D8) | TXD |
| PA10 (D2) | RXD |
| GND | GND |

**RS485 bus (module to module):** A+↔A+, B−↔B−, GND↔GND (twist the A/B pair).

Before powering on: **remove the loopback jumper** (GPIO14→GPIO15 from the driver
self-test), continuity-test the bus (A+↔A+ beeps, A+↔B− does not), and power each
module from its own board's 3.3V.

---

## 12. The exchange (manual test)

On the Pi, with the STM32 flashed and the bus wired:

```bash
cat /sys/class/misc/modbus0/frames_ok       # baseline 0
cat /sys/class/misc/modbus0/crc_errors      # baseline 0

# send a read-holding-registers request: 01 03 00 00 00 04 + CRC(44 09)
printf '\x01\x03\x00\x00\x00\x04\x44\x09' > /dev/modbus0
sleep 1

cat /sys/class/misc/modbus0/frames_ok       # -> 1 (climbs per request)
cat /sys/class/misc/modbus0/crc_errors      # -> 0
```

See the actual response bytes:

```bash
( dd if=/dev/modbus0 bs=262 count=1 2>/dev/null | od -x ) &
sleep 0.3
printf '\x01\x03\x00\x00\x00\x04\x44\x09' > /dev/modbus0
sleep 1
```

**Expected response** (after the 8-byte record header): `01 03 08 00 64 00 c8 01 2c 01 90 90 08`
— function 0x03, 8 data bytes, registers **100, 200, 300, 400**, CRC `90 08`.

Diagnostics:
- `frames_ok` climbs, `crc_errors` 0 → **working.**
- `crc_errors` climbs, `frames_ok` stuck → garbled bytes: baud mismatch or wiring/solder.
- neither moves → no response: STM32 not flashed/powered, or A/B swapped.

---

## 13. Run the SCHED_DEADLINE master

```bash
modbus_master                               # 100 Hz scan cycle, needs root
cat /sys/class/misc/modbus0/frames_ok       # climbs by ~6000 over one run
```

The master toggles GPIO27 at cycle start/end as the logic-analyzer cycle marker.

---

## 14. STM32 slave (build & flash)

The slave is a standalone STM32CubeIDE project under `modbus_slave/`.

- **Import:** File → Import → Existing Projects into Workspace → select `modbus_slave/`
- **Build:** hammer icon (0 errors expected)
- **Flash:** green run arrow (ST-LINK over USB)

Command-line flash alternative:

```bash
sudo apt install -y stlink-tools
st-info --probe                             # confirm the Nucleo is detected
st-flash --format ihex write modbus_slave/Debug/modbus_slave.hex
```

Slave config: USART1 (PA9/PA10) 115200 8N1, function 0x03, 8 holding registers
(100/200/300/400/...), PA8 pulsed high while responding (logic-analyzer marker).

---

## 15. Fast driver iteration (SCP loop, no image rebuild)

For editing the driver without reflashing:

```bash
# on the host
cd ~/fieldbus/build_fieldbus
bitbake modbus-drv -c compile -f
KO=$(find tmp/work/*/modbus-drv/ -name modbus_drv.ko | head -1)
scp "$KO" root@192.168.8.10:/tmp/

# on the Pi
rmmod modbus_drv 2>/dev/null
insmod /tmp/modbus_drv.ko
dmesg | tail -2
```

---

## Repository layout

```
meta-fieldbus/
├── conf/layer.conf
├── recipes-kernel/
│   ├── linux/                     RT config fragment + bbappend
│   └── modbus-drv/                the serdev driver + recipe
├── recipes-bsp/
│   └── modbus-overlay/            device-tree overlay + recipe
├── recipes-fieldbus/
│   └── modbus-master/             SCHED_DEADLINE master + recipe
├── recipes-core/images/           image bbappend (wifi, ssh, root — gitignored creds)
├── modbus_slave/                  STM32CubeIDE bare-metal slave project
├── TROUBLESHOOTING.md             every bug hit and its root cause
└── NEXT_STEPS.md                  resume point
```

## Design notes (interview material)

- **Private compatible string** (`dinesh,modbus-rtu`): deliberately not upstreamable
  — frame semantics belong in userspace/line-discipline for the general case; this
  driver trades generality for deterministic in-kernel timestamping.
- **Fixed t3.5 = 1.75 ms above 19200 baud** (spec, not proportional). A naive
  proportional value at 115200 (~305 µs) would be non-compliant.
- **kfifo + wait queue** bridge interrupt context (frame completion) to process
  context (`read()`) — lockless in the fast path for single-producer/single-consumer.

See TROUBLESHOOTING.md for the full engineering log (dwc_otg FIQ vs PREEMPT_RT,
serdev consume-contract, the pseudo/host-kernel saga, and the scarthgap recipe
migration).
# TROUBLESHOOTING

Field notes from bringing up `meta-fieldbus` — a PREEMPT_RT Modbus RTU fieldbus
on a Raspberry Pi 3B+ under Yocto (walnascar / 5.2). Each entry records the
symptom as it actually appeared, the root cause once found, the fix, and the
general lesson. Grouped by the layer the problem lived in.

Environment for reference:
- Host: Ubuntu, kernel 6.17, Yocto walnascar (5.2), poky + meta-raspberrypi
- Target: Raspberry Pi 3B+ (BCM2837 SoC, BCM43455 WiFi), kernel 6.12.25-v8 PREEMPT_RT
- Access: WiFi + SSH (serial console is architecturally unavailable — see BOOT-3)

---

## Build system (Yocto)

### YOCTO-1 — PREEMPT_RT config fragment appeared not to apply

**Symptom.** After adding `rt.cfg` with `CONFIG_PREEMPT_RT=y` and rebuilding, the
deployed `.config` still showed `CONFIG_PREEMPT_NONE=y` and no `CONFIG_PREEMPT_RT`
line at all.

**Root cause — three layers, peeled back in order:**
1. The kernel was never actually rebuilt. BitBake saw the recipe as done and
   skipped it, so the fragment was never merged.
2. `CONFIG_PREEMPT_RT` is a Kconfig *choice* item. Even once merged, `olddefconfig`
   resolved the choice back to the pre-existing `PREEMPT_NONE` selection because the
   competing options were never explicitly unset.
3. The real culprit: Yocto was building **kernel 6.6**, not 6.12. `CONFIG_PREEMPT_RT`
   only exists in the mainline Raspberry Pi kernel from 6.12 onward, so on 6.6 the
   option simply does not exist and the fragment line was silently dropped.

**Fix.**
```
# local.conf — force the 6.12 kernel
PREFERRED_VERSION_linux-raspberrypi = "6.12%"
```
```
# rt.cfg — explicitly unset the competing choice items
CONFIG_EXPERT=y
# CONFIG_PREEMPT_NONE is not set
# CONFIG_PREEMPT_VOLUNTARY is not set
# CONFIG_PREEMPT is not set
CONFIG_PREEMPT_RT=y
```
```bash
# force a genuine rebuild — sstate must be cleared or configure won't re-run
bitbake linux-raspberrypi -c cleansstate && bitbake linux-raspberrypi
grep -E "^CONFIG_PREEMPT" tmp/work/*/linux-raspberrypi/*/linux-*-build/.config
```

**Diagnostic that cracked it.** Reading the *actual work-directory path* in the
`do_kernel_metadata` log — it said `.../linux-raspberrypi/6.6.63+git/...`, which
immediately revealed the wrong kernel version. The fragment was fine all along.

**Lesson.** When a config silently doesn't apply, verify **what is actually being
built** before assuming the fragment is wrong. Build systems fail quietly; confirm
ground truth (the deployed `.config`, the work-dir path) rather than trusting that
a change took effect.

---

### YOCTO-2 — recipe and parse errors (a cluster of walnascar-specific rules)

Several small, independent failures, each with a one-line rule behind it:

- **`.bb` placed inside `files/`** → not found. The default `BBFILES` glob is
  `recipes-*/*/*.bb` — exactly three levels. A recipe one level deeper (inside
  `files/`) is invisible. *Fix: recipe sits in the recipe dir, only source/config
  goes in `files/`.*

- **Heredoc inside a BitBake shell function** → `unparsed line: 'WPAEOF'`. BitBake's
  parser reads the heredoc terminator as a BitBake statement. *Fix: use `printf`
  with `\n` escapes instead of a heredoc.*

- **Shell function defined in `local.conf`** → `unparsed line: 'setup_wifi() {'`.
  `local.conf` accepts only `VAR = "value"` assignments, not function definitions.
  *Fix: move the function into a `.bbappend`, which is parsed as a recipe.*

**Lesson.** Yocto errors are almost always about *where a thing lives* or *how it
is parsed*, not what it contains. Match the file type to the content: config vars
in `.conf`, functions and tasks in `.bb`/`.bbappend`.

---

### YOCTO-3 — WORKDIR / UNPACKDIR migration (walnascar / 5.2)

**Symptom.** `do_install: cannot stat '.../wpa_supplicant.conf': No such file or
directory`, despite the file being in `SRC_URI`.

**Root cause.** Yocto 5.1+ moved unpacked `SRC_URI` files out of `${WORKDIR}` into
a dedicated `${UNPACKDIR}`. Recipes referencing `${WORKDIR}/file` no longer find
their source; `S = "${WORKDIR}"` is now an outright error.

**Fix.** Reference `${UNPACKDIR}` for `SRC_URI` files; if a recipe set
`S = "${WORKDIR}"`, change to:
```
S = "${WORKDIR}/sources"
UNPACKDIR = "${S}"
```

**Lesson.** Read the migration guide for the *exact* release you are on. Examples
written for older releases will use patterns that now hard-error.

---

### YOCTO-4 — dnf file-ownership conflicts

**Symptom.** `do_rootfs` failed twice with
`file /etc/... conflicts between attempted installs of A and B`:
first `/etc/wpa_supplicant.conf` (custom `wifi-config` vs `wpa-supplicant`),
then `/etc/network/interfaces` (custom recipe vs `init-ifupdown`).

**Root cause.** Two packages cannot own the same file. Any custom recipe that
writes to a path already owned by a base package collides at rootfs assembly.

**Fix.** Abandon the separate package. Write the files with
`ROOTFS_POSTPROCESS_COMMAND`, which runs *after* all packages are installed and so
answers to no package-ownership arbitration:
```
ROOTFS_POSTPROCESS_COMMAND += "setup_wifi;"
setup_wifi() {
    printf '...' > ${IMAGE_ROOTFS}/etc/wpa_supplicant.conf
    chmod 0600 ${IMAGE_ROOTFS}/etc/wpa_supplicant.conf
}
```

**Lesson.** `ROOTFS_POSTPROCESS_COMMAND` is the escape hatch whenever package-level
file ownership fights you — it edits the assembled rootfs directly.

---

### YOCTO-5 — pseudo fails on new host kernel (OPEN / worked around)

**Symptom.** `do_package` fails:
```
got *at() syscall for unknown directory, fd 4
couldn't allocate absolute path for 'modules'
tar: ./lib/modules: Cannot mkdir: Bad address
```
Compile (`do_compile`) succeeds; only packaging fails.

**Root cause.** `pseudo` (Yocto's fakeroot) intercepts filesystem syscalls via
`LD_PRELOAD`. The host runs kernel 6.17, whose glibc emits `openat2()`-family
syscall variants the walnascar-era pseudo binary does not decode. Pseudo cannot
map the returned fd back to a path and returns `EFAULT`.

**Attempted and did NOT help.** `PSEUDO_IGNORE_PATHS`, `PSEUDO_PATHS_CHECK = "0"`,
`bitbake -c clean`, `bitbake pseudo-native -c cleansstate`. Config cannot fix a
binary that mis-handles a syscall.

**Workaround (chosen).** The problem sits entirely off the critical path:
- All driver development uses `bitbake modbus-drv -c compile -f` (compile only —
  never invokes pseudo) plus `scp` of the `.ko` to the target. This is the normal
  kernel-development loop anyway.
- Packaging into the final image is needed exactly once, at the end, and can be
  routed around with a `ROOTFS_POSTPROCESS_COMMAND` that copies the pre-built `.ko`
  into the rootfs — same mechanism already used for WiFi and root-unlock.

**Real fixes (deferred).** Rebuild pseudo from patched source, or run the build in
a container with an older-kernel userspace.

**Lesson — the most important in this document.** Distinguish a *blocker* from a
*nuisance*. This looked fatal but costs nothing until the final image. Knowing what
you can safely defer is as valuable as knowing how to fix things.

---

## Boot and hardware

### BOOT-1 — dwc_otg RCU stall: unrecoverable hang under PREEMPT_RT

**Symptom.** Boot reaches userspace, then hard-hangs. Serial log shows:
```
WARNING: ... rcu_note_context_switch ...
  disable_irq -> local_fiq_disable -> dwc_otg_handle_common_intr
rcu: rcu_preempt kthread starved for NNNN jiffies!
rcu: ... OOM is now expected behavior.
```
The `irq/NN-dwc_otg` thread is stuck in state `D` (uninterruptible) forever.

**Root cause.** The Pi 3B+ USB controller (`dwc_otg`) uses ARM **FIQ** (fast
interrupt) acceleration. Under PREEMPT_RT, `disable_irq()` may sleep (it becomes a
mutex), but `dwc_otg` calls it while holding an RCU read-side lock — forbidden.
This starves the RCU grace-period kthread and deadlocks the system. A documented,
known FIQ-vs-PREEMPT_RT incompatibility on the RPi3.

**Fix.** Disable FIQ acceleration via kernel cmdline (no rebuild needed to test):
```
dwc_otg.fiq_enable=0 dwc_otg.fiq_fsm_enable=0 dwc_otg.microframe_schedule=0
```
Permanent, via Yocto:
```
CMDLINE:append = " dwc_otg.fiq_enable=0 dwc_otg.fiq_fsm_enable=0 dwc_otg.microframe_schedule=0"
```
Trade-off: slightly lower USB throughput. Irrelevant here — the RS485 bus runs on
the UART, and no bulk USB transfer is involved.

**Diagnostic that cracked it.** Reading the call trace, not just the top warning:
`disable_irq -> local_fiq_disable` inside an RCU read section names the exact
mechanism. The fix follows directly from understanding *why* it sleeps.

**Lesson.** RT kernels expose latent driver bugs that a stock kernel tolerates.
The stack trace usually names the offending call chain precisely — read past the
first line to the actual sleeping call.

---

### BOOT-2 — no console output after the rainbow splash

**Symptom.** HDMI shows the Pi splash and nothing further.

**Root cause.** Kernel console is directed to the UART (`console=ttyAMA0`), not the
HDMI framebuffer. Expected behaviour for a minimal Yocto image, not a failure.

**Fix.** Use a USB-TTL serial adapter to watch the UART console, or add
`console=tty1` to the cmdline for an HDMI console. For this project the serial
adapter is the right tool during early bring-up.

---

### BOOT-3 — serial login never works (and why that is correct)

**Symptom.** `INIT: Id "S0" respawning too fast: disabled for 5 minutes`. No login
prompt on the serial console.

**Root cause.** Two things claim the same PL011 UART: the kernel console
(`console=ttyAMA0`) and the serdev binding from the `modbus@0` device-tree node.
A getty cannot cleanly own a port that serdev has claimed. And once the Modbus
driver binds, that UART belongs entirely to the fieldbus anyway.

**Resolution.** Not a bug to fix — an architectural fact. Serial login on this UART
is impossible by design. Pivoted to **WiFi + SSH** as the working console; serial
is used only to watch the boot log before the network is up.

**Lesson.** Some "failures" are the design telling you your mental model is wrong.
The right move was to stop trying to make serial login work and recognise why it
couldn't.

---

## Access — getting into the Pi

### ACCESS-1 — WiFi bring-up (a six-round battle, each round one layer deeper)

Each failure exposed the next layer down. The log always named the immediate cause;
the skill was mapping that to the right layer.

1. **`wpa_supplicant` absent** — not in `core-image-minimal`.
   *Fix:* `IMAGE_INSTALL:append = " wpa-supplicant"`.
2. **Firmware license refusal** — `linux-firmware-rpidistro` has a restricted
   license flag. *Fix:* `LICENSE_FLAGS_ACCEPTED += "synaptics-killswitch"`.
3. **Wrong chip** — Pi 3B+ is **BCM43455**, not BCM43430 (an early wrong guess).
   Driver loaded but `brcmfmac43455-sdio.bin failed with error -2`.
   *Fix:* `linux-firmware-rpidistro-bcm43455` + `wireless-regdb` (for
   `regulatory.db`).
4. **No `wlan0`** — `nl80211 not found`, `wlan0: No such device`. The `cfg80211`
   and `brcmfmac` kernel modules were never built.
   *Fix:* add to `rt.cfg` (`CONFIG_CFG80211=m`, `CONFIG_BRCMFMAC=m`, ...) and
   `IMAGE_INSTALL:append = " kernel-modules"`.
5. **Autostart didn't run** — `rc.local` is not sourced by this sysvinit setup.
   *Fix:* a proper `/etc/init.d/wifi` script with `S99wifi` symlinks in
   `rc2.d/rc3.d/rc5.d`.
6. **Dropped after ~1 min** — `brcmfmac: power save enabled`; the chip slept.
   *Fix:* disable WiFi power save (`iwconfig wlan0 power off`, or a modprobe
   option).

**Lesson.** A layered subsystem fails one layer at a time. The message names the
immediate cause (`No such device`, `firmware load failed`, `nl80211 not found`);
the work is translating that into *which layer* — firmware, kernel module,
userspace daemon, init, or power management.

---

### ACCESS-2 — root login locked; SSH rejects empty password

**Symptom.** Serial getty crashes; `ssh root@pi` rejects the empty password.

**Root cause.** Without `debug-tweaks` (unavailable in this setup), the root
password is locked (`!` in `/etc/shadow`), and `sshd` defaults to
`PermitRootLogin`/`PermitEmptyPasswords` = no.

**Fix (via `ROOTFS_POSTPROCESS_COMMAND`).**
```
unlock_root() { sed -i 's/^root:[^:]*:/root::/' ${IMAGE_ROOTFS}/etc/shadow; }
setup_sshd() {
    sed -i 's/#PermitEmptyPasswords no/PermitEmptyPasswords yes/' ${IMAGE_ROOTFS}/etc/ssh/sshd_config
    sed -i 's/#PermitRootLogin.*/PermitRootLogin yes/'            ${IMAGE_ROOTFS}/etc/ssh/sshd_config
}
```
Dev-only. A production image would use keys and a real password.

---

## Driver

### DRV-1 — Makefile "missing separator"

**Symptom.** `Makefile:5: *** missing separator. Stop.`

**Root cause.** Recipe lines began with spaces, not a literal tab. An editor
expanded the tabs.

**Fix.** Write the Makefile with `printf` to force real tabs; verify with
`cat -A Makefile` (real tabs show as `^I`). Prevent recurrence in vim:
```
autocmd FileType make setlocal noexpandtab
```

---

### DRV-2 — serdev API drift at 4b compile

**Symptom.** Two compile errors on a kernel that is newer than the reference code:
```
initialization of 'size_t (*)(...)' from incompatible pointer type 'ssize_t (*)(...)'
array type has incomplete element type 'struct of_device_id'
```

**Root cause & fix.**
- `receive_buf` must return **`size_t`** on 6.12, not `ssize_t`. The compiler
  printed the exact expected signature.
- `struct of_device_id` needs `#include <linux/mod_devicetable.h>` (and
  `<linux/of.h>`). 4a did not need it because it had no `of_match_table`; 4b is the
  first increment using device-tree matching.

**Lesson.** Kernel API drift across versions is normal. The compiler usually states
the exact signature it wants — read the error before rewriting logic.

---

### DRV-3 — serdev livelock (the consume-contract bug)

**Symptom.** After a valid probe, `dmesg` floods:
```
rx 0 byte(s), total 256
rx 0 byte(s), total 256   (forever)
```
Console sprays garbage from the log flood.

**Root cause.** The serdev contract: `receive_buf` must return the number of bytes
**consumed**. The core holds unconsumed bytes and re-calls until they are taken.
In 4b the buffer filled (no framing yet to drain it), so `n = min(count, 0) = 0`,
the function returned 0 ("consumed nothing"), and serdev re-called immediately —
an infinite loop.

**Fix.** On overflow, discard and claim the bytes so serdev stops re-calling:
```c
if (n < count) {
    priv->rxlen = 0;   /* discard, restart framing */
    return count;      /* tell serdev we consumed everything */
}
return n;
```
Also removed the per-callback `dev_info` — logging on every receive at 115200 baud
is itself a flood; use `dev_dbg` (compiled out unless dynamic debug is enabled).
In normal operation (4c onward) the t3.5 timer drains the buffer every 1.75 ms, so
the overflow branch never triggers.

**Lesson.** A subsystem callback usually has a contract about its return value.
This was not a typo but a misunderstanding of that contract — worth reading the
subsystem docs/headers for what a callback's return is expected to mean.

---

## The Debian / scarthgap rebuild (host migration)

The Ubuntu host became unusable and the project was rebuilt from scratch on Debian
13 (trixie), and the release moved from walnascar to scarthgap (LTS) at the same
time. This section covers what that migration surfaced — including the real
resolution of the pseudo saga (YOCTO-5), which turned out to be a host-kernel
problem all along.

### MIG-1 — pseudo failure RESOLVED by the host-kernel change

**Background.** On the Ubuntu host (kernel 6.17), `do_package`/`do_install`
repeatedly failed with `chown: Invalid argument` — first on the driver, then
wpa-supplicant, then core toolchain recipes (gcc-runtime, gcc-sanitizers). Config
tweaks (`PSEUDO_PATHS_CHECK`, `PSEUDO_ALTPATH`), cleans, and pseudo-native rebuilds
all failed. It blocked every full image build; the workaround was cross-compile +
SCP of individual artifacts, never a packaged image.

**Root cause, confirmed.** pseudo (Yocto's fakeroot) intercepts filesystem syscalls
via `LD_PRELOAD`. Host kernel 6.17 emitted syscall variants the walnascar-era pseudo
could not decode, so it returned `EFAULT`. A host/tooling incompatibility, never a
fault in the recipes.

**Resolution.** The Debian host runs **kernel 6.12** — within the range this Yocto
release's pseudo expects. On the new host the full image — packaging, rootfs, wic,
all of it — **builds clean, with no pseudo error anywhere.** The driver, overlay,
and master now package into the image and the driver auto-loads at boot; the SCP
workaround is retired.

**Lesson.** The single most valuable diagnostic realization of the whole project:
a bug that surfaced across three unrelated recipes was one host-environment cause,
not three recipe bugs. And the fix was environmental, not code. This is exactly the
problem containerized/pinned builds exist to solve — build correctness depends on
the host, not just the recipes. (If a matching-kernel host isn't available, the
equivalent fix is a build container with a compatible userspace.)

### MIG-2 — UNPACKDIR does not exist on scarthgap (inverse of YOCTO-3)

**Symptom.** After cloning the walnascar recipes onto scarthgap, three custom
recipes failed:
- overlay `do_unpack`: `Directory name ${@d.getVar('S') contains unexpanded bitbake variable`
- master `do_compile`: `cc1: fatal error: modbus_master.c: No such file or directory`
- driver `do_compile`: `make: *** No targets specified and no makefile found`

**Root cause.** `UNPACKDIR` was introduced in the 5.1 dev cycle (walnascar). It
**does not exist on scarthgap (5.0).** The recipes carried the walnascar pattern
`S = "${WORKDIR}/sources"` + `UNPACKDIR = "${S}"` (and the overlay's
`S = "${UNPACKDIR}"`). On scarthgap, `UNPACKDIR` is undefined → the overlay's `S`
references an unexpanded variable, and the driver/master look for source in a
directory that doesn't get populated → files "not found".

**Fix.** Remove all `UNPACKDIR` lines and set `S = "${WORKDIR}"`. On scarthgap,
`file://` sources unpack straight into `${WORKDIR}` and building there is fully
supported (the "S=WORKDIR unsupported" error is a *walnascar* thing — the exact
inverse). Three recipes changed; all compiled after.

**Lesson.** This is the mirror image of YOCTO-3. walnascar *required* the
`sources`/`UNPACKDIR` split; scarthgap doesn't *have* it. A recipe correct on one
release is wrong on the other. Match the recipe idiom to the exact release — and
note that "downgrading" LTS is not always simpler, it's just *different*.

### MIG-3 — scarthgap layer compat

**Symptom.** `bitbake-layers add-layer ../meta-fieldbus` would refuse the layer, or
parsing errors on `LAYERSERIES_COMPAT`.

**Root cause / fix.** `conf/layer.conf` named walnascar:
```
LAYERSERIES_COMPAT_meta-fieldbus = "walnascar"
```
Change to `scarthgap`. Every layer declares which release series it supports; a
mismatch is a hard refusal.

### MIG-4 — verify scarthgap ships a >=6.12 kernel BEFORE committing to it

**The check that gated the whole release decision.** scarthgap historically shipped
linux-raspberrypi 6.6 by default, and `CONFIG_PREEMPT_RT` only exists mainline from
6.12. Before committing:
```bash
ls meta-raspberrypi/recipes-kernel/linux/
```
This particular scarthgap meta-raspberrypi checkout had `linux-raspberrypi_6.12.bb`
— so scarthgap (LTS) *and* the RT kernel were both available. If it had shown only
6.6, the fallback was to stay on walnascar. **Confirm the kernel version a release
offers before building on it** — the RT requirement drives the release choice, not
the other way around.

### MIG-5 — Debian locale not set for bitbake

**Symptom.** `bitbake` aborts immediately:
```
locale.Error: unsupported locale setting
```

**Root cause.** Debian generated `en_IN.UTF-8` (or none) but not the `en_US.UTF-8`
bitbake requires, and `oe-init-build-env` sanitizes the environment so the variables
must be exported *before* sourcing it.

**Fix.**
```bash
sudo sed -i 's/^# *en_US.UTF-8 UTF-8/en_US.UTF-8 UTF-8/' /etc/locale.gen
sudo locale-gen
export LANG=en_US.UTF-8
export LC_ALL=en_US.UTF-8
# then source oe-init-build-env
```
Made permanent by adding the two `export` lines to `~/.bashrc`.

### MIG-6 — obsolete package name on trixie

**Symptom.** `apt install ... liblz4-tool` → `Package 'liblz4-tool' has no
installation candidate`.

**Root cause / fix.** `liblz4-tool` was folded into the `lz4` package on Debian
trixie. Install `lz4` instead. (Trivial, but it aborts the whole apt line, so the
rest of the prereqs look uninstalled until you re-run.)

### MIG-7 — setup_sshd failed: sshd_config not present

**Symptom.** `do_rootfs` failed at the very end:
```
sed: can't read .../rootfs/etc/ssh/sshd_config: No such file or directory
```
All other postprocess functions (unlock_root, setup_wifi, setup_wifi_initd) had
already succeeded.

**Root cause.** The `setup_sshd` postprocess function edits `/etc/ssh/sshd_config`,
but the openssh **server** wasn't installed, so the file didn't exist. `EXTRA_IMAGE_FEATURES`
lacked `ssh-server-openssh` in this build's local.conf.

**Fix.** Two parts:
1. `EXTRA_IMAGE_FEATURES = "ssh-server-openssh allow-root-login"` in local.conf.
2. Guard the function so a missing file can't hard-fail the rootfs:
   ```bash
   setup_sshd() {
       if [ -f ${IMAGE_ROOTFS}/etc/ssh/sshd_config ]; then
           sed -i 's/^#*PermitRootLogin.*/PermitRootLogin yes/' ${IMAGE_ROOTFS}/etc/ssh/sshd_config
           sed -i 's/^#*PermitEmptyPasswords.*/PermitEmptyPasswords yes/' ${IMAGE_ROOTFS}/etc/ssh/sshd_config
           echo 'UseDNS no' >> ${IMAGE_ROOTFS}/etc/ssh/sshd_config
       fi
   }
   ```
**Lesson.** A postprocess function that edits a package-owned file must not assume
the package is installed. Guard the edit, and make the dependency explicit.

### MIG-8 — SD-card auto-remount defeats umount; and root SSH login

**Symptom A.** `umount /dev/sdc*` "succeeds" then the partition is immediately
remounted by the Debian desktop — bmaptool then can't get the device.

**Fix A.** Use the desktop mounter's own unmount, which tells it to let go:
```bash
udisksctl unmount -b /dev/sdc1
udisksctl unmount -b /dev/sdc2
```

**Symptom B.** After flashing, `ssh root@<pi>` rejected the password even though
`/etc/shadow` showed `root::` (unlocked) and sshd_config had `PermitRootLogin yes`
+ `PermitEmptyPasswords yes`.

**Fix B.** Empty-password root-over-SSH is blocked by some openssh builds regardless
of config. Set a real password by patching the card's shadow directly:
```bash
sudo mount /dev/sdc2 /mnt/piroot
HASH=$(openssl passwd -6 root)
sudo sed -i "s|^root:[^:]*:|root:${HASH}:|" /mnt/piroot/etc/shadow
sudo umount /mnt/piroot && sync
```
Then `ssh root@<pi>` with password `root`. Cleaner than empty for a networked device
anyway.

### MIG-9 — flashing the wrong device (near miss)

**Symptom.** `lsblk` showed `sdc` (29.7 G, with `boot`/`root` labels) and `sdd`
(0 B). `sdd` at 0 B is an empty card reader; `sdc` was a card with an existing OS.

**Lesson.** Always confirm the target with `lsblk -o NAME,SIZE,LABEL,MOUNTPOINTS`
before `bmaptool`. A 0 B device has no card inserted. A device with `boot`/`root`
labels may be a card you care about — check before overwriting. Flash the **whole
device** (`/dev/sdc`), never a partition — the wic image carries its own table.

### THE RESULT — full stack working on hardware

After the migration, the first live end-to-end exchange succeeded:
```
frames_ok:  0 -> 1 -> 2 -> 3    every request answered
crc_errors: 0                   zero bad frames
```
Response frame captured: `01 03 08 00 64 00 c8 01 2c 01 90 90 08` — function 0x03,
8 data bytes, registers 100/200/300/400, CRC valid. The Pi master, kernel driver,
RS485 bus, and STM32 slave all work together. The self-contained image boots,
`uname` shows `PREEMPT_RT 6.12.93-v8`, and `modbus_drv` auto-loads at boot.

---

## Meta-process — three habits that did the heavy lifting

1. **Read the actual error; identify the layer.** Nearly every fix came from the
   log naming its immediate cause, then asking *which layer* that points to. The
   first-guess fix was rarely the real one — PREEMPT_RT took three passes because
   each fix revealed a deeper cause.

2. **Distinguish blockers from nuisances.** The pseudo bug (YOCTO-5) is the case
   study: hours of effort to fix vs. zero cost to route around via SCP. Deferring
   it kept the whole project moving — and it later turned out (MIG-1) to be a
   host-kernel incompatibility that vanished on a different host, vindicating the
   decision to route around rather than fight it.

3. **Verify state; do not assume it.** Checking the work-dir path (revealed kernel
   6.6), grepping the deployed `.config`, watching `dmesg` for the probe line —
   build systems and boot flows fail silently, so confirming ground truth beat
   trusting that a change took effect.

4. **Suspect the host, not just the code.** The single hardest bug (pseudo) was
   never in the recipes — it was the build host's kernel. When a failure spans
   multiple unrelated components, the common factor is often the environment. Build
   reproducibility is a first-class concern, not an afterthought.

---

*Maintained across the Ubuntu/walnascar build and the Debian/scarthgap rebuild.
The value of this file is the reasoning, not just the fix — every entry names the
root cause and the general lesson, because the same class of bug recurs in new
disguises. The project reached a working end-to-end state on hardware: the story
here is how it got there.*
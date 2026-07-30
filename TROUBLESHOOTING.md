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

## Meta-process — three habits that did the heavy lifting

1. **Read the actual error; identify the layer.** Nearly every fix came from the
   log naming its immediate cause, then asking *which layer* that points to. The
   first-guess fix was rarely the real one — PREEMPT_RT took three passes because
   each fix revealed a deeper cause.

2. **Distinguish blockers from nuisances.** The pseudo bug (YOCTO-5) is the case
   study: hours of effort to fix vs. zero cost to route around via SCP. Deferring
   it kept the whole project moving.

3. **Verify state; do not assume it.** Checking the work-dir path (revealed kernel
   6.6), grepping the deployed `.config`, watching `dmesg` for the probe line —
   build systems and boot flows fail silently, so confirming ground truth beat
   trusting that a change took effect.

---

*Maintained alongside the driver increments (4a–4d). Add new entries as they occur;
the value of this file is the reasoning, not just the fix.*

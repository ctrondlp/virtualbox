---
layout: single
classes: wide
title: Build and run it yourself
---

Everything here can be built from this repository and run on your own machine. It is an unofficial build: it is not signed
by Oracle, and the way it was run for the measurements leaves out the VirtualBox kernel drivers. Read the limits below before
you move VMs you care about to it.

## Build

The tree builds with VirtualBox's own build system, kBuild (`kmk`). The official instructions apply:
[Building VirtualBox from the source code](https://www.virtualbox.org/wiki/Build_instructions). In short:

1. Run `configure.py` to check the tools and create the environment, then build with `kmk` (or `kmk -C <subdirectory>` for
   one part).
2. Put machine-specific settings in `LocalConfig.kmk`; do not commit it.
3. The Qt GUI is built only with `VBOX_WITH_QTGUI` (without it the GUI front end is reported as "Invalid frontend name").
4. `KBUILD_TYPE=release` or `debug` selects the build type; the numbers on this site come from a release build.

The Windows build used Visual Studio, the Linux build GCC 15 under WSL2. The branches on the
[list of changes](changes.html) each build on their own only if they do not depend on another one (the table says which are
stacked); the safest source is the integration branch, which has them all.

## Running without the kernel drivers

The measurements on this site were taken with the build running **driverless**, which is what you get with
`VBOX_WITH_DRIVERLESS_FORCED := 1` and `VBOX_WITHOUT_HARDENING := 1` in `LocalConfig.kmk`. It is also what a build you make
yourself does on a PC that has the official VirtualBox installed: the official kernel driver (`VBoxSup.sys`) is for one
exact build and refuses to work with a different one (it reports a driver version mismatch). Installing your own driver
means signing it, which on Windows means test signing and turning off Secure Boot; this fork does not do that.

### What does not work in a driverless build

| Feature | Why |
|---|---|
| **Bridged networking** | needs the host network filter driver and the internal network switch in the kernel driver |
| Host-only networking | same |
| Internal networks and NAT Networks | same (the switch lives in the driver) |
| **USB passthrough** | needs the USB driver |
| Anything that relies on hardware virtualization directly | the VM runs on the operating system's hypervisor instead (Hyper-V on Windows, KVM on Linux) |

**Plain NAT works**, including port forwarding, and so does everything that does not touch the host network or USB: disks,
audio, shared folders, clipboard, the graphics controllers and the new virtio-gpu device. If a VM was set up with a bridged
adapter, switch it to NAT first or it will not start; the log says "Cannot attach ... in driverless mode".

### What differs in the numbers

Without the driver, a halted virtual CPU waits in user space at the operating system's timer resolution instead of in the
kernel, and nothing raises the system timer to 1 ms. So the halt, wake-up and timer results on the [speed page](speed.html)
(halt polling, high resolution waits, timer lateness) can differ on an official build with the driver, while per-exit costs
and fixes in the emulator carry over. Where the stock 7.2.20 release was measured too, the speed page says so.

## Other limits of an unofficial build

* **Saved settings are newer than the release can read.** This tree writes VM settings files that official VirtualBox 7.2.20
  does not accept (the format moved on). Back up your `.vbox` files and `VirtualBox.xml` before opening VMs with this
  build, and do not expect to open them again with the release without restoring the backup.
* **Do not run it next to the official install in the same session.** The two share the background service
  (`VBoxSDS`/`VBoxSVC`), which accepts only the one registered from its own folder. Switch between them rather than running
  both.
* **Guest Additions** come from the matching release; they are not rebuilt unless you build them.
* **Not tested in an unofficial build:** the Extension Pack (USB 2/3, RDP, disk encryption, PXE boot), the VirtualBox web
  service, hardening and the installer packages. Treat them as unavailable unless you try them.
* **Cross-platform:** the measurements are from one Windows 11 host, with a Linux debug build checked for compile errors in
  WSL2. Linux and macOS hosts, and Arm hosts, were not run.
* This is a development tree, and no change here has been through Oracle's own test and release process.

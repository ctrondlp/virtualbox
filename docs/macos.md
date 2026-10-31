---
layout: default
title: macOS guests
---

# macOS guests

VirtualBox only knows macOS guest types up to 10.13 and has no support for running newer releases on a PC that is not a Mac.
This fork adds the missing pieces so that macOS 10.14 to 26 (Intel builds) can boot, and finds why they booted so slowly on
a Windows host with Hyper-V. Apple's licence only allows macOS on Apple hardware; this page is about the virtualization code,
not about where to get a macOS image. You supply the installer or disk and an OpenCore boot ISO yourself.

This is unlikely to go upstream. macOS 26 Tahoe is the last macOS release that runs on Intel processors, so the guest types
added here are a closed set that will not grow, and the work serves a shrinking audience. It stays in this fork, with the
possible exception of the changes that also help other guests (the x2APIC setting bug, for one, is filed separately; see
[Upstream](index.html#upstream-we-want-these-changes-in-virtualbox)). Each item is a branch on the [list of changes](changes.html#macos-guests).

## What was missing, and what each change does

| Problem on a non-Apple host | Change | Branch |
|---|---|---|
| No guest types after 10.13 | Guest OS types for macOS 10.14 Mojave, 10.15 Catalina, 11 Big Sur, 12 Monterey, 13 Ventura, 14 Sonoma, 15 Sequoia and 26 Tahoe, with GUI icons, OVF mapping, wizard name matching and Mac platform defaults. The guest gets an iMac model its release supports instead of the host's DMI tables (`DmiUseHostInfo` selects the old behaviour). | `macos-guest-types` |
| Kernel panic "Unsupported CPU" on a host CPU macOS does not know (Tiger Lake, Alder Lake, Raptor Lake, Arrow Lake) | New `/CPUM/ForceIntelFamilyModelStep` setting reports a model macOS knows (Coffee Lake by default for these types); `MSR_CORE_THREAD_COUNT` is answered, which the macOS kernel reads unconditionally. | `macos-cpum-hooks` |
| The guest never reaches its user interface, or fails with "Library not loaded: libSystem.B.dylib" | The SMC device now checks for the device key and for a usable CPU (Haswell-generation extensions are required from macOS 13) and says so in the log and as a VM error, instead of leaving you to guess. | `macos-smc-diagnostics` |
| A guest enumerating the SMC keys got the wrong ones, and values it wrote were dropped | Key enumeration by index and key writes fixed, `MSSP` key added. | `macos-smc-fixes` |
| The firmware lacked APFS and large BaseSystem support, which macOS 10.14 and later look for | The Apple firmware feature bits are configurable, and the types above set the right ones. | `macos-efi-apple-features` |
| No boot entry for macOS installed on APFS | The EFI boot manager finds the loader in the APFS Preboot volume (and the installer's staging directory), with the old paths as a fallback. | `macos-efi-apple-loader` |
| The VM halted when OpenCore asked the firmware's device path property database | The three unimplemented protocol methods now answer "not found" instead of asserting. | `macos-efi-appleprop-stubs` |
| The 64-bit EFI firmware does not build with Visual Studio 2017 and later | Build fixes for the MSVC intrinsics. | `efi-msvc-intrinsics` |

## Boot time on a Windows host with Hyper-V

A macOS Sequoia guest (8 virtual CPUs, EFI, OpenCore) took more than 200 seconds to boot on this PC and was still starting
services. The statistics showed 82% of the time outside guest execution: every access to the interrupt controller's memory
(end-of-interrupt, inter-processor interrupts, timer) was an exit, and the exit-history optimization then re-ran about 60
instructions in the emulator for each one, with the eight virtual CPUs queueing on one lock.

| Setting | Boot to login screen |
|---|---|
| Default (xAPIC) | more than 200 s |
| Exit-history optimization off | 100–122 s |
| **x2APIC on** (`VBoxManage modifyvm <vm> --x2apic on`) | **33–44 s** |

This needed no code change, only the setting. On macOS Tahoe (26) the same VM settings reach the login screen in about 35 s,
and a macOS Mojave (10.14) recovery image boots. The speed changes on the [speed page](speed.html) apply to these guests too.
The x2APIC setting itself had a bug of its own, fixed in `861-x2apic-off-keeps-apic`: turning it off also turned the APIC off,
and a 64-bit guest then refused to start.

## What was and was not tested

* Booted: Sequoia and Tahoe to the login screen, Mojave to its recovery environment (verbose boot, no guru meditation), on a
  Windows 11 host with Hyper-V, Intel CPU, no VirtualBox kernel driver.
* The test VMs were created with the older macOS 10.13 guest type, so the new guest types and their Mac platform defaults
  were built and reviewed but a VM using them was not booted end to end.
* Not tested: AMD hosts, Linux or macOS hosts, a full Mojave installation, Apple silicon guests (not supported by VirtualBox
  on x86).
* Reliability depends on the host CPU; the CPU model workaround exists because newer Intel CPUs panic the macOS kernel.

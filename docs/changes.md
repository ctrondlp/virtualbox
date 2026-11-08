---
layout: default
title: All changes
---

# All changes

Generated from git by `docs/_tools/gen-changes.sh` against `origin/main`. One row per feature branch that is merged into the
integration branch. **Planned for upstream** means the change is written up as a stand-alone contribution that will be sent to VirtualBox; **local** means it stays
in this fork.

## Speed on Hyper-V (NEM) hosts

| Branch | What it does | Status |
|---|---|---|
| `nem-hv-delivers-ints` | VMM/NEM/win: Let Hyper-V deliver the APIC and PIC interrupts | Planned for upstream |
| `nem-win-x2apic` | VMM/NEM/win: Keep the x2APIC mode when VirtualBox emulates the APIC | Planned for upstream |
| `nem-msr-exit-import` | VMM/NEM/win: Make guest MSR exits cheaper | Planned for upstream |
| `vmm-halt-poll` | VMM: Poll briefly for a wake-up before a halted EMT blocks (halt polling) | Planned for upstream |
| `gim-kvm-pv-eoi` | VMM/GIM/KVM: Offer the guest the paravirtualized EOI | Planned for upstream |
| `nem-win-register-shadowing` | VMM/NEM/win: Skip exporting registers Hyper-V already holds | Planned for upstream |
| `nem-win-halt-partial-import` | VMM/NEM/win: Import only the minimal state when the guest halts | Planned for upstream |
| `nem-win-gim-xcpt-narrowing` | VMM/NEM/win: Intercept #UD and #GP only when needed | Planned for upstream |
| `726-nem-timer-kick` | VMM/NEM/win: Get the timer EMT out of Hyper-V when a timer is due | Planned for upstream (targets issue [#726](https://github.com/VirtualBox/virtualbox/issues/726)) |
| `em-apic-mmio-history` | VMM/NEM/win: Keep the xAPIC page out of the EM exit history | Planned for upstream |
| `em-apic-mmio-fastpath` | VMM/NEM/win: Do simple xAPIC page accesses without IEM and the page manager | Planned for upstream |
| `iprt-win-ecoqos-optout` | IPRT/win: Opt VM processes out of power throttling (EcoQoS) | Planned for upstream |
| `iprt-win-highres-waits` | IPRT/win: High resolution timed waits for semaphores in VM processes | local: the gain was measured only without the kernel driver, so there is no case for official builds ([why](speed.html#6-high-resolution-waits)) |

## Correctness fixes

| Branch | What it does | Status |
|---|---|---|
| `861-x2apic-off-keeps-apic` | Main/PlatformX86Impl.cpp: Don't disable the APIC when disabling the x2APIC | Planned for upstream (targets issue [#861](https://github.com/VirtualBox/virtualbox/issues/861)) |
| `930-iem-riprel-cbimm` | Fix RIP-relative memory operand decoding of ADCX/ADOX and VEX imm8 instructions (upstream issue [#930](https://github.com/VirtualBox/virtualbox/issues/930)), with a test tidy-up | [Open PR #931](https://github.com/VirtualBox/virtualbox/pull/931) |
| `932-nem-tpr-low-nibble` | Keep the APIC TPR low nibble when syncing CR8 under Hyper-V (upstream issue [#932](https://github.com/VirtualBox/virtualbox/issues/932)) | [Open PR #933](https://github.com/VirtualBox/virtualbox/pull/933) |
| `iem-vcvt-256-source-reg` | IEM: vcvtps2pd and vcvtdq2pd (256-bit) converted the wrong source register | Planned for upstream |
| `nem-lazy-db-exits` | VMM/NEM/win: Only intercept #DB while debugging (WIP, xcpt-2 TF stepping hangs) | Planned for upstream |
| `nem-win-restore-drx-after-debugging` | VMM/NEM/win: Reload the guest's debug registers after debugging | Planned for upstream |
| `virtio-core-restore-queue-select` | VirtIO: Don't refuse to restore a queue select the guest may validly have written | Planned for upstream |

## Storage and boot

| Branch | What it does | Status |
|---|---|---|
| `bios-nvme-boot` | BIOS/NVMe: Boot from NVMe disks with the legacy PC BIOS | Planned for upstream ([storage page](storage.html#1-booting-from-nvme-with-the-bios-firmware)) |
| `nvme-windows-no-cmb` | Main/NVMe: Leave the controller memory buffer out for Windows guests on BIOS | Planned for upstream ([storage page](storage.html#4-windows-guests-on-an-nvme-controller)) |

## Networking

| Branch | What it does | Status |
|---|---|---|
| `nat-throughput` | Guest to host TCP over NAT is faster: libslirp writes the data of a burst of guest packets to the host connection in one call instead of one call per packet (on by default, DeferHostWrites=0 turns it off) | local: it changes the bundled libslirp, so it would go to libslirp first ([speed page](speed.html#7-nat-faster-uploads-from-the-guest)) |

## Graphics

| Branch | What it does | Status |
|---|---|---|
| `virtio-gpu-device` | A virtio-gpu graphics device: 2D display and Vulkan 3D (Venus) for Linux guests, with the EFI driver, API, GUI and VBoxManage support (see the virtio-gpu page) | local: large and experimental, may be offered later ([virtio-gpu page](virtio-gpu.html)) |
| `vmsvga-dx11-gpu-select` | Devices/Graphics/VMSVGA: Create the DX11 device on the high performance GPU, selectable with VMSVGA3dAdapter | Planned for upstream |
| `devvga-wide-scanline` | DevVGA: Check all pages of a scanline for changes in wide display modes | Planned for upstream |

## Guest CPU model

| Branch | What it does | Status |
|---|---|---|
| `cpum-isa-exts` | VMM/CPUM: Expose GFNI, VAES, VPCLMULQDQ, AVX-VNNI, AVX-IFMA, AVX-NE-CONVERT, XSAVEOPT/XSAVEC and fast strings to guests | Planned for upstream |
| `cpum-intel-microarchs` | VMM/CPUM: Know Intel family 6 models from Raptor Lake to Panther Lake | Planned for upstream |

## macOS guests

| Branch | What it does | Status |
|---|---|---|
| `macos-guest-types` | Main/macOS: Add guest OS types for macOS 10.14 to 26 with Mac platform defaults | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |
| `macos-cpum-hooks` | VMM/CPUM: Add ForceIntelFamilyModelStep and fudge MSR_CORE_THREAD_COUNT | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |
| `macos-smc-fixes` | Devices/SMC: Fix key enumeration by index and key writes, add MSSP | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |
| `macos-smc-diagnostics` | Devices/SMC: Warn when a macOS guest lacks the device key or a usable CPU | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |
| `macos-efi-apple-features` | Devices/EFI: Make the Apple firmware feature bits configurable | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |
| `macos-efi-apple-loader` | EFI/BootManager: Find the macOS boot loader on APFS volumes | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |
| `macos-efi-appleprop-stubs` | EFI/VBoxAppleSim: Don't assert in the device path property protocol stubs | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |
| `efi-msvc-intrinsics` | EFI/Firmware: Build the X64 firmware with newer MSVC versions | local: unlikely to go upstream, macOS 26 is the last release for Intel ([macOS page](macos.html)) |

## Test tooling (VBoxBFE, the stand-alone front end used for testing)

| Branch | What it does | Status |
|---|---|---|
| `vboxbfe-config-ctor-rc` | VBoxBFE: Do not fail the CFGM setup when PDM/Drivers already exists | local: only used to run the benchmarks and tests of this fork |
| `vboxbfe-numcpus` | VBoxBFE: Create the VM with the NumCPUs value from the configuration | local: only used to run the benchmarks and tests of this fork |
| `vboxbfe-sdl-update-events` | VBoxBFE/Framebuffer: Always update the SDL surface from the SDL thread | local: only used to run the benchmarks and tests of this fork |
| `vboxbfe-update-clip` | VBoxBFE/Framebuffer: Clip update rectangles to the SDL surface | local: only used to run the benchmarks and tests of this fork |
| `vboxbfe-vbva-display` | VBoxBFE/Display: Support VBVA and render the VGA screen into a per-mode buffer | local: only used to run the benchmarks and tests of this fork |
| `vboxbfe-test-hook` | VBoxBFE: Test hook to reset, save and load the VM from the environment | local: only used to run the benchmarks and tests of this fork |

---
layout: default
title: All changes
---

# All changes

Generated from git by `docs/_tools/gen-changes.sh` against `origin/main`. One row per feature branch that is merged into the
integration branch. **PR draft** means the change is written up as a stand-alone upstream change; **local** means it stays
in this fork. "Size" is the number of commits, files changed and lines added/removed against upstream.

## Speed on Hyper-V (NEM) hosts

| Branch | What it does | Size | Status |
|---|---|---|---|
| `nem-hv-delivers-ints` | VMM/NEM/win: Let Hyper-V deliver the APIC and PIC interrupts | 1 commit, 6 files, +262 -12 | PR draft |
| `nem-win-x2apic` | VMM/NEM/win: Keep the x2APIC mode when VirtualBox emulates the APIC | 1 commit, 4 files, +78 -4 | PR draft |
| `nem-msr-exit-import` | VMM/NEM/win: Make guest MSR exits cheaper | 1 commit, 2 files, +56 -5 | PR draft |
| `vmm-halt-poll` | VMM: Poll briefly for a wake-up before a halted EMT blocks (halt polling) | 2 commits, 4 files, +174 -2 | PR draft |
| `gim-kvm-pv-eoi` | VMM/GIM/KVM: Offer the guest the paravirtualized EOI | 1 commit, 14 files, +392 -15 | PR draft |
| `nem-win-register-shadowing` | VMM/NEM/win: Skip exporting registers Hyper-V already holds | 1 commit, 3 files, +336 -50 | PR draft |
| `nem-win-halt-partial-import` | VMM/NEM/win: Import only the minimal state when the guest halts | 1 commit, 4 files, +39 -2 | PR draft |
| `nem-win-gim-xcpt-narrowing` | VMM/NEM/win: Intercept #UD and #GP only when needed | 1 commit, 2 files, +33 -15 | PR draft |
| `726-nem-timer-kick` | VMM/NEM/win: Get the timer EMT out of Hyper-V when a timer is due | 2 commits, 3 files, +250 -6 | PR draft |
| `iprt-win-ecoqos-optout` | IPRT/win: Opt VM processes out of power throttling (EcoQoS) | 2 commits, 4 files, +767 -2 | PR draft |
| `iprt-win-highres-waits` | IPRT/win: High resolution timed waits for semaphores in VM processes | 1 commit, 9 files, +285 -9 | local |

## Correctness fixes

| Branch | What it does | Size | Status |
|---|---|---|---|
| `861-x2apic-off-keeps-apic` | Main/PlatformX86Impl.cpp: Don't disable the APIC when disabling the x2APIC | 1 commit, 3 files, +81 -1 | PR draft |
| `930-iem-riprel-cbimm` | VMM/IEM: Fix RIP-relative addressing of ADCX/ADOX and VEX imm8 instructions | 2 commits, 11 files, +311 -54 | PR draft |
| `932-nem-tpr-low-nibble` | VMM/NEM/win: Keep the APIC TPR low nibble when syncing CR8 | 2 commits, 2 files, +33 -2 | PR draft |
| `iem-vcvt-256-source-reg` | IEM: vcvtps2pd and vcvtdq2pd (256-bit) converted the wrong source register | 1 commit, 1 file, +2 -2 | PR draft |
| `nem-lazy-db-exits` | VMM/NEM/win: Only intercept #DB while debugging (WIP, xcpt-2 TF stepping hangs) | 2 commits, 3 files, +171 -4 | PR draft |
| `nem-win-restore-drx-after-debugging` | VMM/NEM/win: Reload the guest's debug registers after debugging | 1 commit, 1 file, +8 -0 | PR draft |

## Graphics

| Branch | What it does | Size | Status |
|---|---|---|---|
| `virtio-gpu-device` | A virtio-gpu graphics device: 2D display and Vulkan 3D (Venus) for Linux guests, with the EFI driver, API, GUI and VBoxManage support (see the virtio-gpu page) | 30 commits, 50 files, +7729 -50 | local |
| `vmsvga-dx11-gpu-select` | Devices/Graphics/VMSVGA: Create the DX11 device on the high performance GPU, selectable with VMSVGA3dAdapter | 1 commit, 3 files, +225 -3 | PR draft |
| `devvga-wide-scanline` | DevVGA: Check all pages of a scanline for changes in wide display modes | 1 commit, 2 files, +36 -17 | PR draft |

## Guest CPU model

| Branch | What it does | Size | Status |
|---|---|---|---|
| `cpum-isa-exts` | VMM/CPUM: Expose GFNI, VAES, VPCLMULQDQ, AVX-VNNI, AVX-IFMA, AVX-NE-CONVERT, XSAVEOPT/XSAVEC and fast strings to guests | 1 commit, 7 files, +434 -21 | PR draft |
| `cpum-intel-microarchs` | VMM/CPUM: Know Intel family 6 models from Raptor Lake to Panther Lake | 1 commit, 3 files, +60 -1 | PR draft |

## macOS guests

| Branch | What it does | Size | Status |
|---|---|---|---|
| `macos-guest-types` | Main/macOS: Add guest OS types for macOS 10.14 to 26 with Mac platform defaults | 1 commit, 9 files, +324 -3 | local |
| `macos-cpum-hooks` | VMM/CPUM: Add ForceIntelFamilyModelStep and fudge MSR_CORE_THREAD_COUNT | 1 commit, 2 files, +34 -2 | local |
| `macos-smc-fixes` | Devices/SMC: Fix key enumeration by index and key writes, add MSSP | 1 commit, 2 files, +110 -9 | local |
| `macos-smc-diagnostics` | Devices/SMC: Warn when a macOS guest lacks the device key or a usable CPU | 1 commit, 1 file, +185 -2 | local |
| `macos-efi-apple-features` | Devices/EFI: Make the Apple firmware feature bits configurable | 1 commit, 3 files, +100 -3 | local |
| `macos-efi-apple-loader` | EFI/BootManager: Find the macOS boot loader on APFS volumes | 1 commit, 1 file, +143 -13 | local |
| `macos-efi-appleprop-stubs` | EFI/VBoxAppleSim: Don't assert in the device path property protocol stubs | 1 commit, 1 file, +18 -13 | local |
| `efi-msvc-intrinsics` | EFI/Firmware: Build the X64 firmware with newer MSVC versions | 1 commit, 7 files, +26 -4 | local |

## Test tooling (VBoxBFE, the stand-alone front end used for testing)

| Branch | What it does | Size | Status |
|---|---|---|---|
| `vboxbfe-config-ctor-rc` | VBoxBFE: Do not fail the CFGM setup when PDM/Drivers already exists | 1 commit, 1 file, +1 -0 | local |
| `vboxbfe-numcpus` | VBoxBFE: Create the VM with the NumCPUs value from the configuration | 1 commit, 1 file, +26 -2 | local |
| `vboxbfe-sdl-update-events` | VBoxBFE/Framebuffer: Always update the SDL surface from the SDL thread | 1 commit, 2 files, +5 -11 | local |
| `vboxbfe-update-clip` | VBoxBFE/Framebuffer: Clip update rectangles to the SDL surface | 1 commit, 1 file, +19 -0 | local |
| `vboxbfe-vbva-display` | VBoxBFE/Display: Support VBVA and render the VGA screen into a per-mode buffer | 1 commit, 4 files, +440 -9 | local |
| `vboxbfe-test-hook` | VBoxBFE: Test hook to reset, save and load the VM from the environment | 1 commit, 1 file, +110 -3 | local |

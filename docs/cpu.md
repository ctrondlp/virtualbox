---
layout: default
title: Guest CPU features
---

# Guest CPU features

VirtualBox hides CPU instruction set extensions from a guest unless its CPUID sanitizer has been taught about them, so
guest kernels, crypto libraries and inference libraries take their slower code paths even when the host CPU could run
the fast ones. This fork passes more of them through. Branches: `cpum-isa-exts` and `cpum-intel-microarchs` (both
*Planned for upstream*, see the [list of changes](changes.html#guest-cpu-model)).

## What is now offered to the guest

| Extension | Where in CPUID | Switch under `/CPUM/IsaExts/` |
|---|---|---|
| GFNI | leaf 7.0 ECX | `GFNI` |
| VAES | leaf 7.0 ECX | `VAES` |
| VPCLMULQDQ | leaf 7.0 ECX | `VPCLMULQDQ` |
| AVX-VNNI and AVX-VNNI-INT8 | leaf 7.1 | `AVXVNNI` |
| AVX-IFMA | leaf 7.1 | `AVXIFMA` |
| AVX-NE-CONVERT | leaf 7.1 | `AVXNECONVERT` |
| XSAVEOPT | leaf 0xd.1 EAX | `XSAVEOPT` |
| XSAVEC and XGETBV1 | leaf 0xd.1 EAX | `XSAVEC` |
| Fast strings: ERMS and FSRM | leaf 7.0 EBX / EDX | `FastStrings` |

They are on by default when the guest runs directly on the CPU with nested paging (the same rule as AVX2, BMI and SHA),
off otherwise, never offered when VirtualBox's own emulator executes the guest (it implements none of them), and hidden at
the "portable" CPUID level. Each one can be switched off by itself. The bits they build on (AVX, AES-NI, PCLMUL, AVX2)
must be offered too.

Intel family 6 models from Raptor Lake to Panther Lake (Raptor Lake, Emerald Rapids, Meteor Lake, Granite Rapids, Arrow
Lake, Lunar Lake, Panther Lake) are now recognised instead of being reported as an unknown microarchitecture.

## Result

Measured on an Arrow Lake host (Core Ultra 7 270K Plus), Hyper-V back end, 8 virtual CPUs. The Linux guest runs a test
program that reads CPUID and then **executes** every extension it reports, checking each against a reference
implementation (61 checks).

| | Stock trunk | With this change |
|---|---|---|
| Checks passed / failed / extension absent | 11 / 0 / 50 | **61 / 0 / 0** |
| Switches all off | | 11 / 0 / 50 (same as trunk) |
| Save, load and resume the VM, then the checks | | 61 / 0 / 0 |

This shows that the guest sees and can use the instructions. It is not a speed measurement: how much a program gains
depends on whether it has code paths for these extensions.

## Fixes found on the way

* `cpumCpuIdFindLeafEx` returned the first sub-leaf when the requested one did not match, so the host feature detection
  for leaf 7 sub-leaf 2 (PSFD, IPRED_CTRL and others) read sub-leaf 0.
* The RDPID feature bit was defined as bit 2 instead of 22, and the portable-level mask for CLFLUSHOPT used the RDSEED bit.
* The `/CPUM/IsaExts` string comparison lacked a `== 0`, so every spelled-out value was taken as "disabled".

## Saved states

CPUID leaves are stored in a saved state, so a guest that was offered one of these extensions cannot be resumed on a host
without it. The load check now also covers leaf 7 and leaf 0xd sub-leaf 1: strict (a load error with
`/CPUM/StrictCpuIdChecks`) for the newly offered bits, a warning only for bits older versions already offered, so
existing saved states load as before.

## Not tested

AMD and Hygon hosts (the code reads the same architectural bits on every vendor and nothing in it is vendor specific),
hardware-assisted mode on Windows without the Hyper-V back end, macOS and 32-bit guests, and a real speed gain in an
application.

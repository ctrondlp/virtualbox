---
layout: single
classes: wide
title: Speed
---

Each item is one feature branch (see [all changes](changes.html)). The numbers are medians or ranges of repeated runs of
the same guest on the same host, with the change off and on. "Per second" figures are what a small guest benchmark
counts, so they show the cost of interrupts, halts and exits, not how fast a real application feels.

**Test set-up.** Windows 11 host, 24 cores, VirtualBox running on Hyper-V (NEM). Guests: Windows 10 and Linux, 8 virtual
CPUs. The development build ran without the VirtualBox kernel driver, which is how it runs on this PC with Hyper-V on.
Where the stock 7.2.20 release was measured as well it is said. Absolute numbers on a machine that uses the driver can
differ; the ratios are what to look at.

## 1. Interrupts and timers: keep the x2APIC and let Hyper-V deliver interrupts

*Branches:* `nem-win-x2apic`, `nem-hv-delivers-ints`, `726-nem-timer-kick`

Hyper-V's interface made VirtualBox turn off the x2APIC and inject every interrupt itself, one costly exit at a time.
These changes keep the x2APIC when the guest supports it, let Hyper-V deliver APIC and PIC interrupts, and add a timer
kick so a guest with a far-away timer deadline cannot stall on an idle CPU.

| Measurement (8 virtual CPUs, 3 rounds, medians) | Off | On |
|---|---|---|
| Linux: IPI ping-pong | 3.1k per second | **178k per second (57×)** |
| Linux: timer wake-ups | 17k per second | **68k per second** |
| Linux: timer lateness | 416 µs | **68 µs** |
| Windows 10: thread ping-pong | 26k per second | **35k per second (+32%)** |
| Windows 10: barrier | 14.5k per second | 17k per second (+17%, ranges overlap) |

Boot time and the Linux CPU self-checks (61 of 61) were unchanged, and no run in the 18 hit a guru meditation or a reset.
An early single-run measurement of an idle Ubuntu desktop guest showed 0.84 → 0.45 host cores, but a later three-round
measurement was inconclusive, so no idle CPU claim is made for these changes.

## 2. Halt polling

*Branch:* `vmm-halt-poll`

When a guest CPU halts, the emulation thread used to go straight to sleep and then had to be woken. It now polls for a
short, adaptive time first (grows when polling pays off, shrinks when it does not), and it turns itself off when the host
has fewer than twice as many cores as the guest has virtual CPUs.

| Measurement | Off | On |
|---|---|---|
| Linux 8 virtual CPUs, IPI ping-pong, median of 7 runs | 173k per second | **256k per second (+45%)**, hit rate 93–99% |
| Windows 10 8 virtual CPUs, 100 µs polling | 23.7–26.7k | **30.6–30.7k (+15–25%)** |
| Boot time (pinned 4-on-4 test) | 123 s | 93 s |

Cost: 0.5–0.8 extra host cores while the guest is busy; idle host CPU did not change. It applies where VirtualBox does the
halt itself, which is the case without the kernel driver.

## 3. Cheaper MSR exits

*Branch:* `nem-msr-exit-import`

Every guest write to a model-specific register (the x2APIC end-of-interrupt and IPI registers, 85–99% of Windows' exits)
imported all CPU state before handling it. Now only the registers a handler can need are imported, and end-of-interrupt
writes skip the exit-history optimisation that re-ran them through the instruction emulator.

| Measurement | Before | After |
|---|---|---|
| Linux, one RDMSR that exits | 16.8 µs | **8.5 µs** (2 virtual CPUs: 16.1 → 8.2 µs) |
| Windows 10 8 virtual CPUs, ping-pong, 5 runs | 35.6–44.3k per second | **47.4–49.8k per second** |
| The same, barrier | 7.4–8.4k | 8.3–9.0k |

The cost of an exiting MSR on the stock 7.2.20 release was measured at 15.4–16.4 µs, the same as the unmodified
development build, so the micro-benchmark carries over to official builds.

## 4. Paravirtual EOI for Linux guests

*Branch:* `gim-kvm-pv-eoi`

Offers Linux the KVM paravirtual end-of-interrupt, so the guest tells the host in memory that an interrupt was handled
instead of taking an exit.

* 93% of end-of-interrupt writes skip the exit (85–90k skipped per run).
* Memory exits per run: 294–302k → **197–206k**.
* Linux ping-pong a further **+40%** on top of halt polling.

## 5. Windows no longer throttles minimized VMs

*Branch:* `iprt-win-ecoqos-optout`

Windows puts a process whose window is minimized or covered into "efficiency mode", which moves its threads to the slow
cores and coarsens its timers. A VM running in the background was about **twice as slow** as the same VM in front. VM
processes now opt out of that throttling (the setting can be turned off again with an environment variable).

| Measurement | Throttled (before) | Opted out (after) |
|---|---|---|
| Stock 7.2.20, exiting MSR with the GUI minimized | 25–28.5 µs (15.4–16.4 µs when visible) | |
| Development build, same test | 32.7–33.0 µs | **16.5–16.6 µs** (same as visible) |
| Windows 10 minimized: ping-pong | 26.1–26.6k | **34.4–35.1k** |
| The same: barrier | 3.5–3.6k | **5.5k** |
| The same: timers per second (lateness) | 2.4–2.5k (2.7–2.9 ms) | **5.6–5.8k (0.9 ms)** |
| The same: boot time | 94–97 s | **86–87 s** |

## 6. High resolution waits

*Branch:* `iprt-win-highres-waits` (kept local, reason below)

Windows rounds a timed wait up to its timer tick (15.6 ms, or 1 ms), which stretches every short guest halt. VM processes
use high resolution waits instead. In the driverless set-up, idle host CPU went **1.1 → 0.5 cores** and the "timer
collapse" runs (wake-ups falling to 1.2–1.6k per second, 5–6 ms late) disappeared.

**Why it is not planned for upstream.** Every one of these numbers was measured without the VirtualBox kernel driver. With
the driver, which official Windows x64 builds always use, the system timer runs at about 1 ms and a halted virtual CPU waits
in the kernel, so the coarse waits this change fixes mostly do not occur. The change helps driverless builds like this one
and Windows on Arm hosts, a narrow group, and it adds a system call to every short wait in every VM process. Without a
measurement on an official build there is no case for it. The part that does matter there, Windows throttling minimized VMs,
is item 5 and is planned for upstream.

## 7. Smaller changes that remove work

`nem-win-register-shadowing` skips exporting registers Hyper-V already holds, `nem-win-halt-partial-import` imports only
the minimal state when the guest halts, and `nem-win-gim-xcpt-narrowing` intercepts #UD and #GP only when needed. They
are on the [change list](changes.html); they were measured as part of the totals above, not on their own.

## Measured guidance (no code change)

For guests on this host, from a Debian 13 guest with 8 virtual CPUs:

* **Disk:** at 4 KiB random reads with a queue depth of 32, an NVMe controller reached **51k IOPS**, virtio-SCSI 33–37k and
  the default SATA (AHCI) 12–14k. Sequential throughput was the same on all (about 2.6 GB/s write, 1.5 GB/s read).
* **NAT networking:** 1.7 Gbit/s guest to host and 2.6 Gbit/s host to guest, and the virtio network card was no faster
  than the e1000, so the ceiling is the NAT engine (libslirp), not the card.

## What did not help, and was dropped

Listed because "measured, no gain" is a result too.

* **Paravirtual IPI hypercall (Linux).** No difference in a TLB-shootdown benchmark (93–97k per second on and off): Linux
  already sends most IPIs to a single target, where no hypercall is used.
* **More precise 1 kHz timer kick.** Five of six rounds gave −49 to +63 ms of lateness against +20 to +61 ms; one lucky round
  gave +10 to +15 ms. Not worth the CPU it burns.
* **Hyper-V EOI assist for Windows.** With the current hints Windows ignored it (2.5M armed, none used). With the extra
  hint bit it was used 70% of the time and exits fell by 57%, but ping-pong did not improve and timers got worse
  (3.9k per second at 1.54 ms against 6.5k at 0.73 ms).
* **Re-probing hot exits.** Merged, then reverted: no benefit.
* **A `virtio-net` card instead of e1000:** no change, see above.
* **Idle host CPU for the interrupt changes:** the one measurement taken was inconclusive (the live ISO was still busy
  2.5 minutes after boot), so it is not claimed.

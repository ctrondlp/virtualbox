---
layout: default
title: Home
---

# What this fork adds

This is a fork of VirtualBox (Oracle's open source repository) with two kinds of work:

1. **Speed.** VirtualBox on a Windows host that runs it on top of Hyper-V (the "NEM" path, which is what you get when
   Hyper-V, WSL2 or Windows Sandbox is on) was slow in specific, measurable ways. Most of the changes below fix that.
2. **A virtio-gpu device.** Linux guests get Vulkan on the host's graphics card, so a guest can run an LLM or other
   GPU compute on it.

Everything on this site was measured on one machine, and the pages say how. Nothing here is endorsed by or affiliated
with Oracle; VirtualBox is Oracle's trademark and the code stays under its original license.

## Speed, the headline numbers

All of these compare the same guest and the same host with the change off and on, on a Windows 11 host with 8 virtual
CPUs per guest. "Ping-pong" is how many times per second two threads can wake each other up, which is dominated by the
cost of interrupts and halts. The details, the caveats and the things that did **not** help are on the
[speed page](speed.html).

| Change | Guest | Result |
|---|---|---|
| Keep the x2APIC, let Hyper-V deliver interrupts, reliable timer kick | Linux | thread wake-ups **3.1k → 178k per second (57×)**, timers fire 17k → 68k per second and **416 → 68 µs late** |
| The same | Windows 10 | ping-pong **+32%** (26k → 35k per second) |
| Halt polling (spin briefly before sleeping) | Linux | ping-pong **+45%** more on top (median 173k → 256k per second) |
| The same | Windows 10 | ping-pong **+15–25%** |
| Cheaper MSR exits | any | an exit-causing RDMSR **16.8 → 8.5 µs**; Windows 10 ping-pong 35.6–44.3k → **47.4–49.8k** per second |
| Paravirtual EOI | Linux | 93% of end-of-interrupt writes skip the exit; memory exits 294–302k → **197–206k** per run |
| Stop Windows throttling minimized VMs | Windows 10, VM window minimized | ping-pong **+30%**, barrier **+55%**, timers **2.4k → 5.7k per second**, boot 94–97 s → **86–87 s** |
| High resolution waits | any | idle host CPU **1.1 → 0.5 cores** |

A real check: the long-running Fedora 45 VM (15 virtual CPUs) felt "so much faster" in daily use after these changes.
That is anecdotal, the numbers above are the measured part.

## The virtio-gpu device

A new graphics controller, **VirtioGpu**, with a 2D display and, when you tick *3D Acceleration*, Vulkan on the host
graphics card for Linux guests (through Mesa's Venus driver). The [virtio-gpu page](virtio-gpu.html) has the design, the
results and the limits. The ones that matter most:

| Measured on an RTX 5070 host, 4 virtual CPUs in the guest | Guest CPU | Host GPU through virtio-gpu |
|---|---|---|
| Qwen2.5 3B, prompt processing | 97 tokens/s | about **4,200–7,100** tokens/s (40–70× faster) |
| Qwen2.5 3B, generation | 23.5 tokens/s | **67–81** tokens/s (about 3× faster) |
| Qwen2.5 7B, prompt / generation | not measured | **3,696 / 60** tokens/s |
| Loading a 2 GB model | | **7 seconds** (it was 2 minutes 47 seconds before a cache-mapping fix) |

For ordinary desktop graphics VMSVGA is still better, and the page says so with numbers.

## Everything else

Fixes found along the way (a wrong source register in two AVX conversions, a reload of debug registers, an x2APIC
setting that wrongly turned the APIC off), macOS guest support work and a few test tools are in the
[full list of changes](changes.html), generated from git so it stays accurate.

## How these numbers were made

The guests ran under the development build without the VirtualBox kernel driver, which is how VirtualBox runs on this
PC with Hyper-V on; the official Windows build uses the driver, so absolute numbers can differ there. Where the stock
7.2.20 release was measured too, the speed page says so. Wherever a result was noisy or a change turned out to give
nothing, that is stated too.

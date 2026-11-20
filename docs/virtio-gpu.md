---
layout: default
title: virtio-gpu
---

# virtio-gpu: Vulkan on the host GPU for Linux guests

A new graphics controller, **VirtioGpu**. As a 2D display it replaces the VGA-style device. With **3D Acceleration** ticked
(Settings → Display, or `VBoxManage modifyvm <vm> --accelerate-3d on`), a Linux guest with the Mesa Venus driver sees the
host's graphics card as a Vulkan device, so it can run Vulkan programs and GPU compute such as llama.cpp.

## Results

Measured on an RTX 5070 host. The guest is Fedora 44 with 4 virtual CPUs; the CPU column runs the same model on those 4
virtual CPUs. Models are Qwen2.5 in 4-bit form, run with `llama-bench`.

| | Guest CPU | Host GPU through virtio-gpu |
|---|---|---|
| 0.5B, prompt / generation (tokens per second) | 483 / 108 | 5,218 / 74 |
| 3B, prompt / generation | 97 / 23.5 | **4,200–7,100 / 67–81** |
| 7B, prompt / generation | not measured | **3,696 / 60** |
| Loading a 2 GB model | | **7 s** |

* The 3B result is the range of three runs; the GPU was shared with the desktop. Prompt processing is roughly 40–70 times
  faster than the guest CPU and generation about 3 times.
* The smallest model generates *slower* on the GPU than on the CPU, because each generated token costs a fixed round trip
  between guest and host.
* A 4.4 GiB model (7B) fits and runs; the guest sees the card's 11.7 GiB of device memory.
* Loading took 2 minutes 47 seconds until a cache-mapping bug was found (below).

### Compared with running the model on the host

The same llama.cpp build (b12001) and the same models, run natively on the host card (Vulkan) and in the guest (5 runs each,
Fedora 44 live guest, 4 virtual CPUs, development build without the kernel driver):

| Model | | Host, native | Guest through virtio-gpu | Guest as a share of host |
|---|---|---|---|---|
| 3B | prompt (pp512) | 10,346 | 5,710 | 55 % |
| 3B | long prompt (pp4096) | 9,446 | 4,442 | 47 % |
| 3B | generation (tg128) | 241.7 | 52–79 | 21–33 % |
| 7B | prompt (pp512) | 4,410 | 3,108 | 70 % |
| 7B | long prompt (pp4096) | 4,217 | 2,994 | 71 % |
| 7B | generation (tg128) | 123.8 | 56.8 | 46 % |

* Prompt processing keeps most of the host's speed, more so for the larger model. The generation speed of the same guest
  varied between 52 and 79 tokens per second from one boot to the next; within one boot it was steady (about 5 %).
* Generation is limited by a fixed cost of about 10–15 ms per generated token, whatever the model size, so small models lose
  most. The device is not the cause: the renderer thread wakes about 10 µs (95 µs at worst) after the guest notifies it.
  The time goes to the guest sleeping in short steps while it waits for the GPU. A short sleep in a guest takes about
  0.5 ms (a 10 µs sleep measured 0.5 ms), on this build and also on the installed 7.2.20 with its kernel driver, so
  official builds should behave the same. This was not measured with virtio-gpu on an official build.
* Keeping the guest's virtual CPUs from halting (busy loops) raised generation by only about 13 %. Mesa's own
  `VN_PERF` switches made it slower (31 against 55 tokens per second), so the defaults are the fast setting.

### Other guest distributions

Live ISOs, llama.cpp 3B, 3 runs, 4 virtual CPUs. Every guest found the "Virtio-GPU Venus (NVIDIA GeForce RTX 5070)" device,
including the one with the oldest Mesa.

| Guest | Kernel | Mesa | Prompt (pp512) | Generation (tg128) |
|---|---|---|---|---|
| Arch 2026.10 | 7.2.7 | 26.3.1 | 7,184 | 99 |
| Ubuntu 26.10 | 7.3.0 | 26.2.4 | 7,141 | 107 |
| Fedora 44 | 7.x | 26.x | 5,070–5,710 | 52–79 |
| Pop!_OS 24.04 | 7.0.11 | 25.2.8 | 2,647 | 73 |

Ubuntu 26.10 needs `libssl3t64` for the Ubuntu llama.cpp build. Windows guests were not tested.

### Desktop graphics: use VMSVGA

For ordinary OpenGL desktop work VMSVGA is much faster, and a windowed Vulkan program is slower through virtio-gpu than on
the CPU renderer:

| glmark2, off-screen | Score |
|---|---|
| VMSVGA (host GPU through DX11) | about 15,100 |
| OpenGL on virtio-gpu (Zink, on the Intel GPU) | about 1,700 |
| CPU renderer (llvmpipe) | about 1,440 |

Zink also runs on the NVIDIA card, with a newer guest Mesa (see Limits). A shorter off-screen run (build, texture, shading
and terrain tests, one run) gave 1,434, 1,468, 1,446 and 792 frames per second. The terrain test, which is heavy on the GPU,
is about 46 times faster than the CPU renderer (17); the light tests are no faster than it, because every frame costs a
round trip to the host (about 0.7 ms).

Windowed vkmark scored 213–277 through virtio-gpu and 933 on the CPU renderer, because a windowed frame has to be copied
to the guest compositor. Off-screen Vulkan is fast (vkcube about 800 frames per second). So: virtio-gpu for compute and
off-screen Vulkan, VMSVGA for the desktop.

## How it works

* A virtio-gpu PCI device (virtio 1.0, modern only) with a control queue, a cursor queue and a 64-bit memory window.
* **2D:** the guest's frames are copied into a shadow buffer and shown by VirtualBox's display; the guest cursor shape is
  passed to the frontend.
* **3D:** Vulkan commands go through Mesa's Venus protocol to the *virglrenderer* library running in the VM process, which
  uses the host Vulkan driver. Memory the guest maps is shared with the host through the window; the window is address space
  only (default 1 GiB, `VBoxManage modifyvm --host-visible-size`).
* An EFI driver gives the firmware a display on the 64-bit EFI firmware (BIOS guests show nothing until Linux loads its
  driver).
* 3D is off by default because it loads the renderer library into the VM process. If the library is missing, the device
  stays 2D and says why in the release log.

## Bugs found while building it

* **Slow model load (fixed).** The guest maps the memory window in an area the CPU treats as uncached, and a "cached"
  mapping on top of that ends up uncached, so every store took about 0.8 µs (10 MB/s). Mapping it write-combining
  instead gave 20 GB/s after first touch and a 2 GB load in 7 seconds.
* **A graphics driver limit (worked around).** With an exportable handle requested, the NVIDIA driver refuses to allocate
  device-local, host-visible memory once an exportable system-memory block exists. The guest allocates asynchronously, so
  it then used an object that was never created, which killed its command ring. The renderer now does not advertise those
  memory types to the guest (staging buffers use system memory anyway). Reproduced with a 100-line host-only program.
* **Teardown order.** Linux closes its buffers after it destroys a context, and a guest can destroy a context while its
  memory is still mapped. The device now takes a context's memory out of the window and releases it first, and still
  answers the buffer commands that follow.

## Tested

Parallel Vulkan processes that create and destroy contexts and mappings in every order (including exiting with everything
mapped), `kill -9` at random, unbinding the guest driver under load, pausing the VM and resetting it. After every phase the
device was empty again and the VM was still running; on a fresh guest the guest log had no virtio-gpu errors.

* **Malformed commands:** 320,000 command streams with valid-looking headers and boundary-value arguments, sent straight to
  the renderer: no failures, no VM crash, no assertion. Most streams are rejected at the first command, so this covers the
  decoder's entry checks, not a valid ring.
* **Two guests at once:** two VMs running llama.cpp together shared the card (166 and 148 requests per second against 122
  for one alone); powering one off abruptly did not disturb the other.
* **Long run:** 42 minutes of back-to-back generation (1,500 runs): no errors, steady speed (about 5 % spread), and the device
  held no contexts or memory afterwards.
* **Host memory:** a guest that allocated and filled device memory in a loop took 11.5 GB of the host's 12 GB. Its process
  then aborted; the host, the VM and a host benchmark running at the same time were unaffected (238.8 against 241.7 tokens
  per second), and the memory came back when the process exited. There is **no per-VM cap**, so a guest can hold most of the
  card's memory while it runs. Allocations are not backed until used: four 4 GB allocations (more than the card) succeeded.

## Limits

* Linux guests with Mesa's Venus driver only (tested with Fedora 44, Arch, Ubuntu 26.10 and Pop!_OS 24.04; no Windows or macOS
  guest support).
* Generation speed is about a quarter to a half of running the model on the host (see above), because of the guest's slow
  short sleeps. Prompt processing is 55–70 %.
* The results were measured on a Windows host with an NVIDIA card and a development build without the kernel driver.
* The saved state of a VM that uses it cannot be saved yet, and there is one monitor.
* Zink (OpenGL over Vulkan) needs a recent guest Mesa. Fedora 44's 26.0.3 fails with "failed to choose pdev" (the Venus device
  does not tell it which display node it belongs to); after upgrading to Mesa 26.2.6 it picks the NVIDIA device and
  `glxinfo` reports the RTX 5070. Windowed OpenGL also needs `MESA_VK_WSI_DEBUG=sw` (below). Nothing in VirtualBox had to change.
* Windowed Vulkan to a software-rendered guest compositor needs `MESA_VK_WSI_DEBUG=sw` in the guest.
* A window larger than 1 GiB is untested.

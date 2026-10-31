---
layout: single
classes: wide
title: virtio-gpu
---

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

### Desktop graphics: use VMSVGA

For ordinary OpenGL desktop work VMSVGA is much faster, and a windowed Vulkan program is slower through virtio-gpu than on
the CPU renderer:

| glmark2, off-screen | Score |
|---|---|
| VMSVGA (host GPU through DX11) | about 15,100 |
| OpenGL on virtio-gpu (Zink, on the Intel GPU) | about 1,700 |
| CPU renderer (llvmpipe) | about 1,440 |

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

## Limits

* Linux guests with Mesa's Venus driver only (tested with Fedora 44; no Windows or macOS guest support).
* The saved state of a VM that uses it cannot be saved yet, and there is one monitor.
* Zink (OpenGL over Vulkan) cannot use the NVIDIA device: the guest Mesa does not tell it which device belongs to which
  display node.
* Windowed Vulkan to a software-rendered guest compositor needs `MESA_VK_WSI_DEBUG=sw` in the guest.
* A window larger than 1 GiB is untested.

---
layout: default
title: virtio-gpu
---

# virtio-gpu: Vulkan on the host GPU for Linux guests

A new graphics controller, **VirtioGpu**. As a 2D display it replaces the VGA-style device. With **3D Acceleration** ticked
(Settings → Display, or `VBoxManage modifyvm <vm> --accelerate-3d on`), a Linux guest with the Mesa Venus driver sees the
host's graphics card as a Vulkan device, so it can run Vulkan programs and GPU compute such as llama.cpp. It can also offer
OpenGL, which is what makes [stock SteamOS](#opengl-for-steamos-game-mode-and-desktop) work in Game mode.

## Results

Measured on an RTX 5070 host. The guest is Fedora 44 with 4 virtual CPUs; the CPU column runs the same model on those 4
virtual CPUs. Models are Qwen2.5 in 4-bit form, run with `llama-bench`.

| | Guest CPU | Host GPU through virtio-gpu |
|---|---|---|
| 0.5B, prompt / generation (tokens per second) | 483 / 108 | 5,218 / 74 |
| 3B, prompt / generation | 97 / 23.5 | **6,683 / 105** |
| 7B, prompt / generation | not measured | **3,808 / 71** |
| 14B, prompt / generation | not measured | **1,797 / 46** |
| Loading a 2 GB model | | **7 s** |

* Prompt processing is roughly 70 times faster than the guest CPU and generation about 4.5 times (3B). The 0.5B row was
  measured before the halt poll hint (below).
* The smallest model generates *slower* on the GPU than on the CPU, because each generated token costs a fixed round trip
  between guest and host.
* A 4.4 GiB model (7B) fits and runs; the guest sees the card's 11.7 GiB of device memory.
* Loading took 2 minutes 47 seconds until a cache-mapping bug was found (below).

### Compared with running the model on the host

The same llama.cpp build (b12001) and the same models, run natively on the host card (Vulkan) and in the guest (Fedora 44
live guest, 4 virtual CPUs, development build without the kernel driver). "Before" is without the halt poll hint described
below, "now" with it (5 or 10 runs each):

| Model | | Host, native | Guest before | Guest now | Now as a share of host |
|---|---|---|---|---|---|
| 3B | prompt (pp512) | 10,346 | 5,710 | 6,683 | 65 % |
| 3B | generation (tg128) | 241.7 | 52–79 | 105 | 44 % |
| 7B | prompt (pp512) | 4,410 | 3,108 | 3,808 | 86 % |
| 7B | generation (tg128) | 123.8 | 56.8 | 70.6 | 57 % |
| 14B | prompt (pp512) | 1,813 | | 1,797 | 99 % |
| 14B | generation (tg128) | 64.4 | | 45.6 | 71 % |

* The larger the model, the closer the guest gets to the host: the 14B model (8.4 GiB) processes prompts as fast as the host
  and generates at 71 %. Host VRAM peaked at 9.6 GB.
* Generation used to be limited by a fixed cost of about 10–15 ms per generated token. The device was not the cause (the
  renderer thread wakes about 10 µs after the guest notifies it): the guest waits for the GPU in short sleeps, and a short
  sleep in a VM took about 0.5 ms (a 10 µs sleep measured 0.5 ms, on this build and on the installed 7.2.20 with its kernel
  driver). While waiting, all four virtual CPUs were halted about 100 % of the time, 84 times per token.
* **The fix, the halt poll hint:** after each Vulkan command the device tells the VM, for 10 ms, that the guest will wait for
  the GPU shortly. A halted virtual CPU whose next timer is less than 1 ms away then polls until that timer instead of
  sleeping in the host, so it wakes on time. Generation went from 50–60 to 92–105 tokens per second (3B) and prompt
  processing did not get slower. It costs about half a host CPU core while the guest generates and nothing while it is
  idle; a sleep longer than 1 ms is left alone.
* The generation speed of the same guest still varies from one boot to the next (about ±15 %); within one boot it is
  steady. Mesa's own `VN_PERF` switches make it slower, so the defaults are the fast setting.

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

## OpenGL for SteamOS (Game mode and desktop)

Stock SteamOS showed a black screen in Game mode on the Vulkan-only device. Its compositor (gamescope) uses Vulkan, but
everything it shows (Xwayland, the Steam client, the overlay) draws with OpenGL, and the two could not exchange buffers. The
device can now offer OpenGL (virgl) next to Vulkan, and the renderer lets the Vulkan side use what the OpenGL side draws.

| Stock SteamOS guest, nothing changed inside it | Vulkan only | With OpenGL |
|---|---|---|
| Game mode (gamescope and the Steam interface) | black screen | the Steam interface (first-time setup, sign-in or the library), 40 to 70 seconds after power-on |
| Plasma desktop | works | works, drawn with host OpenGL |
| Keyboard and mouse in Game mode | | work |

* Tested with SteamOS 3.8.14 and 3.9.5 on a Windows 11 host with an RTX 5070. The guest updated itself from 3.8 to 3.9
  through the first-time setup.
* **Turn it on** per VM: `VBoxManage setextradata <vm> VBoxInternal/Devices/virtio-gpu/0/Config/Virgl 1`. It is off by
  default, needs 3D Acceleration, and works on Windows hosts only so far.
* It needs the renderer library built from the branch
  [`vbox-venus-virgl`](https://github.com/ctrondlp/winq-emu-virglrenderer/tree/vbox-venus-virgl) of the renderer fork.
  The renderer changes are offered to the renderer's maintainer as
  [pull request 2, "Share OpenGL (vrend) textures with Venus on Windows"](https://github.com/cmspam/winq-emu-virglrenderer/pull/2)
  (open, not merged yet).

How it works:

* The device offers the two OpenGL capability sets and the commands that go with them: OpenGL resources, transfers between
  a resource and guest memory, and OpenGL contexts. The renderer library runs them on OpenGL contexts of the host driver.
* **Shared buffers without copies.** A texture the guest marks as shared or as a screen buffer gets its storage from Vulkan
  memory. When the guest's Vulkan compositor imports that buffer, the renderer gives it the same memory, with an image laid
  out the way the OpenGL driver stores it. The guest believes the buffer is a plain row-by-row image; the host replaces
  that image when it is bound.
* **A desktop drawn with OpenGL** (Plasma) is read back from the host card into the display's shadow buffer whenever the
  guest flushes a part of the screen.
* **A graphics driver fault found on the way.** The NVIDIA driver lost the Vulkan device (event 13 in the Windows system
  log) when a buffer imported this way was handed to a compute-only queue marked as coming from a "foreign" owner, which is
  how gamescope hands buffers over. Marking it as "external" instead avoids the fault. Reproduced with a stand-alone host
  program before it was worked around in the renderer.
* **An intermittent black screen, fixed.** About half of the boots stayed black, and the NVIDIA OpenGL driver crashed now
  and then. The renderer remembers which of its OpenGL contexts is current, but creating a context leaves the new one
  current without updating that, so the next commands of the Steam interface ran in the wrong context, failed, and the
  renderer refused everything Steam sent from then on. Embedders are expected to call `virgl_renderer_force_ctx_0()`
  before every command, as QEMU does; the device did not. With the call, 30 boots in a row had no refused command and no
  driver crash.
* **Renderer errors reach the VM log.** The renderer's in-process Vulkan server replaced the process-wide logger with one
  that discarded everything, which is why the cause above stayed invisible. That is fixed in the renderer, and its warnings
  and errors now appear in the release log as `VirtioGpu: renderer ...` lines.

Checks on what a guest can make the host do:

* The guest's image must fit the texture's memory (size, offset, format and use), or the bind fails. Sharing is refused when
  the guest's Vulkan device is on another graphics card than the one OpenGL renders with.
* At most 65,536 OpenGL resources, one million guest pages kept mapped as their backing, and 4,096 shared textures at a
  time. The GPU memory limit (below) also covers the estimated size of the OpenGL resources: with a 64 MB limit the guest's
  further allocations were refused and the VM kept running.
* The device and renderer changes went through independent code reviews; the defects those found are fixed, a few
  minor notes (speed, clean-up at exit) are not.

Signing in to Steam and downloading a game work. Not tested yet: running a game, host graphics cards other than NVIDIA,
and Linux hosts (the device logs that OpenGL is not implemented there and stays with Vulkan).

## A simulated Wi-Fi adapter

SteamOS 3.8 has a bug on machines without a Wi-Fi adapter: when Steam starts in Game mode, a system helper stops the
network service, fails to create a Wi-Fi interface and exits before starting the service again. The first-time setup then
says "No networks found" although the wired connection works, and cannot continue. SteamOS 3.9.5 no longer does this.

A small new device, `virtio-wifi-sim`, gives Linux guests a Wi-Fi adapter without emulating radio hardware. It is the
virtio transport of the simulated radio driver Linux already ships (`mac80211_hwsim`): the guest loads the driver because
the device is there and gets a Wi-Fi interface that never finds a network. Frames the guest transmits are dropped unread.

* With it, the first-time setup of stock SteamOS 3.8.14 lists "Wired connection 1" and continues.
* It is added per VM with extra data (there is no setting in the GUI yet):
  `VBoxManage setextradata <vm> VBoxInternal/Devices/virtio-wifi-sim/0/Trusted 1`, and the same with `PCIBusNo 0`,
  `PCIDeviceNo 28` and `PCIFunctionNo 0`.
* Not tested: saving the VM state with it, and guests other than SteamOS.

## Settings

* **VirtioGpu as the display:** `VBoxManage modifyvm <vm> --graphicscontroller virtiogpu --accelerate-3d on`.
* **Compute GPU next to VMSVGA:** `VBoxManage modifyvm <vm> --compute-gpu on` (or *Compute GPU* in Settings → Display)
  adds a second, headless virtio-gpu for Vulkan, while VMSVGA keeps the desktop fast.
* **Host GPU memory limit:** `VBoxManage modifyvm <vm> --gpu-memory-limit <MB>` (or *GPU Memory Limit* in the Display
  settings), 0 for none. An allocation over the limit fails in the guest as it does when the card is full.
* `VBoxManage debugvm <vm> info virtiogpu` shows how much host GPU memory the guest holds against the limit.
* **OpenGL next to Vulkan:** `VBoxManage setextradata <vm> VBoxInternal/Devices/virtio-gpu/0/Config/Virgl 1`
  ([above](#opengl-for-steamos-game-mode-and-desktop)).

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
* **Halt poll hint:** a new device helper (`PDMDevHlpVMHaltPollHint`) lets a device tell the VM that work it started will
  finish soon; the halt code then polls short sleeps instead of blocking (see Results). It is in both halt methods: the
  one the development build uses without the kernel driver (measured above) and the one official builds use with it (not
  measured yet). `/PROF/CPU*/VM/Halt/PollHinted` in the VM statistics counts the halts it turned into polling, about 29 per
  generated token in the 3B test. `/VMM/HaltPollHintMax` sets how short a sleep has to be (default 1 ms); on hosts with
  fewer than two CPUs per virtual CPU hints are ignored.
* **Saved state:** the 2D state (resources with their guest memory, the scanout, the queues) is saved. The Vulkan state lives
  in the host graphics driver and cannot be saved, so saving is refused while a guest program uses it, and VBoxManage and
  the GUI say why ("virtio-gpu cannot save the VM state while the guest uses the host GPU (1 contexts, 4 buffers): close
  the programs that use Vulkan in the guest first"). Before, a device refusing to save only gave "Failed to save machine
  state" without a reason; that fix in Main applies to every device.

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
* **Shared-memory rings:** the transport the guest's Vulkan driver uses, attacked from a guest program: ring layouts with
  boundary values (with a check that nothing outside the ring's control words is ever written), live rings fed malformed
  commands and impossible positions, bytes flipped by a second thread while the host reads them, the shared memory pulled
  away under a running ring, reply streams at the edge of the shared memory, and long valid traffic that wraps small
  rings many times. About 6,000 rounds and 3,000 rings: about 800 rings were stopped by malformed input as they should
  be, every valid wrapping run was processed completely, and there was no failure, no assertion and no VM crash. One
  finding, not a bug: as upstream does, a reply can run past its declared stream up to the end of the guest's own shared
  memory, never beyond it.
* **Two guests at once:** two VMs running llama.cpp together shared the card (166 and 148 requests per second against 122
  for one alone); powering one off abruptly did not disturb the other.
* **Long run:** 42 minutes of back-to-back generation (1,500 runs): no errors, steady speed (about 5 % spread), and the device
  held no contexts or memory afterwards.
* **Host memory:** a guest that allocated and filled device memory in a loop took 11.5 GB of the host's 12 GB. Its process
  then aborted; the host, the VM and a host benchmark running at the same time were unaffected (238.8 against 241.7 tokens
  per second), and the memory came back when the process exited. Allocations are not backed until used: four 4 GB
  allocations (more than the card) succeeded.
* **Memory limit:** with a 4 GB and a 6 GB limit the host's VRAM use rose by 3.9 and about 6 GB. llama.cpp with the 14B
  model under the 6 GB limit printed `ErrorOutOfDeviceMemory` and "failed to load model" when the guest set
  `VN_PERF=no_async_mem_alloc`; without it, Mesa allocates asynchronously and the program stops instead. A renderer library
  without the limit still works; the device logs that it cannot enforce it.
* **Saving the VM state:** save and restore on a VirtioGpu VM with and without 3D, and on a VMSVGA VM with the compute GPU:
  a guest process kept running across the restore, the display redrew and took keyboard input, and Vulkan and llama.cpp
  worked afterwards. With a Vulkan program running the save was refused and the VM kept running; after the program exited
  the save worked.
* **Regression after these changes:** all fuzz suites again (two seeds) plus 120,000 more malformed command streams: no
  failures; the VMM and device code also build on Linux.

## Limits

* Linux guests with Mesa's Venus driver only (tested with Fedora 44, Arch, Ubuntu 26.10 and Pop!_OS 24.04; no Windows or macOS
  guest support).
* Generation speed is 44–71 % of running the model on the host (see above), more for larger models. Prompt processing is
  65–99 %.
* The results were measured on a Windows host with an NVIDIA card and a development build without the kernel driver.
* The VM state cannot be saved while a guest program uses Vulkan or OpenGL on the host card, and there is one monitor.
* OpenGL (virgl) works on Windows hosts only and was tested with an NVIDIA card and SteamOS guests only.
* Zink (OpenGL over Vulkan) needs a recent guest Mesa. Fedora 44's 26.0.3 fails with "failed to choose pdev" (the Venus device
  does not tell it which display node it belongs to); after upgrading to Mesa 26.2.6 it picks the NVIDIA device and
  `glxinfo` reports the RTX 5070. Windowed OpenGL also needs `MESA_VK_WSI_DEBUG=sw` (below). Nothing in VirtualBox had to change.
* Windowed Vulkan to a software-rendered guest compositor needs `MESA_VK_WSI_DEBUG=sw` in the guest.
* A window larger than 1 GiB is untested.

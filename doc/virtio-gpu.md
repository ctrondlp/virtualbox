# virtio-gpu and Vulkan (Venus) for VirtualBox on Windows hosts: progress notes

Branch: `virtio-gpu-device` (based on `local-changes`). Written 2026-10-29. This is a local project and is not
expected to be merged into Oracle's tree. Nothing here is pushed.

## Goal

Give Linux guests on a Windows host (Hyper-V "NEM" backend, no ring-0 driver) a real GPU: first a plain 2D
virtio-gpu display, then 3D with Vulkan through virtio-gpu "Venus" (guest Mesa Venus driver, host renderer
`virglrenderer`).

Why: the existing VMSVGA 3D path gives guests OpenGL 4.1 on the host GPU but no hardware Vulkan, and it is limited by
guest-to-host submit exits (4 port exits per submit, about 4.6 us each, host GPU about 12-25 % busy). A virtio device
rings one doorbell per kick. Measurements are in `vbox-devtools/testkit/gpu-2026-10-29/` (outside the repo).

## What is built (commits on the branch, oldest first)

| Commit | What |
|---|---|
| EFI: Add the virtio-gpu graphics driver to the 64-bit firmware | `OvmfPkgX64.dsc/.fdf` include `VirtioGpuDxe` for VirtualBox; `FirmwareBin/VBoxEFI-amd64.fd` rebuilt (WSL, GCC release) |
| Devices/Graphics: Add a 2D virtio-gpu display device | `DevVirtioGpu.cpp/.h`, modern-only virtio on `VirtioCore`, PCI 1af4:1050, display class 0x03/0x80 |
| Main/Frontends: Add the VirtioGpu graphics controller type | API enum `GraphicsControllerType_VirtioGpu` (=5, new enum UUID), settings v1.22, `ConsoleImplConfigX86`, PCI slot 26, `VBoxManage --graphicscontroller virtiogpu`, GUI converter, manual, changelog |
| VirtIO: Add the shared memory region capability to VirtioCore | `VIRTIO_PCI_CAP_SHARED_MEMORY_CFG` (type 8), `cShmRegions/aShmRegions` |
| VMM/PGM,NEM,PDM: Map host memory into an MMIO2 window at run time | `PGMPHYS_MMIO2_FLAGS_HOST_WINDOW`, `PGMR3PhysMmio2Map/UnmapHostRange`, `NEMR3NotifyPhysHostRangeMap/Unmap` (WHvMapGpaRange), `PDMDevHlpMmio2Map/UnmapHostRange` (devhlp 71.1); also three small fixes in the MMIO2 code it touches |
| Devices/Graphics: Add the Venus 3D commands to virtio-gpu | capsets, contexts, SUBMIT_3D, blob resources (CREATE/MAP/UNMAP), fences; `DevVirtioGpuRenderer.cpp/.h` loads the renderer DLL at run time |

## Architecture

- **Device** (`src/VBox/Devices/Graphics/DevVirtioGpu.cpp`): ring-3 only. Control and cursor queues each have a worker
  thread. 2D resources are shadowed in host memory (always B8G8R8X8, converted at TRANSFER_TO_HOST_2D) and copied
  into Main's buffer by the display refresh timer, never by the worker. One scanout. No hardware cursor (cursor
  commands are accepted and ignored). Saving the VM state is refused. Limits: 8192x8192, 512 resources, 256 MB of
  shadows.
- **3D** is off by default. CFGM keys under `Devices/virtio-gpu/0/Config/`: `Venus` (bool, default false),
  `RendererPath` (absolute path of `libvirglrenderer-1.dll`, default next to the VirtualBox binaries),
  `HostVisibleSize` (power of two, 64 MB to 16 GB, default 1 GB), `MonitorCount`. If the library cannot be loaded or
  the host window cannot be created the device falls back to 2D before the guest sees any 3D feature bit.
- **Renderer** (`DevVirtioGpuRenderer.cpp`): all `virgl_renderer_*` calls come from one dedicated thread. Fence
  callbacks only record progress and wake the control worker, which writes the responses. No file descriptors cross
  the MinGW/MSVC boundary. The library is loaded with NO_UNLOAD.
- **Host memory window**: the host-visible blob memory the renderer maps (`vkMapMemory` pointers) is mapped into a
  64-bit prefetchable BAR (BAR4, virtio shared memory region id 1) page range by page range. A host-window MMIO2
  region has no backing of its own; unbacked pages share one private sink page. Unmapping runs in an EMT rendezvous
  and is refused while a page of the range is still locked by another device, so memory cannot be freed while the
  guest or a device can still reach it. Only the Windows x86 NEM backend without the driver supports it; everything
  else returns `VERR_NOT_SUPPORTED` and the device stays 2D.
- **Firmware**: 64-bit x86 EFI guests get a boot display from `VirtioGpuDxe` (only `OvmfPkgX64` was changed; the
  32-bit EFI image has no driver). BIOS guests get nothing until the guest OS loads its virtio-gpu driver (there is
  no VGA BIOS).
- **Fallback detail**: when the host window cannot be created the 3D feature bits are withdrawn, but the shared
  memory capability that VirtioCore already wrote still points at the absent BAR4. A Linux guest may log one
  harmless complaint about it.
- **Limits worth knowing**: one monitor only (Main accepts more, the VM then fails to start with a clear message);
  VirtioGpu on an ARM VM is accepted by the API and silently gets a plain VGA device; Main does not check the
  build options the device needs.

## Verified so far

- Build: full tree builds with 0 errors; no warnings in any file touched here (MSVC release, Windows).
- 2D device, as of commit d2a395e1e5d (the first version of the device, before the 3D rewrite): Fedora KDE live ISO
  (EFI, 4 GB, 4 vCPU) boots to the desktop on virtio-gpu; the firmware driver set the scanout (640x480, then
  1024x768) and the Linux kernel driver took over; 15128 control commands, 0 errors. glmark2 with the software
  renderer scores 739 on virtio-gpu and 739 on VMSVGA (same ISO and VM settings). The current HEAD device, which
  rewrote the shared command path for 3D, has NOT been run, not even in 2D mode.
- Firmware: the rebuilt image loads `VirtioGpuDxe` and boots the same ISO on the old display with no regression.
- Hyper-V gate test (`testkit/gpu-2026-10-29/whvmap`): WHvMapGpaRange accepts `vkMapMemory` pointers of every
  host-visible type on the RTX 5070 (including the device-local resizable-BAR type) and on the Intel iGPU, plus
  VirtualAlloc and MapViewOfFile memory; a real-mode guest program saw correct reads and writes; unmapping a range
  with holes succeeds; a VirtualAlloc pool imports into Vulkan with VK_EXT_external_memory_host (host-coherent types).

## Observations

- With Venus off the guest reports `-virgl -edid -resource_blob -host_visible -context_init`, 1 scanout and 0 cap sets
  (Fedora kernel 6.19.10), as designed. A Fedora live guest on the 2D device logged once a kernel WARNING from
  `drm_atomic_helper_wait_for_vblanks` (a vblank wait timeout in the fbdev damage worker, about 4 s after the driver
  loaded). The cause is not known: it may be the guest's vblank timer under the Hyper-V backend, or something the
  device should do differently. It did not stop the display from working.

## NOT verified

- macOS and Linux arm64 builds. (The branch was compiled on Linux/GCC in release and debug with -Werror, including
  the ring-0 PGM variant, the Linux NEM stubs and the structure size tests; the Windows debug/strict build was not.)
- That `VBoxEFI-amd64.fd` contains exactly what the DSC/FDF describe: the image was built locally with the GCC
  toolchain and replaced the checked-in one; it was only checked by booting.
- The memory-window code, the VirtioCore capability and the Venus device code have been compiled but never RUN. The
  build with them is not installed yet (the install needs a UAC click).
- Regression of existing VMs on the changed VMM code (PGM/NEM/PDM).
- Linux and ring-0 builds of the touched VMM files (the NEM stubs and `PGMAllPhys.cpp` in ring 0).
- Any real Venus run: no renderer DLL has been built (the build of the third-party fork was blocked pending the
  user's own command). Whether Mesa's Venus driver starts with our device, and GPU/guest coherency and ordering,
  are unknown. winq-emu reports ~5 FPS Vulkan under EFI boots (cause unknown) and an NVIDIA freeze (issue #2).
- Mouse, keyboard and window resizing on the 2D device; the pointer is probably invisible (no hardware cursor).
- The black screen seen during the live ISO's "Test this media" phase (text console on virtio-gpu not checked).

## Security notes

- The guest drives the host Vulkan driver through Venus. All guest values are validated and capped; the renderer
  must be treated as a large attack surface. Reports of real vulnerabilities go to secalert_us@oracle.com, never to
  GitHub.
- The community Windows renderer (virglrenderer fork, MIT) is third-party code that must be reviewed and patched
  before use; an unpatched build must not be used. The review notes and the patch series are kept outside this
  repository (`vbox-devtools/winq-src/patches/`) on purpose, and nothing from that library has been run on this
  machine yet.
- Anything that looks like a vulnerability found while working on this (in this code or in existing VirtualBox
  code) is deliberately not described here; see the reporting rules at the top of this section.

## Results with the real renderer (2026-10-30)

Venus works end to end on this machine (Windows host, Hyper-V backend, RTX 5070 + Intel iGPU, patched winq-emu
virglrenderer). A Fedora KDE live guest sees the host GPUs as Vulkan devices (`Virtio-GPU Venus (NVIDIA GeForce
RTX 5070)`, Vulkan 1.4, 11.66 GiB device-local heap, 4 GiB maximum storage buffer), `vkcube` renders on it, and the
device counters stay balanced (every deferred fence is signalled).

- **LLM inference in a guest** (llama.cpp prebuilt Vulkan build, guest CPU = 4 vCPUs, `llama-bench`):
  Qwen2.5-0.5B Q4: GPU 5218 tok/s prompt, 73.6 tok/s generation; CPU 483 / 107.8.
  Qwen2.5-3B Q4_K_M: GPU 3970 / 48.8; CPU 97 / 23.5 (41x prompt, 2.1x generation).
  Loading the 2 GB of weights through the device took about two minutes (not profiled).
- **Second-GPU mode**: the guest can have VMSVGA as its display and the virtio-gpu device as an extra compute GPU.
  It is configured through VBoxInternal extra data (`Devices/virtio-gpu/0/Config/Venus`, `RendererPath`,
  `HostVisibleSize`) and needs `Devices/virtio-gpu/0/Trusted` = 1, otherwise the device stays 2D.
  Restrict the host loader to one GPU with `VBoxManage startvm <vm> --putenv=VK_LOADER_DRIVERS_SELECT=nv-vk64.json`.
- **OpenGL**: VMSVGA scores about 15,100 in glmark2 off-screen in the same guest, software rendering about 1,440.
  Zink over Venus works only for the Intel Venus device (about 1,700): Zink cannot pick the NVIDIA Venus device
  because Venus devices expose no DRM identity (`MESA: ZINK: failed to choose pdev`).
- **Not working yet**: presenting Venus windows to a software-rendered guest compositor (the compositor rejects
  the dmabufs: `importing the supplied dmabufs failed`), so `vkmark` fails; `vkcube` works. The device is idle in
  that case, so it is a guest compositor / buffer sharing interaction and not a device error.

## Independent review (2026-10-29)

Four read-only reviews of the code (memory window, device, renderer wrapper, Main/VirtioCore/firmware config) were
done after the first build. Confirmed problems were fixed in follow-up commits (stale cached paging-structure
pointers after a window unmap, fail-open Hyper-V unmap, a deadlock when a device thread unmaps during suspend or
power off, an unbounded wait on a stuck renderer call, `VIRGL_DISABLE_MT`, a debug-log NULL dereference).
Findings that were judged low priority and are NOT fixed:

- The display copy is not serialized with Main's resize at power-down (needs a dedicated display lock held across
  the resize and the copy and taken in `pfnSetRenderVRAM`).
- Completions of commands in flight across a guest driver reset can land on the new used ring (needs a reset
  generation counter); only the guest itself is affected, but strict builds assert.
- Other vCPUs may keep a stale IEM instruction buffer after a window unmap; only relevant if IEM_WITH_CODE_TLB is
  defined (arm64 builds only).
- Blob memory is not released when its creating context is destroyed; this is safe only if the renderer keeps blob
  memory alive independently of the context, which must be checked against the real library.
- A synchronous Cleanup request to a hung library can still block forever (calls other than the lock acquisition are
  not bounded).

## What is left

1. Install the build (`vbox-switch.ps1 -To Dev`, UAC) and regression-test existing VMs, then the 2D device again.
2. Boot the test VM with `Venus=true` and no DLL (fallback), then check `lspci -vv` for the shared memory
   capability and BAR4 where the host window can be created.
3. Build the patched renderer DLL (needs the user to run `winq-src/build-virgl.sh`), inspect imports and exports,
   apply the unpatched review items, then run it for the first time.
4. Venus end to end in a Linux guest: `vulkaninfo`, `vkcube`, then glmark2/vkmark through Zink; fix what breaks.
5. Stress and lifecycle: map/unmap races on 2+ vCPUs, BAR moved by the guest, VM reset, suspend/resume,
   power-off during rendering, many contexts.
6. Performance comparison against VMSVGA and llvmpipe, and the EFI-versus-BIOS question.
7. Hardware cursor, mode hints and resizing, input checks; decide about saving the VM state.
8. Security review of the whole path, including the renderer patches and the hardening (signed DLL) question.
9. Compile checks on Linux (WSL) for the VMM stubs and ring-0 PGM; run `scm` if this is ever offered upstream.
10. Merge into `local-changes` (only when the user says), remove the old worktree `vbox-wt/virtio-gpu-device`.

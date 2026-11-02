---
layout: default
title: Storage
---

# Storage

Three things: the legacy PC BIOS can now boot from an NVMe disk, how much the disk controller type and the host I/O cache
matter, and a fix so that a running virtio-SCSI VM can be saved and restored. Each item is one feature branch (see
[all changes](changes.html)). Numbers come from repeated runs of the same guest on the same host, the way the
[speed page](speed.html) describes.

**Test set-up.** Windows 11 host, VirtualBox running on Hyper-V (NEM), a Debian 13 guest with 8 virtual CPUs and 4 GB, the
same virtual disk moved between controllers. The tests used `fio` in the guest with direct I/O on a 1 GiB file, so they show
the cost of the virtual controller and of the host, not the speed of a particular drive.

## 1. Booting from NVMe with the BIOS firmware

*Branch:* `bios-nvme-boot` (*Planned for upstream*)

The PC BIOS had drivers for IDE, SATA (AHCI) and SCSI, but none for NVMe. A BIOS virtual machine whose disk was on an NVMe
controller stopped with "Could not read from the boot medium", so NVMe needed EFI firmware. Now the BIOS has an NVMe
driver.

How it works: the NVMe device already offers a pair of I/O ports (an index and a data port) through which the whole
register file can be reached, the same trick the AHCI device uses, so the 16-bit BIOS code needs no memory-mapped access.
The driver sets up one admin and one I/O queue, shows each namespace to the rest of the BIOS as a SCSI disk and turns
the few commands the BIOS issues (identify, read capacity, read, write) into NVMe commands. Transfers of up to 127 sectors
use PRP lists. It adds about 3 KB to the BIOS ROM and uses about 25 KB of conventional memory, only when the VM has an NVMe
controller. A VM without one behaves exactly as before.

What was checked:

* Debian 13 boots to its desktop from an NVMe disk with the BIOS firmware.
* A small self-checking boot sector writes patterns with INT 13h and reads them back, with single sectors, buffers that are
  not page aligned, buffers that span pages and 126-sector transfers, which need PRP lists. All of them pass.
* Windows 10 loads its boot manager and loader through the driver. It then needs its own NVMe driver to start at boot,
  which a Windows installation that has never run on NVMe does not do, as with any move to a new storage controller.
* The other controllers still boot with the new BIOS, and the boot times did not change.

Speed: the part of the boot between the BIOS handing over and the kernel starting (it includes the boot loader's menu
timeout) was about 0.4–0.6 s shorter than from SATA and about 0.35 s shorter than from virtio-SCSI, and very steady from
run to run. Total boot time to a working login did not change measurably, because the boot loader's reading is a small
part of it.

Limits: only 512-byte sectors, and the BIOS driver is only about booting. After the kernel starts it uses its own NVMe
driver.

## 2. Controller type and the host I/O cache

*No code change, a setting.*

4 KiB random reads, in operations per second (and the average latency of one read when the queue holds one request):

| Controller | Default (no host cache) queue of 1 | Default, queue of 32 | Host I/O cache on, queue of 1 | Host I/O cache on, queue of 32 |
|---|---|---|---|---|
| SATA (AHCI) | 5.3k (159 µs) | 15.0k | 8.7k (85 µs) | 15.8k |
| virtio-SCSI | 7.5k (115 µs) | 39.5k | 17.1k (43 µs) | 52.1k |
| NVMe | 9.1k (91 µs) | 48.2k | 15.1k (50 µs) | 55.2k |

* **The controller type matters a lot.** NVMe and virtio-SCSI are about 2.6–3.2 times faster than SATA with a deep queue.
  This is also why the BIOS can now boot from NVMe: a BIOS VM no longer has to use the slow controller to be bootable.
* **The host I/O cache is the other big lever.** SATA, NVMe and virtio-SCSI controllers default to no host I/O cache, so
  every guest I/O waits for the host drive. Measuring the NVMe device from the inside, the device's own code took about 10
  µs of each request and the wait for the host drive about 55 µs for a read and 27 µs for a write. Turning the cache on
  removed most of that wait: random reads with a queue of 1 went up by about 60% on NVMe and more than doubled on
  virtio-SCSI. Sequential reads of 128 KiB about doubled on all three.
* **What you give up.** A write the guest believes is done sits in host memory until the host writes it out, so a host
  crash or power loss can lose it. Flush requests from the guest are still honoured, and a clean shutdown loses nothing.
  The read numbers above are best case, because the test file was already in the host's memory.

Turn the cache on for one controller with `VBoxManage storagectl <vm> --name <controller> --hostiocache on`.

Why the NVMe device code itself was not optimised: only about 10 µs of a request is its own code, the rest is the wait for
the host drive. A read-ahead in the BIOS driver could cut the boot by about another half second, but it would cost 32 KB
of conventional memory, so it was not done.

## 3. Saving and restoring a running virtio-SCSI VM

*Branch:* `virtio-core-restore-queue-select` (*Planned for upstream*)

A VM with a virtio-SCSI controller (or any other device built on the shared VirtIO code) could be saved while it was
running, but could not be started again afterwards: loading the state failed with a configuration mismatch and the VM
ended up in the "aborted-saved" state. The cause is in the VirtIO core. A guest may select any queue number below the
maximum of 24, and Linux leaves the selector one past its last queue after probing, but the loader insisted that the saved
selector be below the number of queues the device actually has. It now checks against the same limit that the code which
accepts the guest's writes uses.

Saving and restoring, measured on a 4 GB Linux guest, with the fix:

* **Save state, then start again:** takes about 4 s, and the guest continues where it was, with its uptime and its files
  intact.
* **A snapshot taken while the VM is briefly paused,** then restored later: about 4 s, and the guest is back exactly at
  the snapshot.
* **`VBoxManage snapshot take --live`** (a snapshot that keeps the VM running) is very slow on Hyper-V (NEM) hosts, and
  the guest nearly stops while it runs. Saving the state while the guest runs requires watching every page of guest memory
  for changes, and the Hyper-V back end can only do that by unmapping every page and mapping the pages back one at a time
  as the guest touches them (about 2,000 pages per second here, so minutes for 4 GB). The graphical front end and
  `VBoxManage snapshot take` without `--live` pause the VM for a few seconds instead and are not affected.

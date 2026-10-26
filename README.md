# mt7925-macos-wifi

**MediaTek MT7925 (Wi-Fi 7, PCI `14c3:7925`) Wi-Fi for macOS on x86 Hackintosh.** On macOS 26 Tahoe and 15 Sequoia the card shows
up as a real Wi-Fi interface: the Wi-Fi menu, the network list in System Settings, and joining a network with its password.
No password goes into the kext.

[Tiếng Việt](README.vi.md) · **[Install guide](docs/GUIDE.md)** · [Roadmap](docs/ROADMAP.md) · [Releases](https://github.com/thanhnndev/mt7925-macos-wifi/releases) · [AirDrop / AWDL work](docs/ROADMAP-AWDL.md)

> [!WARNING]
> **Experimental, AI-written, tested on one laptop.** Almost all code and documentation was written by an AI (Claude), directed
> and tested by one person on one **ASUS ROG Zephyrus G14 2025 (GA403WM)**. Nobody has audited it. The kexts run in the kernel, so
> a bug can panic or hang the machine. Back up first and always keep another way online (USB Ethernet, USB tethering).
> No warranty, see [LICENSE](LICENSE).

## Status at a glance (2026-10-26)

| | macOS 26 Tahoe (`25G241`, 26.7.1) | macOS 15 Sequoia (`24H32`, 15.8.1) |
|---|---|---|
| **Native mode** (Wi-Fi menu, join from System Settings) | ✅ **works**. Download 176.6 Mb/s, upload 65.0 Mb/s (`networkQuality`), HE 160 MHz 2×2, DFS channels | ✅ works: 28-36 MB/s on one TCP stream, `networkQuality` 293 Mb/s down / 45 Mb/s up |
| **Legacy mode** (shows up as Ethernet, password injected into the kext) | not tested on Tahoe | ✅ stable, the longest-tested path |
| Where to get it | branch `main`, release **v2.x** | branch `sequoia`, release **v1.1** |
| AirDrop / AWDL / Continuity | 🔧 in progress on branch `dev/awdl` (off by default) | ❌ |

The native kext is tied to the macOS build it was written against: the Tahoe build is checked only on `25G241` and the
Sequoia build only on `24H32`. On any other build, check it first (see [GUIDE §7](docs/GUIDE.md#7-other-macos-builds)) before loading it.

## What works

- **Firmware and chip bring-up:** the MediaTek firmware is embedded in the kext, and the MAC address is read from the card.
- **Scanning:** 2.4 GHz channels 1-13 and all of 5 GHz (36-165), including the DFS channels 52-64 and 100-144. The AP table
  holds up to 128 networks, so 2.4 GHz networks are not pushed out of the list in crowded places.
- **Security:** WPA2-PSK with CCMP, plus WPA/WPA2 mixed networks (CCMP pairwise, TKIP group). The 4-way handshake runs in the
  driver, and in native mode macOS supplies the key.
- **Speed:** a link ladder from level 0 to level 6 covers HT, VHT and HE, 160 MHz, 2 spatial streams and Block Ack. If a network
  fails the 4-way handshake at a high level, the driver steps down a level by itself.
- **Native macOS integration:** an `IO80211Controller` and a Skywalk interface. You get the Wi-Fi menu, the System Settings list,
  RSSI and rate, link-quality events (so macOS does not drop the link after 60 s), CoreCapture, and the NSS ioctl.
- **Recovery:** when the link is lost, the driver resets the chip, reloads the firmware and joins again.
- **ItlWiFi.app:** a menu bar app that scans, joins and runs a speed test, for the Ethernet-style legacy mode.
- **Host tests in CI:** WPA2 crypto vectors, HT/VHT/HE capability parsing, the Block Ack reorder buffer, and a check that the
  generated stubs match the headers.

## What does not work yet

WPA3 / SAE, open networks, enterprise (802.1X), 6 GHz, AirDrop / AWDL (work in progress), and sleep/wake (not verified).
Upload is slower than download. A-MSDU receive is off on purpose. Full list and plan: **[docs/ROADMAP.md](docs/ROADMAP.md)**.

## Quick start

1. Pick the release that matches your macOS (table above) from [Releases](https://github.com/thanhnndev/mt7925-macos-wifi/releases)
   and unzip `itlmt7925.kext` and `itlmt7925Native.kext`.
2. Copy both to `EFI/OC/Kexts` and add them to `Kernel → Add` in this order: **`itlmt7925.kext` first, then `itlmt7925Native.kext`**.
   Remove any other Wi-Fi kext for this card.
3. Add the boot-args `itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1 itlmt_fast=6`.
4. Reboot, then join your network from the Wi-Fi menu.

Every step with checks, troubleshooting and how to undo it: **[docs/GUIDE.md](docs/GUIDE.md)**.

## Branches and releases

| Branch | What it is |
|---|---|
| `main` | Current driver: macOS 26 Tahoe native mode plus the latest core fixes. Releases `v2.*` are built from here |
| `dev/awdl` | AirDrop / AWDL work (phase 3: transmit works; receive reaches Apple's peer manager). Only active with `itlmt_awdl=1`. May panic |
| `sequoia` | Frozen macOS 15 line, the source of `v1.0` / `v1.1` |

| Release | macOS | Contents |
|---|---|---|
| `v1.0` | 15 Sequoia | `itlmt7925.kext` (legacy mode only), `ItlWiFi.app`. Copied from `ga403wm-hackintosh` `v1` |
| `v1.1` | 15 Sequoia | `itlmt7925.kext` + `itlmt7925Native.kext` (native front end, A-MSDU off), `ItlWiFi.app`. Copied from `ga403wm-hackintosh` `kext-v1.1` |
| `v2.*` | 26 Tahoe | built by CI from `main` when a `v2*` tag is pushed |

## Hardware

Any x86 Mac or Hackintosh with an MT7925 card (PCI vendor/device `14c3:7925`) should match. Only the GA403WM has been tested,
so reports from other laptops are welcome: use the bug report template and attach `scripts/native-wifi-test.sh logs`.

The Bluetooth half of the same card is a separate driver (`MtkBtFw` + `BtLeDlFix`) and stays in
[ga403wm-hackintosh](https://github.com/thanhnndev/ga403wm-hackintosh).

## Repository layout

| Path | Contents |
|---|---|
| `kexts/itlmt7925/` | core driver: firmware download, DMA rings, MCU commands, scan/auth/assoc, WPA2, Block Ack, data path, legacy Ethernet front end |
| `kexts/itlmt7925Native/` | native front end: `IO80211Controller`, infrastructure interface, (AWDL interface on `dev/awdl`) |
| `macos/ItlWiFi/` | menu bar app for legacy mode |
| `fw/` | MediaTek firmware from linux-firmware (its own license, see `fw/README.md`) |
| `scripts/` | build, install and test helpers (`build.sh`, `native-wifi-test.sh`, `inject-creds.ps1`, ...) |
| `tools/` | ABI research tools (kernel collection readers, vtable/slot checkers, header generators) |
| `abi/native-wifi/` | IO80211 / Skywalk ABI dumps for `24H32` and `25G241` |
| `docs/` | guide, roadmap, design notes, the full hardware test log (`TEST-NATIVE-WIFI.md`) |
| `test/` | host unit tests |

## History

This driver was developed inside [thanhnndev/ga403wm-hackintosh](https://github.com/thanhnndev/ga403wm-hackintosh) until
2026-10-26 and was moved here with its full commit history (only the Wi-Fi files were kept). Older links and issues live there.

## License and credits

Code: [Clear BSD License](LICENSE). The MT7925 hardware layer is re-implemented from the Linux `mt76` driver (© MediaTek,
BSD-3-Clause-Clear). The firmware in `fw/` is under MediaTek's own license (`fw/LICENCE.mediatek`). The project is clean-room:
no code from `itlwm`, `AirportItlwm`, OWL or OpenDrop (all GPL). Apple-side layouts come from read-only inspection of
macOS binaries. Details: [NOTICE.md](NOTICE.md).

# Why the Wi-Fi card shows up as "Ethernet", and what it would take to change that

`itlmt7925` registers an `IOEthernetController` and publishes an `en*` interface, so System Settings lists it as **Ethernet**.
Networking works (DHCP, DNS, TCP); what is missing is the Wi-Fi *classification*, the menu bar item and the System Settings
Wi-Fi pane.

## What decides "Wi-Fi" (verified in Apple's source)

`configd` (`SystemConfiguration.fproj/SCNetworkInterface.c`, `processNetworkInterface`) looks at the **controller** that
provides the `IONetworkInterface`. For an Ethernet-type interface it sets the type to `IEEE80211` only if that controller
conforms to one of three class names:

```c
if (IOObjectConformsTo(controller, "IO80211Controller") ||
    IOObjectConformsTo(controller, "AirPortPCI") ||
    IOObjectConformsTo(controller, "AirPortDriver")) {
    interface_type = kSCNetworkInterfaceTypeIEEE80211;
```

There is no property, `Info.plist` key or `DeviceProperties` entry that changes this: it is a class-hierarchy check
(`OSMetaClass` ancestry by name). Renaming the service in System Settings only changes the label.

## What macOS 15.6.1 has (read from the Recovery `BootKernelExtensions.kc`)

* `com.apple.iokit.IO80211Family` (the V2/Skywalk family), `com.apple.iokit.IOSkywalkFamily`, `AppleBCMWLAN*`.
* `IO80211Controller` with an exported vtable (`_ZTV17IO80211Controller`), `gMetaClass` and 231 exported methods, plus
  `IO80211SkywalkInterface` and `IO80211InfraInterface`. `AirPortDriver` does not exist.

## Options

| Option | Result | Cost / risk |
|---|---|---|
| **A. Stay Ethernet** (today) | works, no Wi-Fi menu, no CoreWLAN | none |
| **B. Cosmetic**: `networksetup -renamenetworkservice` plus a small status app | label only; still `IEEE80211`-less | small; no system integration |
| **C. Subclass `IO80211Controller`** (what OpenIntelWireless' `AirportItlwm` does) | System Settings shows Wi-Fi, scan and join from the UI, menu bar item | large: Skywalk interface creation, the `apple80211` ioctl set (scan, SSID, BSSID, channel, RSSI, state, join, power), the private vtable layout of *this* macOS build, `IOSkywalkFamily` lifecycle |

Deriving from `IO80211Controller` only to satisfy the name check is not a shortcut: the family's `start()` expects a
work queue and the Skywalk interface, so a half implementation risks panics.

## Notes on option C

* The private ABI is not in MacKernelSDK. `AirportItlwm` ships its own headers (`include/Airport/`) and a separate
  `Info.plist` per macOS generation (Monterey legacy family, Sonoma+ `IO80211Family` 1.5 + `IOSkywalkFamily`). Its last
  upstream release is 2.4.0 (June 2024); Sequoia reports exist but were not verified here.
* **License**: `itlwm` is GPL-2.0. This repository is Clear BSD and contains no itlwm code (see `NOTICE.md`). Option C would
  have to be written against Apple's binaries (`tools/kc.py` reads the vtable) rather than from the itlwm sources, or the
  result would be GPL.
* A reasonable plan: (1) extract the Sequoia vtable of `IO80211Controller`; (2) a controller class that owns today's
  `itlmt7925` data path and publishes the Skywalk interface; (3) implement only `SCAN_REQ`/`SCAN_RESULT`, `SSID`, `BSSID`,
  `CHANNEL`, `RSSI`, `STATE`, `ASSOCIATE`, `POWER`, `CARD_CAPABILITIES`; (4) test on the installed system, not Recovery.
* It also needs the NVRAM-free user interface the current driver lacks (credentials are compiled into the kext).

## Status (2026-10-11)

* Step 1 done: vtable of `IO80211Controller` for Darwin 24.6.0 and the 12 pure virtual slots are in `IO80211-ABI.md`.
* Option B done in code (2026-10-11, untested on hardware): runtime scan/join/disconnect through a user client and the
  ItlWiFi menu bar app (see `WIFI.md`, "Runtime control"). It also removes the "credentials only in the plist" blocker
  listed above. System Settings still says Ethernet.
* Next for option C: map `IO80211SkywalkInterface`, the work queue/command gate and the `apple80211_*_data` structs the same way, then a
  stub kext that registers a bare controller on the existing `itlmt7925` data path. Not started.


# mt7925-macos-wifi: `sequoia` branch (macOS 15)

**This branch is frozen.** It is the macOS 15 Sequoia line of the MT7925 driver, the source of releases
[`v1.0`](https://github.com/thanhnndev/mt7925-macos-wifi/releases/tag/v1.0) and
[`v1.1`](https://github.com/thanhnndev/mt7925-macos-wifi/releases/tag/v1.1). Development continues on
[`main`](https://github.com/thanhnndev/mt7925-macos-wifi) (macOS 26 Tahoe).

* The native front end here (`itlmt7925Native`) is built against the **24H32** (15.8.1) IO80211 ABI. Do not load it on macOS 26:
  set `MaxKernel 24.99.99` on its `Kernel → Add` entry.
* Install guide: [main/docs/GUIDE.md](https://github.com/thanhnndev/mt7925-macos-wifi/blob/main/docs/GUIDE.md) (pick release `v1.1`).
  This branch's own notes: [docs/WIFI-DRIVERS.md](docs/WIFI-DRIVERS.md), [docs/WIFI.md](docs/WIFI.md),
  [docs/TEST-NATIVE-WIFI.md](docs/TEST-NATIVE-WIFI.md).
* Build: `bash scripts/build.sh itlmt7925 itlmt7925Native` on macOS (the script also lists kexts of the old hackintosh repository
  that are not in this tree; build only these two).

Bug fixes made on `main` (cipher-aware keys for WPA/WPA2 mixed networks, the 128-entry AP table) are not in this branch.

# Which Wi-Fi driver to use: `itlmt7925` (works) or the native one (does not work yet)

Tiếng Việt: [bên dưới](#tiếng-việt).

There are **two** Wi-Fi drivers for the MT7925 in this project. They are not alternatives you can mix: the native one sits on top of
the legacy one. **Use the legacy driver. The native driver is an experiment and does not work yet.**

| | **Legacy driver `itlmt7925.kext`** | **Native driver `itlmt7925Native.kext`** |
|---|---|---|
| State | ✅ works, full link speed (HE, 160 MHz), WPA2-PSK, DHCP, 4 saved networks | 🔧 **experimental, being tested and built, does not work** |
| macOS sees it as | **Ethernet** (`en*`): no Wi-Fi menu, no System Settings Wi-Fi pane, no AirDrop | a real Wi-Fi interface (that is the goal; not reached) |
| Network name / password | injected into the kext's `Info.plist` ([INSTALL.md §4](INSTALL.md#4-wi-fi-inject-your-network)) or set from the ItlWiFi app | not decided yet (the goal is none in the kext) |
| Source in this repo | yes, `kexts/itlmt7925/` | **no** (work in progress elsewhere) |
| Built by `scripts/build.sh` / in release v1 | yes | **no** |
| Needed for | everything Wi-Fi today | nothing; leave it out |

## What to do

1. **Use the legacy driver**, as in [INSTALL.md](INSTALL.md#4-wi-fi-inject-your-network):
   * `itlmt7925.kext` is in `EFI/OC/Kexts` and enabled in `config.plist` (`Kernel -> Add`);
   * the boot-args contain `itlmt_m1=0 itlmt_m2=1 itlmt_m3=1 itlmt_fast=6` (what each one does: [WIFI.md](WIFI.md#boot-args));
   * there is **no** `itlmt7925Native.kext` entry, or its `Enabled` is `false`, and **no** `itlmt_native*` boot-arg.
2. Do not install, enable or "try" `itlmt7925Native.kext` unless you are the person developing it, and you have a way to get
   online without it (a wired adapter or USB tethering): with the experiment on, **this machine has no working Wi-Fi**.

## How to know which one is active

```bash
kextstat | grep -E "itlmt"                         # com.itlmt7925.kext = legacy; com.itlmt7925.native = native experiment loaded
ioreg -rc itlmt7925 -d1 | grep wifi-               # legacy: wifi-state should be "connected" (and wifi-ssid / wifi-rate ...)
ioreg -rc itlmt7925Wifi -d1 | head                 # exists only when the native class is loaded
sysctl -n kern.bootargs | tr ' ' '\n' | grep itlmt # the boot-args that are really in effect
```

* Legacy working: `wifi-state = "connected"` and an `en*` device listed as Ethernet in `networksetup -listallhardwareports`.
* Native experiment loaded: an `itlmt7925Wifi` class and an `itlmt7925WifiInterface` object exist, but nothing associates.
  On the reference unit on 2026-10-12 the boot-args `itlmt_native=1 itlmt_native_if=1` (without `itlmt_m2/m3`) gave
  `wifi-state = "off"`, an interface that never leaves the "not running" state and no network at all (the owner used USB tethering).

## Going back from the experiment to the working driver

Back up first. The ESP is the volume that holds `EFI/OC` (here `/Volumes/ESP`, mount it if needed); `tools/occonfig.py` writes a
`config.plist.<tag>` backup in the same folder before each change and `check` runs `ocvalidate`:

```bash
C=/Volumes/ESP/EFI/OC/config.plist
python3 tools/occonfig.py -c $C set 'Kernel/Add/[BundlePath=itlmt7925Native.kext]/Enabled' false --tag pre-legacy-wifi
python3 tools/occonfig.py -c $C args del --tag pre-legacy-wifi-args -- itlmt_native=1 itlmt_native_if=1
python3 tools/occonfig.py -c $C args add --tag pre-legacy-wifi-args2 -- itlmt_m2=1 itlmt_m3=1 itlmt_fast=6
python3 tools/occonfig.py -c $C check
```

Remove any other `itlmt_native_*` argument the same way (`args del -- itlmt_native_trace=1` and so on), keep `itlmt_m1=0`, reboot. If
the machine does not come back, pick the backup `config.plist.<tag>` from the ESP (copy it over `config.plist` from Windows, Linux or
the recovery shell). Never run NullMoth's "1401 Build" on a config you edited: it overwrites `config.plist`.

## What the native driver is, as far as is known

* A separate kext, `com.itlmt7925.native` 0.1.0, class `itlmt7925Wifi`, a subclass of `IO80211Controller`. It **matches on the legacy
  driver** (`IOProviderClass = itlmt7925`) and depends on `com.itlmt7925.kext`, so the legacy kext must be loaded as well.
* It needs `IO80211Family`, `IOSkywalkFamily` and `corecapture` (macOS 15 internals): another reason it is macOS 15 only.
* Boot-args it reads (names only; their meaning is not documented yet): `itlmt_native`, `itlmt_native_if`, `itlmt_native_publish`,
  `itlmt_native_role`, `itlmt_native_trace`, `itlmt_native_unit`, `itlmt_native_wl`. The registry property `itlmt_native_trace` on the
  `itlmt7925Wifi` object holds the step-by-step start log (`ioreg -rc itlmt7925Wifi -d1`).
* Design and research: [WIFI-AS-WIFI.md](WIFI-AS-WIFI.md), [IO80211-ABI.md](IO80211-ABI.md). Roadmap: workstream 6 and milestone M2
  in [ROADMAP.md](ROADMAP.md).

**When it will be the recommended one:** only when the Wi-Fi menu and System Settings pane work, it connects and keeps a
connection across sleep/reboot, reaches at least the legacy driver's speed, and the kext source is in this repository with a
release build. Until a release note says so, assume it is not.

---

## Tiếng Việt

Dự án có **hai** driver Wi-Fi cho MT7925. Chúng **không** thay thế lẫn nhau để trộn: driver native chạy chồng lên driver legacy.
**Hãy dùng driver legacy. Driver native chỉ là bản thử nghiệm và chưa chạy được.**

| | **Legacy `itlmt7925.kext`** | **Native `itlmt7925Native.kext`** |
|---|---|---|
| Trạng thái | ✅ chạy, đạt tốc độ tối đa của link (HE, 160 MHz), WPA2-PSK, DHCP, lưu 4 mạng | 🔧 **thử nghiệm, đang test và build, chưa chạy** |
| macOS thấy là | **Ethernet** (`en*`): không có menu Wi-Fi, không có mục Wi-Fi trong System Settings, không AirDrop | giao diện Wi-Fi thật (là mục tiêu, chưa đạt) |
| Tên mạng / mật khẩu | inject vào `Info.plist` của kext ([INSTALL.vi.md §4](INSTALL.vi.md#4-wi-fi-inject-mạng)) hoặc đặt từ app ItlWiFi | chưa chốt (mục tiêu: không để trong kext) |
| Mã nguồn trong repo | có, `kexts/itlmt7925/` | **không** (đang làm ở nơi khác) |
| Có trong `scripts/build.sh` / release v1 | có | **không** |
| Cần cho | mọi thứ liên quan Wi-Fi hiện nay | không cần gì; đừng đưa vào |

### Việc cần làm

1. **Dùng driver legacy**, theo [INSTALL.vi.md](INSTALL.vi.md#4-wi-fi-inject-mạng):
   * `itlmt7925.kext` nằm trong `EFI/OC/Kexts` và được bật trong `config.plist` (`Kernel -> Add`);
   * boot-args có `itlmt_m1=0 itlmt_m2=1 itlmt_m3=1 itlmt_fast=6` (ý nghĩa từng cái: [WIFI.md](WIFI.md#boot-args));
   * **không có** mục `itlmt7925Native.kext`, hoặc `Enabled` = `false`, và **không có** boot-arg `itlmt_native*`.
2. Đừng cài, bật hay "thử" `itlmt7925Native.kext` trừ khi bạn là người đang phát triển nó và có cách lên mạng khác (USB LAN hoặc
   tethering qua điện thoại): khi bật thử nghiệm, **máy không có Wi-Fi dùng được**.

### Cách biết đang chạy driver nào

Dùng đúng các lệnh ở phần tiếng Anh phía trên (`kextstat`, `ioreg -rc itlmt7925 -d1 | grep wifi-`, `ioreg -rc itlmt7925Wifi -d1`,
`sysctl -n kern.bootargs`):

* Legacy chạy tốt: `wifi-state = "connected"` và có một thiết bị `en*` hiện là Ethernet.
* Native thử nghiệm đã nạp: có class `itlmt7925Wifi` và object `itlmt7925WifiInterface`, nhưng không kết nối được gì. Trên máy tham chiếu
  ngày 2026-10-12, boot-args `itlmt_native=1 itlmt_native_if=1` (không có `itlmt_m2/m3`) cho `wifi-state = "off"`, giao diện không bao giờ
  chuyển sang trạng thái chạy, và không có mạng (chủ máy dùng USB tethering).

### Quay lại driver chạy được từ bản thử nghiệm

Sao lưu trước. `tools/occonfig.py` tự ghi bản `config.plist.<tag>` cùng thư mục trước mỗi lần sửa, `check` chạy `ocvalidate`. Dùng đúng
khối lệnh trong phần tiếng Anh ("Going back from the experiment"): tắt `itlmt7925Native.kext`, xoá `itlmt_native=1 itlmt_native_if=1`
(và mọi `itlmt_native_*` khác), thêm `itlmt_m2=1 itlmt_m3=1 itlmt_fast=6`, giữ `itlmt_m1=0`, rồi reboot. Nếu máy không lên, chép bản
backup `config.plist.<tag>` đè lên `config.plist` từ Windows, Linux hoặc recovery. Đừng chạy "1401 Build" của NullMoth trên config đã sửa
tay: nó ghi đè `config.plist`.

### Khi nào native trở thành bản được khuyên dùng

Chỉ khi menu Wi-Fi và mục Wi-Fi trong System Settings chạy, kết nối được và giữ kết nối qua sleep/reboot, tốc độ không thấp hơn
legacy, và mã nguồn kext đã có trong repo cùng bản build trong release. Cho đến khi release note nói như vậy, hãy coi như chưa.

# Handoff: native Wi-Fi (`IO80211Controller`) work, cloud session -> local macOS (2026-10-12; updated after stage 2)

Branch **`wifi-native-80211`** (based on `main` 7635608, contains all of main; newest work: round 4 + native stub stage 1). Continue on the Mac with
the installed macOS 15.8.1 (24H32, Darwin 24.6.0); the cloud session has no access to the kernel collection or the hardware.
No SMBIOS values, passphrases, MACs or neighbour SSIDs belong in this file or in `abi/`.

## Goal and ground rules

System Settings shows the MT7925 as **Wi-Fi** (menu bar, scan/join from the UI) instead of Ethernet: `itlmt7925` must expose an
`IO80211Controller` subclass. Plan, milestones and open questions: [NATIVE-WIFI.md](NATIVE-WIFI.md). Background: [WIFI-AS-WIFI.md](WIFI-AS-WIFI.md).

* **Clean room.** `itlwm` / `AirportItlwm` are GPL-2.0 (this repo is Clear BSD): do not read, copy or paraphrase them. Evidence comes
  from Apple's binaries (symbols, vtables, disassembly) via `tools/kc.py` and friends.
* **Behind a switch.** The proven data path stays untouched; the native class is a *separate kext* (`itlmt7925Native`, matches the
  core driver) and does anything only with boot-arg `itlmt_native=1`; the core kext's Info.plist and binary are not changed. A panic on the native path must never cost a working machine; test on the installed system with the old kext on a
  stick as the way back.
* **Every ABI fact is per macOS build** (hash of the KC in `abi/native-wifi/24H32/source.json`). New macOS update = new dump.
* Read-only extraction: no `sudo`, no kext load, no NVRAM changes while collecting. Do not commit the `.kc`.

## What exists on the branch

| Path | What |
|---|---|
| `docs/NATIVE-WIFI.md` | plan, milestones, extraction commands, open questions, running status |
| `docs/IO80211-ABI-2.md` | extraction rounds 1-4 and analysis (controller `start` contract, interface/data-path registration, request routing, feasibility; round 4 = logger options, RegistrationInfo, queue/pool contract, header verification) |
| `abi/native-wifi/24H32/`, `round3/`, `round4/` | raw dumps: vtables, method lists, disassembly (~3 MB) |
| `tools/kc.py`, `kc_vtable.py` | read the KC: symbols, vtables, disassembly |
| `tools/collect_native_wifi_abi.py`, `dump_native_wifi_dispatch.py`, `dump_native_wifi_round4.py` | the collectors (the last two only accept the recorded KC hash) |
| `tools/annotate_vcalls.py` | names virtual calls (`call [reg+0xN]` -> slot) in a dump; the receiver class is not known, choose by what the object is |
| `tools/gen_io80211_header.py`, `check_io80211_header.py` (+ `io80211_abi.py`, `io80211_check.py`) | generate the headers and **verify** them: clang's vtable layout of a probe subclass equals Apple's. Default = Controller (463 slots, 0 mismatches); `--all` = the seven interface-side classes (0 mismatches each) |
| `kexts/itlmt7925Native/` | **stage-1 stub kext** `com.itlmt7925.native` (`itlmt7925Wifi : IO80211Controller`, idle unless `itlmt_native=1`, no interface yet); `corecapture_min.h` = the four factory functions it calls |
| `kexts/itlmt7925/io80211_compile_check.cpp`, `scripts/build.sh io80211check` / `itlmt7925Native` | compile-only header test with the kext flags; build target of the stub |
| `tools/check_kext_links.py` | read-only link check: every undefined symbol of a built kext must be exported by the KC from a declared library (a known-good kext as baseline) |
| `kexts/itlmt7925/io80211/*.h` | `IO80211Controller`, `IO80211WorkQueue` 0x50, `IO80211CommandGate` 0x18, `IOSkywalkInterface` (0xb0, SDK value), `IOSkywalkNetworkInterface` 0xd0, `IOSkywalkEthernetInterface` 0x110, `IO80211SkywalkInterface` 0x118, `IO80211InfraInterface` 0x120; generated, **not compiled into the kext yet** |

## Established (with evidence in the dumps)

* Instance sizes (MetaClass constructors): Controller 0x128, SkywalkInterface 0x118, InfraInterface 0x120, WorkQueue 0x50, CommandGate
  0x18, BCM core 0x130, BCM interface 0x130, IOSkywalkEthernetInterface 0x110, NetworkInterface 0xd0, PacketQueue 0xd8, PacketBufferPool
  0xb8, Tx submission 0x168, Rx completion 0x138, CCPipe/CCStream 0x90, CCLogStream 0x98, CCFaultReporter 0x90. The Controller's ivars
  pointer is at `this+0x120` (last 8 bytes), interfaces' at `+0x128`.
* `IO80211Controller::start` needs: a working `getWorkQueue()` (the subclass creates the queue **before** calling the base `start`),
  a **non-null `getLogger()`** (stored in a global without a null check, used at every log site), `CreatePostOffice()`, and it creates the
  command gate, timers, RNG agent and frame pool itself.
* Data path = IOSkywalkFamily packet queues and two `IOSkywalkPacketBufferPool`s registered via
  `IO80211InfraInterface::registerInfraEthernetInterface(RegistrationInfo*, IOSkywalkPacketQueue**, n, pool, pool)`; not `mbuf`s.
* Requests: `apple80211RouteInternal` indexes GET table `0xffffff80023e7770` / SET table `0xffffff80023e8900` by request ID (1..561). SET
  10 `SCAN_REQ`, GET 11 `SCAN_RESULT`, SET 20 `ASSOCIATE`, SET 22 `DISASSOCIATE`, SET 441 `WCL_SCAN_REQ`, SET 442 `WCL_ASSOCIATE`. The WCL
  wrappers call virtuals **on the interface** (slots 594/595 of the BCM interface = `AppleBCMWLANInfraProtocol::setWCL_*`), correcting
  round 2 which said controller. Card-specific codes `0x3f1..0x3f5` are a different namespace.
* `CCLogStream::withPipeAndName(pipe, name, opts)` can be called directly; it reads the log level (`CCStreamLogLevel`) at `opts+8`.
* **Round 4** (details and addresses in IO80211-ABI-2.md "Extraction round 4"): `CCPipeOptions` is 0x350 bytes (+0 = 0 selects
  `CCLogPipe`), `CCStreamOptions` 0x358 (+0 = 0 selects `CCLogStream`; the pipe is cast to `CCLogPipe` without a null check; levels
  `-1..7`/`0x7f`); Apple's own values are listed. `RegistrationInfo` is 0x130 bytes (version 1, size 0x130, MAC at +0x108), with
  defaults from the interface's virtuals. The driver supplies queues + pools only (`registerNetworkInterface` builds the queue set
  and logical link): one pool, `TxSubmission` x4 (ACs 0..3), `TxCompletion`, `RxCompletion`, common `RxSubmission`, optional multicast;
  default capacities `a=0x200, b=0x400, c=0x400`, pool 0x1300 packets of 0x800 bytes. Headers for the whole interface chain verify.

* **Stub stage 1** (IO80211-ABI-2.md "Native stub, stage 1"): built, slot-checked and link-checked, never loaded. New facts: a null
  fault reporter panics `IO80211Controller::start` just like a null logger; the object is an `IO80211FaultReporter` wrapping a
  `CCFaultReporter` on a `CCDataStream`; `isCommandProhibited` returns 0 = allowed.

## Stage 2 (done in the cloud, never loaded)

`itlmt7925WifiInterface : IO80211InfraProtocol` + pool + 4 Tx / TxComp / RxComp queues, created by `itlmt7925Wifi::makeInterface`
behind a second boot-arg `itlmt_native_if=1` (unit: `itlmt_native_unit`, default 9). Details, evidence (the
`createSkywalkInterface` sequence, `start(core, info)` = vtable slot 657, `attachInterface` 443 / `detachInterface` 444) and the
list of knowing gaps: IO80211-ABI-2.md "Native stub, stage 2". New tools: `tools/gen_native_iface.py` (191 pure-virtual stubs),
`tools/check_native_slots.py` (override slots against Apple's vtable: 209/0 and 19/0), `tools/check_native_symbols.py`.
Header generator gained non-virtual member declarations (`STATICS`): `setInterfaceRole/Id`, `getWorkQueue`, `initRegistrationInfo`,
`registerInfraEthernetInterface`.

**Build and link check on the Mac (done, commit 66092dd)**: `bash scripts/build.sh io80211check itlmt7925Native` (call it through `bash`:
the script has no execute bit) gives `io80211check: ok` and `build/itlmt7925Native.kext` (Mach-O x86_64). `KC=/System/Library/KernelCollections/BootKernelExtensions.kc
python3 tools/check_kext_links.py build/itlmt7925Native.kext build/itlmt7925.kext` exits 0. The only unresolved symbols are 6
`IONetworkController` ones that the KC does not export; they are missing in the baseline `itlmt7925.kext` as well (the checker reports
them as baseline-missing), so they are not new. `__ZN20IO80211InfraProtocolD2Ev`, the one suspect symbol, is exported by
`com.apple.iokit.IO80211Family` in the KC (0xffffff8002257bc8). It is absent from the round-5 dumps only because they list vtable and
method symbols, not the KC's full symbol table, so it is **no longer a blocker**. Next: the first load. Known gaps that a load may expose: all
four Tx queues have the default service class (no subclass to pass the AC table), queue callbacks do nothing, the common Rx
submission queue is not registered, the `createSkywalkInterface` caller / the primary interface's id are not known (id 0, role 2 used).

## Open (what blocks milestone 2, the stub that loads)

1. ~~Logger options~~, ~~RegistrationInfo + queue/pool contract~~, ~~interface-side headers~~: closed in round 4. What remains of them:
   field *names* of a few option words (not needed while Apple's values are used), `IOSkywalkInterface`'s own instance size
   (SDK value used), and nothing of this has run yet: **the first real test is the stub**.
2. Return types of virtuals are not in mangled names; the generator marks guesses. Only methods the stub overrides must be right.
3. What `AppleBCMWLANInfraProtocol`'s `setWCL_*`/`getWCL_*` expect and return (the stub must answer `IO80211InfraInterface`'s
   protocol slots); `IO80211SkywalkInterface::start` creates a pipe/stream and a peer manager itself - check what it needs from the
   controller (`getPostOffice`, entitlements, `wlan.entitlement` boot-arg seen in `start`).
4. Payload layouts of the `apple80211_*` structs (scan request/result, associate, capabilities); who owns the WPA2 4-way handshake on
   this macOS (decides whether `itl_wpa.cpp` is reused); the `OSBundleLibraries` needed (`com.apple.iokit.IO80211Family`, `IOSkywalkFamily`,
   `corecapture`) and their versions.

## Next steps, in order

1. ~~Extraction round 4~~ and ~~interface-side headers~~ are done (see IO80211-ABI-2.md).
2. ~~Headers compile with the kext flags~~ and ~~controller stub (work queue, logger, fault reporter) builds and links~~ are done.
3. ~~Stage 2 code~~ (done, see above). **Next: build + link-check on the Mac, then the first load** (see "Before the first load";
   `itlmt_native=1 itlmt_native_if=1`). Success = loads, shows under the IO80211 plane, System Settings lists a Wi-Fi service, no
   panic on load/unload/sleep. Then service classes for the Tx queues (subclass `IOSkywalkTxSubmissionQueue`).
4. Milestones 3-7 in NATIVE-WIFI.md.

## Before the first load

Stage A already ran once (2026-10-12) and panicked in `IONetworkController::start` (`ml_thread_policy(NULL)`: our `getWorkLoop()`
differed from the provider's and `IO80211WorkQueue::getThread` returns NULL); `itlmt7925Wifi::getWorkLoop()` now returns the
provider's loop (docs/TEST-NATIVE-WIFI.md result log). That path is fixed on paper only; whatever comes after it in `start` is untested.

Confidence for a first load (stage 1 + 2, honest estimate, nothing has run): the build, link check, vtable slots and symbol
presence are verified, but object lifetime of Family internals (`IO80211InfraProtocol` start/stop, queue enable on
`registerInfraEthernetInterface`, post office/peer manager) is only inferred from Apple's call order. Expect a clean failure
(start returns false, controller keeps working as Ethernet) more likely than a panic; a panic is possible in stop/teardown. That
is below the 90% the user asked for, so the first load stays an experiment on a stick, not a daily-driver change.

* Keep `itlmt7925Native.kext` out of `config.plist` until stage 2 is done; when testing, put it **after** `itlmt7925.kext` in
  `Kernel/Add` (it depends on it) and set `itlmt_native=1` only for that boot. Without the boot-arg it is inert (start returns false).
* Run `KC=... python3 tools/check_kext_links.py build/itlmt7925Native.kext build/itlmt7925.kext` after every build; a symbol that
  is not exported means a link failure when OpenCore rebuilds the collection.
* Test with a way back (the previous kext set on a stick); a panic on a daily driver is the cost of a wrong guess.

## Round 4 (done), stub stage 1 (done) and the next prompt

Round 4 ran as specified (`abi/native-wifi/24H32/round4/`, `IO80211-ABI-2.md` "Extraction round 4"). Not done: task 4, payload sizes
of the scan/associate requests (the wrappers only forward pointers; only `apple80211_capability_data` shows an extent, >= 0x18 bytes).
Stub stage 1 followed the same day (controller + logger + fault reporter, see ABI-2 "Native stub, stage 1").
Suggested prompt for the next session (stage 2, still without loading):

```text
Nhánh wifi-native-80211, worktree riêng. Đọc docs/HANDOFF-NATIVE-WIFI.md và docs/IO80211-ABI-2.md (round 4 + "Native stub,
stage 1"). Việc: trong kexts/itlmt7925Native thêm lớp itlmt7925WifiInterface : IO80211InfraInterface và tạo nó từ
itlmt7925Wifi::start (sau super::start): RegistrationInfo 0x130 byte theo ABI-2 §2 (MAC từ core driver, version 1, size 0x130),
1 pool + 4 TxSubmission + TxCompletion + RxCompletion + RxSubmission chung theo bảng queue §2, đăng ký bằng
registerInfraEthernetInterface, tear-down đúng thứ tự stop() của BCM. Mỗi virtual phải override đều kiểm slot như stage 1 (clang
-fdump-vtable-layouts so với vtable BCM). Chạy tools/check_kext_links.py. KHÔNG nạp kext, không sudo. Dừng và báo nếu một hàm
của interface cần kiểu/layout chưa có bằng chứng (ghi 'chưa xác định', đừng đoán).
```

## Housekeeping

* `main` is the only long-lived branch; `wifi-native-80211` is the working branch for this feature (merge into `main` only when the
  stub loads safely behind `itlmt_native`). `ccr-fc111298-ip8jp5`, `diag-esp-log`, `diag-mirror-m2m3` are retired.
* Run the generator/checker after any change: `python3 tools/check_io80211_header.py abi/native-wifi/24H32/round3` (needs `clang++`,
  `c++filt`, MacKernelSDK path via the 2nd argument or `$MACKERNELSDK`; the default path is only valid in the cloud sandbox).

# First load of the native Wi-Fi kext (`itlmt7925Native`)

Status: nothing in the native path has ever run. Stage 1 (controller) and stage 2 (interface + queues) are verified only up to
build, link check and vtable slots (HANDOFF-NATIVE-WIFI.md). Treat this as an experiment with a way back, not as an update.

## 0. Before you start

* Have a stick with a working copy of the current EFI (or `scripts/native-wifi-test.sh backup` output copied to it) and know how
  to boot from it (OpenCore picker, F8/Esc on the ASUS). A panic with `debug=0x100` in the boot-args halts instead of rebooting.
* Know that the box is offline for Wi-Fi if the experiment fails: keep the USB Ethernet / phone tether nearby.
* `nvram boot-args` shows what NVRAM overrides; if it is non-empty it wins over `config.plist` (the helper stores it in the backup).

## 1. Build

On the Mac, in the worktree: `bash scripts/build.sh itlmt7925 itlmt7925Native` (build/ contains both kexts), then
`KC=<path to the .kc> python3 tools/check_kext_links.py build/itlmt7925Native.kext build/itlmt7925.kext` must exit 0 with only
the 6 baseline-missing `IONetworkController` symbols. The native kext must be the build from the same commit as the core kext.

## 2. Backup (done automatically by install, can be run alone)

    scripts/esp-automount.sh          # mounts the ESP at /Volumes/ESP (password box)
    scripts/native-wifi-test.sh backup

Creates `~/itlmt-backup/EFI-<time>/` (+ `.tar.gz`, + `.nvram-boot-args.txt`). Copy the `.tar.gz` to the stick too.

## 3. Stage A: controller only (lowest risk)

    scripts/native-wifi-test.sh install build/itlmt7925Native.kext

This backs up again, copies the kext, adds it to `Kernel/Add` right after `itlmt7925.kext` and sets `itlmt_native=1` (stage A is
the default; `--stage b` and `--exclusive` are described in section 4). Reboot.

Expected: boots; the Ethernet-style Wi-Fi from the core kext is replaced by the native controller, no Wi-Fi in System Settings
yet (no interface). Collect: `scripts/native-wifi-test.sh logs`. Fail = panic or hang on boot, or the core kext lost its network.

## 4. Stage B: controller + interface

One path at a time: stage A ran next to the old Wi-Fi (the core joined and gave the Ethernet-style link). For stage B use the helper's
options instead of editing `config.plist` by hand:

    scripts/native-wifi-test.sh install build/itlmt7925Native.kext --stage b --exclusive

`--stage b` adds `itlmt_native_if=1` (default `--stage a` removes it); `--exclusive` removes `itlmt_m2`, `itlmt_m3` and `itlmt_fast`
from the boot-args, so the core loads (and with `itlmt_m1` brings the firmware up) but never scans/joins and creates no Ethernet
interface; the only network device left from this driver is the native one. No Wi-Fi at all then: use the phone tether. Going back =
`restore` (it puts the whole EFI, including the old boot-args, back). Without the core's `IOMACAddress` (no Ethernet front end) the
native interface uses a placeholder locally-administered MAC (`02:00:79:25:00:01`), which is expected in this mode.

Add `itlmt_native_if=1` back to `boot-args` (`itlmt_native_unit=N` is optional; default 9) and reboot. Look for:

* `kextstat | grep itlmt` shows both kexts;
* `ioreg -l | grep -i itlmt7925Wifi` shows the controller with a child interface (class `itlmt7925WifiInterface`);
* `ifconfig -a` has a new `en9`; System Settings > Network may list a Wi-Fi service (it will not connect: every handler answers
  unsupported, the radio is not driven yet).

A clean failure is also useful: the log line says which step of `makeInterface`/`startWithInfo` returned false.

## 5. Sleep / unload (only if B loaded)

Sleep and wake once; then reboot normally (do not unload by hand). Report whether either panics: teardown order is the least
verified part.

## 6. Go back

    scripts/native-wifi-test.sh restore            # newest backup; or: restore ~/itlmt-backup/EFI-<time>

Reboot. If the native boot-args stick: OpenCore picker > Reset NVRAM, or boot from the stick. By hand: remove
`itlmt7925Native.kext` from `EFI/OC/Kexts` and its `Kernel/Add` entry, and delete `itlmt_native=1 itlmt_native_if=1`.
A missing `boot-args` token alone makes the native kext inert (its `start` returns false), the core driver then works as before.

## 7. What to send back

`scripts/native-wifi-test.sh logs` output (kextstat, ioreg, `log show --last boot`, dmesg; remove SSIDs/MACs), the stage reached
(A/B/5), and for a panic the first 30 lines of the panic text (photo is fine). The ItlDiag `ITLLOG.BIN` on the ESP also has the
early log (`scripts/usblog.ps1 -Esp -Read` on Windows).

## Result log

* 2026-10-12, stage A, commit 5dca5d5: native kext did not load. Kernel: `library kext com.itlmt7925.kext not compatible with
  requested version 0.1.0` (0xdc00800e), because the core Info.plist had no `OSBundleCompatibleVersion`, so it was not a library
  kext. Native `start` was never reached; the Wi-Fi loss in that boot was **not** caused by the native code (cause not established;
  `itlmt_fast=6` and `nvrmacpibl=1` were also new in boot-args). Fix: core Info.plist now has `OSBundleCompatibleVersion`; `install`
  patches the ESP copy of the core Info.plist in place (credentials stay), no need to replace the core kext.
* Same boot, the Wi-Fi loss itself (core driver, native never loaded): scan works, then every join dies with
  `efuse hw type failed rc=-5` -> `hw-prep-fail` (`itlmt7925_pwr.cpp` efuseRead, the power-table code of a45e8c1 had never run on
  hardware). Fix: power tables are best effort (failure is logged and the join continues), and the rejected efuse event is logged
  (`efuse event rejected: len= tag= addr= want= valid=`) so the parser can be corrected from a real event. This needs a rebuilt
  **core** kext (replace it on the ESP, then re-inject the credentials with `scripts/inject-creds.ps1`).
* 2026-10-12, stage A, commit 9339299 (native kext now links): **kernel panic at 0.47 s uptime**, before the filesystem is up
  (decoded from `AAPL,PanicInfo0000..0007`, 7-bit unpacking, in the user's tarball). Page fault CR2=RSI=RDI=0x648 (RBX=0) in
  `IOLockLock+0x3`, called from `IONetworkController::start+0x1d4` <- `IO80211Controller::start+0x41` (the `call [rax+0x5d0]`
  to the IONetworkController start) <- `itlmt7925Wifi::start+0x8c` (our `super::start`). So the work queue, CoreCapture logger and
  fault reporter built before it did not fault; `IONetworkController::start` locks a lock inside an object it holds as NULL
  (`lock = NULL + 0x648`). Which pointer is NULL is **chưa xác định** (candidates: a field `IONetworkController::init` fills, the
  provider being the core driver and not a PCI device, a global of IONetworkingFamily not ready at 0.47 s). Rolled back by the
  user (native `Enabled=False`, `itlmt_native*` removed). **Cause found from the round-6 dump** (`abi/native-wifi/24H32/round6/`):
  the `IOLockLock` is inside `ml_thread_policy(thread, 1, 0x10000001)`, and `thread` is NULL. `IONetworkController::start`
  (+0x1a0..0x1d4) compares `this->getWorkLoop()` with `provider->getWorkLoop()`; when they differ it calls
  `ml_thread_policy(workLoop->getThread(), ...)`. Our `getWorkLoop()` was `IO80211Controller`'s, which returns something other than
  the provider's loop, and `IO80211WorkQueue::getThread` (0xffffff80022591fa, 8 bytes between two method addresses) is a stub that
  returns NULL, so the call faulted at `thread+0x648`. Round 7 (`abi/native-wifi/24H32/round7/`): `IO80211Controller::getWorkLoop` is a pure tail call to virtual
  `getWorkQueue` (slot 399; Family default returns NULL, BCM returns `ivars+0x7898`), and `IO80211WorkQueue::getThread` is
  `xor eax,eax`. So Apple's BCM also returns an `IO80211WorkQueue` from `getWorkLoop`; it avoids the policy call only if its provider
  (the bus layer, which supplies that queue by default, see `createWorkQueue`) returns the *same* queue from `getWorkLoop`, so the
  compare matches. That is an inference (the bus class was not dumped): **chưa xác định**. Our provider is the core `itlmt7925`
  (plain `IOService::getWorkLoop` = the PCI device's loop), so the equal-loops condition is met by returning the provider's loop. Fix (commit after 4435061): `itlmt7925Wifi::getWorkLoop()` returns the
  provider's work loop (fallback: an own `IOWorkLoop`), so the compare matches and `ml_thread_policy` is skipped. Slots 20/0 and
  compile check pass. **Not run on hardware.** Residual risk: code in the IO80211 family that casts `getWorkLoop()` to an
  `IO80211WorkQueue` (none seen in the dumps read so far) and any further fault later in `IONetworkController::start` (after the policy call it creates an
  `IOCommandGate` and `addEventSource`s it to the loop returned by `getWorkLoop()`, i.e. now the provider's loop, which must stay
  enabled/running for the life of the controller).
* 2026-10-12, stage A, commit b6693cb (`docs/native-wifi-results/20261012-stage-a-b6693cb/`): **PASS, no panic.** `com.itlmt7925.native`
  loaded, `itlmt7925Wifi` registered/matched/active, `kern.bootargs` had `itlmt_native=1` only. Confirms the `getWorkLoop()` fix
  (`ml_thread_policy` path skipped, command gate on the provider's loop did not fault). The `controller started` IOLog line was not seen
  (registry entry is the evidence). Not tested: stage B (interface), sleep/wake, unload. The core joined and got DHCP (`RESULT rc=0
  link up`) without power tables: `efuse event rejected: len=124 tag=0002 addr=00000a70 want=00000a70 valid=00000001`. The event is the
  expected shape (address and valid match) but its tag is **2**, the parser demanded 1. Fix: `efuseRead` accepts tag 1 or 2 and logs
  the first 8 bytes of the block (`efuse a71 tag= block=`). First run of the CLC and tx-power commands on hardware follows from this,
  still best effort (failure is logged, join continues). Whether tag 2 puts the 16-byte block at the same offset is **chưa xác định**
  until the logged block is checked against the known encapsulation byte. Needs a rebuilt core kext.
* 2026-10-12, phase 2, stage B exclusive, commit 7428c5e (`docs/native-wifi-results/20261012-phase2-7428c5e/`): booted without
  panic; both kexts loaded and controller registered/matched/active. Child `itlmt7925WifiInterface` exists but is not
  registered/matched; no en9 and no service named Wi-Fi in networksetup. Native start/failure IOLog messages were absent from
  collected logs, so the precise publication/failure step is unknown. Sleep/wake not tested. Phase 1 power-table test was
  skipped at the user's direction; exclusive mode disables core scan/join. Persistent per-step logging is needed before retry.

* 2026-10-12, analysis of the phase-2 result (cloud, from `abi/native-wifi/24H32/round3|4`, no new hardware data): the interface child has
  only `IO80211InterfaceRole` in the registry. `IOSkywalkNetworkInterface::registerNetworkInterface` (round4 `-a`/`-b`) sets
  `IOInterfaceNamePrefix` (via `getBSDNamePrefix`, slot 318) and `IOInterfaceUnit` on the interface before anything is published, and
  neither is present, so the registration either was not reached or failed before that point. `IOService::attach(core)` keeps the
  child in the registry even after `detachInterface(ifc, false)` and `release()` (retain 4 fits both a failed and a successful
  start), so the object's presence says nothing about the outcome. The earlier "unverified" item is the strongest suspect:
  `registerNetworkInterface` builds a queue set with `IOSkywalkQueueSet::isWMMSchedModelCapable` (type 0xb if capable, else 3)
  and `createWithQueues`, failing with `0xe00002c2`; our four Tx queues all have service class 0, Apple's have
  {0, 0x00100080, 0x00380100, 0x00400180}. Not proven: `isWMMSchedModelCapable`/`createWithQueues` are not in the dumps yet
  (round 8 tool: `tools/dump_native_wifi_round8.py`). Role 2 is `IO80211InterfaceRole = LowLatency` in the registry; whether that is
  the intended role for the primary Wi-Fi interface is **chưa xác định**.
* Next build (commit after 7428c5e) records every step of `makeInterface`/`startWithInfo` in the registry property
  `itlmt_native_trace` of the controller (IOLog output was not found in any collected log, stage A or B). Read it with
  `ioreg -l | grep itlmt_native_trace` or `scripts/native-wifi-test.sh logs` (`native-trace.txt`). Expected last entry on success:
  `sw:registerInfraEthernetInterface=0; mk:startWithInfo ok`.
* 2026-10-12, correction after round 8 (`abi/native-wifi/24H32/round8/`): (1) the phase-2 registry extracts are filtered (the JSON keeps a fixed
  key set, the text file a grep window), so the absence of `IOInterfaceNamePrefix`/`IOMACAddress` there is **not** evidence that
  registration failed. (2) `isWMMSchedModelCapable` counts Tx queues per service class against {0x100080, 0, 0x380100, 0x400180} and
  needs each exactly once, so our four class-0 queues make it false, but `registerNetworkInterface` then uses queue-set type 3 and
  `IOSkywalkQueueSet::initWithQueues` only rejects a false result when info bit 0x8 is set (type 0xb). Service classes are therefore
  not the established blocker; no code change was made for them. (3) `copyRegistrationInfo` publishes `IOMACAddress` and the
  registration sets `IOInterfaceNamePrefix`/`IOInterfaceUnit` on the interface: the next log collection reads the full interface node
  (`native-interface-full.txt`) instead of a filtered one. One reboot of the already installed a8c7ae2 build is the cheapest next datum.

* 2026-10-12, Stage B exclusive trace build cf1bd62 (`docs/native-wifi-results/20261012-stageb-trace-cf1bd62/`): boot completed, no new panic reported. Persistent trace: `mk:withController=ok; mk:attachInterface=1; sw:IO80211InfraInterface::start=0; mk:startWithInfo failed`. Base interface start returns false before getWorkQueue, pool/queue creation or registration. WMM/service class is not the failing step in this boot. Controller active, interface child unregistered/unmatched, no en9. Sleep/wake not tested.

* 2026-10-12, analysis of the stage-B trace (static, `abi/native-wifi/24H32/IO80211SkywalkInterface-start.asm.txt`, `round4/AppleBCMWLANCore-createSkywalkInterface.asm.txt`, `round4/AppleBCMWLANSkywalkInterface-start-full.asm.txt`): `IO80211InfraInterface::start` runs `IO80211SkywalkInterface::start` first, and for `ivars->role == 2` that function needs `ivars->parentInterface` (`ivars+0x68`); with a NULL parent it logs a failure from `IO80211SkywalkInterface.cpp` line 0x1f8 (label 0xffffff8002261e4b) and returns false. Our interface was created with role 2 and no parent, which matches the trace exactly. In `createSkywalkInterface` the role comes from the creation parameters (`[params+0x18]`): role 2 builds `AppleBCMWLANLowLatencyInterface` (only when a bus flag is set) and receives the parent as the 4th argument; every other role builds `AppleBCMWLANSkywalkInterface`. So role 2 / "LowLatency" is a secondary interface; the primary station interface is role 1 (the role 1 and `ivars+0x58 == 1` branches exist in `IO80211InfraInterface::start`/`IO80211SkywalkInterface::start`: `IO80211Glue`, ScanManager). Which exact value Apple passes for the primary interface in `[params+0x18]` is **chưa xác định** from a caller; role 1 is inferred from those branches and from `AppleBCMWLANSkywalkInterface::start` (`getInterfaceRole() == 1` then `deferBSDAttach(true)`, vtable column 0xa08).
* Role 1 differences from the role-2 path (all from the same disassembly): `[info+0x10] = 1`; no BSD prefix/unit in the RegistrationInfo (`[info+0x30]/[+0x38]/[+0x40]` are only filled in the role-2 branch, `+0x40 |= 4`); the interface is created by `AppleBCMWLANSkywalkInterface::withProvider(core, id, role, ...)`. Code change (this commit): `makeInterface` uses role 1 by default (`itlmt_native_role=2` keeps the old path for comparison), `startWithInfo` calls `deferBSDAttach(true)` for role 1 before the base start (traced as `sw:deferBSDAttach=`), the BSD name is left to the family unless `itlmt_native_unit=N` is given. Not hardware-tested. The remaining failure points of `IO80211SkywalkInterface::start` that apply to role 1 are `createPeerManager` (vtable col 0xb10, not overridden by Apple's subclass) returning NULL, `IO80211FlowQueueDatabase::initialize`, `IO80211Glue::withOptions`, `IO80211MacAddressAgent::withOptions`; the trace will show only `sw:IO80211InfraInterface::start=0` again if one of them fails, then the next step is a finer split.
* Test for this build: the same exclusive stage-B install as before; read `native-trace.txt`. Expected on success: `mk:start role=1 ...; sw:deferBSDAttach=0; sw:IO80211InfraInterface::start=1; sw:getWorkQueue=ok; sw:pool+queues=1; ...; sw:registerInfraEthernetInterface=0; mk:startWithInfo ok`.

* 2026-10-12, Stage B exclusive role-1 build 4bd0819 (`docs/native-wifi-results/20261012-stageb-role1-4bd0819/`): reboot completed, no new panic reported. Trace confirms base start=1, pool/queues=1, addEventSource x6 ok, registerInfraEthernetInterface=0 and startWithInfo ok. Child role Infrastructure, IODeferBSDAttach=Yes; still unregistered/unmatched and no BSD unit/new en interface. Next: determine primary-interface BSD publication after deferral. Sleep/wake and teardown not tested.

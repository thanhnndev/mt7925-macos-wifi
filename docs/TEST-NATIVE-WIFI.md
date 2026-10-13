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

* 2026-10-12, Stage B role 1 (`docs/native-wifi-results/20261012-stageb-role1-4bd0819/`): trace complete, `mk:startWithInfo ok`; base start, pool/queues (class-0 Tx queues accepted), six event sources and `registerInfraEthernetInterface=0` all succeed. Interface child: `IOInterfaceNamePrefix=en`, `IOMACAddress` present, `IODeferBSDAttach=Yes`, !registered/!matched, no `IOInterfaceUnit`, no new en. Static reading (round3/4 dumps): `IOSkywalkNetworkInterface`'s `start` is plain `IOService::start` (vtable col 0x5d0), and the only caller of `deferBSDAttach` (col 0xa08) found so far is `AppleBCMWLANSkywalkInterface::start` with `true` for role 1. Who passes `false`, who calls `registerService` on the interface and what `deferBSDAttach` does exactly are **chưa xác định**; the dumps do not contain them. Next: `tools/dump_native_wifi_round9.py` on the Mac (read-only, no reboot) dumps `deferBSDAttach`, `prepare/finalizeBSDInterface`, the functions that reference the string `IODeferBSDAttach`, and every `call [vptr+0x9f8]` / `registerService`-slot call site in the IO80211/AppleBCMWLAN/IOSkywalk code.

* 2026-10-12, round 9 analysis (`abi/native-wifi/24H32/round9/`) and publication build: (1) `deferBSDAttach(true)` = `setProperty("IODeferBSDAttach", true)`; `deferBSDAttach(false)` = `removeProperty("IODeferBSDAttach")` + `registerService(0)`; `AppleBCMWLANCore::setupDriver` calls the latter on the object at `[core ivars+0x2c20]`. (2) `IOSkywalkNetworkBSDClient::start` needs the property not true **and** an `IOInterfaceUnit` OSNumber; it then builds "%s%u" (prefix col 0x9e0, unit col 0x9e8) and sets `BSD Name`/`IOInterfaceName`. (3) `registerNetworkInterface` sets `IOInterfaceUnit` only when `info+0x38 >= 0`, and `initRegistrationInfo` leaves it 0xFFFFFFFF; role 1 never fills it, so unit has to come from us. (4) `AppleBCMWLANSkywalkInterface::start` ends, on `registerInfraEthernetInterface == 0`, with `registerService(8)` and `setRunningState(false)`. Code change: role 1 now sets `info+0x38 = unit` (default 9, `itlmt_native_unit`), calls `registerService(kIOServiceAsynchronous)` + `setRunningState(false)` after a successful registration (traced `sw:registerService(8) done`, `sw:setRunningState(false) done`) and then `deferBSDAttach(false)` (traced `mk:deferBSDAttach(false)=%x`). Boot-arg `itlmt_native_publish`: 0 = none of these, 1 = registerService + setRunningState only, 2 (default) = also deferBSDAttach(false). Not hardware-tested.
* Test (one reboot, stage B exclusive, same install): read `native-trace.txt`; expected tail `sw:registerInfraEthernetInterface=0; sw:registerService(8) done; sw:setRunningState(false) done; mk:startWithInfo ok; mk:deferBSDAttach(false)=0`. Success signs: `IOInterfaceUnit=9` and no `IODeferBSDAttach` on the interface node, `en9` in `ifconfig`, a BSD client child. Risks (chưa xác định): a panic when the BSD client attaches/enables with our queues (no queue callbacks implemented yet), and no "Wi-Fi" service in System Settings (needs userland IO80211 integration). Fallback if it panics: boot with `itlmt_native_publish=1` to see whether registerService alone is safe.

* 2026-10-12, Stage B exclusive publish build 06840d9 (`docs/native-wifi-results/20261012-stageb-publish-06840d9/panic-decoded.txt`): **kernel panic** at 0.47 s of uptime, `"trying to send on thread panic \n" @IO80211Glue.cpp:417`, thread = IOConfigThread (`IOService::doServiceMatch`). Backtrace: `IOSkywalkNetworkBSDClient::start -> kern_nexus_controller_alloc_net_provider_instance -> prepareNexusCallback -> IOCommandGate::runAction -> gatedPrepareNexus -> IO80211InfraInterface::prepareBSDInterface -> updateStaticProperties+0x8e -> IO80211Glue::sendIOUCToWcl+0x2bd -> panic`. So the publication worked: the interface was registered, `IODeferBSDAttach` was cleared, `IOInterfaceUnit` was present and the BSD client got as far as `prepareBSDInterface`.
* Analysis of the panic (`round3/IO80211Glue-sendIOUCToWcl.asm.txt`): the function takes `W = [[glue+0x18]+0x38]` (an IOWorkLoop-derived object, vptr+0x130 = `IOWorkLoop::onThread`, vptr+0x138 = `IOWorkLoop::inGate` after the +0x10 column shift), and only proceeds when `W->inGate() == true && W->onThread() == false`; otherwise it logs "[wcl] sendIOUCToWcl: Trying to send iouc msg<%d> on thread=<%d> inGate=<%d>" (only when the CCLogStream accepts level 1) and panics. `prepareBSDInterface` runs inside the BSD client's `IOCommandGate::runAction`, so the check passes exactly when that gate belongs to the same loop W. W is most likely the `IO80211WorkQueue` we return from `getWorkQueue()`. Our controller's `getWorkLoop()` returns the provider's real IOWorkLoop (stage-A fix), a different object, so the gate the BSD client uses (if it follows `getWorkLoop()` through the registry) is not W's, `inGate()` is false. This is a hypothesis: **chưa xác định** which loop the BSD client's gate is attached to and what is stored at glue ivars+0x38 (`IO80211Glue::initWithOptions` and `IOSkywalkBSDClient` init are not in any dump yet).
* Build after 06840d9: default `itlmt_native_publish` is now 1 (registerService + setRunningState only, no `deferBSDAttach(false)`), so the BSD client does not attach and the panic path is not reached; installing it over the current Stage B setup is safe to boot. New trace line `mk:loops wq=.. provider=.. ctl=.. iface=.. wq.inGate=.. wq.onThread=..` records which work loop each layer returns. Experiment knobs, both opt-in: `itlmt_native_publish=2` (also `deferBSDAttach(false)`, panics unless the loop problem is solved) and `itlmt_native_wl=1` (after `IO80211Controller::start` returned, `getWorkLoop()` answers with the `IO80211WorkQueue` like Apple's controller; the early answer stays the provider's loop so the stage-A `ml_thread_policy(NULL)` panic does not return). Do not combine them until the round-10 dumps back the hypothesis.
* Round 10 (read-only, no reboot): `KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round10.py abi/native-wifi/24H32/round10` dumps `updateStaticProperties`, `IO80211InfraInterface::prepare/finalizeBSDInterface`, `IO80211Glue::initWithOptions/withOptions/runActionBlockOnSerial`, all `IOSkywalkNetworkBSDClient`/`IOSkywalkBSDClient` functions and every `getWorkLoop` of the IO80211/IOSkywalk/AppleBCMWLAN classes. It answers: which loop the BSD client's gate uses, what the Glue keeps at +0x38, and which ioctls `updateStaticProperties` sends (our `isCommandProhibited` refuses all of them for now).

* 2026-10-12, post-panic diagnostics (`docs/native-wifi-results/20261012-stageb-loops-2220380/`): current trace includes new loop diagnostics; wq differs from provider, ctl equals provider. The line is truncated at iface, so inGate/onThread are unavailable (96-byte per-entry trace buffer). Interface remains BSD-deferred, consistent with safe publish=1 default; no experimental wl=1/publish=2 retry. Round 10 and supplementary Glue dumps collected for cloud analysis.

* 2026-10-12, round 10 analysis (`abi/native-wifi/24H32/round10/`, static, high confidence) and the all-in build: (1) `sendIOUCToWcl` needs `W = glue+0x38 = interface ivars+0x38 = controller->getWorkQueue()` to satisfy `W->inGate() && !W->onThread()`. (2) The BSD client's `IOCommandGate` (`client+0x90`) is attached to the loop the `IOService::getWorkLoop` chain returns (interface -> controller -> `controller->getWorkLoop()`). (3) Apple's `IO80211Controller::getWorkLoop` tail-calls the `getWorkQueue` slot, so both are the same object; our `getWorkLoop()` returned the provider's loop (the stage-A `ml_thread_policy(NULL)` fix), so the gate belonged to a different loop and `inGate()` was false. Hardware trace (2220380) agrees: wq != provider, ctl == provider. (4) `IO80211WorkQueue` is an `IOWorkLoop` subclass whose `workQueue()` runs the virtual `init` (real gate lock and thread), so a command gate can be attached to it. (5) `updateStaticProperties` sends ioctl 0x2c (HARDWARE_VERSION) and 0x2b (DRIVER_VERSION) through `sendIOUCToWcl`, skips the property when the result is nonzero, and falls back to the controller getter only for result 0xe082280f, then calls `updateCountryCodeProperty(false)`.
* Code changes in this build: `getWorkLoop()` returns the `IO80211WorkQueue` once `start()` has finished (`itlmt_native_wl`, default 1; 0 = old provider-loop answer); the early answer stays the provider loop. `itlmt_native_publish` default is back to 2 (registerService + setRunningState + `deferBSDAttach(false)`). `isCommandProhibited` allows ioctl 0x2b/0x2c, `getDRIVER_VERSION`/`getHARDWARE_VERSION` return real strings. The trace buffer is 1536 bytes and the loop diagnostics are split into `lp:wq/prov`, `lp:ctl/if`, `lp:inGate/onThread/wl`.
* Test (one reboot, stage B exclusive, install the CI build over the current EFI without restoring): read `native-trace.txt`. Expected tail: `lp:wq=X prov=Y`, `lp:ctl=X if=..`, `lp:inGate=.. onThread=.. wl=1`, `sw:registerService(8) done; sw:setRunningState(false) done; mk:startWithInfo ok; mk:deferBSDAttach(false)=0`. Success signs: `en9` in `ifconfig`, no `IODeferBSDAttach` on the interface node, a BSD client child, no panic. Fallbacks: `itlmt_native_wl=0` (old loop answer) and/or `itlmt_native_publish=1` (no BSD attach). If it panics again the backtrace (not the trace) is the evidence. Likely next blockers (chưa xác định): queue callbacks on enable, `setInterfaceEnable`, `finalizeBSDInterface`, whether configd/airportd touch en9; `tools/dump_native_wifi_round11.py` (read-only, run with `KC=...`) dumps those paths and the ioctl routing.

* 2026-10-12, round 11 analysis (`abi/native-wifi/24H32/round11/`, static, build 24H32; CI build of 5e3afae is green, run 38269276468): **ioctl path** — `sendIOUCToWcl` (gate check `inGate() && !onThread()` on W, satisfied if `getWorkLoop()` == `getWorkQueue()`) -> `runActionBlockOnSerial` -> in Glue dqMode 2 (default) the caller queues the block and `commandSleep`s on the Glue gate (`ivars+0x70`, 50 s deadline) while the Glue's own `IO80211WorkQueue` (`ivars+0x58`) thread runs `processPendingIoucQueue` -> `receiveMessageInternal` -> `apple80211RouteInternal` -> `apple80211getXXX` -> interface `isCommandProhibited(type)` -> controller. A nonzero `isCommandProhibited` is returned as the ioctl result and `updateStaticProperties`/`updateCountryCodeProperty`/`updateLocaleProperty` just skip that property. So 0x2b/0x2c (version strings) succeed, 0x33 (country code), locale and the rest are refused with 0x3a harmlessly. **Finalize** — `finalizeBSDInterface` is teardown only (calls disable), it is not on the attach path. **Enable** — `IOSkywalkNetworkBSDClient::enableInterface/adjustInterfaceEnable` need the client's gate loop `inGate()`, then call interface col 0x860 (`IO80211SkywalkInterface::enable`), presumably -> `setInterfaceEnable(true)` (col 0x938; not verified) -> `IO80211MacAddressAgent::setInterfaceEnable`, `createIOReporters` (if none and `IOInterfaceName` exists) and, for role 1, an `IO80211AssociationJoinSnapshot` allocation.
* Main risk of the test (chưa xác định, cannot be settled statically): the first ioctl in dqMode 2 depends on `commandSleep`/`commandWakeup` and on the Glue's second `IO80211WorkQueue` thread really running; failure panics in `IO80211Glue.cpp:0x20c` ("IO80211 Sleep failed -> proably deadlock") after up to 50 s. Boot-arg `wlan.wclDqMode` (value other than 2) makes the Glue invoke blocks directly; untested, use only if that panic shows up. Other unknowns: body of `IO80211SkywalkInterface::enable/disable`, who calls `enableDatapath` (our stub returns `kIOReturnUnsupported`; no call site in any dump so far), `createIOReporters`, `IO80211MacAddressAgent::setInterfaceEnable`, `IO80211AssociationJoinSnapshot::withInterfaceAndOptions`, `commandSleep/commandWakeup`, `apple80211getHARDWARE_VERSION/COUNTRY_CODE`. Verdict: the 5e3afae build is fit to test as is; no code change from round 11.

* 2026-10-13, Stage B exclusive build 5e3afae (`docs/native-wifi-results/20261013-stageb-bsd-5e3afae/`): boot completed, no new panic reported; wq=controller=interface loop, deferBSDAttach(false)=0, en9 and active BSD client present, no deferral property. Static version properties populated. Native link remains down; ifconfig en9 reports SIOCGIFXMEDIA I/O error; no Wi-Fi service in networksetup. Trace sample inGate=0 is before publication, not a measurement inside the BSD gate. Sleep/wake and teardown not tested.

* 2026-10-13, Stage B exclusive build 5e3afae (`docs/native-wifi-results/20261013-stageb-bsd-5e3afae/`): **BSD attach works**, no panic. Trace reaches `mk:deferBSDAttach(false)=0`; `wq == ctl == iface`, provider loop differs; interface has `IOInterfaceUnit=9`, `BSD Name=en9`, no `IODeferBSDAttach`, an active `IOSkywalkNetworkBSDClient` child (with the full queue/pool reporter legend), and `ifconfig -l` lists en9. `IO80211DriverVersion` / `IO80211HardwareVersion` are populated on the interface node, which proves the whole ioctl chain of round 11 (`sendIOUCToWcl` -> Glue serial queue/`commandSleep` -> `apple80211RouteInternal` -> our `getDRIVER_VERSION/getHARDWARE_VERSION`) ran to completion on real hardware; so the dqMode-2 sleep/wake risk and the gate check did not bite. (`lp:inGate=0` in the trace is a sample taken outside any gate and says nothing about the BSD client's gate.) Not working yet: `ifconfig en9` shows `flags=8822<BROADCAST,SMART,SIMPLEX,MULTICAST>` (no UP/RUNNING), `options=400<CHANNEL_IO>` and fails with `SIOCGIFXMEDIA: Input/output error`; controller `IOLinkStatus=0`, `IOLinkSpeed=0`, `IOSelectedMedium=""`, `IOActiveMedium=""`; `networksetup -listallnetworkservices` has no Wi-Fi service.
* Static reading for the media error (round 3 + 11 dumps): `IO80211InfraInterface::processBSDCommand` first calls `IO80211SkywalkInterface::processBSDCommand`, which for non-802.11 ioctls tail-calls the super slot (asm vptr+0x920 = `IOSkywalkEthernetInterface/IOSkywalkNetworkInterface::processBSDCommand`), and only afterwards, for `0xc02c6938` (SIOCGIFXMEDIA), runs `IO80211InfraInterface::updateMediumStatus`. The latter returns void: it reads `IOSelectedMedium`/`IOActiveMedium` names from the controller's properties and looks them up in the controller's medium dictionary (controller vptr+0x890), skipping everything when the dictionary or the names are missing. So the EIO is returned by the base class (`IOSkywalkNetworkInterface::ioctl_gifmedia` at 0xffffff80029825d6 is the likely site; it is **not dumped yet**, as are `getSupportedMediaArray`, `getInitialMedia`, `handleChosenMedia`, `IOSkywalkEthernetInterface::processBSDCommand`/`enable`). Our interface overrides none of those media virtuals, and our controller publishes no medium dictionary (`IOSelectedMedium=""`). Whether the fix is a medium dictionary on the controller, an override on the interface, or a different property is **chưa xác định** until the dump. Enable: nothing has enabled the interface (flags have no UP); `IO80211SkywalkInterface::enable`, `AppleBCMWLANSkywalkInterface::enable/enableDatapath` and the userland (configd/airportd/CoreWLAN) expectations are **chưa xác định**. The existing Wi-Fi front end (core Ethernet path) is not involved.
* Next (read-only, no reboot): `KC=<BootKernelExtensions.kc> python3 tools/dump_native_wifi_round12.py abi/native-wifi/24H32/round12` dumps `IOSkywalkNetworkInterface` and `IOSkywalkEthernetInterface` (all functions, incl. `processBSDCommand`, `ioctl_gifmedia/sifmedia/sifflags`, `enable`, `getSupportedMediaArray`, `getInitialMedia`), `IO80211SkywalkInterface::enable/disable/createIOReporters/enableDatapath`, the BCM equivalents, `IO80211MacAddressAgent`, `IO80211AssociationJoinSnapshot`, `IO80211CommandGate::commandSleep/commandWakeup`, the HARDWARE_VERSION/COUNTRY_CODE getters and link-state/postMessage functions.

* 2026-10-13, local round 12 collection: 204 entries, zero missing; 12 supplemental executable media/datapath functions. Media EIO path identified: inherited IO80211 supported-media getter needs controller medium dictionary; ioctl_gifmedia converts getter failure to errno 5. Local fix publishes/selects one IEEE80211Auto medium, no active-link claim. Build/link check passed (six reserved baseline missing only); controller slots 21 correct, zero wrong. Not installed or hardware-tested. Details in abi/native-wifi/24H32/round12/README.md. Enable/datapath and userland integration remain unresolved.

## Local datapath experiment (2026-10-13, not hardware-tested)

The next build includes the medium dictionary fix, a TX/RX bridge to the existing core radio engine, and lifecycle/queue/packet-ownership traces. Build **both** kexts; old core does not implement the internal bridge. `itlmt_native_dp=1` enables core scan/auth/join with installed credentials but prevents legacy Ethernet creation. `itlmt_native_nvtrace=1` opts into bounded best-effort NVRAM checkpoint writes; kernel log and registry traces/counters are also collected. No native CoreWLAN scan/join or radio-power support is claimed.

Datapath ABI evidence: `abi/native-wifi/24H32/round13/`. Build and link check passed with only the six reserved baseline missing symbols; controller slots 21 correct/0 wrong, interface 213 correct/0 wrong. New tracing includes the first 32 distinct ioctl IDs without payloads. The logs command collects native checkpoints and panic chunks plus a private full core registry; sanitize before sharing.

After the next boot, first collect baseline logs and media/discovery, then enable en9 explicitly and inspect TX/RX/completion/link counters. A read-only live query already confirms explicit BSD lookup identifies en9 as IEEE80211; a service can be created with `build/native_wifi_service create <absolute-private-preferences-backup.plist>` under administrator privileges. It preserves existing services and refuses duplicate en9 creation. Do not run sleep/wake/unload tests before the new datapath has been assessed. If panic occurs, stop and collect backtrace plus NVRAM checkpoints; checkpoint persistence is not guaranteed.

* 2026-10-13, datapath c2e21ce boot: no panic; core join/efuse/CLC succeed, en9 media EIO fixed, explicit enable succeeds, IEEE80211 service created. DHCP remains INACTIVE and traffic counters zero; delivery unverified. Follow-up stable station identity/linkState/software checksum changes prepared. See docs/native-wifi-results/20261013-datapath-c2e21ce/README.md.

* 2026-10-13, c78492b boot: no panic, native auto-enable/core link up; DHCP still blocked by inactive BSD media, traffic counters zero. Follow-up corrects ifmediareq status after Family handling, applies station address and clears BSD offloads during prepare. Interface slots 217 correct/0 wrong; only six baseline symbols missing. See docs/native-wifi-results/20261013-datapath-c78492b/.

* 2026-10-13, 78852cc boot: no panic; en9 media active, DHCP transmits, TX/completion work. No lease; RX reaches bridge but first frame drops during allocation/validation/enqueue (substep not traced). Live MAC correction succeeded after prepare returned ENXIO. See docs/native-wifi-results/20261013-datapath-78852cc/.

## RX root investigation — 2026-10-13

Round14 identifies omitted packet preparation, missing Family station seed and missing Infra link-state transition. The next build replaces the earlier BSD media/MAC patches with proper lifecycle calls, supplies RX Ethernet metadata, and includes step-specific RX failure tracing. Build **both** core and native: link callback bridge ABI is now v2. Slots: controller 22/interface 214 correct, zero wrong; all generated interface headers have zero slot mismatches; link check permits only six reserved baseline symbols. Hardware outcome remains pending.

Expected next boot: fixed station identity without shell repair; `dp:Family link=1 begin gate=1 thread=0` followed by result=1/state=2; real media active without patched ifmediareq; `dp:RX prepare=0 buffers=0->1`, `dp:first RX delivered`, and a DHCP lease. Different before-count can occur for recycled objects; preparation return and after-count matter. If RX fails, send step/rc/packet/len trace plus counters. No CoreWLAN scan/join implementation or end-to-end networking success is claimed yet.

Legacy ItlWiFi login agent is disabled for this experiment; its prior plist and launchctl state are under `~/itlmt-backup/legacy-ui-20261013`. On returning to legacy mode after EFI restore, re-enable with `launchctl enable gui/$(id -u)/com.itlmt7925.ItlWiFi` and bootstrap its saved/current LaunchAgents plist if it is not loaded. No legacy Ethernet driver object was present; required core firmware/radio engine stays enabled.

## CARD_CAPABILITIES (ioctl 12) — 2026-10-13, build after 3b4ff24 (not hardware-tested)

* Live evidence on the 3b4ff24 boot (`itlmt_native=1 itlmt_native_if=1 itlmt_native_dp=1`): en9 `UP,RUNNING`, `status: inactive`; registry
  trace `ioctl:cmd=c prohibited` (also 0x216, 0x33, 0x1c) and `itlmt_ioctl_rejected` = 1409. CoreWLAN lists no interface and
  `SCNetworkInterfaceCopyAll()` does not return en9, although a direct BSD lookup of en9 reports IEEE80211. Apple's client library
  asks DRIVER_VERSION (0x2b) and then CARD_CAPABILITIES (0xc) when it binds an interface; the controller refused 0xc with 0x3a and
  `getCARD_CAPABILITIES` returned unsupported, so the bind can stop there. That the refusal is *the* reason Settings sees no Wi-Fi is
  **a hypothesis**: it is the only ioctl in the trace that the bind step needs and that we refuse; the next boot decides.
* Change (`itlmt7925_wifi.cpp`): `isCommandProhibited` also allows 0xc; `getCARD_CAPABILITIES` writes the 0x18-byte extent that
  `AppleBCMWLANCore::getCARD_CAPABILITIES` (0xffffff80015f16ca) writes, reporting only the bits Apple sets without consulting chip state
  (constants, two unconditional ORs, and the two bits set when BCM feature flags 1 and 7 are absent). Bits Apple derives from firmware
  features / chip revision / radio state stay clear. Which bit is which capability is **chưa xác định** (names live in userland).
* Build + link check: `bash scripts/build.sh itlmt7925Native` ok; `check_kext_links.py` exit 0, only the 6 baseline-missing symbols.
  No virtual signature changed (slots unaffected).
* Next boot, read: `ioctl:cmd=c allowed`, `ioctl:CARD_CAPABILITIES answered`; then which *new* ioctl IDs appear in the trace
  (first 32 distinct are recorded: the library's next requests after the bind are the next handlers to implement), and whether
  `networksetup -listallhardwareports` / CoreWLAN list en9. If still absent, the matching/user-client path airportd uses is the
  next thing to dump (not done).

## Full-flow call recorder (`itlmt_native_calls`) — 2026-10-13, build after 19e4999 (not hardware-tested)

Reason: a reboot is expensive and the old trace kept only the first 32 distinct ioctl IDs, nothing about GET/SET handlers or
callers, no counters. The recorder keeps one table (160 entries, ~8 KiB text) published as the registry property
`itlmt_native_calls` on the controller whenever an entry is new or its count reaches a power of two (so it is current at a panic-free
shutdown and loses at most a doubling of a count). `native-wifi-test.sh logs` already greps `itlmt_native_` into `native-trace.txt`.

Format: `<kind>:<hex id>[@<ra0>/<ra1>/<ra2>]x<count>;` ...

| kind | what | id |
|---|---|---|
| `I` | controller `isCommandProhibited(cmd)`, every request, allowed or refused | ioctl number (0xc CARD_CAPABILITIES, 0x13 POWER, 0xd STATE, 0xe PHY_MODE, 0xf OP_MODE, 0x1b SUPPORTED_CHANNELS, 0x1c LOCALE, 0x2b DRIVER_VERSION, 0x2c HARDWARE_VERSION, 0x33 COUNTRY_CODE) |
| `C` | controller handler actually reached (`getCARD_CAPABILITIES`, `getDRIVER_VERSION`, ...) | ioctl number; `0x8000000N` = set |
| `H` | `handleCardSpecific` | cmd, `0x80000000` set bit = set |
| `S` | interface protocol stub (one of the 191 unimplemented get/set handlers, all return unsupported) | vtable slot (467..656, see `round3/AppleBCMWLANSkywalkInterface.vtable.txt`) with 3 return addresses |
| `U` / `u` | controller `newUserClient` entry / result | type (0x57694669 = 'WiFi' firmware loader); `u` id = `type << 16 | result & 0xffff` |
| `W` / `w` | interface `newUserClient` entry / result | same as `U`/`u` |
| `B` | `processBSDCommand` | ioctl command word |
| `E` | interface `enable` / `disable` | 1 / 0 |

`ra0/ra1/ra2` are the three return addresses above the recording function (for `I` the Family function that called
`isCommandProhibited`, then its callers; for `S` the dispatcher above the stub), already corrected by the KASLR slide (runtime
address of the exported `IO80211WorkQueue::workQueue` minus its static 0xffffff8002258fcc) and truncated to the low 32 bits: prefix
`0xffffff80` is implied (`021f65a4` = `0xffffff80021f65a4`). Look them up with `tools/kc.py` (`kc.syms` nearest lower symbol) against
the same KC hash. A zero means the frame walk stopped (bounded: 3 hops, 0x400 per hop, kernel addresses only).

What it still cannot tell: payload contents or what userland did with an answer; that needs the userland log (`logs` now also
collects `userland-wifi.log`, `airport-profile.txt`, `hardwareports.txt`).

## IOSkywalkLegacyEthernet shim + first real ioctl handlers — 2026-10-13, build after ef42853 (not hardware-tested)

### What the ef42853 boot established

* The recorder (see above) showed userland reaching the interface: `0x2c/0x2b` (versions), `0xc` (x37, answered), then `0x13` POWER (x71,
  polled), `0x33` COUNTRY_CODE, `0x1c` LOCALE, `0x5` POWERSAVE, `0xf` OP_MODE (x20), `0x4` CHANNEL, `0x10` RSSI, `0x1b`
  SUPPORTED_CHANNELS (x12), `0xcf` CHANNELS_INFO, `0xd8` ROAM_PROFILE, `0x103` MAX_NSS_FOR_APP, set `0x16b` USB_HOST_NOTIFICATION,
  `0x1d5` OS_FEATURE_FLAGS, `0x200` INFRA_ENUMERATED, and `0x216` (not identified). All but 0xc/0x2b/0x2c were still refused.
* `SCNetworkInterfaceCopyAll()` and configd's InterfaceNamer enumerate only `IONetworkInterface` objects (via IONetworkStack). Our
  interface is an `IOSkywalkEthernetInterface` (not an `IONetworkInterface`), so en9 existed as a BSD interface but not for
  SystemConfiguration.

### Mechanism (from Apple's own 24H32 disassembly, read-only)

`IOSkywalkFamily` carries a personality `IOSkywalkLegacyEthernet` (`IOProviderClass` = `IOSkywalkEthernetInterface`,
`IOMatchCategory` = `IOSkywalkLegacyEthernet`): an `IOEthernetController` shim whose `createInterface()` builds an
`IOSkywalkLegacyEthernetInterface` (an `IOEthernetInterface`, i.e. an `IONetworkInterface`, with `IOClassNameOverride`). Its
`probe()` refuses a provider that already has `IOInterfaceUnit`. Our controller pre-assigned the unit (RegistrationInfo +0x38 = 9) so
the shim never matched. Apple's own role-1 interface leaves +0x38 = 0xFFFFFFFF; the unit is assigned later by IONetworkStack /
configd through the shim's `attachToDataLinkLayer`, which sets `IOInterfaceUnit` on the provider and re-registers it, after which
`IOSkywalkNetworkBSDClient::start` names the BSD interface.

### Change

* `makeInterface`: role 1 no longer writes the unit; `itlmt_native_unit=N` (explicit mode) and role 2 are unchanged.
* Watcher (own IOWorkLoop + IOTimerEventSource, 5 s ticks, `unit:*` trace lines, overwritten property `itlmt_native_shim`): records every
  `IONetworkInterface` in the registry (class / `BSD Name` / unit) and the registry children of our interface. If no `IOInterfaceUnit`
  appears within `itlmt_native_unit_wait` seconds (default 45; 0 disables the fallback) it applies the previous behaviour
  (`IOInterfaceUnit` = `itlmt_native_unit_fb`, default 9, then `registerService`) so the BSD interface still comes up. Trace line
  `unit:none after ...` means the fallback fired, i.e. the shim did not match.
* Handlers with layouts taken from Apple's `AppleBCMWLANCore` (addresses in the source): controller `getPOWER` ([+4]=1, [+8]=state, the
  state is what userland set, initially 0), `setPOWER` (recorded only: no firmware), `getCOUNTRY_CODE` / `setCOUNTRY_CODE` (3 bytes at
  +4, initially Apple's "XZ" = unknown); interface `getCHANNEL` (0xe0822801 while the link is down), `getPOWERSAVE` ([+4]=0), `getRATE` /
  `getRSSI` (0xe0822403 = not associated), `getOP_MODE` ({1, 0}), `setUSB_HOST_NOTIFICATION`, `setOS_FEATURE_FLAGS` (qword stored),
  `setINFRA_ENUMERATED` (0). While the core reports its link up the value getters return unsupported rather than an invented answer.
* Allowed commands: 0x4, 0x5, 0x8, 0xc, 0xf, 0x10, 0x13, 0x2b, 0x2c, 0x33, 0x16b, 0x1d5, 0x200. Still refused (data comes from firmware
  tables this driver has not): 0x1b, 0xcf, 0xd8, 0x103, 0x1c, 0x216.
* `itlmtTrace`: once the 1500-byte prefix is full, entries now append to `itlmt_native_trace_tail` instead of replacing it.

### Checks (host-only)

`scripts/build.sh itlmt7925Native` ok; `check_kext_links.py` only the 6 baseline-missing symbols; `check_native_slots.py`:
interface 215/0, controller 23/0; `gen_native_iface.py --check` ok.

### One reboot, read in this order

1. `ioreg -l | grep itlmt_native_shim` — an `IOSkywalkLegacyEthernetInterface`/`IOEthernetInterface` entry with a `BSD Name`
   and `u=` is the shim worked (then `networksetup -listallhardwareports`, CoreWLAN, System Settings should list it as Wi-Fi).
2. `unit:assigned t=..s fb=0` (shim) / `unit:none after 45s` + `fb=1` (shim did not match; send the `first` snapshot).
3. `itlmt_native_calls`: which `I:` ids remain refused, which `S:` slots (467+) are reached next.
4. `ifconfig` for the BSD name chosen by IONetworkStack (not necessarily en9).

### Result of the f6b7162 boot (user report)

The shim matched: the interface came up as `en4` (Wi-Fi hardware port) and the Wi-Fi switch in System Settings turns on and off.
Scan and join were still refused at that point; the section below is the next build.

## Scan + WPA2-PSK join through the native path — 2026-10-13, build after f6b7162 (not hardware-tested)

### What is new

Core (`itlmt7925`, `itl_native_bridge.h`): platform functions `itlmt-native-scan-v1`, `-nets-v1`, `-join-v1`, `-leave-v1`,
`-status-v1` (only answered in native mode; they only touch a mailbox and snapshots, nothing blocks on the chip).
The AP table now keeps probe/beacon flag, capability, beacon interval and up to 512 bytes of whole information elements.
`Net` gained a PMK: when userland supplies the 32-byte PMK the core's M3 handshake uses it directly (`itl_wpa_pmk` is skipped).
A failed native attempt is reported once (state FAILED, fail stage) and the network is dropped instead of being retried forever.

Interface (`itlmt7925Native`): handlers for ioctl 0x1b / 0xfe (supported channels: the 12 channels the core scans), 0x1b9
(SCAN_REQ), 0x1b3 (SCAN_ABORT), 0x1ba (ASSOCIATE: needs a raw 32-byte PMK, cipher type 6 or 9, otherwise Unsupported), 0x1b6
(JOIN_ABORT), 0x1a9 (LEAVE_NETWORK), 0x1b1 (WCL_BSS_INFO). `getCHANNEL/RATE/RSSI/OP_MODE` answer from the core status while linked.
`pollControl()` (100 ms inside the datapath tick) delivers results: scan table as one 0xc9 event per AP (beacon message 0x40 + IE
length) followed by 0xed (scan done); join as 0x4e, 0xd3 and, on success, 0xd5, posted with
`IO80211Controller::postMessage`. Scan timeout 25 s, join timeout 45 s (then LEAVE and a failure result).

### Not determined (chưa xác định)

`apple80211_channels_info` layout; Apple's numeric failure codes (ieee status 1 / status1 1 are placeholders); the 0xd3 flag byte and
the words at +0xc/+0x10; whether the Family needs a current-BSS object / SSID property update before Settings shows the joined name;
directed scans (the SSID is only a report filter); open, SAE and enterprise joins (refused with Unsupported); ROAM_PROFILE,
BTCOEX_PROFILE, LOCALE layouts. The station address is still the placeholder shared by core and controller (core firmware setup uses
the same value in native mode).

### Checks (host-only)

Both kexts build; `check_kext_links.py` only the 6 baseline-missing symbols (postMessage resolves in IO80211Family);
`check_native_slots.py` 215 on the right slot, 0 wrong; `gen_native_iface.py --check` ok.

### One reboot, read in this order

1. Settings → Wi-Fi: does the list of networks appear? `bash scripts/native-wifi-test.sh logs`.
2. `itlmt_native_calls`: `S:594` (SCAN_REQ), `P:201`/`P:237` (0xc9/0xed posted), then `S:595` (ASSOCIATE), `P:78`/`P:211`/`P:213`
   (0x4e/0xd3/0xd5).
3. `itlmt_native_trace`: `scan:` and `assoc:` lines (fail stage when a join fails), `wifi-events` in the core log.
4. Join a WPA2-PSK network from Settings; `ifconfig` for the BSD interface and DHCP.

## Join sequence fix (round 16)

### Result of the 4b9273b boot (user report + log)

Settings listed the scanned networks; joining failed immediately. `setAssociateRequest` returned 58 (`kProhibited`): the family's
`WCLJoinManager::handleJoinRequest` (0xffffff800220ac36) begins every join with an internal GET of ioctl 0x1ab
`WCL_LOW_LATENCY_INFO` and aborts when it fails or returns a nonzero dword. `isCommandProhibited` refused 0x1ab (not in
`commandAllowed()`), so the join never reached 0x1ba ASSOCIATE.

### What changed (itlmt7925Native only; core kext unchanged)

* `commandAllowed()`: + 0x170 SET_MAC_ADDRESS, 0x1ab LOW_LATENCY_INFO, 0x1be SET_SCAN_HOME_AWAY_TIME, 0x1b7 TRIGGER_CC.
  The recorded-ioctl list holds 96 distinct ids (was 32).
* Handlers: `getWCL_LOW_LATENCY_INFO` (one zero dword), `setWCL_SET_SCAN_HOME_AWAY_TIME` (accepted, first two words traced),
  `setWCL_WNM_OPS`, `setWCL_TRIGGER_CC`, `setWCL_LINK_UP_DONE` (accepted), `setWCL_LINK_STATE_UPDATE` (accepted; bssid/up/flags/reason
  traced as `wcl:link state ...`). `setMacAddress` answers `kIOReturnNotPermitted` for a different address (the join manager
  tolerates 0 and 0xe00002e2 only).
* Event 0xd4 (first beacon, exactly 0x14 bytes: u16 status 0, u16 ieee status 0, +4 bssid, +0xc u64 zero) is posted between 0xd3
  and 0xd5 (`WCLJoinManager::firstBeaconEventHandler`, `WCLJoinRequest::updateFirstBeaconEvent`).
* `getCHANNEL/RATE/RSSI/WCL_BSS_INFO` use the core's association state only (no longer also the family-visible radio link, which
  can lag by one tick when WCLNetManager::linkUp asks for BSS info right after 0xd5).

### Not needed from the driver (read from the disassembly)

The family's `WCLNetManager::linkUp` -> `updateBss` -> `WCLBssManager::setCurrentBSS(WCLBSSBeacon*)` sets the current BSS, so
SSID/BSSID/CHANNEL are answered by the family; the driver does not build a `IO80211BSSBeacon`. `linkUp` also issues a GET 0x1b2
TRAFFIC_COUNTERS (0x38 bytes) whose result it does not check; 0x1b2 is still refused (layout not determined).

**Correction (round 19):** "once 0xd3/0xd4/0xd5 arrive" was wrong. `linkUp` is only reached from the net-manager FSM's LINK_UP
event, which comes from event 0xd8 (see round 19 below); 0xd3/0xd4/0xd5 alone leave the current BSS null.

### Checks (host-only)

Both kexts build; `check_kext_links.py` only the 6 baseline-missing symbols; `check_native_slots.py` interface 215/0;
`gen_native_iface.py --check` ok.

### One reboot, read in this order

1. Join a WPA2-PSK network from Settings. `bash scripts/native-wifi-test.sh logs`.
2. `itlmt_native_trace`: `ioctl:cmd=1ab/170/1ba allowed`, `assoc:request`, `assoc:result ok=1`, `wcl:link state up=...`,
   `wcl:link up done`, `if:setMacAddress ...`.
3. `itlmt_native_calls`: `S:595` ASSOCIATE, `P:78/211/212/213` posted events, `S:600` LINK_STATE_UPDATE, `S:597` LINK_UP_DONE,
   which `I:` ids appear next (SSID/BSSID are family-answered; look for 0x1b2, 0x1b8 and any still-refused id).
4. `ifconfig` for the BSD interface and DHCP; System Settings shows the joined name.

### Result of the round-16 boot (2026-10-13)

* The password prompt in Settings no longer reports an error (round 15: `setAssociateRequest` = 58 right after the prompt).
* The captured window (boot 16:26 to 16:51, `log show` + ioreg) contains **no join request at all**: no `APPLE80211_IOC_WCL_ASSOCIATE`,
  no GET 0x1ab / SET 0x170 / 0x1ba, no `assoc:request` in `itlmt_native_trace`; airportd logged only auto-join scans (known
  networks all "unused for 72 weeks", so auto-join never targets one). So the join attempt was not inside the capture; no verdict on
  the join FSM or on data yet.
* Still failing on every cycle, all family-visible: GET SSID/BSSID/RSSI/RATE/NOISE `0xe0822403` (no association, expected), CHANNEL
  `0xe0822801`, CHANNELS_INFO (0xcf) `0xe00002c7` (about 500 calls), MCS and NSS `0x3a` (prohibited, not in `commandAllowed()`),
  BTCOEX_PROFILE `0xe00002c7` x35, ROAM_PROFILE `0xe0822403`, SET IE (0x55) `0x3a`, POWERSAVE `0xe00002c7`.
* `itlmt_tx_*` / `itlmt_rx_*` counters are 0 (no association, so no datapath yet).

### Next capture (no reboot needed: the kext is loaded)

1. Join from Settings, then within a minute: `bash scripts/native-wifi-test.sh logs` (from the worktree path, not the main checkout)
   or just read `ioreg -l -w0 | grep itlmt_native_trace_tail` and
   `log show --last 3m --info --debug --predicate 'process == "kernel" AND eventMessage CONTAINS "IOC DEBUG"'`.
2. Expect `INTERNAL: Set type=<APPLE80211_IOC_WCL_ASSOCIATE>` and the 0x1ab/0x170 GET/SET before it.

### Result of the join capture (2026-10-13, round-16 build still loaded)

* **Join works.** Trace: GET 0x1ab, SET 0x170 (setMacAddress answered "change refused", tolerated), 0x1ba `WCL_ASSOCIATE`,
  `assoc:request ssidLen=8 cipher=6 keyLen=32`, `assoc:result ok=1`, family link up, DHCP lease obtained (ACK from the gateway).
  Counters at the time: TX 66 / RX 20 frames, no drops.
* **No internet: every unicast IPv4 frame was dropped after BPF.** `tcpdump -i en4` (root) showed ICMP echo replies, TCP SYN-ACK and
  DNS answers arriving with the correct destination MAC, yet `netstat -s -p icmp` never counted an echo reply and TCP kept
  retransmitting the SYN. ARP (including unicast requests) and broadcast DHCP worked, so the destination MAC match was not the
  problem; only IPv4/IPv6 unicast was.
* **Root cause:** `IOSkywalkNetworkPacket::setIsLinkBroadcast(bool)` (24H32 `0xffffff800297f262`) and `setIsLinkMulticast(bool)`
  (`0xffffff800297f290`) ignore their argument: both call `kern_packet_set_link_broadcast` / `_multicast` unconditionally. The RX
  path called both for every frame, so each frame was marked link-broadcast **and** link-multicast; the IP stack discards a
  unicast IP packet that arrived as a link-layer broadcast/multicast (ICMP echo, TCP), ARP and DHCP do not care.
* **Fix (round 17):** call `setIsLinkBroadcast(true)` only for the all-ones destination and `setIsLinkMulticast(true)` only for
  other group addresses; never call them otherwise (the call itself is the flag).
* Expected next boot: `ping` to the gateway answers, DNS/TCP work. The default route may still prefer the en1 tether service;
  if browsing keeps using en1, reorder Network service order (not a driver problem).

## Result of round 17 (2026-10-13): unicast works, firmware stops answering ~50 s after link-up

Round 17 (`595561c`) booted. Unicast IPv4 reaches the stack (ping to the gateway, DNS). New failure: about 35 s after
link-up the radio goes silent (no beacons, no data), 15 s later the core logs `link lost: no beacon or data from the AP for
15 s`, and every MCU command after that returns `rc=-110` (`bss_info(add)`, `edca`), scans find nothing. The chip stays dead
until reboot. Cause: chưa xác định (candidates: firmware power management, a specific TX/RX frame, DMA / MCU queue stall,
PCIe link state).

Round 18 adds diagnostics and recovery in the core (`itlmt7925`), so one boot gives a verdict:

- `wifi-frames` (ioreg property of the core): last 24 Ethernet frames, oldest first, `<ms ago><t|r> <ethertype>/<ip proto> <a>><b> <len>`
  (ports for UDP/TCP, ICMP type or ARP op in `a`).
- `wifi-postmortem`: chip id, LPCTL / MISC / DUMMY / GLO_CFG / HOST_INT status+enable, PCI command / status / link control /
  link status, CPU/DMA indices of every ring, count of descriptors marked done. Written at 4 s without beacon ("silent 4s"),
  again after a `driverOwn()` wake attempt ("after driverOwn") and when the link is declared lost ("lost").
- Recovery: after a lost link the supervisor redoes the cold start (`wfsys` reset, rings, firmware, interface, scan) via
  `chipRecover()` (at most 8 times per boot) and rejoins. `wifi-events` shows `chip not answering: reset and firmware reload #n`
  and `recover #n rc=...`.

Read after the reboot (no root needed): `ioreg -l -w0 | grep -E "wifi-postmortem|wifi-frames|wifi-events"`.

## Result of round 18 (2026-10-13): link stable, internet over en4

After the reboot the link stayed up through the point where round 17 died (about 35 s after link-up) and for more than
5 minutes of use: 3 x 20 MB HTTP downloads over en4 (about 1.7 MB/s each, legacy level 0 / 6 Mb/s path), DNS to the
gateway, ping 0 % loss. The post-mortem and recovery code never triggered (`wifi-postmortem` absent). The round-17 hang did
not repeat; its cause stays chưa xác định (the previous boot had a root tcpdump + dig run on en4 at the time).
Open: `itlmt_fast` is not set, so the link runs at `lvl 0 bw 0`; en1 (tether) is still the primary default route.

## Round 19 (2026-10-13): the family never saw a link — event 0xd8 (not hardware-tested)

### Symptom (user report after round 18)

Internet works over en4, but System Settings > Wi-Fi and the quick-settings Wi-Fi item do not show the joined network.
airportd/WiFiAgent: GET SSID/BSSID answered `0xe0822403` (no association) although the core is associated.

### Root cause (read-only from the 24H32 IO80211Family disassembly)

* `WCLConfigManager::getSSID` fails with `0xe0822403` while `WCLBssManager`'s current BSS is null.
* The current BSS is set only by `WCLNetManager::updateBss` (GET 0x1b1 WCL_BSS_INFO into a 0x840-byte buffer, then
  `setCurrentBSS`), called from `linkUp`, `wake` and `assocTimerAction`.
* `linkUp` is reached from the net-manager FSM (states LINK_DOWN, WAITING_FOR_CONNECT_COMPLETE, WAITING_FOR_IP, LINK_UP,
  DEAUTH, SLEEP, SLEEP_DEAUTH): LINK_DOWN --LINK_UP--> `firstLinkUp` -> WAITING_FOR_CONNECT_COMPLETE; CONNECT_COMPLETE (from the
  join manager's 0xc0003 "join done") -> WAITING_FOR_IP; IPV4_STATUS -> LINK_UP.
* The FSM's LINK_UP / LINK_DOWN_IND events come from `WCLNetManager::linkStatusInd`, a bulletin-board subscriber for the driver
  event **0xd8**: `processEvent(byte[data+6] == 0 ? LINK_DOWN_IND : LINK_UP)`. The driver never posted 0xd8, so the FSM stayed in
  LINK_DOWN, no `0x1b1`/`0x1bc`/`0x1c6` ever came (the call recorder shows none of them), and the current BSS stayed null.
* Event routing: `postMessage(code)` -> `IO80211Glue::routeEventToWcl` -> `CommonGlue::sendMessage` ->
  `WCLGlue::receiveMessageInternal`, bulletin id `(code << 16) | 2`; `WCLGlue::isMsgNeeded` drops events nobody subscribed to.
* Pointer decode used for the KC data/vtables: `target = 0xffffff8000100000 + (raw & 0xffffffff)`.

### Change (`itlmt7925Native` only; core unchanged)

* `finishJoin(ok)` posts **0xd8** (0x10 bytes: `+0` BSSID, `+6` = 1) first, before 0x4e / 0xd3 / 0xd4 / 0xd5, so LINK_UP precedes
  CONNECT_COMPLETE and the join manager is still busy when `firstLinkUp` checks it.
* A core link loss (`linkFromCore(up=false)` while the family was told "up") makes `pollControl` post 0xd8 with `+6` = 0 and
  reason `+8` = 1. `WCLNetManager::linkDownInd` treats reasons 1 and 3 as the plain path: no debounce, no recreate; it issues
  `leaveNetworkCommand`, which reaches the driver's existing 0x1a9 handler (a core leave on an already-lost link).
* Trace lines: `wcl:link status posted up=<0|1> reason=<n>`, `wcl:bss info answered ch= ssidLen= ie=`; the existing
  `wcl:link up done` (0x1bc) and `wcl:link state ...` (0x1c6) now show whether the family reached `linkUp`.
* Still refused (result not checked by `linkUp`): 0x1b2 TRAFFIC_COUNTERS.

### One reboot, read in this order

1. `ioreg -l -w0 | grep -E "itlmt_native_calls|itlmt_native_trace"`: expect `P:d8` before `P:4e P:d3 P:d4 P:d5`, then
   `I:1b1`, `S:522`, `S:597` (0x1bc), `S:600` (0x1c6), and the trace `wcl:bss info answered`.
2. Settings > Wi-Fi shows the network as connected; `log show --last 2m --predicate 'process == "airportd"'` has no
   `0xe0822403` for SSID/BSSID.
3. If `P:d8` is there but no `I:1b1`: the FSM rejected LINK_UP (read the `wifi` log of WCLNetManager for the state), or the
   subscription table has no 0xd8 entry in the live build — then dump `gNET_MANAGERCallBackArray` again.
4. Link loss: force one (move the AP out of range); expect `wcl:link status posted up=0 reason=1` then `assoc:leave=...`.


## Handshake correction after the 36e0da6 boot — 2026-10-13

Settings scan works, but two native joins failed at M1 -> M2 -> no M3 (stage 6), before the new link-status event.
Corrected discarded M1 retransmissions and single-frame EAPOL RX mailbox; added replay/ANonce/MIC checks, AP-leave reasons,
optional PMKID equality and a persistent `wifi-handshake` summary. Both kexts built, link/slot/OC checks passed and executables
were staged on ESP with credentials/config preserved. Hardware result pending; the old logs cannot prove the exact failure cause.
See [result and next observations](native-wifi-results/20261013-handshake-36e0da6/README.md).

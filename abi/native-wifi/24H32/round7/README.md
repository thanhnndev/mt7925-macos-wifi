# Round 7: work-loop and queue evidence

Read from the recorded 24H32 KC; SHA-256 and function addresses are in `source.json`.
Reproduce with `KC=<path> python3 tools/dump_native_wifi_round7.py abi/native-wifi/24H32/round7` (Capstone required).

* `IO80211Controller::getWorkLoop` tail-calls virtual offset `0xc68` from the vptr. This is dump slot 399 (two header words),
  `getWorkQueue`, confirmed by the round-3 controller/BCM vtables. There is no conversion or extra work in this getter.
* The Family's default `IO80211Controller::getWorkQueue` returns NULL; BCM overrides it and returns `[this+0x128]+0x7898`.
* `IO80211WorkQueue::getThread` returns NULL, directly confirming the NULL-thread premise of the Stage A panic analysis.
* `IO80211WorkQueue::workQueue` allocates a 0x50-byte object, invokes its virtual initializer, releases it on failure, and
  returns the initialized object on success. This dump does not establish a real thread for that object.
* The core class `itlmt7925` inherits `IOService` and has no `getWorkLoop` override. `IOService::getWorkLoop` calls vptr
  offset `0x680` (`getProvider`, slot 210), then delegates to that provider's `getWorkLoop` at `0x688` (slot 211); it returns
  NULL if there is no provider. Therefore the new native getter may use its own fallback loop depending on the provider chain.

These getters do not explain why BCM avoids the policy panic, or rule out casts by other Family callers of `getWorkLoop`.
The command-gate/addEventSource path after the policy call remains untested on hardware.

Local checks against source commit `61ce48e`: `io80211check` and native build passed; KC link check passed with only the six
baseline-missing IONetworkController reserved symbols; the controller slot checker passed 20 overrides, zero wrong slots.
The slot checker needs `c++filt -n` on Darwin because its inputs already have the Mach-O prefix removed.
No ESP install or hardware test was performed for this revision.

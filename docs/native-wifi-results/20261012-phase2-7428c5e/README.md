# Phase 2 — Stage B exclusive, 2026-10-12

Installed source: `7428c5e`. User reports boot completed without panic and uses USB networking; native networking is unavailable. Boot kernel events begin at approximately 17:36:23 local time (Asia/Ho_Chi_Minh).

* Core `com.itlmt7925.kext` and native `com.itlmt7925.native` are loaded (indexes 89 and 109).
* Actual `kern.bootargs` includes `itlmt_native=1 itlmt_native_if=1`, and excludes `itlmt_m2`, `itlmt_m3`, `itlmt_fast`: exclusive mode. Core is retained as the native dependency; its legacy scan/join path is disabled.
* `itlmt7925Wifi` is registered/matched/active. A child `itlmt7925WifiInterface` exists, with text registry flags `!registered, !matched, active`, retain count 4, IOServiceState 0, role 2. The controller's IOServiceState is 30.
* `ifconfig -a` contains no `en9`. `networksetup -listallnetworkservices` contains no service named Wi-Fi. System Settings UI was not manually inspected.
* Unified log contains native init and CoreCapture creation. Neither `controller and interface started` nor `interface creation failed` was found in the collected unified log or current dmesg ring. Therefore the precise outcome/failed step in `makeInterface` / `startWithInfo` cannot be determined from these logs.
* Stage B is NOT a publication success: interface object exists, but no native BSD interface/service is observable. Native data transfer is not implemented in the stub anyway; the missing en9 is the diagnostic issue.
* No sleep/wake or further reboot performed. No new panic reported; panic blobs were not collected.

## Evidence and collection limitations

`native-registry.txt` and `native-registry-state.json` preserve the native subtree structure, class/state and role. `kextstat.txt`, `kern-bootargs.txt`, `interfaces.txt` and `native-kernel-events.txt` are diagnostic extracts without SSIDs, MACs, passphrases or SMBIOS.

Full collection remains local at `~/itlmt-backup/logs-20261012-174210/` and its sibling `.tar.gz`. The script could not read dmesg without privileges; `dmesg-full.txt` was subsequently collected with administrator privileges. At collection time the ring no longer contained the native start/failure IOLog messages.

Phase 1 efuse/CLC/tx-power test was skipped at the user's direction in favor of exclusive native testing. This run cannot validate the new efuse parser or power-table commands, because legacy core scan/join is disabled.

Cloud follow-up: inspect interface lifecycle/publication and add persistent step/result logging around withController, attachInterface, RegistrationInfo, base interface start, getWorkQueue, pool/queue creation, addEventSource and registerInfraEthernetInterface. No failure stage should be assumed from object presence alone. Existing queue service-class/RegistrationInfo/teardown uncertainties remain. Configuration was left unchanged for investigation.

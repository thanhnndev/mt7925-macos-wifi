# Native Wi-Fi Stage A hardware result — 2026-10-12

Installed source commit: `b6693cb`. Boot at approximately 17:06 local time (Asia/Ho_Chi_Minh).
User reports reboot completed without panic. Both core and native were freshly built and installed; credentials preserved.

* `com.itlmt7925.native` is loaded (kextstat index 110), with dependency on loaded core index 93.
* `itlmt7925Wifi` is registered, matched and active in IORegistry. The prior dependency and base-start panic blockers did not recur in this boot.
* Actual `kern.bootargs` contains `itlmt_native=1`, without `itlmt_native_if=1`: controller-only Stage A.
* Unified log records native init and CoreCapture pipe/stream creation. The explicit `controller started` IOLog line was not found in the collected unified log or current dmesg buffer; the active controller registry entry is the runtime evidence.
* Core logs show `efuse event rejected: len=124 tag=0002 addr=00000a70 want=00000a70 valid=00000001`, followed by `efuse hw type failed rc=-5, joining without power tables`.
* Core subsequently authenticated and associated (status 0), completed EAPOL, installed PTK/GTK, reported `RESULT rc=0 link up`, created its Ethernet interface and recorded DHCP/DNS traffic. This is the core Ethernet path, not native Wi-Fi data transfer.
* Native interface Stage B and sleep/wake have not been tested. No new panic was reported, so panic NVRAM blobs were not collected for this run.

## Evidence

`kextstat.txt`, `kern-bootargs.txt`, `native-kernel-events.txt`, `native-registry.txt`, `core-events.txt` and `interfaces.txt` contain diagnostic extracts. Network identifiers and full raw registry data are omitted from these repository artifacts.

Full local collection: `~/itlmt-backup/logs-20261012-170854/` and its sibling `.tar.gz`. The collector lacked dmesg permission; `dmesg-full.txt` was subsequently captured with administrator privileges. Core m3 logs were extracted from the live core IORegistry, because they were absent from the current dmesg buffer.

Cloud follow-up: review Stage A evidence, the efuse event shape above, and prerequisites for Stage B. Do not treat controller-only success as validation of interface creation or teardown.

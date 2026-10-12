# Stage B persistent trace — 2026-10-12

Installed source commit `cf1bd62` (native code from `a8c7ae2`). User completed reboot; no new panic reported. Native controller and core kext are loaded. Controller is registered/matched/active; child interface remains unregistered/unmatched, and there is no en9. Actual boot args contain both native switches and omit legacy m2/m3/fast: Stage B exclusive.

## Exact persistent trace

```text
mk:start unit=9; mk:withController=ok; mk:attachInterface=1; sw:IO80211InfraInterface::start=0; mk:startWithInfo failed
```

Object construction and attachInterface succeeded. The base interface start returned false. `startWithInfo` returned before getWorkQueue, createPoolAndQueues, addEventSource and registerInfraEthernetInterface. Consequently queue service classes / WMM validation are not the failing step in this boot. Inspect the base interface start path and its controller/provider/role prerequisites next.

`native-interface-full.txt` contains the entire collected interface node: only IO80211InterfaceRole=LowLatency, no BSD name/unit. `native-trace.txt` also includes global IOInterfaceUnit/NamePrefix properties of unrelated interfaces; those en0/en1 properties must not be attributed to the native child.

No sleep/wake, new hardware retry or teardown test performed. Configuration left unchanged pending cloud analysis.

## Collection

Full local collection: `~/itlmt-backup/logs-20261012-180641/` and sibling `.tar.gz`. Script dmesg collection failed for lack of privilege; this is recorded rather than treating an empty dmesg file as evidence of no errors. Persistent registry trace provides the precise failure result. Repository extracts were reviewed for SSIDs/MACs/passphrases/SMBIOS before commit.

Round 8 dumps and preliminary analysis are already on this branch (`37333e1`, `21d92ed`). Cloud should read this trace before selecting a WMM/service-class patch.

# Stage B role 1 — 2026-10-12

Installed source commit: `4bd0819`. User completed reboot; no new panic reported. Core and native are loaded (kextstat indexes 108 and 112). Stage B exclusive boot args confirmed; legacy m2/m3/fast absent.

## Persistent trace

```text
mk:start role=1 unit=9 explicit=0; mk:withController=ok; mk:attachInterface=1; sw:deferBSDAttach=0; sw:IO80211InfraInterface::start=1; sw:getWorkQueue=ok; pool=ok; queues rxSub=1 txComp=1 rxComp=1; sw:pool+queues=1; sw:addEventSource x6 ok; sw:registerInfraEthernetInterface=0; mk:startWithInfo ok
```

Changing role to 1 passed the previous base-start failure. Base start, work queue lookup, pool/queue creation, six event-source additions and registerInfraEthernetInterface all succeeded. The trace ends with startWithInfo success. This hardware result confirms that the present all-zero service-class queues do not prevent this registration path from returning success.

## Publication still pending

The child has role Infrastructure, IOInterfaceNamePrefix=en, IOMACAddress present (expected placeholder; value omitted from repo), IODeferBSDAttach=Yes and power-management properties. It remains !registered/!matched/active with retain count 6. No IOInterfaceUnit is present on the native child; no en9 or other new en interface appears in ifconfig. The unit=9 trace field is only the default local variable: explicit=0 means this build left BSD naming/unit selection to the Family. Global en0/en1 properties in native-trace.txt belong to other interfaces.

A service exactly named Wi-Fi in networksetup: no. System Settings UI was not manually inspected. Successful registration is not yet BSD publication or native radio connectivity. Cloud next step: determine the proper primary-interface publication/completion path after deferBSDAttach(true), including who clears deferral / performs BSD attach / registers the service. No local functional patch made.

## Evidence

Trace, filtered full native interface/controller nodes, kextstat, actual boot args, interface list and native kernel events are included. SSIDs, MACs (including placeholder), passphrases and SMBIOS omitted. Full local archive: `~/itlmt-backup/logs-20261012-212834.tar.gz`. The collector could not read dmesg without privileges; persistent trace is sufficient for the step results above. No sleep/wake, teardown or further reboot test performed. Configuration remains Stage B exclusive.

# Round 8 collection and preliminary analysis

Recorded 24H32 KC, same SHA-256 as `source.json`. Original script produced 18 symbol dumps and eight unmatched patterns.

## Collector limitations

* Broad QueueSet prefix matches `gMetaClass` and `superClass`, which are data symbols, not functions. Their `.asm.txt` files are raw script output and must NOT be interpreted as code.
* MAXN=6 on the broad prefix omitted `initWithQueues`; a supplemental exact-function dump is provided.
* TxSubmissionQueue `initWithPool` has a 12-character method name; the original pattern used 8. The correctly matched function is included in the supplemental dumps.
* `candidates.txt` uses substring search and is only a hint: e.g. the NetworkKDPPoller start candidate is not NetworkInterface::start.
* `supplemental-source.json` records exact names/addresses for the three supplemental function dumps. `wmm-service-classes.txt` records the four data words referenced by isWMMSchedModelCapable, rather than interpreting that table as code. All supplementary data came from the same checked KC.

## WMM hypothesis: not established as the publication blocker

`isWMMSchedModelCapable` checks queues of the queried type 1 and counts their service classes against the table at 0xffffff80029a6e40: {0x00100080, 0, 0x00380100, 0x00400180}. It returns true only when each appears exactly once. Four relevant queues with class 0 therefore do not satisfy that test.

However, round4 `IOSkywalkNetworkInterface-registerNetworkInterface-a.asm.txt` initializes QueueSetInfo type to 3 (0xffffff8002981dc5), replaces it with 0xb ONLY when the WMM test succeeds, and calls createWithQueues for either result.

Supplemental `IOSkywalkQueueSet-initWithQueues.asm.txt` tests bit 0x8 of QueueSetInfo at 0xffffff800298abcb. It calls isWMMSchedModelCapable and rejects a false result only when that bit is set. Type 3 has bit 0x8 clear. Thus the WMM test itself is not a mandatory success condition for the observed fallback path. Other queue-set, logical-link, interface-start and registration validations can still fail. This does not prove the complete type-3 registration path works for this native interface.

There is not enough runtime evidence to assert that setting the four service classes alone fixes the missing en9. Obtain `itlmt_native_trace` from the installed a8c7ae2 trace build before selecting a corrective patch. No functional native change was made locally from this preliminary analysis.

## Installed state for the next boot

Native rebuilt from a8c7ae2 and installed Stage B exclusive; core unchanged. Link check passed with only six baseline reserved IONetworkController symbols. Config and NVRAM match; OpenCore schema/ocvalidate passed. Pre-install EFI backup: `~/itlmt-backup/EFI-20261012-175648`. No reboot or new runtime trace collected yet.

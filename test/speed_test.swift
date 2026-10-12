// SPDX-License-Identifier: BSD-3-Clause-Clear
import Foundation
@main
struct SpeedTests {
    static func main() {
        var s = CurlSample()
        s.code = 28; s.http = 200; s.down = 1000000; s.total = 2
        assert(s.validTransfer(upload: false))
        s.http = 403; assert(!s.validTransfer(upload: false)); assert(s.failure == "HTTP 403")
        s.http = 200; s.code = 7; assert(!s.validTransfer(upload: false))
        s.code = 0; s.up = 8000000; assert(s.validTransfer(upload: true))
        s.http = 0; assert(!s.validTransfer(upload: true))
        s.http = 200; s.total = 0; assert(!s.validTransfer(upload: true))
        assert(SpeedTest.fmt(.infinity) == "–")
        assert(Set(SpeedTest.servers.map(\.id)).count == SpeedTest.servers.count)
        assert(SpeedTest.servers.filter { $0.upload != nil }.count == 1)
        print("Speed result validation passed")
        if CommandLine.arguments.contains("--live") {
            for server in SpeedTest.servers {
                let probe = SpeedTest.curl(nil, ["--max-time", "5", "--range", "0-1023", server.probe])
                assert(probe.probeOK, "\(server.name): \(probe.failure)")
                print("\(server.name): HTTP \(probe.http), \(probe.remoteIP)")
            }
            let internet = SpeedTest.run(secs: CommandLine.arguments.contains("--full") ? 10 : 2)
            assert(internet.error == nil, internet.error ?? "")
            assert(internet.downMbps != nil)
            print("Internet: \(internet.path); ↓\(SpeedTest.fmt(internet.downMbps)) ↑\(SpeedTest.fmt(internet.upMbps)) Mbps; \(internet.note ?? "OK")")
            let direct = SpeedTest.run(secs: 1, serverID: "cloudflare", path: .wifi)
            if internet.path.contains("VPN") && direct.error != nil {
                assert(direct.error?.contains("VPN blocks direct Wi-Fi") == true, direct.error ?? "")
            }
            assert(direct.downMbps != nil || direct.error != nil)
            print("Direct Wi-Fi: \(direct.error ?? SpeedTest.fmt(direct.downMbps) + " Mbps")")
        }
    }
}

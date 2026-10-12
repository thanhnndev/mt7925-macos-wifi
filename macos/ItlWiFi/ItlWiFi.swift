// SPDX-License-Identifier: BSD-3-Clause-Clear
// Copyright (c) 2026 thanhnndev and contributors
// ItlWiFi: menu bar Wi-Fi control for itlmt7925: a popover with the current network, speed test, scan list, join with a
// password (inline), disconnect / forget, open at login. SwiftUI in an NSPopover; SF Symbols only.
// Talks to the kext through its user client (ABI: kexts/itlmt7925/itl_uc_abi.h) and reads the wifi-* registry properties.
// Passwords are kept in the login keychain (service com.itlmt7925.wifi, account = SSID), never in the kext's Info.plist.
// Build: scripts/build-itlwifi.sh
import AppKit
import SwiftUI
import IOKit
import Security

// ---- driver access -------------------------------------------------------------------------------------------------------
struct WifiNet { let ssid: String; let channel: Int; let rssi: Int; let security: String
    var joinable: Bool { security == "wpa2" || security == "wpa2/wpa3" }
    var locked: Bool { security != "open" } }

struct WifiStatus {
    var present = false, state = "absent", error = "", ssid = "", channel = 0, rssi = 0, rate = 0, scanGen = 0
    var nets: [WifiNet] = []
}

enum Driver {
    static let abi: UInt32 = 1
    enum Sel: UInt32 { case scan = 0, join = 1, disconnect = 2 }

    static func service() -> io_service_t { IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("itlmt7925")) }

    static func status() -> WifiStatus {
        var s = WifiStatus()
        let svc = service(); guard svc != 0 else { return s }
        defer { IOObjectRelease(svc) }
        func prop(_ k: String) -> Any? { IORegistryEntryCreateCFProperty(svc, k as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() }
        func num(_ k: String) -> Int { (prop(k) as? NSNumber).map { Int(Int32(truncatingIfNeeded: $0.int64Value)) } ?? 0 }
        s.present = true
        s.state = prop("wifi-state") as? String ?? "off"
        s.error = prop("wifi-error") as? String ?? ""
        s.ssid = prop("wifi-ssid") as? String ?? ""
        s.channel = num("wifi-channel"); s.rssi = num("wifi-rssi"); s.rate = num("wifi-rate"); s.scanGen = num("wifi-scan-gen")
        for d in prop("wifi-networks") as? [[String: Any]] ?? [] {
            guard let ssid = d["ssid"] as? String, !ssid.isEmpty else { continue }
            let rssi = (d["rssi"] as? NSNumber).map { Int(Int32(truncatingIfNeeded: $0.int64Value)) } ?? 0
            s.nets.append(WifiNet(ssid: ssid, channel: (d["channel"] as? NSNumber)?.intValue ?? 0, rssi: rssi,
                                  security: d["security"] as? String ?? "?"))
        }
        // one entry per SSID: the strongest BSS
        var best: [String: WifiNet] = [:]
        for n in s.nets { if let b = best[n.ssid], (b.rssi != 0 && b.rssi >= n.rssi) || n.rssi == 0 { continue }; best[n.ssid] = n }
        s.nets = best.values.sorted { ($0.rssi == 0 ? -200 : $0.rssi) > ($1.rssi == 0 ? -200 : $1.rssi) }
        return s
    }

    @discardableResult
    static func call(_ sel: Sel, input: UnsafeRawBufferPointer? = nil) -> kern_return_t {
        let svc = service(); guard svc != 0 else { return kIOReturnNotFound }
        defer { IOObjectRelease(svc) }
        var conn: io_connect_t = 0
        var kr = IOServiceOpen(svc, mach_task_self_, 0, &conn)
        guard kr == KERN_SUCCESS else { return kr }
        defer { IOServiceClose(conn) }
        kr = IOConnectCallStructMethod(conn, sel.rawValue, input?.baseAddress, input?.count ?? 0, nil, nil)
        return kr
    }

    static func join(ssid: String, psk: String) -> kern_return_t {
        // struct itl_uc_join { uint32_t abi; char ssid[36]; char psk[68]; }
        var buf = [UInt8](repeating: 0, count: 4 + 36 + 68)
        withUnsafeBytes(of: abi.littleEndian) { buf.replaceSubrange(0..<4, with: $0) }
        let s = Array(ssid.utf8.prefix(32)), p = Array(psk.utf8.prefix(63))
        buf.replaceSubrange(4..<(4 + s.count), with: s)
        buf.replaceSubrange(40..<(40 + p.count), with: p)
        defer { for i in buf.indices { buf[i] = 0 } }
        return buf.withUnsafeBytes { call(.join, input: $0) }
    }
}

// ---- keychain ------------------------------------------------------------------------------------------------------------
enum Keychain {
    static let service = "com.itlmt7925.wifi"
    static func query(_ ssid: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service, kSecAttrAccount as String: ssid]
    }
    static func get(_ ssid: String) -> String? {
        var q = query(ssid); q[kSecReturnData as String] = true; q[kSecMatchLimit as String] = kSecMatchLimitOne
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }
    static func set(_ ssid: String, _ psk: String) {
        SecItemDelete(query(ssid) as CFDictionary)
        var q = query(ssid); q[kSecValueData as String] = Data(psk.utf8)
        q[kSecAttrLabel as String] = "Wi-Fi \(ssid) (itlmt7925)"
        SecItemAdd(q as CFDictionary, nil)
    }
    static func remove(_ ssid: String) { SecItemDelete(query(ssid) as CFDictionary) }
    static func known() -> [String] {
        var q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecReturnAttributes as String: true, kSecMatchLimit as String: kSecMatchLimitAll]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let a = out as? [[String: Any]] else { return [] }
        q.removeAll()
        return a.compactMap { $0[kSecAttrAccount as String] as? String }
    }
}

// ---- login item (a user LaunchAgent; no signing needed) ------------------------------------------------------------------
enum LoginItem {
    static let label = "com.itlmt7925.ItlWiFi"
    static var plist: URL { FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/\(label).plist") }
    static var enabled: Bool { FileManager.default.fileExists(atPath: plist.path) }
    static func set(_ on: Bool) {
        if !on { try? FileManager.default.removeItem(at: plist); return }
        let exe = Bundle.main.executablePath ?? CommandLine.arguments[0]
        let d: [String: Any] = ["Label": label, "ProgramArguments": [exe], "RunAtLoad": true, "ProcessType": "Interactive",
                                "LimitLoadToSessionType": "Aqua"]
        try? FileManager.default.createDirectory(at: plist.deletingLastPathComponent(), withIntermediateDirectories: true)
        (d as NSDictionary).write(to: plist, atomically: true)
    }
}

// ---- model -----------------------------------------------------------------------------------------------------------------
@MainActor
final class WifiModel: ObservableObject {
    @Published var st = WifiStatus()
    @Published var speed: SpeedResult?
    @Published var speedRunning = false
    @Published var speedServer = "auto"
    @Published var speedPath = SpeedPath.internet
    @Published var expanded: String?            // SSID whose password row is open; "" = "Other network"
    @Published var message: String?
    @Published var loginEnabled = LoginItem.enabled
    @Published var known = Set(Keychain.known())
    var pending: (ssid: String, psk: String, remember: Bool)?
    var userOff = false
    var autoTried = Set<String>()
    var onChange: (() -> Void)?
    private var timer: Timer?

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in Task { @MainActor in self?.refresh() } }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.autoJoin() }
    }

    var connected: Bool { st.state == "connected" }
    var busy: Bool { st.state == "connecting" || st.state == "scanning" }

    func refresh() {
        let old = st
        st = Driver.status()
        if let p = pending, connected, st.ssid == p.ssid {
            if p.remember { Keychain.set(p.ssid, p.psk); known = Set(Keychain.known()) }
            pending = nil
        } else if let p = pending, st.state == "failed", st.ssid == p.ssid, old.state != "failed" {
            pending = nil
            message = "Could not join \(p.ssid): " + (st.error.isEmpty ? "the connection failed." : st.error)
        }
        if (st.state == "idle" || st.state == "failed") && !userOff && pending == nil && st.scanGen != old.scanGen { autoJoin() }
        onChange?()
    }

    func scan() {
        let kr = Driver.call(.scan)
        if kr != KERN_SUCCESS && kr != kIOReturnBusy { message = "Scan failed: " + explain(kr) }
    }

    func select(_ n: WifiNet) {
        message = nil
        if connected && st.ssid == n.ssid { return }
        if let psk = Keychain.get(n.ssid) { join(n.ssid, psk, remember: false); return }
        expanded = expanded == n.ssid ? nil : n.ssid
    }

    func join(_ ssid: String, _ psk: String, remember: Bool) {
        guard (1...32).contains(ssid.utf8.count) else { message = "The network name must be 1-32 bytes."; return }
        guard (8...63).contains(psk.utf8.count) else { message = "The password must be 8-63 characters."; return }
        let kr = Driver.join(ssid: ssid, psk: psk)
        if kr != KERN_SUCCESS { message = "Could not join \(ssid): " + explain(kr); return }
        userOff = false; expanded = nil; message = nil
        pending = (ssid, psk, remember)
        refresh()
    }

    func disconnect() {
        let kr = Driver.call(.disconnect)
        if kr != KERN_SUCCESS { message = "Disconnect failed: " + explain(kr); return }
        userOff = true
        refresh()
    }

    func forget(_ ssid: String) {
        Keychain.remove(ssid); known = Set(Keychain.known())
        if st.ssid == ssid && (connected || busy) { disconnect() }
    }

    func setLogin(_ on: Bool) { LoginItem.set(on); loginEnabled = LoginItem.enabled }

    func runSpeedTest() {
        guard !speedRunning, connected else { return }
        speedRunning = true; message = nil
        let server = speedServer, path = speedPath
        Task.detached(priority: .userInitiated) {
            let r = SpeedTest.run(serverID: server, path: path)
            await MainActor.run { self.speed = r; self.speedRunning = false; self.onChange?() }
        }
    }

    // At start and after scans while not connected: join the strongest saved network in range, once per network.
    func autoJoin() {
        guard st.present, !userOff, pending == nil, st.state == "idle" || st.state == "failed" else { return }
        guard let n = st.nets.first(where: { known.contains($0.ssid) && $0.joinable && !autoTried.contains($0.ssid) }),
              let psk = Keychain.get(n.ssid) else { return }
        autoTried.insert(n.ssid)
        join(n.ssid, psk, remember: false)
    }

    func explain(_ kr: kern_return_t) -> String {
        switch kr {
        case kIOReturnNotPrivileged: return "only the user at the console (or root) can change Wi-Fi."
        case kIOReturnNotReady: return "the driver is still starting. Try again in a moment."
        case kIOReturnNotFound: return "itlmt7925 is not loaded."
        case kIOReturnUnsupported: return "this itlmt7925 build has no runtime control."
        default: return String(format: "IOKit error 0x%08x", kr)
        }
    }
}

func bars(_ rssi: Int) -> Double { rssi == 0 ? 0.66 : rssi >= -60 ? 1 : rssi >= -70 ? 0.66 : rssi >= -80 ? 0.33 : 0.1 }

// ---- views -------------------------------------------------------------------------------------------------------------------
struct SectionTitle: View {
    let text: String
    var body: some View {
        Text(text).font(.system(size: 11, weight: .semibold)).foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 14).padding(.top, 8).padding(.bottom, 2)
    }
}

struct HoverRow<Content: View>: View {
    let action: (() -> Void)?
    @ViewBuilder let content: Content
    @State private var hover = false
    var body: some View {
        content
            .padding(.horizontal, 8).padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 6).fill(hover && action != nil ? Color.primary.opacity(0.08) : .clear))
            .contentShape(Rectangle())
            .onHover { hover = $0 }
            .onTapGesture { action?() }
            .padding(.horizontal, 6)
    }
}

struct SignalIcon: View {
    let rssi: Int
    var active = false
    var body: some View {
        ZStack {
            Circle().fill(active ? Color.accentColor : Color.primary.opacity(0.1)).frame(width: 26, height: 26)
            Image(systemName: "wifi", variableValue: bars(rssi))
                .font(.system(size: 12, weight: .semibold)).foregroundStyle(active ? .white : .primary)
        }
    }
}

struct PasswordRow: View {
    @ObservedObject var m: WifiModel
    let ssid: String?                           // nil: ask for the name too
    @State private var name = ""
    @State private var psk = ""
    @State private var remember = true
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            if ssid == nil { TextField("Network name", text: $name).textFieldStyle(.roundedBorder) }
            SecureField("Password", text: $psk).textFieldStyle(.roundedBorder).onSubmit(go)
            HStack {
                Toggle("Remember", isOn: $remember).toggleStyle(.checkbox).font(.system(size: 12))
                Spacer()
                Button("Cancel") { m.expanded = nil }.controlSize(.small)
                Button("Join", action: go).controlSize(.small).keyboardShortcut(.defaultAction)
                    .disabled(psk.utf8.count < 8 || (ssid == nil && name.isEmpty))
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 6)
    }
    func go() { m.join(ssid ?? name, psk, remember: remember) }
}

struct SpeedCard: View {
    @ObservedObject var m: WifiModel
    func cell(_ title: String, _ value: Double?, _ unit: String) -> some View {
        VStack(spacing: 1) {
            Text(SpeedTest.fmt(value)).font(.system(size: 17, weight: .semibold, design: .rounded)).monospacedDigit()
            Text("\(title) · \(unit)").font(.system(size: 10)).foregroundStyle(.secondary)
        }.frame(maxWidth: .infinity)
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Picker("Path", selection: $m.speedPath) {
                ForEach(SpeedPath.allCases, id: \.self) { Text($0.label).tag($0) }
            }.disabled(m.speedRunning)
            Picker("Server", selection: $m.speedServer) {
                Text("Auto · fallback servers").tag("auto")
                ForEach(SpeedTest.servers) { Text($0.name).tag($0.id) }
            }.disabled(m.speedRunning)
            if let r = m.speed {
                if r.downMbps != nil || r.upMbps != nil {
                    HStack { cell("Down", r.downMbps, "Mb/s"); cell("Up", r.upMbps, "Mb/s"); cell("TCP", r.pingMs, "ms") }
                }
                if let e = r.error {
                    Text(e).font(.system(size: 11)).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true)
                }
                if !r.server.isEmpty {
                    Text(r.server + (r.remoteIP.isEmpty ? "" : " · " + r.remoteIP))
                        .font(.system(size: 10)).foregroundStyle(.secondary).textSelection(.enabled)
                }
                if let note = r.note { Text(note).font(.system(size: 10)).foregroundStyle(.secondary) }
            }
            HStack {
                if m.speedRunning {
                    ProgressView().controlSize(.small)
                    Text("Testing…").font(.system(size: 11)).foregroundStyle(.secondary)
                } else if let r = m.speed {
                    VStack(alignment: .leading, spacing: 2) {
                        Text((r.downMbps == nil && r.upMbps == nil ? "Attempted " : "Measured ") + r.at.formatted(date: .omitted, time: .shortened))
                        Text(r.path)
                    }.font(.system(size: 10)).foregroundStyle(.secondary)
                }
                Spacer()
                Button(m.speed == nil ? "Run Speed Test" : "Run Again") { m.runSpeedTest() }
                    .controlSize(.small).disabled(m.speedRunning || !m.connected)
            }
        }
        .font(.system(size: 11))
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 8).fill(Color.primary.opacity(0.05)))
        .padding(.horizontal, 14)
    }
}

struct ContentView: View {
    @ObservedObject var m: WifiModel
    var statusLine: String {
        switch m.st.state {
        case "connected": return m.st.ssid
        case "connecting": return m.st.ssid.isEmpty ? "Connecting…" : "Connecting to \(m.st.ssid)…"
        case "scanning": return "Scanning…"
        case "failed": return "Not connected"
        case "off": return "Off (boot-arg itlmt_m3=1 missing)"
        case "absent": return "Driver not loaded"
        default: return "Not connected"
        }
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                VStack(alignment: .leading, spacing: 1) {
                    Text("Wi-Fi").font(.system(size: 13, weight: .semibold))
                    Text("MediaTek MT7925 · itlmt7925").font(.system(size: 10)).foregroundStyle(.secondary)
                }
                Spacer()
                if m.busy { ProgressView().controlSize(.small) }
                Button { m.scan() } label: { Image(systemName: "arrow.clockwise") }
                    .buttonStyle(.borderless).help("Scan now").disabled(!m.st.present)
            }
            .padding(.horizontal, 14).padding(.top, 12).padding(.bottom, 6)

            if m.st.present && m.st.state != "absent" {
                HoverRow(action: nil) {
                    HStack(spacing: 10) {
                        SignalIcon(rssi: m.st.rssi, active: m.connected)
                        VStack(alignment: .leading, spacing: 1) {
                            Text(statusLine).font(.system(size: 13, weight: .medium)).lineLimit(1)
                            if m.connected {
                                Text("Channel \(m.st.channel)" + (m.st.rssi == 0 ? "" : " · \(m.st.rssi) dBm") + " · \(m.st.rate) Mb/s")
                                    .font(.system(size: 11)).foregroundStyle(.secondary)
                            } else if !m.st.error.isEmpty {
                                Text(m.st.error).font(.system(size: 11)).foregroundStyle(.secondary).lineLimit(2)
                            }
                        }
                        Spacer()
                        if m.connected || m.busy {
                            Menu {
                                Button("Disconnect") { m.disconnect() }
                                if m.known.contains(m.st.ssid) { Button("Forget This Network") { m.forget(m.st.ssid) } }
                            } label: { Image(systemName: "ellipsis.circle") }
                            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
                        }
                    }
                }
            } else {
                Text(statusLine).font(.system(size: 12)).foregroundStyle(.secondary).padding(.horizontal, 14).padding(.vertical, 6)
            }

            if let msg = m.message {
                HStack(alignment: .top, spacing: 6) {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    Text(msg).font(.system(size: 11)).fixedSize(horizontal: false, vertical: true)
                    Spacer()
                    Button { m.message = nil } label: { Image(systemName: "xmark") }.buttonStyle(.borderless)
                }
                .padding(8).background(RoundedRectangle(cornerRadius: 6).fill(Color.orange.opacity(0.12)))
                .padding(.horizontal, 14).padding(.vertical, 4)
            }

            if m.connected || m.speed != nil {
                SectionTitle(text: "Speed")
                SpeedCard(m: m)
            }

            SectionTitle(text: "Networks")
            ScrollView {
                VStack(spacing: 0) {
                    if m.st.nets.isEmpty {
                        Text("No networks found yet").font(.system(size: 12)).foregroundStyle(.secondary)
                            .padding(.horizontal, 14).padding(.vertical, 6).frame(maxWidth: .infinity, alignment: .leading)
                    }
                    ForEach(m.st.nets.filter { !(m.connected && $0.ssid == m.st.ssid) }.prefix(20), id: \.ssid) { n in
                        HoverRow(action: n.joinable ? { m.select(n) } : nil) {
                            HStack(spacing: 10) {
                                SignalIcon(rssi: n.rssi)
                                VStack(alignment: .leading, spacing: 1) {
                                    Text(n.ssid).font(.system(size: 13)).lineLimit(1)
                                        .foregroundStyle(n.joinable ? .primary : .secondary)
                                    Text(sub(n)).font(.system(size: 10)).foregroundStyle(.secondary)
                                }
                                Spacer()
                                if n.locked { Image(systemName: "lock.fill").font(.system(size: 10)).foregroundStyle(.secondary) }
                            }
                        }
                        if m.expanded == n.ssid { PasswordRow(m: m, ssid: n.ssid) }
                    }
                    HoverRow(action: { m.expanded = m.expanded == "" ? nil : "" }) {
                        HStack(spacing: 10) {
                            ZStack { Circle().fill(Color.primary.opacity(0.1)).frame(width: 26, height: 26)
                                     Image(systemName: "ellipsis").font(.system(size: 12, weight: .semibold)) }
                            Text("Other Network…").font(.system(size: 13))
                            Spacer()
                        }
                    }
                    if m.expanded == "" { PasswordRow(m: m, ssid: nil) }
                }
            }
            .frame(height: listHeight)

            Divider().padding(.top, 6)
            HStack {
                Toggle("Open at Login", isOn: Binding(get: { m.loginEnabled }, set: { m.setLogin($0) }))
                    .toggleStyle(.switch).controlSize(.mini).font(.system(size: 12))
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }.buttonStyle(.borderless).font(.system(size: 12))
            }
            .padding(.horizontal, 14).padding(.vertical, 8)
        }
        .frame(width: 330)
    }
    // the list grows with its rows (38 pt each, plus the open password row) and scrolls beyond 320 pt
    var listHeight: CGFloat {
        let rows = m.st.nets.filter { !(m.connected && $0.ssid == m.st.ssid) }.prefix(20).count + 1
        return min(CGFloat(max(rows, 2)) * 38 + (m.expanded != nil ? (m.expanded == "" ? 110 : 80) : 0), 320)
    }
    func sub(_ n: WifiNet) -> String {
        var s = n.channel > 14 ? "5 GHz" : "2.4 GHz"
        if n.rssi != 0 { s += " · \(n.rssi) dBm" }
        s += n.locked ? " · " + n.security.uppercased() : " · Open"
        if m.known.contains(n.ssid) { s += " · Saved" }
        if !n.joinable { s += " · not supported yet" }
        return s
    }
}

// ---- status item -------------------------------------------------------------------------------------------------------------
@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
    let popover = NSPopover()
    let model = WifiModel()

    func applicationDidFinishLaunching(_ n: Notification) {
        popover.behavior = .transient
        popover.animates = false
        popover.contentViewController = NSHostingController(rootView: ContentView(m: model))
        item.button?.target = self
        item.button?.action = #selector(toggle)
        model.onChange = { [weak self] in self?.updateIcon() }
        updateIcon()
    }

    @objc func toggle() {
        guard let b = item.button else { return }
        if popover.isShown { popover.performClose(nil); return }
        model.refresh()
        popover.show(relativeTo: b.bounds, of: b, preferredEdge: .minY)
        popover.contentViewController?.view.window?.makeKey()
    }

    func updateIcon() {
        guard let b = item.button else { return }
        let s = model.st
        var name = "wifi.slash", value = 1.0, alpha: CGFloat = 1
        switch s.state {
        case "connected": name = "wifi"; value = bars(s.rssi)
        case "connecting", "scanning": name = "wifi"; alpha = 0.45
        case "failed": name = "wifi.exclamationmark"
        default: break
        }
        let img = NSImage(systemSymbolName: name, variableValue: value, accessibilityDescription: "Wi-Fi")
        img?.isTemplate = true
        b.image = img
        b.alphaValue = alpha
        var tip = s.state == "connected" ? "\(s.ssid) · channel \(s.channel)" + (s.rssi == 0 ? "" : " · \(s.rssi) dBm") : s.state
        if let r = model.speed, r.error == nil { tip += " · ↓\(SpeedTest.fmt(r.downMbps)) ↑\(SpeedTest.fmt(r.upMbps)) Mb/s" }
        b.toolTip = tip
    }
}

@main
struct ItlWiFiMain {
    static func main() {
        MainActor.assumeIsolated {
            let app = NSApplication.shared
            let delegate = AppDelegate()
            app.delegate = delegate
            app.setActivationPolicy(.accessory)
            app.run()
        }
    }
}

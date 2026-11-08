// MacWifiApp.swift — the menu bar app for macwifid (Phase 5). Original code (MIT).
//
// Talks to the background service over /var/run/macwifid.sock (one JSON request per connection; see
// Sources/macwifictl/daemon.c). The socket is root:admin 0660, so this works for admin users. Passwords go
// only to the service, which keeps a derived key (PMK), never the password.

import AppKit
import SwiftUI

// MARK: - Protocol

struct NetworkInfo: Decodable, Identifiable, Hashable {
    let ssid: String
    let rssi: Int
    let freq: Int
    let secured: Bool
    let usable: Bool
    let known: Bool
    let why: String?
    var id: String { ssid }
}

struct StatusReply: Decodable {
    let ok: Bool
    let state: String?
    let adapter: Bool?
    let power: Bool?
    let ssid: String?
    let rssi: Int?
    let freq: Int?
    let ip: String?
    let error: String?
    let version: String?
}

struct NetworksReply: Decodable {
    let ok: Bool
    let error: String?
    let networks: [NetworkInfo]?
}

struct SimpleReply: Decodable {
    let ok: Bool
    let error: String?
}

enum DaemonError: Error {
    case notRunning
    case io
}

enum Daemon {
    static let socketPath = "/var/run/macwifid.sock"

    // Blocking: call from a background queue.
    static func request(_ object: [String: Any], timeout: Int = 75) throws -> Data {
        var body = try JSONSerialization.data(withJSONObject: object)
        body.append(0x0A)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { throw DaemonError.notRunning }
        defer { close(fd) }
        var tv = timeval(tv_sec: timeout, tv_usec: 0)
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        addr.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let path = Array(socketPath.utf8)
        withUnsafeMutableBytes(of: &addr.sun_path) { raw in
            raw.copyBytes(from: path.prefix(raw.count - 1))
        }
        let rc = withUnsafePointer(to: &addr) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        guard rc == 0 else { throw DaemonError.notRunning }

        try body.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            var off = 0
            while off < p.count {
                let n = write(fd, p.baseAddress! + off, p.count - off)
                if n <= 0 { throw DaemonError.io }
                off += n
            }
        }
        body.resetBytes(in: 0..<body.count)   // may hold a password

        var out = Data()
        var buf = [UInt8](repeating: 0, count: 65536)
        while true {
            let n = read(fd, &buf, buf.count)
            if n <= 0 { break }
            out.append(buf, count: n)
            if buf[0..<n].contains(0x0A) { break }
        }
        if out.isEmpty { throw DaemonError.io }
        return out
    }
}

// MARK: - Model

final class Model: ObservableObject {
    @Published var status: StatusReply?
    @Published var networks: [NetworkInfo] = []
    @Published var daemonRunning = true
    @Published var scanning = false
    @Published var joining: String?

    private var timer: Timer?
    private let queue = DispatchQueue(label: "macwifi.requests", qos: .userInitiated)

    init() {
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 3, repeats: true) { [weak self] _ in self?.refresh() }
    }

    var state: String { daemonRunning ? (status?.state ?? "unknown") : "no_service" }
    var connectedSSID: String? { state == "connected" ? status?.ssid : nil }

    // Signal strength 0...1 for the bars.
    static func strength(_ rssi: Int) -> Double {
        if rssi >= -55 { return 1.0 }
        if rssi >= -67 { return 0.67 }
        if rssi >= -78 { return 0.34 }
        return 0.1
    }

    var iconName: String {
        switch state {
        case "connected": return "wifi"
        case "off": return "wifi.slash"
        case "connecting", "starting", "scanning", "idle", "asleep": return "wifi"
        default: return "wifi.exclamationmark"
        }
    }

    var iconValue: Double {
        if state == "connected", let r = status?.rssi { return Model.strength(r) }
        return state == "idle" || state == "scanning" || state == "connecting" || state == "starting" ? 0.0 : 1.0
    }

    var statusLine: String {
        switch state {
        case "no_service": return "The macwifid service isn't running."
        case "off": return "Wi-Fi is off."
        case "no_adapter": return "Plug in the UGREEN adapter."
        case "starting": return "Starting the adapter…"
        case "idle": return "Not connected."
        case "scanning": return "Looking for networks…"
        case "connecting": return "Connecting to \(status?.ssid ?? "…")…"
        case "connected":
            let ip = status?.ip ?? ""
            return ip.isEmpty ? "Connected, getting an address…" : "Connected · \(ip)"
        case "asleep": return "Paused while the Mac sleeps."
        case "error": return status?.error?.isEmpty == false ? status!.error! : "The adapter needs attention."
        default: return ""
        }
    }

    func refresh() {
        queue.async {
            let reply = (try? Daemon.request(["cmd": "status"], timeout: 5)).flatMap {
                try? JSONDecoder().decode(StatusReply.self, from: $0)
            }
            DispatchQueue.main.async {
                self.daemonRunning = reply != nil
                self.status = reply
            }
        }
    }

    func scan() {
        guard !scanning, daemonRunning, state != "off", state != "no_adapter" else { return }
        scanning = true
        queue.async {
            let reply = (try? Daemon.request(["cmd": "scan"], timeout: 35)).flatMap {
                try? JSONDecoder().decode(NetworksReply.self, from: $0)
            }
            DispatchQueue.main.async {
                self.scanning = false
                if let nets = reply?.networks { self.networks = nets }
                self.refresh()
            }
        }
    }

    func join(_ net: NetworkInfo) {
        guard net.usable, joining == nil else { return }
        var request: [String: Any] = ["cmd": "join", "ssid": net.ssid]
        if net.secured && !net.known {
            guard let password = Model.askPassword(for: net.ssid) else { return }
            request["password"] = password
        }
        send(request, joining: net.ssid)
    }

    func joinWithPassword(_ net: NetworkInfo) {
        guard let password = Model.askPassword(for: net.ssid) else { return }
        send(["cmd": "join", "ssid": net.ssid, "password": password], joining: net.ssid)
    }

    func disconnect() { send(["cmd": "disconnect"]) }
    func forget(_ net: NetworkInfo) { send(["cmd": "forget", "ssid": net.ssid]) }
    func setPower(_ on: Bool) { send(["cmd": "power", "on": on]) }

    private func send(_ request: [String: Any], joining ssid: String? = nil) {
        joining = ssid
        queue.async {
            var message: String?
            do {
                let data = try Daemon.request(request)
                let reply = try JSONDecoder().decode(SimpleReply.self, from: data)
                if !reply.ok { message = reply.error ?? "It didn't work." }
            } catch {
                message = "The macwifid service didn't answer."
            }
            DispatchQueue.main.async {
                self.joining = nil
                self.refresh()
                if ssid != nil { self.scanSoon() }
                if let message = message { Model.showError(message) }
            }
        }
    }

    private func scanSoon() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { self.scan() }
    }

    static func askPassword(for ssid: String) -> String? {
        let alert = NSAlert()
        alert.messageText = "Join “\(ssid)”"
        alert.informativeText = "Enter the Wi-Fi password. macwifid keeps a key derived from it, not the password itself."
        alert.addButton(withTitle: "Join")
        alert.addButton(withTitle: "Cancel")
        let field = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        field.placeholderString = "Password"
        alert.accessoryView = field
        alert.window.initialFirstResponder = field
        NSApp.activate(ignoringOtherApps: true)
        let answer = alert.runModal()
        let password = field.stringValue
        field.stringValue = ""
        return answer == .alertFirstButtonReturn && !password.isEmpty ? password : nil
    }

    static func showError(_ text: String) {
        let alert = NSAlert()
        alert.messageText = "UGREEN Wi-Fi"
        alert.informativeText = text.prefix(1).uppercased() + text.dropFirst()
        alert.alertStyle = .warning
        NSApp.activate(ignoringOtherApps: true)
        alert.runModal()
    }
}

// MARK: - Views

struct NetworkRow: View {
    @EnvironmentObject var model: Model
    let net: NetworkInfo

    var body: some View {
        let isCurrent = model.connectedSSID == net.ssid
        HStack(spacing: 8) {
            Image(systemName: "wifi", variableValue: Model.strength(net.rssi))
                .frame(width: 18)
            Text(net.ssid)
                .fontWeight(isCurrent ? .semibold : .regular)
                .lineLimit(1)
            if model.joining == net.ssid { ProgressView().controlSize(.small) }
            Spacer()
            Text(net.freq >= 5000 ? "5 GHz" : "2.4 GHz")
                .font(.caption2)
                .foregroundStyle(.secondary)
            if net.secured { Image(systemName: "lock.fill").font(.caption).foregroundStyle(.secondary) }
            if isCurrent { Image(systemName: "checkmark").font(.caption) }
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 6)
        .contentShape(Rectangle())
        .opacity(net.usable ? 1 : 0.45)
        .help(net.usable ? (net.known ? "Remembered network" : "") : (net.why ?? "Not supported yet"))
        .onTapGesture { if !isCurrent { model.join(net) } }
        .contextMenu {
            if net.usable && !isCurrent { Button("Join") { model.join(net) } }
            if net.usable && net.secured { Button("Join with a different password…") { model.joinWithPassword(net) } }
            if net.known { Button("Forget This Network") { model.forget(net) } }
        }
    }
}

struct MenuView: View {
    @EnvironmentObject var model: Model

    var versionLine: String {
        let app = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
        let svc = model.status?.version ?? "not running"
        return "MacWifi \(app) · service \(svc) · Quit keeps Wi-Fi running"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("UGREEN Wi-Fi").font(.headline)
                Spacer()
                Toggle("", isOn: Binding(get: { model.status?.power ?? false }, set: { model.setPower($0) }))
                    .toggleStyle(.switch)
                    .labelsHidden()
                    .disabled(!model.daemonRunning)
            }
            Text(model.statusLine)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if let ssid = model.connectedSSID {
                HStack {
                    Image(systemName: "wifi", variableValue: Model.strength(model.status?.rssi ?? -90))
                    Text(ssid).fontWeight(.semibold).lineLimit(1)
                    Spacer()
                    Button("Disconnect") { model.disconnect() }
                }
            }
            Divider()
            HStack {
                Text("Networks").font(.caption).foregroundStyle(.secondary)
                Spacer()
                if model.scanning { ProgressView().controlSize(.small) }
            }
            if model.networks.isEmpty {
                Text(model.scanning ? "Looking…" : "No networks yet.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(model.networks) { NetworkRow(net: $0) }
                    }
                }
                .frame(maxHeight: 320)
            }
            Divider()
            HStack {
                Button("Scan Again") { model.scan() }.disabled(model.scanning)
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
            Text(versionLine)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(12)
        .frame(width: 320)
        .onAppear { model.refresh(); model.scan() }
    }
}

@main
struct MacWifiApp: App {
    @StateObject private var model = Model()

    var body: some Scene {
        MenuBarExtra {
            MenuView().environmentObject(model)
        } label: {
            Image(systemName: model.iconName, variableValue: model.iconValue)
        }
        .menuBarExtraStyle(.window)
    }
}

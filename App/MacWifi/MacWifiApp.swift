// MacWifiApp.swift — the menu bar app for macwifid (Phase 5). Original code (MIT).
//
// Talks to the background service over /var/run/macwifid.sock (one JSON request per connection; see
// Sources/macwifictl/daemon.c). The socket is root:admin 0660, so this works for admin users. Passwords go
// only to the service, which keeps a derived key (PMK) for WPA2 networks and the password itself only for networks
// that offer WPA3 (in a root-only file).

import AppKit
import ApplicationServices
import SwiftUI
import WebKit

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
    let security: String?  // "wpa3", "wpa2" or "open" while connected
    let captive: String?   // "yes" (sign-in page in the way), "no", "unknown"
    let portal: String?
}

struct NetworksReply: Decodable {
    let ok: Bool
    let error: String?
    let networks: [NetworkInfo]?
}

struct BtDevice: Decodable, Identifiable, Hashable {
    let id: String          // the device's address (never shown)
    let name: String
    let transport: String   // "le" or "classic"
    let type: String        // "Keyboard", "Mouse", "Audio", "Phone"... or ""
    let rssi: Int
    let connectable: Bool
    let age: Int
    let paired: Bool?
    let state: String?      // "connecting", "pairing", "connected", "failed", ""
    let passkey: Int?       // shown while pairing a keyboard
    let why: String?        // after "failed"

    var isConnected: Bool { state == "connected" }
    var isBusy: Bool { state == "connecting" || state == "pairing" }
}

struct BtReply: Decodable {
    let ok: Bool
    let power: Bool?
    let up: Bool?
    let scanning: Bool?
    let controller: String?
    let error: String?
    let devices: [BtDevice]?
    let last_classic: Int?
    let last_le: Int?
    let input_app: Bool?
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

    // Opens a connection, sends one request and returns the socket for a streaming reply (bt_input).
    static func openStream(_ object: [String: Any]) -> Int32? {
        guard var body = try? JSONSerialization.data(withJSONObject: object) else { return nil }
        body.append(0x0A)
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else { return nil }
        var tv = timeval(tv_sec: 5, tv_usec: 0)   // the service sends a keepalive every 0.5 s
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        addr.sun_len = UInt8(MemoryLayout<sockaddr_un>.size)
        let path = Array(socketPath.utf8)
        withUnsafeMutableBytes(of: &addr.sun_path) { raw in raw.copyBytes(from: path.prefix(raw.count - 1)) }
        let rc = withUnsafePointer(to: &addr) { p in
            p.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
        }
        let wrote = body.withUnsafeBytes { p in write(fd, p.baseAddress!, p.count) }
        if rc != 0 || wrote != body.count { close(fd); return nil }
        return fd
    }
}

// MARK: - Bluetooth keyboard and mouse input

// macwifid decodes the devices' reports and streams small text lines; this posts them as keyboard and mouse
// events into the user's session (a system daemon can't). Needs Accessibility permission for MacWifi.
final class InputInjector {
    static let shared = InputInjector()
    private let source = CGEventSource(stateID: .hidSystemState)
    private var buttons: UInt8 = 0
    private var lastClick = Date.distantPast
    private var lastClickPoint = CGPoint.zero
    private var clickCount: Int64 = 1
    private var started = false

    func start() {
        guard !started else { return }
        started = true
        Thread.detachNewThread { self.run() }
    }

    static var trusted: Bool { AXIsProcessTrusted() }

    static func askForTrust() {
        let key = kAXTrustedCheckOptionPrompt.takeUnretainedValue() as String
        _ = AXIsProcessTrustedWithOptions([key: true] as CFDictionary)
    }

    private func run() {
        while true {
            if let fd = Daemon.openStream(["cmd": "bt_input"]) {
                var pending = Data()
                var buf = [UInt8](repeating: 0, count: 8192)
                while true {
                    let n = read(fd, &buf, buf.count)
                    if n <= 0 { break }
                    pending.append(buf, count: n)
                    while let nl = pending.firstIndex(of: 0x0A) {
                        let line = String(decoding: pending[pending.startIndex..<nl], as: UTF8.self)
                        pending.removeSubrange(pending.startIndex...nl)
                        handle(line)
                    }
                }
                close(fd)
            }
            sleep(2)
        }
    }

    private func handle(_ line: String) {
        let f = line.split(separator: " ")
        guard let kind = f.first else { return }      // empty lines are keepalives; the first is the JSON reply
        if kind == "k", f.count == 4, let code = UInt16(f[1]), let down = Int(f[2]), let mods = UInt8(f[3]) {
            key(code, down == 1, mods)
        } else if kind == "m", f.count == 5, let b = UInt8(f[1]), let dx = Int(f[2]), let dy = Int(f[3]), let w = Int(f[4]) {
            mouse(b, dx, dy, w)
        }
    }

    private static func flags(_ mods: UInt8) -> CGEventFlags {
        var fl: CGEventFlags = []
        if mods & 0x11 != 0 { fl.insert(.maskControl) }
        if mods & 0x22 != 0 { fl.insert(.maskShift) }
        if mods & 0x44 != 0 { fl.insert(.maskAlternate) }
        if mods & 0x88 != 0 { fl.insert(.maskCommand) }
        return fl
    }

    private static let modifierKeys: Set<UInt16> = [0x37, 0x36, 0x38, 0x3C, 0x3A, 0x3D, 0x3B, 0x3E]

    private func key(_ code: UInt16, _ down: Bool, _ mods: UInt8) {
        guard let e = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down) else { return }
        if InputInjector.modifierKeys.contains(code) { e.type = .flagsChanged }
        e.flags = InputInjector.flags(mods)
        e.post(tap: .cghidEventTap)
    }

    private static func screenBounds() -> CGRect {
        var ids = [CGDirectDisplayID](repeating: 0, count: 16)
        var count: UInt32 = 0
        guard CGGetActiveDisplayList(16, &ids, &count) == .success, count > 0 else { return CGRect(x: 0, y: 0, width: 1e5, height: 1e5) }
        return ids.prefix(Int(count)).map { CGDisplayBounds($0) }.reduce(CGRect.null) { $0.union($1) }
    }

    private func mouse(_ b: UInt8, _ dx: Int, _ dy: Int, _ wheel: Int) {
        let current = CGEvent(source: nil)?.location ?? .zero
        // A little acceleration: boot-protocol mice report raw counts.
        func accel(_ d: Int) -> CGFloat { CGFloat(d) * (1.0 + min(abs(CGFloat(d)), 20) * 0.12) }
        let r = InputInjector.screenBounds()
        let p = CGPoint(x: min(max(current.x + accel(dx), r.minX), r.maxX - 1),
                        y: min(max(current.y + accel(dy), r.minY), r.maxY - 1))
        if dx != 0 || dy != 0 {
            let type: CGEventType = b & 1 != 0 ? .leftMouseDragged : b & 2 != 0 ? .rightMouseDragged
                                  : b & 4 != 0 ? .otherMouseDragged : .mouseMoved
            let btn: CGMouseButton = b & 1 != 0 ? .left : b & 2 != 0 ? .right : .center
            CGEvent(mouseEventSource: source, mouseType: type, mouseCursorPosition: p, mouseButton: btn)?.post(tap: .cghidEventTap)
        }
        let changes: [(UInt8, CGEventType, CGEventType, CGMouseButton)] = [
            (1, .leftMouseDown, .leftMouseUp, .left), (2, .rightMouseDown, .rightMouseUp, .right),
            (4, .otherMouseDown, .otherMouseUp, .center),
        ]
        for (bit, downType, upType, btn) in changes where (b & bit) != (buttons & bit) {
            let down = b & bit != 0
            if down && bit == 1 {
                let near = abs(p.x - lastClickPoint.x) < 5 && abs(p.y - lastClickPoint.y) < 5
                clickCount = (Date().timeIntervalSince(lastClick) < NSEvent.doubleClickInterval && near) ? clickCount + 1 : 1
                lastClick = Date()
                lastClickPoint = p
            }
            if let e = CGEvent(mouseEventSource: source, mouseType: down ? downType : upType, mouseCursorPosition: p, mouseButton: btn) {
                if bit == 1 { e.setIntegerValueField(.mouseEventClickState, value: clickCount) }
                e.post(tap: .cghidEventTap)
            }
        }
        buttons = b
        if wheel != 0 {
            CGEvent(scrollWheelEvent2Source: source, units: .line, wheelCount: 1, wheel1: Int32(wheel), wheel2: 0, wheel3: 0)?
                .post(tap: .cghidEventTap)
        }
    }
}

// MARK: - Model

final class Model: ObservableObject {
    @Published var status: StatusReply?
    @Published var networks: [NetworkInfo] = []
    @Published var daemonRunning = true
    @Published var scanning = false
    @Published var joining: String?
    @Published var bt: BtReply?
    @Published var tab = 0                 // 0 Wi-Fi, 1 Bluetooth
    // Kept here rather than in @State: on newer SDKs @State is a macro whose plugin ships only with Xcode, and this
    // app is built with the Command Line Tools alone. Don't use @State/@Observable/other macros in this file.
    @Published var btShowUnnamed = false

    private var timer: Timer?
    private var portalShownFor: String?    // open the sign-in window by itself once per connection
    private let queue = DispatchQueue(label: "macwifi.requests", qos: .userInitiated)

    init() {
        InputInjector.shared.start()
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
            if status?.captive == "yes" { return "Connected · sign-in needed" }
            let sec = ["wpa3": "WPA3 · ", "wpa2": "WPA2 · ", "open": "not encrypted · "][status?.security ?? ""] ?? ""
            return ip.isEmpty ? "Connected, getting an address…" : "Connected · \(sec)\(ip)"
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
            let btReply = reply == nil ? nil : (try? Daemon.request(["cmd": "bt"], timeout: 5)).flatMap {
                try? JSONDecoder().decode(BtReply.self, from: $0)
            }
            DispatchQueue.main.async {
                self.daemonRunning = reply != nil
                self.status = reply
                self.bt = btReply
                self.updatePortal()
            }
        }
    }

    // MARK: Bluetooth

    var btPower: Bool { bt?.power ?? false }
    var btScanning: Bool { bt?.scanning ?? false }
    var btDevices: [BtDevice] {
        (bt?.devices ?? []).sorted { a, b in
            if (a.paired == true) != (b.paired == true) { return a.paired == true }   // my devices first
            if a.name.isEmpty != b.name.isEmpty { return !a.name.isEmpty }   // then named devices
            return a.rssi > b.rssi
        }
    }

    var btStatusLine: String {
        guard daemonRunning, let bt = bt else { return "The macwifid service isn't running." }
        if bt.power != true { return "Bluetooth is off." }
        if bt.up == true {
            let what = bt.controller?.isEmpty == false ? bt.controller! : "Bluetooth"
            if bt.scanning == true { return "\(what) · looking for devices…" }
            if let c = bt.last_classic, let l = bt.last_le, c >= 0 {
                return "\(what) · on · last scan: \(c) classic, \(l) LE responses"
            }
            return "\(what) · on"
        }
        if state == "off" { return "Bluetooth needs the adapter on (it shares the Wi-Fi chip)." }
        if state == "no_adapter" { return "Plug in the UGREEN adapter." }
        if state == "starting" || state == "asleep" { return "Waiting for the adapter…" }
        if let e = bt.error, !e.isEmpty { return "Bluetooth: \(e)" }
        return "Starting Bluetooth…"
    }

    func btScan() {
        queue.async {
            let reply = (try? Daemon.request(["cmd": "bt_scan"], timeout: 5)).flatMap {
                try? JSONDecoder().decode(BtReply.self, from: $0)
            }
            DispatchQueue.main.async { if let r = reply { self.bt = r } }
            // the scan runs for about 8 s in the service; the 3 s refresh picks up results as they come
        }
    }

    var btNeedsAccessibility: Bool {
        (bt?.devices ?? []).contains { $0.isConnected } && !InputInjector.trusted
    }

    func btConnect(_ dev: BtDevice) {
        if !InputInjector.trusted && (dev.type == "Keyboard" || dev.type == "Mouse" || dev.type.contains("mouse") || dev.type == "Input") {
            InputInjector.askForTrust()
        }
        btAction("bt_connect", dev)
    }
    func btDisconnect(_ dev: BtDevice) { btAction("bt_disconnect", dev) }
    func btForget(_ dev: BtDevice) { btAction("bt_forget", dev) }

    private func btAction(_ cmd: String, _ dev: BtDevice) {
        queue.async {
            var message: String?
            if let data = try? Daemon.request(["cmd": cmd, "id": dev.id], timeout: 5),
               let reply = try? JSONDecoder().decode(SimpleReply.self, from: data) {
                if !reply.ok { message = reply.error ?? "It didn't work." }
            } else {
                message = "The macwifid service didn't answer."
            }
            DispatchQueue.main.async {
                self.refresh()
                if let m = message { Model.showError(m) }
            }
        }
    }

    static func openAccessibilitySettings() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility") {
            NSWorkspace.shared.open(url)
        }
    }

    func setBtPower(_ on: Bool) {
        queue.async {
            let reply = (try? Daemon.request(["cmd": "bt_power", "on": on], timeout: 5)).flatMap {
                try? JSONDecoder().decode(BtReply.self, from: $0)
            }
            DispatchQueue.main.async {
                if let r = reply { self.bt = r } else { Model.showError("The macwifid service didn't answer.") }
            }
        }
    }

    var needsSignIn: Bool { state == "connected" && status?.captive == "yes" }

    func openPortal() {
        let url = URL(string: status?.portal ?? "") ?? URL(string: "http://captive.apple.com/hotspot-detect.html")!
        PortalWindow.open(url, title: "Sign in to “\(status?.ssid ?? "network")”")
    }

    // Like macOS's own captive network assistant: show the page when a portal appears, close it when it's gone.
    private func updatePortal() {
        if needsSignIn {
            let key = status?.ssid ?? ""
            if portalShownFor != key {
                portalShownFor = key
                openPortal()
            }
        } else {
            if state != "connected" { portalShownFor = nil }
            if status?.captive == "no" { PortalWindow.close() }
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
        alert.informativeText = "Enter the Wi-Fi password. It's kept in a file only the system can read: as a derived key for WPA2 networks, and as the password itself for networks that offer WPA3 (WPA3 needs it at every sign-in)."
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

// MARK: - Sign-in window for captive portals

final class PortalWindow: NSObject, NSWindowDelegate {
    private static var shared: PortalWindow?
    private let window: NSWindow
    private let web: WKWebView

    private init(url: URL, title: String) {
        let frame = NSRect(x: 0, y: 0, width: 900, height: 700)
        web = WKWebView(frame: frame, configuration: WKWebViewConfiguration())
        window = NSWindow(contentRect: frame, styleMask: [.titled, .closable, .resizable, .miniaturizable],
                          backing: .buffered, defer: false)
        super.init()
        window.title = title
        window.contentView = web
        window.isReleasedWhenClosed = false
        window.delegate = self
        window.center()
        web.load(URLRequest(url: url))
    }

    static func open(_ url: URL, title: String) {
        if shared == nil { shared = PortalWindow(url: url, title: title) }
        shared?.window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    static func close() {
        shared?.window.close()
        shared = nil
    }

    func windowWillClose(_ notification: Notification) { PortalWindow.shared = nil }
}

// MARK: - Views

struct NetworkRow: View {
    static let height: CGFloat = 32
    @EnvironmentObject var model: Model
    let net: NetworkInfo

    var body: some View {
        let isCurrent = model.connectedSSID == net.ssid
        HStack(spacing: 8) {
            Image(systemName: "wifi", variableValue: Model.strength(net.rssi))
                .font(.body)
                .frame(width: 22)
            Text(net.ssid)
                .font(.body)
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
        .padding(.vertical, 5)
        .padding(.horizontal, 8)
        .frame(height: NetworkRow.height)
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

struct BtDeviceRow: View {
    @EnvironmentObject var model: Model
    let dev: BtDevice

    var subtitle: String {
        switch dev.state ?? "" {
        case "connecting": return "Connecting…"
        case "pairing":
            if let pk = dev.passkey { return String(format: "Type %06ld on the keyboard, then press Return", pk) }
            return "Pairing…"
        case "connected": return "Connected"
        case "failed": return "Couldn't connect: \(dev.why ?? "unknown reason")"
        default: return dev.paired == true ? "Paired · not connected" : dev.type
        }
    }

    static func symbol(_ type: String, _ transport: String) -> String {
        switch type {
        case "Keyboard": return "keyboard"
        case "Mouse": return "computermouse"
        case "Keyboard+mouse", "Input": return "keyboard"
        case "Audio": return "headphones"
        case "Phone": return "iphone"
        case "Computer": return "laptopcomputer"
        case "Watch", "Wearable": return "applewatch"
        case "Gamepad", "Joystick", "Toy": return "gamecontroller"
        case "Display", "Media player", "Remote": return "tv"
        case "Heart rate", "Health": return "heart"
        case "Imaging": return "printer"
        default: return "dot.radiowaves.left.and.right"
        }
    }

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: BtDeviceRow.symbol(dev.type, dev.transport))
                .font(.body)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 0) {
                Text(dev.name.isEmpty ? (dev.type.isEmpty ? "Unnamed device" : "Unnamed \(dev.type.lowercased())") : dev.name)
                    .font(.body)
                    .foregroundStyle(dev.name.isEmpty ? Color.secondary : Color.primary)
                    .lineLimit(1)
                if !subtitle.isEmpty {
                    Text(subtitle)
                        .font(dev.passkey != nil ? Font.callout.weight(.semibold) : Font.caption2)
                        .foregroundStyle(dev.state == "failed" ? Color.orange : dev.isConnected ? Color.accentColor : Color.secondary)
                        .lineLimit(2)
                }
            }
            Spacer()
            if dev.isBusy { ProgressView().controlSize(.small) }
            if dev.isConnected { Image(systemName: "checkmark").font(.caption) }
            Text(dev.transport == "le" ? "LE" : "Classic")
                .font(.caption2)
                .foregroundStyle(.secondary)
            if dev.rssi > -127 {
                Image(systemName: "cellularbars", variableValue: Model.strength(dev.rssi))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 8)
        .frame(minHeight: NetworkRow.height + 4)
        .contentShape(Rectangle())
        .opacity(dev.transport == "le" && dev.paired != true ? 0.6 : 1)
        .help(dev.transport == "le" && dev.paired != true ? "Bluetooth LE devices can't be connected yet."
              : dev.isConnected ? "Right-click to disconnect or forget." : "Click to connect.")
        .onTapGesture {
            if !dev.isConnected && !dev.isBusy && !(dev.transport == "le" && dev.paired != true) { model.btConnect(dev) }
        }
        .contextMenu {
            if !dev.isConnected && !dev.isBusy { Button("Connect") { model.btConnect(dev) } }
            if dev.isConnected || dev.isBusy { Button("Disconnect") { model.btDisconnect(dev) } }
            if dev.paired == true { Button("Forget This Device") { model.btForget(dev) } }
        }
    }
}

struct BluetoothPane: View {
    @EnvironmentObject var model: Model

    var body: some View {
        // "Hidden" = no name and no known type (mostly beacons). A device whose type is known (keyboard, mouse,
        // headphones...) is always listed, even before its name arrives.
        let named = model.btDevices.filter { $0.paired == true || !$0.name.isEmpty || !$0.type.isEmpty }
        let unnamed = model.btDevices.filter { $0.paired != true && $0.name.isEmpty && $0.type.isEmpty }
        let rows = model.btShowUnnamed ? named + unnamed : named
        VStack(alignment: .leading, spacing: 8) {
            Text(model.btStatusLine)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            if model.btNeedsAccessibility {
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Image(systemName: "hand.raised")
                        Text("Allow MacWifi to control this Mac").font(.callout)
                        Spacer()
                        Button("Open Settings…") { Model.openAccessibilitySettings() }
                    }
                    Text("The keyboard and mouse are connected, but their input reaches the Mac only after you turn on MacWifi in Privacy & Security › Accessibility. After an update you may need to turn it off and on again.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 6).fill(Color.orange.opacity(0.15)))
            }
            Divider()
            HStack {
                Text("Devices").font(.caption).foregroundStyle(.secondary)
                Spacer()
                if model.btScanning { ProgressView().controlSize(.small) }
            }
            if !model.btPower {
                Text("Turn Bluetooth on to see nearby devices.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else if rows.isEmpty {
                Text(model.btScanning ? "Looking…" : unnamed.isEmpty ? "No devices yet." : "Only unnamed devices nearby.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 0) {
                        ForEach(rows) { BtDeviceRow(dev: $0) }
                    }
                }
                .frame(height: CGFloat(min(max(rows.count, 4), 9)) * (NetworkRow.height + 4))
            }
            if model.btPower && !unnamed.isEmpty {
                Toggle("Show \(unnamed.count) unnamed device\(unnamed.count == 1 ? "" : "s")", isOn: $model.btShowUnnamed)
                    .toggleStyle(.checkbox)
                    .font(.caption)
            }
        }
    }
}

struct WifiPane: View {
    @EnvironmentObject var model: Model

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
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
            if model.needsSignIn {
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Image(systemName: "person.badge.key")
                        Text("This network wants you to sign in.").font(.callout)
                        Spacer()
                        Button("Sign In…") { model.openPortal() }
                    }
                    Text("If the page doesn't load, unplug Ethernet or turn off the Mac's own Wi-Fi for a moment.")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding(8)
                .background(RoundedRectangle(cornerRadius: 6).fill(Color.orange.opacity(0.15)))
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
                // A MenuBarExtra window sizes a bare ScrollView to its minimum (one row),
                // so give it a real height: every network up to ten rows, then scroll.
                .frame(height: CGFloat(min(max(model.networks.count, 4), 10)) * NetworkRow.height)
            }
        }
    }
}

struct MenuView: View {
    @EnvironmentObject var model: Model

    var versionLine: String {
        let app = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "?"
        let svc = model.status?.version ?? "not running"
        return "MacWifi \(app) · service \(svc) · Quit keeps Wi-Fi and Bluetooth running"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Picker("", selection: $model.tab) {
                    Text("Wi-Fi").tag(0)
                    Text("Bluetooth").tag(1)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 200)
                Spacer()
                if model.tab == 0 {
                    Toggle("", isOn: Binding(get: { model.status?.power ?? false }, set: { model.setPower($0) }))
                        .toggleStyle(.switch)
                        .labelsHidden()
                        .disabled(!model.daemonRunning)
                } else {
                    Toggle("", isOn: Binding(get: { model.btPower }, set: { model.setBtPower($0) }))
                        .toggleStyle(.switch)
                        .labelsHidden()
                        .disabled(!model.daemonRunning || model.bt == nil)
                }
            }
            if model.tab == 0 { WifiPane() } else { BluetoothPane() }
            Divider()
            HStack {
                if model.tab == 0 {
                    Button("Scan Again") { model.scan() }.disabled(model.scanning)
                } else {
                    Button("Scan Again") { model.btScan() }.disabled(model.btScanning || !model.btPower)
                }
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
            Text(versionLine)
                .font(.caption2)
                .foregroundStyle(.tertiary)
        }
        .padding(14)
        .frame(width: 380)
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

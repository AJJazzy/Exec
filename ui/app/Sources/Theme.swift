import AppKit
import SwiftUI

/// Colours and fonts shared by the AppKit editor/console and the SwiftUI views.
/// Every colour is dynamic, so it adapts automatically when the appearance changes.
enum Theme {
    static func dynamic(light: NSColor, dark: NSColor) -> NSColor {
        NSColor(name: nil) { appearance in
            appearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? dark : light
        }
    }

    static func hex(_ value: UInt32, alpha: CGFloat = 1) -> NSColor {
        NSColor(srgbRed: CGFloat((value >> 16) & 0xff) / 255, green: CGFloat((value >> 8) & 0xff) / 255,
                blue: CGFloat(value & 0xff) / 255, alpha: alpha)
    }

    static let editorBackground = dynamic(light: hex(0xFBFBFD), dark: hex(0x16171D))
    static let editorText = dynamic(light: hex(0x1F2328), dark: hex(0xD6DAE3))
    static let gutterBackground = dynamic(light: hex(0xF2F3F7), dark: hex(0x1B1C23))
    static let gutterText = dynamic(light: hex(0x9AA0AC), dark: hex(0x5C6170))
    static let currentLine = dynamic(light: hex(0x7C5CFF, alpha: 0.07), dark: hex(0x7C5CFF, alpha: 0.12))
    static let bracketMatch = dynamic(light: hex(0x7C5CFF, alpha: 0.22), dark: hex(0x9D85FF, alpha: 0.32))

    static func tokenColor(_ kind: NXTokenKind) -> NSColor {
        switch kind {
        case .keyword: return dynamic(light: hex(0xA626A4), dark: hex(0xC792EA))
        case .constant: return dynamic(light: hex(0xB76B01), dark: hex(0xF78C6C))
        case .number: return dynamic(light: hex(0x986801), dark: hex(0xF78C6C))
        case .string: return dynamic(light: hex(0x2E7D32), dark: hex(0xC3E88D))
        case .comment: return dynamic(light: hex(0x8A8F98), dark: hex(0x676E7D))
        case .operator: return dynamic(light: hex(0x4B5263), dark: hex(0x89DDFF))
        case .builtin: return dynamic(light: hex(0x0B61C4), dark: hex(0x82AAFF))
        case .robloxGlobal: return dynamic(light: hex(0xC2410C), dark: hex(0xFFCB6B))
        case .executorGlobal: return dynamic(light: hex(0xD32F2F), dark: hex(0xFF5370))
        case .function: return dynamic(light: hex(0x3B5BDB), dark: hex(0x82AAFF))
        case .property: return dynamic(light: hex(0x0E7490), dark: hex(0x80CBC4))
        case .attribute: return dynamic(light: hex(0x7C3AED), dark: hex(0xBB9AF7))
        case .error: return dynamic(light: hex(0xD32F2F), dark: hex(0xFF5370))
        default: return editorText
        }
    }

    static func consoleColor(_ stream: NXOutputStream) -> NSColor {
        switch stream {
        case .output: return editorText
        case .warning: return dynamic(light: hex(0xB45309), dark: hex(0xFFCB6B))
        case .error: return dynamic(light: hex(0xC62828), dark: hex(0xFF6B81))
        case .info: return dynamic(light: hex(0x0B61C4), dark: hex(0x82AAFF))
        case .system: return gutterText
        @unknown default: return editorText
        }
    }

    static func codeFont(name: String, size: CGFloat) -> NSFont {
        if name != "SF Mono", let f = NSFont(name: name, size: size) { return f }
        return NSFont.monospacedSystemFont(ofSize: size, weight: .regular)
    }

    static func color(fromHex string: String) -> Color {
        var s = string.trimmingCharacters(in: .whitespaces)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6, let v = UInt32(s, radix: 16) else { return .accentColor }
        return Color(nsColor: hex(v))
    }

    static func hexString(from color: Color) -> String {
        let ns = NSColor(color).usingColorSpace(.sRGB) ?? .systemPurple
        return String(format: "#%02X%02X%02X", Int(round(ns.redComponent * 255)), Int(round(ns.greenComponent * 255)),
                      Int(round(ns.blueComponent * 255)))
    }

    static func stateColor(_ state: String) -> Color {
        switch state {
        case "CONNECTED", "succeeded", "available", "RUNNING", "STOPPED": return .green
        case "INTEGRATION_AVAILABLE", "partial", "CHECKING_COMPATIBILITY", "DISCOVERING", "CLIENT_FOUND", "COMPILING",
             "INITIALIZING", "READY", "STOPPING", "unknown": return .orange
        case "UNSUPPORTED", "unavailable", "DISCONNECTED": return .secondary
        case "ERROR", "FAILED", "runtime-error", "compile-error", "timed-out", "memory-limit",
             "internal-error": return .red
        default: return .secondary
        }
    }
}

extension Color {
    static let nxSubtleBackground = Color(nsColor: Theme.dynamic(light: Theme.hex(0xF5F5F8), dark: Theme.hex(0x1E1F26)))
    static let nxCardBorder = Color(nsColor: Theme.dynamic(light: Theme.hex(0xE3E4EA), dark: Theme.hex(0x2C2E38)))
}

/// Small rounded "card" container used across the dashboard views.
struct Card<Content: View>: View {
    let content: Content
    init(@ViewBuilder content: () -> Content) { self.content = content() }
    var body: some View {
        content
            .padding(16)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(RoundedRectangle(cornerRadius: 10).fill(Color.nxSubtleBackground))
            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Color.nxCardBorder))
    }
}

struct StateBadge: View {
    let text: String
    var body: some View {
        Text(text.replacingOccurrences(of: "_", with: " "))
            .font(.caption.weight(.semibold))
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .foregroundStyle(Theme.stateColor(text))
            .background(Capsule().fill(Theme.stateColor(text).opacity(0.14)))
    }
}

func formatBytes(_ bytes: UInt64) -> String {
    bytes == 0 ? "—" : ByteCountFormatter.string(fromByteCount: Int64(bytes), countStyle: .memory)
}

func formatMs(_ ms: Double) -> String {
    if ms < 1 { return String(format: "%.0f µs", ms * 1000) }
    if ms < 1000 { return String(format: "%.1f ms", ms) }
    return String(format: "%.2f s", ms / 1000)
}

import AppKit
import SwiftUI

struct SettingsView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        Form {
            Section("Appearance") {
                Picker("Theme", selection: model.binding(\.appearance)) {
                    Text("System").tag("system")
                    Text("Light").tag("light")
                    Text("Dark").tag("dark")
                }
                .pickerStyle(.segmented)
                ColorPicker("Accent colour", selection: Binding(get: { Theme.color(fromHex: model.settings.accentColor) },
                                                                set: { c in model.updateSettings { $0.accentColor = Theme.hexString(from: c) } }),
                            supportsOpacity: false)
                HStack {
                    Text("Editor font size")
                    Spacer()
                    Stepper(value: model.binding(\.editorFontSize), in: 8...40, step: 1) {
                        Text("\(Int(model.settings.editorFontSize)) pt").monospacedDigit()
                    }
                }
                TextField("Editor font", text: model.binding(\.editorFontName), prompt: Text("SF Mono"))
            }
            Section("Editor") {
                Stepper("Indent width: \(model.settings.indentWidth)", value: model.binding(\.indentWidth), in: 1...8)
                Toggle("Indent with tabs", isOn: model.binding(\.useTabs))
                Toggle("Auto-close brackets and quotes", isOn: model.binding(\.autoCloseBrackets))
                Toggle("Automatic indentation", isOn: model.binding(\.autoIndent))
                Toggle("Highlight matching brackets", isOn: model.binding(\.highlightMatchingBrackets))
                Toggle("Live syntax diagnostics", isOn: model.binding(\.liveDiagnostics))
                Toggle("Autocomplete while typing", isOn: model.binding(\.autocomplete))
                Toggle("Show minimap", isOn: model.binding(\.showMinimap))
                Toggle("Restore unsaved drafts on launch (never executed automatically)", isOn: model.binding(\.restoreDrafts))
            }
            Section("Execution") {
                Picker("Default target", selection: model.binding(\.defaultTarget)) {
                    Text("Luau — isolated process (recommended)").tag("luau-isolated")
                    Text("Luau — in-process").tag("luau-inprocess")
                    Text("Roblox Studio (MCP)").tag("roblox-studio")
                }
                Stepper("Time limit: \(model.settings.timeoutMs / 1000) s", value: Binding(get: { model.settings.timeoutMs / 1000 },
                                                                                              set: { v in model.updateSettings { $0.timeoutMs = v * 1000 } }), in: 1...3600)
                Stepper("Memory limit: \(model.settings.memoryLimitMB) MB", value: model.binding(\.memoryLimitMB), in: 8...8192, step: 64)
                Stepper("Output limit: \(model.settings.maxOutputKB) KB", value: model.binding(\.maxOutputKB), in: 16...262144, step: 1024)
                Toggle("Sandbox the isolated runner (macOS Seatbelt, no files/network)", isOn: model.binding(\.runnerSandbox))
                Text("Limits apply to the Nexus Luau runtime. Roblox Studio enforces its own rules; Nexus cannot limit code running inside Studio.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Section("Roblox") {
                Picker("Studio data model", selection: model.binding(\.studioDataModel)) {
                    Text("Edit").tag("Edit")
                    Text("Server").tag("Server")
                    Text("Client").tag("Client")
                }
                Toggle("Only launch StudioMCP when signed by Roblox (Team 2CFABCH843)", isOn: model.binding(\.requireRobloxSignature))
                Toggle("Allow online Roblox version check (one request to clientsettingscdn.roblox.com, on demand)", isOn: model.binding(\.allowRobloxVersionCheck))
            }
            Section("Privacy") {
                Text("Nexus collects no telemetry and makes no network requests except the optional, user-initiated version check above. Logs, history and drafts stay in \(model.host.dataDirectory).")
                    .font(.callout).foregroundStyle(.secondary)
                HStack {
                    Button("Open Data Folder") { NSWorkspace.shared.open(URL(fileURLWithPath: model.host.dataDirectory)) }
                    Button("Clear Execution History") { model.host.clearHistory(); model.history = [] }
                }
            }
        }
        .formStyle(.grouped)
        .navigationTitle("Settings")
    }
}

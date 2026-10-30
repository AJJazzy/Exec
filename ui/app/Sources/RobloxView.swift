import SwiftUI

struct RobloxView: View {
    @EnvironmentObject var model: AppModel
    @State private var confirmVersionCheck = false
    @State private var confirmStudioConnect = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 18) {
                HStack {
                    Text("Roblox Integration").font(.title2.weight(.semibold))
                    Spacer()
                    if model.robloxBusy { ProgressView().controlSize(.small) }
                    Button { model.refreshRoblox() } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                }
                playerCard
                studioCard
                Card {
                    VStack(alignment: .leading, spacing: 6) {
                        Label("How detection works", systemImage: "lock.shield").font(.headline)
                        Text("Nexus reads application bundles and code signatures from disk and lists running processes. It never opens a task port, attaches a debugger, injects code, or sends signals to Roblox. Launches and exits are observed through NSWorkspace notifications — there is no polling.")
                            .font(.callout).foregroundStyle(.secondary)
                    }
                }
            }
            .padding(24)
        }
        .navigationTitle("Roblox Integration")
        .confirmationDialog("Check the latest Roblox version online?", isPresented: $confirmVersionCheck) {
            Button("Allow and Check") {
                model.updateSettings { $0.allowRobloxVersionCheck = true }
                model.checkLatestRobloxVersion()
            }
        } message: {
            Text("Nexus will send one HTTPS GET request to clientsettingscdn.roblox.com (Roblox’s public client-version endpoint). No cookies, account data or identifiers are sent. You can turn this off in Settings.")
        }
        .confirmationDialog("Connect to Roblox Studio?", isPresented: $confirmStudioConnect) {
            Button("Launch StudioMCP and Connect") { model.connectStudio() }
        } message: {
            Text("Nexus will launch Studio’s built-in MCP server (/Applications/RobloxStudio.app/Contents/MacOS/StudioMCP) after verifying it is signed by Roblox. Scripts you run on the Roblox Studio target can read and modify the place open in Studio. Enable “Studio as MCP server” in Studio (Assistant → … → Manage MCP Servers).")
        }
    }

    private var playerCard: some View {
        let p = model.player
        return Card {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Image(systemName: "gamecontroller").font(.title2)
                    Text("Roblox Player").font(.headline)
                    StateBadge(text: p.state)
                    Spacer()
                    Button("Check Latest Version") {
                        if model.settings.allowRobloxVersionCheck { model.checkLatestRobloxVersion() } else { confirmVersionCheck = true }
                    }
                }
                Text(p.summary.isEmpty ? "Not checked yet." : p.summary)
                if let result = model.versionCheckResult { Text(result).font(.callout).foregroundStyle(.secondary).textSelection(.enabled) }
                if p.installed {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                        row("Version", "\(p.version) (build \(p.build))")
                        if !p.previousVersion.isEmpty { row("Updated from", p.previousVersion) }
                        row("Bundle", p.bundlePath)
                        row("Architecture", p.architectures.joined(separator: ", "))
                        row("Minimum macOS", p.minimumSystemVersion)
                        row("Code signature", p.signatureChecked ? (p.signatureValid ? "Valid — Team \(p.signatureTeamId)" : "Not valid: \(p.signatureError)") : "Not verified on this platform")
                        row("Hardened Runtime", p.hardenedRuntime ? "Enabled" : "Not enabled")
                        row("Running", p.runningDescriptions.isEmpty ? "No" : p.runningDescriptions.joined(separator: "; "))
                    }
                    .font(.callout)
                }
                Divider()
                Text("Live script execution: not supported").font(.subheadline.weight(.semibold))
                ForEach(p.unsupportedReasons, id: \.self) { reason in
                    Label(reason, systemImage: "minus").font(.callout).foregroundStyle(.secondary)
                }
                if !p.history.isEmpty {
                    DisclosureGroup("Connection state history") {
                        ForEach(p.history, id: \.self) { Text($0).font(.caption.monospaced()) }
                    }
                }
            }
        }
    }

    private var studioCard: some View {
        let s = model.studio
        return Card {
            VStack(alignment: .leading, spacing: 10) {
                HStack {
                    Image(systemName: "hammer").font(.title2)
                    Text("Roblox Studio (official MCP server)").font(.headline)
                    StateBadge(text: s.state)
                    Spacer()
                    if model.studioBusy { ProgressView().controlSize(.small) }
                    if s.connected {
                        Button("Disconnect") { model.disconnectStudio() }
                    } else {
                        Button("Connect") { confirmStudioConnect = true }
                            .disabled(model.studioBusy || !(s.state == "INTEGRATION_AVAILABLE" || s.state == "ERROR" || s.state == "CONNECTED"))
                    }
                }
                Text(s.message).font(.callout)
                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                    row("Installed", s.installed ? "Yes — \(s.version)" : "No")
                    row("MCP server", s.mcpPath.isEmpty ? "Not found" : s.mcpPath)
                    if !s.serverName.isEmpty { row("Server", "\(s.serverName) \(s.serverVersion) (MCP \(s.protocolVersion))") }
                    if !s.executeTool.isEmpty { row("Execution tool", s.executeTool) }
                    if !s.tools.isEmpty { row("Tools", s.tools.joined(separator: ", ")) }
                    if !s.mcpSignatureError.isEmpty { row("Signature", s.mcpSignatureError) }
                }
                .font(.callout)
                if !s.studios.isEmpty {
                    Picker("Studio instance", selection: Binding(get: { s.selectedStudioId }, set: { model.host.selectStudio($0); model.studio = model.host.studioStatus() })) {
                        ForEach(s.studios, id: \.identifier) { st in Text(st.name.isEmpty ? st.identifier : "\(st.name) (\(st.identifier))").tag(st.identifier) }
                    }
                    .frame(maxWidth: 420)
                }
                Picker("Data model", selection: model.binding(\.studioDataModel)) {
                    Text("Edit").tag("Edit")
                    Text("Server (Play)").tag("Server")
                    Text("Client (Play)").tag("Client")
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 420)
                Text("Studio executes code with the full Roblox engine API. This is Roblox Studio, not the live Roblox Player; Nexus cannot enforce time or memory limits inside Studio, and a stop request may not terminate a script that Studio is already running.")
                    .font(.caption).foregroundStyle(.secondary)
                if !s.history.isEmpty {
                    DisclosureGroup("Connection state history") {
                        ForEach(s.history, id: \.self) { Text($0).font(.caption.monospaced()) }
                    }
                }
            }
        }
    }

    private func row(_ title: String, _ value: String) -> some View {
        GridRow {
            Text(title).foregroundStyle(.secondary)
            Text(value).textSelection(.enabled).lineLimit(3)
        }
    }
}

import SwiftUI

struct RootView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        NavigationSplitView {
            List(SidebarSection.allCases, selection: Binding(get: { model.section }, set: { if let s = $0 { model.section = s } })) { section in
                Label(section.title, systemImage: section.symbol)
                    .tag(section)
                    .badge(badge(for: section))
            }
            .listStyle(.sidebar)
            .navigationSplitViewColumnWidth(min: 190, ideal: 210, max: 260)
            .safeAreaInset(edge: .bottom) { ConnectionFooter().padding(10) }
        } detail: {
            detail
                .transition(.opacity)
                .animation(.easeInOut(duration: 0.15), value: model.section)
        }
        .toolbar { MainToolbar(model: model) }
        .alert(item: $model.alert) { info in
            Alert(title: Text(info.title), message: Text(info.message), dismissButton: .default(Text("OK")))
        }
    }

    @ViewBuilder private var detail: some View {
        switch model.section {
        case .home: HomeView()
        case .editor: EditorWorkspace(store: model.documents)
        case .library: LibraryView()
        case .console: ConsoleView()
        case .roblox: RobloxView()
        case .compatibility: CompatibilityView(store: model.documents)
        case .diagnostics: DiagnosticsView()
        case .settings: SettingsView()
        }
    }

    private func badge(for section: SidebarSection) -> Int {
        switch section {
        case .console: return model.consoleLines.filter { $0.stream == .error }.count > 0 && model.section != .console ? 1 : 0
        default: return 0
        }
    }
}

/// Run/Stop/Restart controls, execution target picker and connection status.
/// ToolbarContent is not re-evaluated reliably when the model changes, so every control whose
/// state depends on the model lives in a View that observes it.
struct MainToolbar: ToolbarContent {
    let model: AppModel

    var body: some ToolbarContent {
        ToolbarItem(placement: .navigation) {
            RunControls(model: model, store: model.documents)
        }
        ToolbarItem(placement: .principal) {
            TargetPicker(model: model)
        }
        ToolbarItemGroup(placement: .primaryAction) {
            Button { model.analyzeCurrent(); model.section = .compatibility } label: { Label("Check Compatibility", systemImage: "checkmark.shield") }
                .help("Analyse the current script (⇧⌘K)")
            Button { model.openFilePanel() } label: { Label("Open Script", systemImage: "folder") }
                .help("Open a script file (⌘O)")
            Button { if let d = model.documents.selected { model.documents.save(d) } } label: { Label("Save Script", systemImage: "square.and.arrow.down") }
                .help("Save the current script (⌘S)")
            Button { model.clearConsole() } label: { Label("Clear Console", systemImage: "trash") }
                .help("Clear the console (⌘K)")
            ConnectionStatusButton(model: model)
        }
    }
}

struct RunControls: View {
    @ObservedObject var model: AppModel
    @ObservedObject var store: DocumentStore

    var body: some View {
        ControlGroup {
            Button { model.runCurrent() } label: { Label("Run", systemImage: "play.fill") }
                .help("Run the current script (⌘R)")
                .disabled(model.isRunning || store.selected == nil)
            Button { model.stop() } label: { Label("Stop", systemImage: "stop.fill") }
                .help("Stop the running script (⌘.)")
                .disabled(!model.isRunning)
            Button { model.restart() } label: { Label("Restart", systemImage: "arrow.clockwise") }
                .help("Stop and run the last script again (⇧⌘R)")
                .disabled(model.history.isEmpty && !model.isRunning)
        }
        .labelStyle(.iconOnly)
    }
}

struct TargetPicker: View {
    @ObservedObject var model: AppModel

    var body: some View {
        Picker("Target", selection: $model.selectedTarget) {
            ForEach(model.targets, id: \.identifier) { t in
                Text(t.displayName + (t.available ? "" : " — unavailable")).tag(t.identifier)
            }
        }
        .labelsHidden()
        .pickerStyle(.menu)
        .frame(width: 250)
        .help("Where scripts run. The live Roblox Player is listed for transparency but cannot execute scripts.")
    }
}

struct ConnectionStatusButton: View {
    @ObservedObject var model: AppModel
    var body: some View {
        Button { model.section = .roblox } label: {
            HStack(spacing: 5) {
                Circle().fill(color).frame(width: 8, height: 8)
                Text(label).font(.caption)
            }
        }
        .help("Roblox integration status")
    }

    private var label: String {
        if model.studio.connected { return "Studio connected" }
        if !model.player.runningPids.isEmpty { return "Player detected (not attachable)" }
        return "No Roblox connection"
    }

    private var color: Color {
        if model.studio.connected { return .green }
        if !model.player.runningPids.isEmpty { return .orange }
        return .secondary
    }
}

struct ConnectionFooter: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Circle().fill(model.isRunning ? Color.green : Color.secondary).frame(width: 7, height: 7)
                Text(model.isRunning ? "Running \(model.session.scriptName)" : "Runtime idle").font(.caption)
            }
            HStack {
                Circle().fill(Theme.stateColor(model.studio.state)).frame(width: 7, height: 7)
                Text("Studio: \(model.studio.state.replacingOccurrences(of: "_", with: " ").lowercased())").font(.caption)
            }
            HStack {
                Circle().fill(Theme.stateColor(model.player.state)).frame(width: 7, height: 7)
                Text("Player: \(model.player.state.replacingOccurrences(of: "_", with: " ").lowercased())").font(.caption)
            }
        }
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

struct HomeView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                VStack(alignment: .leading, spacing: 6) {
                    Text("Nexus").font(.system(size: 34, weight: .bold))
                    Text("Native Luau scripting for macOS — Luau \(model.host.luauVersion) · \(model.host.operatingSystem)")
                        .foregroundStyle(.secondary)
                }
                HStack(alignment: .top, spacing: 14) {
                    statusCard(title: "Luau runtime", state: model.targets.first { $0.identifier == "luau-isolated" }?.available == true ? "available" : "unavailable",
                               detail: "Sandboxed, resource-limited execution in an isolated process.", action: ("New Script", { model.documents.newDocument(); model.section = .editor }))
                    statusCard(title: "Roblox Studio", state: model.studio.state, detail: model.studio.message,
                               action: ("Open Integration", { model.section = .roblox }))
                    statusCard(title: "Roblox Player", state: model.player.state,
                               detail: model.player.summary.isEmpty ? "Not checked yet" : model.player.summary,
                               action: ("Details", { model.section = .roblox }))
                }
                Card {
                    VStack(alignment: .leading, spacing: 8) {
                        Label("What Nexus can and cannot do", systemImage: "info.circle").font(.headline)
                        Text("• Runs Luau in the official Luau VM, sandboxed, with time, memory and output limits.")
                        Text("• Runs Roblox engine code in Roblox Studio through Studio’s official MCP server (when you connect it).")
                        Text("• Detects the Roblox Player and reports its version and architecture — but cannot run scripts inside it: no authorized interface exists, and Nexus does not inject code or bypass protections.")
                    }
                    .font(.callout)
                }
                HStack(alignment: .top, spacing: 14) {
                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Recent scripts").font(.headline)
                            let recent = model.host.recentScripts()
                            if recent.isEmpty { Text("Scripts you open from the library appear here.").foregroundStyle(.secondary) }
                            ForEach(recent.prefix(8), id: \.self) { path in
                                Button(path) { model.documents.openLibraryScript(path); model.section = .editor }.buttonStyle(.link)
                            }
                        }
                    }
                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Last run").font(.headline)
                            if let last = model.history.first {
                                HStack { StateBadge(text: last.status); Text(last.scriptName) }
                                Text(last.summary).foregroundStyle(.secondary)
                                Text("\(formatMs(last.wallMs)) · peak \(formatBytes(last.peakMemoryBytes)) · \(last.targetId)").font(.caption).foregroundStyle(.secondary)
                            } else {
                                Text("Nothing has run yet. Press ⌘R in the editor.").foregroundStyle(.secondary)
                            }
                        }
                    }
                }
            }
            .padding(24)
        }
        .navigationTitle("Home")
    }

    private func statusCard(title: String, state: String, detail: String, action: (String, () -> Void)) -> some View {
        Card {
            VStack(alignment: .leading, spacing: 8) {
                HStack { Text(title).font(.headline); Spacer(); StateBadge(text: state) }
                Text(detail).font(.callout).foregroundStyle(.secondary).lineLimit(4).frame(maxWidth: .infinity, minHeight: 52, alignment: .topLeading)
                Button(action.0, action: action.1)
            }
        }
    }
}

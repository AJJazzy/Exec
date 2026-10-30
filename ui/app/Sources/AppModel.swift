import AppKit
import Combine
import SwiftUI
import UniformTypeIdentifiers

enum SidebarSection: String, CaseIterable, Identifiable {
    case home, editor, library, console, roblox, compatibility, diagnostics, settings
    var id: String { rawValue }

    var title: String {
        switch self {
        case .home: return "Home"
        case .editor: return "Script Editor"
        case .library: return "Script Library"
        case .console: return "Runtime Console"
        case .roblox: return "Roblox Integration"
        case .compatibility: return "Compatibility"
        case .diagnostics: return "Diagnostics"
        case .settings: return "Settings"
        }
    }

    var symbol: String {
        switch self {
        case .home: return "house"
        case .editor: return "chevron.left.forwardslash.chevron.right"
        case .library: return "books.vertical"
        case .console: return "terminal"
        case .roblox: return "cube.transparent"
        case .compatibility: return "checkmark.shield"
        case .diagnostics: return "stethoscope"
        case .settings: return "gearshape"
        }
    }
}

struct AlertInfo: Identifiable {
    let id = UUID()
    let title: String
    let message: String
}

/// Application state shared by all views. Talks to the C++ core exclusively through NXHost.
final class AppModel: ObservableObject {
    static let shared = AppModel()

    let host: NXHost
    lazy var documents = DocumentStore(model: self)

    @Published var section: SidebarSection = .editor
    @Published var session: NXSessionSnapshot
    @Published private(set) var consoleLines: [NXConsoleLine] = []
    @Published private(set) var consoleResetToken = 0
    @Published var settings: NXSettings
    @Published var targets: [NXTargetInfo] = []
    @Published var selectedTarget: String
    @Published var player: NXRobloxStatus
    @Published var studio: NXStudioStatus
    @Published var robloxBusy = false
    @Published var studioBusy = false
    @Published var library: NXLibraryItem
    @Published var history: [NXExecutionRecord] = []
    @Published var compatReport: NXCompatReport?
    @Published var compatBusy = false
    @Published var versionCheckResult: String?
    @Published var alert: AlertInfo?
    @Published var showConsoleInEditor = true

    /// Library folder that receives new scripts ("" = root).
    var libraryTargetFolder = ""
    private let consoleLimit = 8000

    private init() {
        let args = CommandLine.arguments
        var dataDir: String?
        if let i = args.firstIndex(of: "--data-dir"), i + 1 < args.count { dataDir = args[i + 1] }
        host = NXHost(dataDirectory: dataDir, runnerPath: nil)
        session = host.currentSession()
        settings = host.currentSettings()
        selectedTarget = host.currentSettings().defaultTarget
        player = host.robloxStatus()
        studio = host.studioStatus()
        library = host.libraryTree()

        host.sessionHandler = { [weak self] snapshot in self?.sessionChanged(snapshot) }
        host.consoleHandler = { [weak self] lines in self?.appendConsole(lines) }
        host.robloxHandler = { [weak self] player, studio in
            self?.player = player
            self?.studio = studio
            self?.refreshTargets()
        }
    }

    // MARK: Lifecycle

    func didFinishLaunching() {
        refreshTargets()
        history = host.executionHistory()
        let restored = documents.restoreDrafts()
        if documents.documents.isEmpty { documents.newDocument(text: Self.welcomeScript) }
        if restored > 0 {
            host.logInfo("Restored \(restored) unsaved draft(s); they were opened but not executed", category: "editor")
        }
        host.startWatchingRobloxProcesses()
        refreshRoblox()
        if SelfTest.isRequested { SelfTest.start(model: self) }
    }

    func prepareForTermination() {
        for doc in documents.documents where doc.isDirty {
            let draft = NXDraft()
            draft.draftId = doc.id
            draft.title = doc.title
            draft.content = doc.text
            draft.libraryPath = doc.libraryPath ?? ""
            try? host.saveDraft(draft)
        }
        host.shutdown()
    }

    // MARK: Execution

    var isRunning: Bool { session.active }

    func runCurrent() {
        guard let doc = documents.selected else { return }
        if selectedTarget == "roblox-player" {
            alert = AlertInfo(title: "Live Roblox Player execution is not supported",
                              message: "No authorized interface exists for running code inside the live Roblox Player. Use Roblox Studio (MCP) or the standalone Luau runtime. See Roblox Integration for details.")
            return
        }
        do {
            try host.run(source: doc.text, name: doc.title.replacingOccurrences(of: ".luau", with: ""), target: selectedTarget)
            if section != .editor && section != .console { section = .editor }
        } catch {
            present(error, title: "Could not start the script")
        }
    }

    func stop() { _ = host.stop() }

    func restart() {
        do { try host.restart() } catch { present(error, title: "Could not restart") }
    }

    private func sessionChanged(_ snapshot: NXSessionSnapshot) {
        session = snapshot
        if snapshot.hasResult && !snapshot.active {
            history = host.executionHistory()
            if let doc = documents.selected {
                var diags = host.syntaxDiagnostics(source: doc.text)
                if snapshot.errorLine > 0, snapshot.resultStatus == "runtime-error" {
                    diags.append(contentsOf: snapshot.diagnostics)
                }
                doc.editor.showDiagnostics(diags, runtimeErrorLine: snapshot.resultStatus == "runtime-error" ? snapshot.errorLine : 0)
            }
        }
    }

    private func appendConsole(_ lines: [NXConsoleLine]) {
        consoleLines.append(contentsOf: lines)
        if consoleLines.count > consoleLimit {
            consoleLines.removeFirst(consoleLines.count - consoleLimit + consoleLimit / 8)
            consoleResetToken += 1
        }
    }

    func clearConsole() {
        host.clearConsole()
        consoleLines.removeAll()
        consoleResetToken += 1
    }

    func refreshTargets() {
        targets = host.executionTargets()
    }

    // MARK: Roblox

    func refreshRoblox() {
        robloxBusy = true
        DispatchQueue.global(qos: .userInitiated).async {
            let player = self.host.refreshRoblox()
            let studio = self.host.refreshStudio()
            DispatchQueue.main.async {
                self.player = player
                self.studio = studio
                self.robloxBusy = false
                self.refreshTargets()
            }
        }
    }

    func connectStudio() {
        studioBusy = true
        DispatchQueue.global(qos: .userInitiated).async {
            let status = self.host.connectStudio()
            DispatchQueue.main.async {
                self.studio = status
                self.studioBusy = false
                self.settings = self.host.currentSettings()
                self.refreshTargets()
                if status.connected { self.selectedTarget = "roblox-studio" }
            }
        }
    }

    func disconnectStudio() {
        host.disconnectStudio()
        studio = host.studioStatus()
        refreshTargets()
        if selectedTarget == "roblox-studio" { selectedTarget = "luau-isolated" }
    }

    /// One HTTPS GET to Roblox's public client-version endpoint, only after explicit consent.
    func checkLatestRobloxVersion() {
        guard settings.allowRobloxVersionCheck else { return }
        guard let url = URL(string: "https://clientsettingscdn.roblox.com/v2/client-version/MacPlayer") else { return }
        host.logInfo("Requesting \(url.absoluteString) (user-initiated version check)", category: "network")
        versionCheckResult = "Checking…"
        var request = URLRequest(url: url)
        request.timeoutInterval = 15
        request.httpShouldHandleCookies = false
        URLSession(configuration: .ephemeral).dataTask(with: request) { data, _, error in
            let text: String
            if let error { text = "Version check failed: \(error.localizedDescription)" }
            else { text = self.host.describeRemoteVersion(response: String(decoding: data ?? Data(), as: UTF8.self)) }
            DispatchQueue.main.async { self.versionCheckResult = text }
        }.resume()
    }

    // MARK: Compatibility

    func analyzeCurrent() {
        guard let doc = documents.selected else { return }
        compatBusy = true
        let source = doc.text
        DispatchQueue.global(qos: .userInitiated).async {
            let report = self.host.analyze(source: source)
            DispatchQueue.main.async {
                self.compatReport = report
                self.compatBusy = false
                doc.editor.showDiagnostics(report.editorDiagnostics, runtimeErrorLine: 0)
            }
        }
    }

    // MARK: Library

    func reloadLibrary() { library = host.libraryTree() }

    func openFilePanel() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = Self.scriptTypes
        panel.allowsMultipleSelection = true
        panel.message = "Opened scripts are shown in the editor; nothing is executed automatically."
        guard panel.runModal() == .OK else { return }
        for url in panel.urls { documents.openFile(url) }
        section = .editor
    }

    static var scriptTypes: [UTType] {
        ["luau", "lua", "txt"].compactMap { UTType(filenameExtension: $0) } + [.plainText]
    }

    // MARK: Settings

    func updateSettings(_ change: (NXSettings) -> Void) {
        guard let copy = settings.copy() as? NXSettings else { return }
        change(copy)
        do {
            try host.updateSettings(copy)
            settings = host.currentSettings()
            for doc in documents.documents { doc.editor.apply(settings: settings) }
            refreshTargets()
        } catch {
            present(error, title: "Could not save settings")
        }
    }

    func binding<T>(_ keyPath: ReferenceWritableKeyPath<NXSettings, T>) -> Binding<T> {
        Binding(get: { self.settings[keyPath: keyPath] }, set: { value in self.updateSettings { $0[keyPath: keyPath] = value } })
    }

    var colorScheme: ColorScheme? {
        switch settings.appearance {
        case "light": return .light
        case "dark": return .dark
        default: return nil
        }
    }

    var accentColor: Color { Theme.color(fromHex: settings.accentColor) }

    // MARK: Errors

    func present(_ error: Error, title: String) {
        alert = AlertInfo(title: title, message: error.localizedDescription)
    }

    static let welcomeScript = """
    -- Welcome to Nexus. Press ⌘R to run this script in the sandboxed Luau runtime.
    local function fibonacci(n: number): number
        if n < 2 then
            return n
        end
        return fibonacci(n - 1) + fibonacci(n - 2)
    end

    for i = 1, 10 do
        print(`fib({i}) = {fibonacci(i)}`)
    end

    task.spawn(function()
        task.wait(0.25)
        print("Background task finished")
    end)

    """
}

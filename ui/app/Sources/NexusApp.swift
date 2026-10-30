import AppKit
import SwiftUI

@main
struct NexusApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var model = AppModel.shared

    var body: some Scene {
        WindowGroup("Nexus", id: "main") {
            RootView()
                .environmentObject(model)
                .frame(minWidth: 980, minHeight: 620)
                .preferredColorScheme(model.colorScheme)
                .tint(model.accentColor)
        }
        .defaultSize(width: 1280, height: 820)
        .windowToolbarStyle(.unified)
        .commands { NexusCommands(model: model) }

        Settings {
            SettingsView()
                .environmentObject(model)
                .frame(width: 620, height: 560)
                .preferredColorScheme(model.colorScheme)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        AppModel.shared.didFinishLaunching()
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        AppModel.shared.prepareForTermination()
        return .terminateNow
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

struct NexusCommands: Commands {
    @ObservedObject var model: AppModel

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("New Script") { model.documents.newDocument(); model.section = .editor }.keyboardShortcut("n")
            Button("Open…") { model.openFilePanel() }.keyboardShortcut("o")
        }
        CommandGroup(replacing: .saveItem) {
            Button("Save") { if let d = model.documents.selected { model.documents.save(d) } }.keyboardShortcut("s")
            Button("Save to Library As…") { if let d = model.documents.selected { model.documents.saveAs(d) } }.keyboardShortcut("s", modifiers: [.command, .shift])
            Button("Close Tab") { if let d = model.documents.selected { model.documents.close(d) } }.keyboardShortcut("w", modifiers: [.command, .option])
        }
        TextEditingCommands()
        CommandMenu("Script") {
            Button("Run") { model.runCurrent() }.keyboardShortcut("r").disabled(model.isRunning)
            Button("Stop") { model.stop() }.keyboardShortcut(".").disabled(!model.isRunning)
            Button("Restart") { model.restart() }.keyboardShortcut("r", modifiers: [.command, .shift])
            Divider()
            Button("Check Compatibility") { model.analyzeCurrent(); model.section = .compatibility }.keyboardShortcut("k", modifiers: [.command, .shift])
            Button("Format Document") { model.documents.selected?.editor.format() }.keyboardShortcut("f", modifiers: [.command, .option, .shift])
            Button("Go to Line…") { goToLine() }.keyboardShortcut("l")
            Divider()
            Button("Clear Console") { model.clearConsole() }.keyboardShortcut("k")
            Button("Increase Font Size") { model.documents.selected?.editor.adjustFontSize(by: 1) }.keyboardShortcut("+")
            Button("Decrease Font Size") { model.documents.selected?.editor.adjustFontSize(by: -1) }.keyboardShortcut("-")
        }
        CommandGroup(after: .sidebar) {
            ForEach(Array(SidebarSection.allCases.enumerated()), id: \.element) { index, section in
                Button(section.title) { model.section = section }.keyboardShortcut(KeyEquivalent(Character("\(index + 1)")), modifiers: .command)
            }
        }
    }

    private func goToLine() {
        guard let doc = model.documents.selected else { return }
        let alert = NSAlert()
        alert.messageText = "Go to Line"
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 120, height: 24))
        alert.accessoryView = field
        alert.addButton(withTitle: "Go")
        alert.addButton(withTitle: "Cancel")
        if alert.runModal() == .alertFirstButtonReturn, let line = Int(field.stringValue) {
            model.section = .editor
            doc.editor.goTo(line: line)
        }
    }
}

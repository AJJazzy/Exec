import AppKit
import SwiftUI

/// `Nexus --self-test <result.json> [--data-dir <dir>]` launches the real application, exercises the
/// main features through the same code paths as the UI, writes the results (and window snapshots
/// next to the JSON file) and quits. Used by CI as evidence that the built app launches and works.
enum SelfTest {
    static var isRequested: Bool { CommandLine.arguments.contains("--self-test") }

    static var outputPath: String {
        let args = CommandLine.arguments
        if let i = args.firstIndex(of: "--self-test"), i + 1 < args.count, !args[i + 1].hasPrefix("--") { return args[i + 1] }
        return NSTemporaryDirectory() + "nexus-self-test.json"
    }

    private static var checks: [[String: Any]] = []
    private static var screenshots: [String] = []
    private static let lock = NSLock()

    private static func check(_ name: String, _ passed: Bool, _ detail: String = "") {
        lock.lock()
        checks.append(["name": name, "passed": passed, "detail": detail])
        lock.unlock()
        NSLog("[self-test] %@ %@ %@", passed ? "PASS" : "FAIL", name, detail)
    }

    static func start(model: AppModel) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { uiChecks(model: model) }
    }

    private static func uiChecks(model: AppModel) {
        let windows = NSApp.windows.filter { $0.isVisible }
        check("window-visible", !windows.isEmpty, "\(windows.count) visible window(s)")
        let source = "local Players = game:GetService(\"Players\")\nprint('self-test')\n"
        let doc = model.documents.newDocument(title: "SelfTest.luau", text: source)
        model.section = .editor
        let tokens = model.host.highlight(source: source).count / MemoryLayout<NXTokenRange>.stride
        check("editor-highlighting", tokens > 5, "\(tokens) tokens")
        check("editor-text-view", doc.editor.textView.string == source)
        let formatted = (try? model.host.format(source: "if x then\nprint(1)\nend\n")) ?? ""
        check("formatter", formatted == "if x then\n    print(1)\nend\n")
        let completion = model.host.completions(source: "math.fl", cursor: 7)
        check("completion", completion.items.first?.label == "floor", completion.items.first?.label ?? "none")
        let report = model.host.analyze(source: source)
        check("compatibility-analysis", report.services.contains("Players") && report.verdicts.count == 3,
              report.verdicts.map { "\($0.targetId)=\($0.verdict)" }.joined(separator: ", "))
        let syntax = model.host.syntaxDiagnostics(source: "local = 1")
        check("syntax-diagnostics", !syntax.isEmpty && syntax[0].line == 1, syntax.first?.message ?? "")

        DispatchQueue.global(qos: .userInitiated).async { executionChecks(model: model) }
    }

    private static func runAndWait(_ model: AppModel, _ source: String, target: String, timeout: TimeInterval = 20,
                                   afterStart: (() -> Void)? = nil) -> NXSessionSnapshot? {
        do {
            try model.host.run(source: source, name: "SelfTest", target: target)
        } catch {
            check("start-\(target)", false, error.localizedDescription)
            return nil
        }
        afterStart?()
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            let s = model.host.currentSession()
            if !s.active && s.hasResult { return s }
            Thread.sleep(forTimeInterval: 0.05)
        }
        return model.host.currentSession()
    }

    private static func executionChecks(model: AppModel) {
        if let s = runAndWait(model, "print('nexus self-test ok')\nprint(6 * 7)", target: "luau-isolated") {
            check("run-isolated-runner", s.resultStatus == "succeeded", s.resultSummary)
        }
        Thread.sleep(forTimeInterval: 0.2)
        let console = model.host.consoleLines(after: 0, limit: 500).map { $0.text }
        check("console-output", console.contains("nexus self-test ok") && console.contains("42"), "\(console.count) console lines")
        let sandboxLine = console.first { $0.contains("sandbox:") } ?? "no runner info line"
        check("runner-sandbox-reported", sandboxLine.contains("Seatbelt"), sandboxLine)

        if let s = runAndWait(model, "local t = nil\nprint(t.x)", target: "luau-isolated") {
            check("runtime-error-line", s.resultStatus == "runtime-error" && s.errorLine == 2, "\(s.resultStatus) line \(s.errorLine): \(s.errorMessage)")
        }
        let stopStarted = Date()
        if let s = runAndWait(model, "while true do end", target: "luau-isolated", afterStart: {
            Thread.sleep(forTimeInterval: 0.4)
            _ = model.host.stop()
        }) {
            let elapsed = Date().timeIntervalSince(stopStarted)
            check("stop-infinite-loop", s.resultStatus == "cancelled" && elapsed < 5, String(format: "%@ after %.2fs", s.resultStatus, elapsed))
        }
        if let s = runAndWait(model, "task.wait(0.05) print('in-process ok')", target: "luau-inprocess") {
            check("run-inprocess", s.resultStatus == "succeeded", s.resultSummary)
        }
        if let s = runAndWait(model, "print('x')", target: "roblox-player") {
            check("player-target-refuses", s.resultStatus == "unavailable", s.resultSummary)
        }
        let player = model.host.refreshRoblox()
        check("roblox-player-detection", ["DISCONNECTED", "UNSUPPORTED"].contains(player.state), player.summary)
        let studio = model.host.refreshStudio()
        check("roblox-studio-detection", studio.state != "CONNECTED", "\(studio.state): \(studio.message)")
        do {
            let path = try model.host.createScript(folder: "", name: "self-test", content: "print('library')\n")
            let back = try model.host.readScript(path)
            check("library-roundtrip", back == "print('library')\n", path)
        } catch {
            check("library-roundtrip", false, error.localizedDescription)
        }
        DispatchQueue.main.async {
            model.player = player
            model.studio = studio
            model.history = model.host.executionHistory()
            model.reloadLibrary()
            model.analyzeCurrent()
            captureSections(model: model, remaining: [.editor, .home, .roblox, .compatibility, .library, .diagnostics, .settings])
        }
    }

    private static func captureSections(model: AppModel, remaining: [SidebarSection]) {
        guard let section = remaining.first else {
            finish(model: model)
            return
        }
        model.section = section
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            capture(name: section.rawValue)
            captureSections(model: model, remaining: Array(remaining.dropFirst()))
        }
    }

    /// Renders the main window's view hierarchy into a PNG (no screen-recording permission needed).
    private static func capture(name: String) {
        guard let window = NSApp.windows.first(where: { $0.isVisible && $0.frame.width > 400 }),
              let view = window.contentView?.superview ?? window.contentView else { return }
        let bounds = view.bounds
        guard let rep = view.bitmapImageRepForCachingDisplay(in: bounds) else { return }
        view.cacheDisplay(in: bounds, to: rep)
        guard let data = rep.representation(using: .png, properties: [:]) else { return }
        let path = (outputPath as NSString).deletingPathExtension + "-\(name).png"
        if (try? data.write(to: URL(fileURLWithPath: path))) != nil { screenshots.append(path) }
    }

    private static func finish(model: AppModel) {
        lock.lock()
        let all = checks
        lock.unlock()
        let passed = all.allSatisfy { ($0["passed"] as? Bool) == true }
        let result: [String: Any] = [
            "format": "nexus-self-test-v1",
            "nexusVersion": model.host.nexusVersion,
            "luauVersion": model.host.luauVersion,
            "operatingSystem": model.host.operatingSystem,
            "allPassed": passed,
            "checks": all,
            "screenshots": screenshots,
        ]
        if let data = try? JSONSerialization.data(withJSONObject: result, options: [.prettyPrinted, .sortedKeys]) {
            try? data.write(to: URL(fileURLWithPath: outputPath))
        }
        NSLog("[self-test] finished: %@", passed ? "all checks passed" : "FAILURES")
        NSApp.terminate(nil)
    }
}

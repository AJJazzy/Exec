import AppKit
import SwiftUI

struct ConsoleFilter: Equatable {
    var output = true
    var warnings = true
    var errors = true
    var system = true
    var timestamps = false

    func shows(_ stream: NXOutputStream) -> Bool {
        switch stream {
        case .output: return output
        case .warning: return warnings
        case .error: return errors
        case .info, .system: return system
        @unknown default: return true
        }
    }
}

/// Append-only console backed by NSTextView: new lines are appended incrementally, the view is
/// only rebuilt when the bounded buffer drops old lines or the filter changes.
struct ConsoleTextView: NSViewRepresentable {
    let lines: [NXConsoleLine]
    let resetToken: Int
    let filter: ConsoleFilter
    let fontSize: CGFloat

    final class Coordinator {
        var lastSequence: UInt64 = 0
        var resetToken = -1
        var filter = ConsoleFilter()
        var fontSize: CGFloat = 0
        static let timeFormatter: DateFormatter = {
            let f = DateFormatter()
            f.dateFormat = "HH:mm:ss.SSS"
            return f
        }()
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        scroll.borderType = .noBorder
        scroll.autohidesScrollers = true
        if let tv = scroll.documentView as? NSTextView {
            tv.isEditable = false
            tv.isSelectable = true
            tv.isRichText = false
            tv.usesFindBar = true
            tv.backgroundColor = Theme.editorBackground
            tv.textContainerInset = NSSize(width: 8, height: 6)
            tv.setAccessibilityLabel("Runtime console")
        }
        scroll.backgroundColor = Theme.editorBackground
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let tv = scroll.documentView as? NSTextView, let storage = tv.textStorage else { return }
        let c = context.coordinator
        let font = NSFont.monospacedSystemFont(ofSize: fontSize, weight: .regular)
        let rebuild = c.resetToken != resetToken || c.filter != filter || c.fontSize != fontSize ||
            (lines.first.map { $0.sequence > c.lastSequence + 1 && c.lastSequence != 0 } ?? false) || lines.isEmpty
        let atBottom = scroll.contentView.bounds.maxY >= tv.frame.maxY - 20
        let newLines = rebuild ? lines : lines.filter { $0.sequence > c.lastSequence }
        if rebuild { storage.setAttributedString(NSAttributedString()) }
        if !newLines.isEmpty {
            let chunk = NSMutableAttributedString()
            for line in newLines where filter.shows(line.stream) {
                var text = line.text
                if filter.timestamps { text = "[\(Coordinator.timeFormatter.string(from: line.time))] " + text }
                chunk.append(NSAttributedString(string: text + "\n", attributes: [.font: font, .foregroundColor: Theme.consoleColor(line.stream)]))
            }
            storage.append(chunk)
        }
        c.lastSequence = lines.last?.sequence ?? 0
        c.resetToken = resetToken
        c.filter = filter
        c.fontSize = fontSize
        if atBottom || rebuild { tv.scrollToEndOfDocument(nil) }
    }
}

struct ConsolePanel: View {
    @EnvironmentObject var model: AppModel
    @State private var filter = ConsoleFilter()
    var compact = false

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Label("Console", systemImage: "terminal").font(.subheadline.weight(.semibold))
                if model.isRunning {
                    ProgressView().controlSize(.small)
                    Text(model.session.stateName.capitalized).font(.caption).foregroundStyle(.secondary)
                } else if model.session.hasResult {
                    StateBadge(text: model.session.resultStatus)
                    Text(model.session.resultSummary).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                }
                Spacer()
                Toggle("Output", isOn: $filter.output).toggleStyle(.button).controlSize(.small)
                Toggle("Warnings", isOn: $filter.warnings).toggleStyle(.button).controlSize(.small)
                Toggle("Errors", isOn: $filter.errors).toggleStyle(.button).controlSize(.small)
                Toggle("System", isOn: $filter.system).toggleStyle(.button).controlSize(.small)
                Toggle(isOn: $filter.timestamps) { Image(systemName: "clock") }
                    .toggleStyle(.button).controlSize(.small).help("Show timestamps")
                Button { model.clearConsole() } label: { Image(systemName: "trash") }
                    .buttonStyle(.borderless).help("Clear console (⌘K)")
                if !compact {
                    Button { exportConsole() } label: { Image(systemName: "square.and.arrow.up") }
                        .buttonStyle(.borderless).help("Export console output")
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            Divider()
            ConsoleTextView(lines: model.consoleLines, resetToken: model.consoleResetToken, filter: filter,
                            fontSize: CGFloat(max(10, model.settings.editorFontSize - 1)))
        }
    }

    private func exportConsole() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "nexus-console.txt"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let text = model.consoleLines.map { $0.text }.joined(separator: "\n") + "\n"
        do { try text.write(to: url, atomically: true, encoding: .utf8) } catch { model.present(error, title: "Export failed") }
    }
}

struct ConsoleView: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(spacing: 0) {
            ConsolePanel()
            if model.session.hasResult && !model.session.active {
                Divider()
                RunStatisticsBar(session: model.session)
            }
        }
        .navigationTitle("Runtime Console")
    }
}

struct RunStatisticsBar: View {
    let session: NXSessionSnapshot
    var body: some View {
        HStack(spacing: 18) {
            stat("Target", session.targetId)
            stat("Wall time", formatMs(session.wallMs))
            stat("CPU", formatMs(session.cpuMs))
            stat("Compile", formatMs(session.compileMs))
            stat("Peak memory", formatBytes(session.peakMemoryBytes))
            stat("Output", formatBytes(session.outputBytes))
            stat("Tasks", "\(session.threadsCreated)")
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .font(.caption)
    }

    private func stat(_ title: String, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).foregroundStyle(.secondary)
            Text(value).monospacedDigit()
        }
    }
}

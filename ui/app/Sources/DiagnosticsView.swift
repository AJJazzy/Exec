import AppKit
import SwiftUI

struct LogRow: Identifiable {
    let id: UInt64
    let entry: NXLogEntry
}

struct HistoryRow: Identifiable {
    let id: UInt64
    let record: NXExecutionRecord
}

struct DiagnosticsView: View {
    @EnvironmentObject var model: AppModel
    @State private var logs: [LogRow] = []
    @State private var levelFilter = "all"
    @State private var tab = 0

    private static let timeFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd HH:mm:ss"
        return f
    }()

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Diagnostics").font(.title2.weight(.semibold))
                Spacer()
                Button { exportDiagnostics() } label: { Label("Export Diagnostics…", systemImage: "square.and.arrow.up") }
                Button { exportLogs() } label: { Label("Export Logs…", systemImage: "doc.text") }
            }
            .padding(20)
            HStack(alignment: .top, spacing: 12) {
                Card {
                    Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
                        info("Nexus", model.host.nexusVersion)
                        info("Luau", model.host.luauVersion)
                        info("System", model.host.operatingSystem)
                        info("Data", model.host.dataDirectory)
                        info("Runner", model.host.runnerPath)
                    }
                    .font(.callout)
                }
                Card {
                    VStack(alignment: .leading, spacing: 6) {
                        Text("Execution targets").font(.headline)
                        ForEach(model.targets, id: \.identifier) { t in
                            HStack(alignment: .top) {
                                Circle().fill(t.available ? Color.green : Color.secondary).frame(width: 8, height: 8).padding(.top, 5)
                                VStack(alignment: .leading) {
                                    Text(t.displayName)
                                    Text(t.availability).font(.caption).foregroundStyle(.secondary).lineLimit(2)
                                }
                            }
                        }
                    }
                }
            }
            .padding(.horizontal, 20)
            Picker("", selection: $tab) {
                Text("Execution History").tag(0)
                Text("Application Log").tag(1)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .frame(maxWidth: 360)
            .padding(20)
            if tab == 0 { historyTable } else { logTable }
        }
        .navigationTitle("Diagnostics")
        .onAppear { reloadLogs(); model.refreshTargets(); model.history = model.host.executionHistory() }
    }

    private var historyTable: some View {
        VStack(spacing: 0) {
            Table(model.history.map { HistoryRow(id: $0.sessionId, record: $0) }) {
                TableColumn("Started") { r in Text(Self.timeFormatter.string(from: r.record.startedAt)).monospacedDigit() }.width(min: 130, ideal: 150)
                TableColumn("Script") { r in Text(r.record.scriptName) }
                TableColumn("Target") { r in Text(r.record.targetId) }
                TableColumn("Status") { r in StateBadge(text: r.record.status) }.width(min: 90, ideal: 110)
                TableColumn("Wall") { r in Text(formatMs(r.record.wallMs)).monospacedDigit() }.width(min: 60, ideal: 70)
                TableColumn("CPU") { r in Text(formatMs(r.record.cpuMs)).monospacedDigit() }.width(min: 60, ideal: 70)
                TableColumn("Peak memory") { r in Text(formatBytes(r.record.peakMemoryBytes)).monospacedDigit() }.width(min: 80, ideal: 90)
                TableColumn("Summary") { r in Text(r.record.summary).lineLimit(2) }
            }
            HStack {
                Text("\(model.history.count) run(s) recorded").font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button("Clear History") { model.host.clearHistory(); model.history = [] }.buttonStyle(.link)
            }
            .padding(8)
        }
    }

    private var logTable: some View {
        VStack(spacing: 0) {
            Table(logs.filter { levelFilter == "all" || $0.entry.level == levelFilter }) {
                TableColumn("Time") { r in Text(Self.timeFormatter.string(from: r.entry.time)).monospacedDigit() }.width(min: 130, ideal: 150)
                TableColumn("Level") { r in Text(r.entry.level).foregroundStyle(r.entry.level == "error" ? .red : r.entry.level == "warning" ? .orange : .primary) }.width(min: 60, ideal: 70)
                TableColumn("Category") { r in Text(r.entry.category) }.width(min: 70, ideal: 90)
                TableColumn("Message") { r in Text(r.entry.message + (r.entry.fields.isEmpty ? "" : "  " + r.entry.fields)).lineLimit(3).textSelection(.enabled) }
            }
            HStack {
                Picker("Level", selection: $levelFilter) {
                    Text("All").tag("all")
                    Text("Info").tag("info")
                    Text("Warnings").tag("warning")
                    Text("Errors").tag("error")
                }
                .frame(maxWidth: 200)
                Spacer()
                Button("Reload") { reloadLogs() }.buttonStyle(.link)
            }
            .padding(8)
        }
    }

    private func info(_ title: String, _ value: String) -> some View {
        GridRow {
            Text(title).foregroundStyle(.secondary)
            Text(value).textSelection(.enabled).lineLimit(2).truncationMode(.middle)
        }
    }

    private func reloadLogs() {
        logs = model.host.logEntries(after: 0).reversed().map { LogRow(id: $0.sequence, entry: $0) }
    }

    private func exportDiagnostics() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "nexus-diagnostics.json"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try model.host.exportDiagnostics(to: url.path) } catch { model.present(error, title: "Export failed") }
    }

    private func exportLogs() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "nexus-log.txt"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try model.host.exportLogs(to: url.path) } catch { model.present(error, title: "Export failed") }
    }
}

import AppKit
import SwiftUI

struct FindingRow: Identifiable {
    let id: Int
    let finding: NXCompatFinding
}

struct CompatibilityView: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var store: DocumentStore
    @State private var selectedFinding: Int?

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Compatibility").font(.title2.weight(.semibold))
                    Text(store.selected.map { "Script: \($0.title)" } ?? "No script open").foregroundStyle(.secondary)
                }
                Spacer()
                if model.compatBusy { ProgressView().controlSize(.small) }
                Button { model.analyzeCurrent() } label: { Label("Check Compatibility", systemImage: "checkmark.shield") }
                    .keyboardShortcut(.defaultAction)
                    .disabled(store.selected == nil)
                if let report = model.compatReport {
                    Menu {
                        Button("Export as JSON…") { export(report.json, name: "compatibility.json") }
                        Button("Export as Text…") { export(report.text, name: "compatibility.txt") }
                    } label: { Label("Export", systemImage: "square.and.arrow.up") }
                        .fixedSize()
                }
            }
            .padding(20)
            if let report = model.compatReport {
                HStack(alignment: .top, spacing: 12) {
                    ForEach(report.verdicts, id: \.targetId) { v in
                        Card {
                            VStack(alignment: .leading, spacing: 6) {
                                HStack { Text(v.displayName).font(.headline).lineLimit(2); Spacer(); StateBadge(text: v.verdict) }
                                Text(v.summary).font(.callout).foregroundStyle(.secondary)
                                if !v.blockers.isEmpty {
                                    Text(v.blockers.prefix(6).joined(separator: ", ") + (v.blockers.count > 6 ? " …" : ""))
                                        .font(.caption).foregroundStyle(.secondary).lineLimit(3)
                                }
                            }
                        }
                    }
                }
                .padding(.horizontal, 20)
                HStack(spacing: 16) {
                    Text("\(report.findings.count) finding(s)")
                    if !report.services.isEmpty { Text("Services: " + report.services.joined(separator: ", ")).lineLimit(1) }
                    Spacer()
                    Text("Analysed in \(formatMs(report.analysisMs))").foregroundStyle(.secondary)
                }
                .font(.caption)
                .padding(.horizontal, 20)
                .padding(.vertical, 8)
                Table(rows(report), selection: $selectedFinding) {
                    TableColumn("Severity") { r in
                        Text(r.finding.severity.capitalized)
                            .foregroundStyle(r.finding.severity == "error" ? Color.red : r.finding.severity == "warning" ? Color.orange : Color.secondary)
                    }
                    .width(min: 70, ideal: 80, max: 90)
                    TableColumn("Line") { r in Text(r.finding.line > 0 ? "\(r.finding.line)" : "—").monospacedDigit() }
                        .width(min: 40, ideal: 50, max: 60)
                    TableColumn("Symbol") { r in Text(r.finding.symbol).font(.body.monospaced()) }
                        .width(min: 90, ideal: 140)
                    TableColumn("Finding") { r in
                        VStack(alignment: .leading, spacing: 2) {
                            Text(r.finding.message).lineLimit(3)
                            if !r.finding.risk.isEmpty { Text("Risk: \(r.finding.risk)").font(.caption).foregroundStyle(.red) }
                        }
                    }
                    TableColumn("Standalone") { r in StateBadge(text: r.finding.standalone) }.width(min: 80, ideal: 95, max: 110)
                    TableColumn("Studio") { r in StateBadge(text: r.finding.studio) }.width(min: 80, ideal: 95, max: 110)
                }
                .contextMenu(forSelectionType: Int.self) { _ in } primaryAction: { ids in
                    if let id = ids.first, let row = rows(report).first(where: { $0.id == id }), row.finding.line > 0 {
                        model.section = .editor
                        DispatchQueue.main.async { store.selected?.editor.goTo(line: row.finding.line) }
                    }
                }
                if !report.syntaxOk {
                    Text("The script has syntax errors; fix them in the editor first.").foregroundStyle(.red).padding(12)
                }
            } else {
                VStack(spacing: 10) {
                    Image(systemName: "checkmark.shield").font(.system(size: 40)).foregroundStyle(.secondary)
                    Text("Check which Roblox engine, executor-specific and dangerous APIs a script depends on, and where it can run.")
                        .multilineTextAlignment(.center).foregroundStyle(.secondary).frame(maxWidth: 460)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .navigationTitle("Compatibility")
    }

    private func rows(_ report: NXCompatReport) -> [FindingRow] {
        report.findings.enumerated().map { FindingRow(id: $0.offset, finding: $0.element) }
    }

    private func export(_ text: String, name: String) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = name
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do { try text.write(to: url, atomically: true, encoding: .utf8) } catch { model.present(error, title: "Export failed") }
    }
}

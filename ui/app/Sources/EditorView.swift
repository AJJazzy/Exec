import AppKit
import SwiftUI

struct EditorWorkspace: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var store: DocumentStore

    var body: some View {
        VSplitView {
            VStack(spacing: 0) {
                TabStrip(store: store)
                Divider()
                if let doc = store.selected {
                    CodeEditorView(document: doc)
                        .id(doc.id)
                    Divider()
                    EditorStatusBar(document: doc)
                } else {
                    EmptyEditor()
                }
            }
            .frame(minHeight: 220)
            if model.showConsoleInEditor {
                ConsolePanel(compact: true)
                    .frame(minHeight: 120, idealHeight: 220)
            }
        }
        .navigationTitle(store.selected?.title ?? "Script Editor")
    }
}

struct TabStrip: View {
    @ObservedObject var store: DocumentStore

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 2) {
                ForEach(store.documents) { doc in
                    TabButton(document: doc, selected: doc.id == store.selectedID,
                              select: { store.selectedID = doc.id }, close: { store.close(doc) })
                }
                Button { store.newDocument() } label: { Image(systemName: "plus") }
                    .buttonStyle(.borderless)
                    .padding(.horizontal, 8)
                    .help("New script (⌘N)")
            }
            .padding(.horizontal, 6)
            .padding(.vertical, 4)
        }
        .background(Color.nxSubtleBackground)
    }
}

struct TabButton: View {
    @ObservedObject var document: EditorDocument
    let selected: Bool
    let select: () -> Void
    let close: () -> Void
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: document.libraryPath != nil ? "doc.text" : document.fileURL != nil ? "doc" : "doc.badge.plus")
                .foregroundStyle(.secondary)
            Text(document.title).lineLimit(1)
            ZStack {
                if document.isDirty && !hovering {
                    Circle().fill(Color.secondary).frame(width: 7, height: 7).help("Unsaved changes")
                } else {
                    Button(action: close) { Image(systemName: "xmark").font(.system(size: 9, weight: .bold)) }
                        .buttonStyle(.borderless)
                        .opacity(hovering || selected ? 1 : 0)
                        .help("Close tab")
                }
            }
            .frame(width: 14, height: 14)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Color.accentColor.opacity(0.16) : Color.clear))
        .contentShape(Rectangle())
        .onTapGesture(perform: select)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.12), value: hovering)
    }
}

struct EditorStatusBar: View {
    @EnvironmentObject var model: AppModel
    @ObservedObject var document: EditorDocument

    private var errors: Int { document.diagnostics.filter { $0.severity == "error" }.count }
    private var warnings: Int { document.diagnostics.filter { $0.severity == "warning" }.count }

    var body: some View {
        HStack(spacing: 14) {
            if errors + warnings == 0 {
                Label("No problems", systemImage: "checkmark.circle").foregroundStyle(.green)
            } else {
                Menu {
                    ForEach(Array(document.diagnostics.enumerated()), id: \.offset) { _, d in
                        Button("Line \(d.line): \(d.message)") { document.editor.goTo(line: d.line) }
                    }
                } label: {
                    Label("\(errors) error\(errors == 1 ? "" : "s"), \(warnings) warning\(warnings == 1 ? "" : "s")",
                          systemImage: errors > 0 ? "xmark.octagon" : "exclamationmark.triangle")
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .foregroundStyle(errors > 0 ? .red : .orange)
            }
            Spacer()
            Text(document.libraryPath ?? document.fileURL?.path ?? "Not saved").foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
            Text("Ln \(document.cursorLine), Col \(document.cursorColumn)").monospacedDigit()
            Text("Luau").foregroundStyle(.secondary)
            Button { model.showConsoleInEditor.toggle() } label: { Image(systemName: "rectangle.bottomthird.inset.filled") }
                .buttonStyle(.borderless).help("Show or hide the console")
        }
        .font(.caption)
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
    }
}

struct EmptyEditor: View {
    @EnvironmentObject var model: AppModel
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "doc.text").font(.largeTitle).foregroundStyle(.secondary)
            Text("No script open").font(.headline)
            HStack {
                Button("New Script") { model.documents.newDocument() }
                Button("Open…") { model.openFilePanel() }
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}

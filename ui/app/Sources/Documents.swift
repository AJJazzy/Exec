import AppKit
import Foundation

/// One editor tab. Each document owns its own AppKit text view (through CodeEditorController),
/// so every tab keeps an independent undo history.
final class EditorDocument: ObservableObject, Identifiable {
    let id: String
    @Published var title: String
    @Published var libraryPath: String?
    @Published var fileURL: URL?
    @Published var isDirty = false
    @Published var diagnostics: [NXDiagnostic] = []
    @Published var cursorLine = 1
    @Published var cursorColumn = 1

    private(set) lazy var editor = CodeEditorController(document: self)
    private let initialText: String

    init(id: String = EditorDocument.makeId(), title: String, text: String, libraryPath: String? = nil, fileURL: URL? = nil) {
        self.id = id
        self.title = title
        self.libraryPath = libraryPath
        self.fileURL = fileURL
        self.initialText = text
    }

    var initialContent: String { initialText }
    var text: String { editor.text }

    var displayName: String { (isDirty ? "• " : "") + title }

    static func makeId() -> String {
        "tab-" + UUID().uuidString.replacingOccurrences(of: "-", with: "").lowercased().prefix(12)
    }
}

/// Owns the open tabs, persists unsaved buffers as drafts and restores them on launch.
final class DocumentStore: ObservableObject {
    @Published private(set) var documents: [EditorDocument] = []
    @Published var selectedID: String?

    private unowned let model: AppModel
    private var draftTimers: [String: DispatchWorkItem] = [:]

    init(model: AppModel) { self.model = model }

    var selected: EditorDocument? { documents.first { $0.id == selectedID } }

    @discardableResult
    func newDocument(title: String? = nil, text: String = "") -> EditorDocument {
        let n = documents.filter { $0.title.hasPrefix("Untitled") }.count
        let doc = EditorDocument(title: title ?? (n == 0 ? "Untitled" : "Untitled \(n + 1)"), text: text)
        documents.append(doc)
        selectedID = doc.id
        return doc
    }

    func openLibraryScript(_ path: String) {
        if let existing = documents.first(where: { $0.libraryPath == path }) {
            selectedID = existing.id
            return
        }
        do {
            let text = try model.host.readScript(path)
            let doc = EditorDocument(title: (path as NSString).lastPathComponent, text: text, libraryPath: path)
            documents.append(doc)
            selectedID = doc.id
            model.host.markOpened(path)
        } catch {
            model.present(error, title: "Could not open script")
        }
    }

    func openFile(_ url: URL) {
        if let existing = documents.first(where: { $0.fileURL == url }) {
            selectedID = existing.id
            return
        }
        do {
            let data = try Data(contentsOf: url)
            guard data.count <= 8 * 1024 * 1024 else { throw NexusUIError("\(url.lastPathComponent) is larger than 8 MB") }
            // Opened files are shown, never executed automatically.
            let text = String(decoding: data, as: UTF8.self)
            let doc = EditorDocument(title: url.lastPathComponent, text: text, fileURL: url)
            documents.append(doc)
            selectedID = doc.id
        } catch {
            model.present(error, title: "Could not open file")
        }
    }

    func close(_ doc: EditorDocument) {
        if doc.isDirty {
            let alert = NSAlert()
            alert.messageText = "Close “\(doc.title)” without saving?"
            alert.informativeText = "Your changes will be lost. (An autosaved draft is discarded as well.)"
            alert.addButton(withTitle: "Save")
            alert.addButton(withTitle: "Cancel")
            alert.addButton(withTitle: "Discard Changes")
            switch alert.runModal() {
            case .alertFirstButtonReturn:
                guard save(doc) else { return }
            case .alertSecondButtonReturn:
                return
            default:
                break
            }
        }
        model.host.removeDraft(doc.id)
        if let index = documents.firstIndex(where: { $0.id == doc.id }) {
            documents.remove(at: index)
            if selectedID == doc.id { selectedID = documents.isEmpty ? nil : documents[max(0, index - 1)].id }
        }
        if documents.isEmpty { newDocument() }
    }

    /// Saves to the library path, the external file, or asks where to save. Returns false if cancelled/failed.
    @discardableResult
    func save(_ doc: EditorDocument) -> Bool {
        do {
            if let path = doc.libraryPath {
                try model.host.writeScript(path, content: doc.text)
            } else if let url = doc.fileURL {
                try doc.text.write(to: url, atomically: true, encoding: .utf8)
            } else {
                return saveAs(doc)
            }
            markSaved(doc)
            return true
        } catch {
            model.present(error, title: "Could not save")
            return false
        }
    }

    /// Saves into the script library under a new name.
    @discardableResult
    func saveAs(_ doc: EditorDocument) -> Bool {
        let alert = NSAlert()
        alert.messageText = "Save to Script Library"
        alert.informativeText = "Name for the script (.luau is added automatically):"
        let field = NSTextField(frame: NSRect(x: 0, y: 0, width: 260, height: 24))
        field.stringValue = doc.title.replacingOccurrences(of: ".luau", with: "")
        alert.accessoryView = field
        alert.addButton(withTitle: "Save")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return false }
        do {
            let path = try model.host.createScript(folder: model.libraryTargetFolder, name: field.stringValue, content: doc.text)
            doc.libraryPath = path
            doc.fileURL = nil
            doc.title = (path as NSString).lastPathComponent
            markSaved(doc)
            model.reloadLibrary()
            return true
        } catch {
            model.present(error, title: "Could not save")
            return false
        }
    }

    private func markSaved(_ doc: EditorDocument) {
        doc.isDirty = false
        model.host.removeDraft(doc.id)
        draftTimers[doc.id]?.cancel()
    }

    func documentDidChange(_ doc: EditorDocument) {
        if !doc.isDirty { doc.isDirty = true }
        guard model.settings.restoreDrafts else { return }
        draftTimers[doc.id]?.cancel()
        let work = DispatchWorkItem { [weak self, weak doc] in
            guard let self, let doc, doc.isDirty else { return }
            let draft = NXDraft()
            draft.draftId = doc.id
            draft.title = doc.title
            draft.content = doc.text
            draft.libraryPath = doc.libraryPath ?? ""
            try? self.model.host.saveDraft(draft)
        }
        draftTimers[doc.id] = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5, execute: work)
    }

    /// Restores unsaved buffers from the previous session (they are opened, never run).
    func restoreDrafts() -> Int {
        guard model.settings.restoreDrafts else { return 0 }
        let drafts = model.host.savedDrafts()
        for d in drafts.reversed() {
            let doc = EditorDocument(id: d.draftId, title: d.title.isEmpty ? "Recovered" : d.title, text: d.content,
                                     libraryPath: d.libraryPath.isEmpty ? nil : d.libraryPath)
            doc.isDirty = true
            documents.append(doc)
        }
        selectedID = documents.first?.id
        return drafts.count
    }

    var hasUnsavedChanges: Bool { documents.contains { $0.isDirty } }
}

struct NexusUIError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

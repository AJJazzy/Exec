import AppKit
import SwiftUI

struct LibraryNode: Identifiable, Hashable {
    let id: String // relative path
    let name: String
    let isFolder: Bool
    let isProject: Bool
    let favorite: Bool
    let size: UInt64
    let modified: Date
    let children: [LibraryNode]?

    init(_ item: NXLibraryItem) {
        id = item.path
        name = item.name
        isFolder = item.isFolder
        isProject = item.isProject
        favorite = item.favorite
        size = item.sizeBytes
        modified = item.modified
        children = item.children?.map(LibraryNode.init)
    }
}

struct LibraryView: View {
    @EnvironmentObject var model: AppModel
    @State private var selection: String?
    @State private var query = ""
    @State private var results: [NXSearchHit] = []
    @State private var renaming: LibraryNode?
    @State private var newName = ""
    @State private var deleting: LibraryNode?

    private var roots: [LibraryNode] { LibraryNode(model.library).children ?? [] }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                TextField("Search names and contents", text: $query)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit(search)
                    .onChange(of: query) { _ in search() }
                Menu {
                    Button("New Script") { createScript() }
                    Button("New Folder") { createFolder(project: false) }
                    Button("New Project") { createFolder(project: true) }
                } label: { Label("New", systemImage: "plus") }
                    .fixedSize()
                Button { importScripts() } label: { Label("Import", systemImage: "square.and.arrow.down") }
                    .help("Copy .lua/.luau/.txt files into the library (they are not executed)")
                Button { model.reloadLibrary() } label: { Image(systemName: "arrow.clockwise") }.help("Reload")
            }
            .padding(10)
            Divider()
            if !query.isEmpty {
                List(results, id: \.self) { hit in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(hit.path).font(.body.weight(.medium))
                        Text(hit.nameMatch ? "Name matches" : "Line \(hit.line): \(hit.preview)").font(.caption).foregroundStyle(.secondary).lineLimit(1)
                    }
                    .contentShape(Rectangle())
                    .onTapGesture(count: 2) { open(hit.path, line: hit.nameMatch ? 0 : hit.line) }
                }
                .overlay { if results.isEmpty { Text("No matches").foregroundStyle(.secondary) } }
            } else {
                List(selection: $selection) {
                    let favorites = model.host.favoriteScripts()
                    if !favorites.isEmpty {
                        Section("Favorites") {
                            ForEach(favorites, id: \.self) { path in
                                Label(path, systemImage: "star.fill").onTapGesture(count: 2) { open(path) }
                            }
                        }
                    }
                    Section("Library") {
                        OutlineGroup(roots, children: \.children) { node in
                            row(node)
                        }
                    }
                }
                .overlay {
                    if roots.isEmpty {
                        VStack(spacing: 8) {
                            Image(systemName: "books.vertical").font(.largeTitle).foregroundStyle(.secondary)
                            Text("Your library is empty").font(.headline)
                            Text("Save a script from the editor (⇧⌘S), import files, or create a project.").foregroundStyle(.secondary)
                        }
                    }
                }
            }
            Divider()
            HStack {
                Text(model.host.libraryRoot).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                Spacer()
                Button("Show in Finder") { NSWorkspace.shared.open(URL(fileURLWithPath: model.host.libraryRoot)) }.buttonStyle(.link).font(.caption)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
        }
        .navigationTitle("Script Library")
        .onAppear { model.reloadLibrary() }
        .alert("Rename", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Name", text: $newName)
            Button("Rename") { performRename() }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
        .confirmationDialog("Move “\(deleting?.name ?? "")” to the library trash?", isPresented: Binding(get: { deleting != nil }, set: { if !$0 { deleting = nil } })) {
            Button("Move to Trash", role: .destructive) { performDelete() }
        } message: {
            Text("Items are moved to the hidden .trash folder inside the library and can be recovered from Finder.")
        }
    }

    @ViewBuilder
    private func row(_ node: LibraryNode) -> some View {
        HStack {
            Image(systemName: node.isFolder ? (node.isProject ? "shippingbox" : "folder") : "doc.text")
                .foregroundStyle(node.isProject ? Color.accentColor : .secondary)
            Text(node.name)
            if node.favorite { Image(systemName: "star.fill").foregroundStyle(.yellow).font(.caption) }
            Spacer()
            if !node.isFolder { Text(formatBytes(node.size)).font(.caption).foregroundStyle(.secondary) }
        }
        .tag(node.id)
        .contentShape(Rectangle())
        .onTapGesture(count: 2) { if !node.isFolder { open(node.id) } }
        .contextMenu {
            if !node.isFolder { Button("Open") { open(node.id) } }
            if node.isFolder {
                Button("New Script Here") { model.libraryTargetFolder = node.id; createScript() }
                Button(node.isProject ? "Unmark as Project" : "Mark as Project") { act { try model.host.setProject(!node.isProject, folder: node.id) } }
            } else {
                Button(node.favorite ? "Remove from Favorites" : "Add to Favorites") { act { try model.host.setFavorite(!node.favorite, item: node.id) } }
            }
            Button("Rename…") { newName = node.name; renaming = node }
            Button("Duplicate") { act { _ = try model.host.duplicateItem(node.id) } }
            Button("Export…") { export(node) }
            Button("Reveal in Finder") {
                NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: model.host.libraryRoot).appendingPathComponent(node.id)])
            }
            Divider()
            Button("Delete…", role: .destructive) { deleting = node }
        }
    }

    private func act(_ body: () throws -> Void) {
        do {
            try body()
            model.reloadLibrary()
        } catch {
            model.present(error, title: "Library operation failed")
        }
    }

    private func open(_ path: String, line: Int = 0) {
        model.documents.openLibraryScript(path)
        model.section = .editor
        if line > 0 { DispatchQueue.main.async { model.documents.selected?.editor.goTo(line: line) } }
    }

    private func currentFolder() -> String {
        guard let sel = selection else { return "" }
        func find(_ nodes: [LibraryNode]) -> LibraryNode? {
            for n in nodes {
                if n.id == sel { return n }
                if let c = n.children, let f = find(c) { return f }
            }
            return nil
        }
        guard let node = find(roots) else { return "" }
        return node.isFolder ? node.id : (node.id as NSString).deletingLastPathComponent
    }

    private func createScript() {
        let folder = model.libraryTargetFolder.isEmpty ? currentFolder() : model.libraryTargetFolder
        model.libraryTargetFolder = ""
        act {
            let path = try model.host.createScript(folder: folder, name: "New Script", content: "-- New script\n")
            selection = path
        }
    }

    private func createFolder(project: Bool) {
        act { selection = try model.host.createFolder(parent: currentFolder(), name: project ? "New Project" : "New Folder", project: project) }
    }

    private func performRename() {
        guard let node = renaming else { return }
        renaming = nil
        act {
            let newPath = try model.host.renameItem(node.id, to: newName)
            for doc in model.documents.documents where doc.libraryPath == node.id {
                doc.libraryPath = newPath
                doc.title = (newPath as NSString).lastPathComponent
            }
        }
    }

    private func performDelete() {
        guard let node = deleting else { return }
        deleting = nil
        act { try model.host.deleteItem(node.id) }
    }

    private func importScripts() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = AppModel.scriptTypes
        panel.allowsMultipleSelection = true
        panel.message = "Imported files are copied byte-for-byte into the library. They are not executed."
        guard panel.runModal() == .OK else { return }
        let folder = currentFolder()
        act {
            for url in panel.urls { _ = try model.host.importFile(url.path, folder: folder) }
        }
    }

    private func export(_ node: LibraryNode) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = node.name
        guard panel.runModal() == .OK, let url = panel.url else { return }
        act { try model.host.exportItem(node.id, to: url.path) }
    }

    private func search() {
        results = query.trimmingCharacters(in: .whitespaces).isEmpty ? [] : model.host.searchLibrary(query)
    }
}

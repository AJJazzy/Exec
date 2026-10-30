import AppKit
import SwiftUI

// MARK: - Text view

/// NSTextView with code-editing behaviour: auto-closing pairs, auto-indent, soft tabs and
/// completion backed by the Nexus language services.
final class LuauTextView: NSTextView {
    weak var editorController: CodeEditorController?
    var indentUnit = "    "
    var autoClose = true
    var autoIndentEnabled = true

    private static let pairs: [String: String] = ["(": ")", "[": "]", "{": "}", "\"": "\"", "'": "'", "`": "`"]
    private static let closers: Set<String> = [")", "]", "}", "\"", "'", "`"]

    private var nsText: NSString { string as NSString }

    private func character(at index: Int) -> String? {
        guard index >= 0, index < nsText.length else { return nil }
        return nsText.substring(with: NSRange(location: index, length: 1))
    }

    private static func isIdentifier(_ s: String?) -> Bool {
        guard let s, let scalar = s.unicodeScalars.first else { return false }
        return CharacterSet.alphanumerics.contains(scalar) || s == "_"
    }

    override func insertText(_ insertString: Any, replacementRange: NSRange) {
        guard autoClose, let s = insertString as? String, s.count == 1, !hasMarkedText() else {
            super.insertText(insertString, replacementRange: replacementRange)
            return
        }
        let sel = selectedRange()
        // Typing a closing character over an identical one just moves the cursor.
        if Self.closers.contains(s), sel.length == 0, character(at: sel.location) == s {
            setSelectedRange(NSRange(location: sel.location + 1, length: 0))
            return
        }
        if let close = Self.pairs[s] {
            let isQuote = s == "\"" || s == "'" || s == "`"
            if isQuote && sel.length == 0 && (Self.isIdentifier(character(at: sel.location - 1)) || Self.isIdentifier(character(at: sel.location))) {
                super.insertText(insertString, replacementRange: replacementRange)
                return
            }
            if sel.length > 0 {
                let wrapped = s + nsText.substring(with: sel) + close
                super.insertText(wrapped, replacementRange: sel)
                return
            }
            if !isQuote, let next = character(at: sel.location), Self.isIdentifier(next) {
                super.insertText(insertString, replacementRange: replacementRange)
                return
            }
            super.insertText(s + close, replacementRange: replacementRange)
            let caret = selectedRange().location
            setSelectedRange(NSRange(location: max(0, caret - 1), length: 0))
            return
        }
        super.insertText(insertString, replacementRange: replacementRange)
    }

    override func deleteBackward(_ sender: Any?) {
        let sel = selectedRange()
        if autoClose, sel.length == 0, sel.location > 0, let open = character(at: sel.location - 1),
           let close = Self.pairs[open], character(at: sel.location) == close {
            insertText("", replacementRange: NSRange(location: sel.location - 1, length: 2))
            return
        }
        super.deleteBackward(sender)
    }

    override func insertNewline(_ sender: Any?) {
        guard autoIndentEnabled else {
            super.insertNewline(sender)
            return
        }
        let sel = selectedRange()
        let lineRange = nsText.lineRange(for: NSRange(location: sel.location, length: 0))
        let before = nsText.substring(with: NSRange(location: lineRange.location, length: sel.location - lineRange.location))
        let indent = String(before.prefix { $0 == " " || $0 == "\t" })
        let trimmed = Self.stripComment(before).trimmingCharacters(in: .whitespaces)
        let next = character(at: sel.location)
        let betweenPair = (trimmed.hasSuffix("{") && next == "}") || (trimmed.hasSuffix("(") && next == ")") || (trimmed.hasSuffix("[") && next == "]")
        if betweenPair {
            let text = "\n" + indent + indentUnit + "\n" + indent
            insertText(text, replacementRange: sel)
            setSelectedRange(NSRange(location: sel.location + 1 + (indent + indentUnit).utf16.count, length: 0))
            return
        }
        insertText("\n" + indent + (Self.opensBlock(trimmed) ? indentUnit : ""), replacementRange: sel)
    }

    override func insertTab(_ sender: Any?) {
        if indentUnit == "\t" { super.insertTab(sender) } else { insertText(indentUnit, replacementRange: selectedRange()) }
    }

    override func insertBacktab(_ sender: Any?) {
        let lineRange = nsText.lineRange(for: NSRange(location: selectedRange().location, length: 0))
        let line = nsText.substring(with: lineRange)
        let removable = min(line.prefix { $0 == " " }.count, indentUnit == "\t" ? 0 : indentUnit.count)
        if line.hasPrefix("\t") {
            insertText("", replacementRange: NSRange(location: lineRange.location, length: 1))
        } else if removable > 0 {
            insertText("", replacementRange: NSRange(location: lineRange.location, length: removable))
        }
    }

    static func stripComment(_ line: String) -> String {
        // Good enough for indentation decisions: ignores "--" inside simple strings.
        var inString: Character?
        var previous: Character?
        for (offset, ch) in line.enumerated() {
            if let q = inString {
                if ch == q && previous != "\\" { inString = nil }
            } else if ch == "\"" || ch == "'" || ch == "`" {
                inString = ch
            } else if ch == "-" && previous == "-" {
                return String(line.prefix(offset - 1))
            }
            previous = ch
        }
        return line
    }

    static func opensBlock(_ line: String) -> Bool {
        if line.isEmpty { return false }
        for suffix in [" then", " do", "else", "repeat", "{", "(", "["] where line.hasSuffix(suffix) || line == suffix.trimmingCharacters(in: .whitespaces) {
            return true
        }
        if line.hasSuffix(")"), line.range(of: #"\bfunction\b"#, options: .regularExpression) != nil, !line.hasSuffix("end") { return true }
        return false
    }

    // Completion through the Nexus language services.
    override var rangeForUserCompletion: NSRange {
        editorController?.completionRange() ?? super.rangeForUserCompletion
    }

    override func completions(forPartialWordRange charRange: NSRange, indexOfSelectedItem index: UnsafeMutablePointer<Int>) -> [String]? {
        guard let items = editorController?.completionItems(), !items.isEmpty else { return nil }
        index.pointee = -1
        return items
    }
}

// MARK: - Line numbers

final class LineNumberRuler: NSRulerView {
    private weak var codeView: NSTextView?
    private(set) var lineStarts: [Int] = [0]
    var markers: [Int: NSColor] = [:] // line (1-based) -> marker colour

    init(textView: NSTextView, scrollView: NSScrollView) {
        codeView = textView
        super.init(scrollView: scrollView, orientation: .verticalRuler)
        clientView = textView
        ruleThickness = 46
    }

    required init(coder: NSCoder) { fatalError("init(coder:) is not supported") }

    func recomputeLines() {
        guard let tv = codeView else { return }
        let s = tv.string.utf16
        var starts = [0]
        starts.reserveCapacity(starts.count + s.count / 30)
        var i = 0
        for unit in s {
            i += 1
            if unit == 10 { starts.append(i) }
        }
        lineStarts = starts
        let digits = max(3, String(starts.count).count)
        let width = CGFloat(digits) * 8 + 20
        if abs(ruleThickness - width) > 0.5 { ruleThickness = width }
        needsDisplay = true
    }

    func lineIndex(forCharacter index: Int) -> Int {
        var lo = 0, hi = lineStarts.count - 1
        while lo < hi {
            let mid = (lo + hi + 1) / 2
            if lineStarts[mid] <= index { lo = mid } else { hi = mid - 1 }
        }
        return lo
    }

    override func drawHashMarksAndLabels(in rect: NSRect) {
        Theme.gutterBackground.setFill()
        bounds.fill()
        guard let tv = codeView, let lm = tv.layoutManager, let tc = tv.textContainer, let sv = scrollView else { return }
        let font = NSFont.monospacedDigitSystemFont(ofSize: max(9, (tv.font?.pointSize ?? 13) - 2), weight: .regular)
        let attrs: [NSAttributedString.Key: Any] = [.font: font, .foregroundColor: Theme.gutterText]
        let visible = sv.contentView.bounds
        let origin = tv.textContainerOrigin
        let currentLine = lineIndex(forCharacter: tv.selectedRange().location)

        func draw(line: Int, fragment: NSRect) {
            let y = convert(NSPoint(x: 0, y: fragment.minY + origin.y), from: tv).y
            let label = "\(line + 1)" as NSString
            let size = label.size(withAttributes: attrs)
            var a = attrs
            if line == currentLine { a[.foregroundColor] = Theme.editorText }
            label.draw(at: NSPoint(x: ruleThickness - size.width - 8, y: y + (fragment.height - size.height) / 2), withAttributes: a)
            if let color = markers[line + 1] {
                color.setFill()
                NSBezierPath(ovalIn: NSRect(x: 4, y: y + fragment.height / 2 - 3, width: 6, height: 6)).fill()
            }
        }

        if lm.numberOfGlyphs == 0 {
            draw(line: 0, fragment: lm.extraLineFragmentRect)
            return
        }
        let glyphRange = lm.glyphRange(forBoundingRect: visible, in: tc)
        let charRange = lm.characterRange(forGlyphRange: glyphRange, actualGlyphRange: nil)
        var line = lineIndex(forCharacter: charRange.location)
        let length = (tv.string as NSString).length
        while line < lineStarts.count {
            let start = lineStarts[line]
            if start > NSMaxRange(charRange) { break }
            if start >= length {
                draw(line: line, fragment: lm.extraLineFragmentRect)
                break
            }
            let glyph = lm.glyphIndexForCharacter(at: start)
            draw(line: line, fragment: lm.lineFragmentRect(forGlyphAt: glyph, effectiveRange: nil))
            line += 1
        }
    }
}

// MARK: - Minimap

final class MinimapView: NSView {
    weak var textView: NSTextView?
    private var lineLengths: [Int] = []
    override var isFlipped: Bool { true }

    func update(text: String) {
        lineLengths = text.split(separator: "\n", omittingEmptySubsequences: false).map { min($0.count, 160) }
        needsDisplay = true
    }

    private var lineHeight: CGFloat { max(0.6, min(2.5, bounds.height / CGFloat(max(lineLengths.count, 1)))) }

    override func draw(_ dirtyRect: NSRect) {
        Theme.editorBackground.setFill()
        bounds.fill()
        NSColor.separatorColor.setFill()
        NSRect(x: 0, y: 0, width: 1, height: bounds.height).fill()
        let h = lineHeight
        Theme.gutterText.withAlphaComponent(0.55).setFill()
        for (i, len) in lineLengths.enumerated() where len > 0 {
            let y = CGFloat(i) * h
            if y > dirtyRect.maxY { break }
            if y + h < dirtyRect.minY { continue }
            NSRect(x: 6, y: y, width: CGFloat(len) * 0.55, height: max(h - 0.4, 0.5)).fill()
        }
        if let tv = textView, let sv = tv.enclosingScrollView, tv.frame.height > 0 {
            let visible = sv.contentView.bounds
            let total = CGFloat(lineLengths.count) * h
            let top = visible.minY / tv.frame.height * total
            let height = max(8, visible.height / tv.frame.height * total)
            NSColor.controlAccentColor.withAlphaComponent(0.15).setFill()
            NSRect(x: 1, y: top, width: bounds.width - 1, height: height).fill()
        }
    }

    override func mouseDown(with event: NSEvent) { scroll(to: event) }
    override func mouseDragged(with event: NSEvent) { scroll(to: event) }

    private func scroll(to event: NSEvent) {
        guard let tv = textView, let sv = tv.enclosingScrollView else { return }
        let p = convert(event.locationInWindow, from: nil)
        let total = CGFloat(lineLengths.count) * lineHeight
        guard total > 0 else { return }
        let fraction = min(max(p.y / total, 0), 1)
        let y = fraction * tv.frame.height - sv.contentView.bounds.height / 2
        sv.contentView.scroll(to: NSPoint(x: sv.contentView.bounds.minX, y: max(0, min(y, tv.frame.height - sv.contentView.bounds.height))))
        sv.reflectScrolledClipView(sv.contentView)
    }
}

// MARK: - Controller

/// Owns the AppKit view hierarchy for one document and connects it to the Nexus language services.
final class CodeEditorController: NSObject, NSTextViewDelegate {
    let container = NSView()
    let scrollView = NSScrollView()
    let textView: LuauTextView
    let ruler: LineNumberRuler
    let minimap = MinimapView()
    private weak var document: EditorDocument?
    private var highlightWork: DispatchWorkItem?
    private var diagnosticsWork: DispatchWorkItem?
    private var completionWork: DispatchWorkItem?
    private var bracketRanges: [NSRange] = []
    private var diagnostics: [NXDiagnostic] = []
    private var settings: NXSettings
    private var loading = false

    private var model: AppModel { AppModel.shared }

    init(document: EditorDocument) {
        self.document = document
        settings = AppModel.shared.settings

        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        storage.addLayoutManager(layout)
        let containerSize = NSSize(width: CGFloat.greatestFiniteMagnitude, height: CGFloat.greatestFiniteMagnitude)
        let textContainer = NSTextContainer(size: containerSize)
        textContainer.widthTracksTextView = false
        layout.addTextContainer(textContainer)
        textView = LuauTextView(frame: .zero, textContainer: textContainer)
        ruler = LineNumberRuler(textView: textView, scrollView: scrollView)
        super.init()

        textView.editorController = self
        textView.delegate = self
        textView.isRichText = false
        textView.importsGraphics = false
        textView.allowsUndo = true
        textView.isAutomaticQuoteSubstitutionEnabled = false
        textView.isAutomaticDashSubstitutionEnabled = false
        textView.isAutomaticTextReplacementEnabled = false
        textView.isAutomaticSpellingCorrectionEnabled = false
        textView.isContinuousSpellCheckingEnabled = false
        textView.smartInsertDeleteEnabled = false
        textView.usesFindBar = true
        textView.isIncrementalSearchingEnabled = true
        textView.isHorizontallyResizable = true
        textView.isVerticallyResizable = true
        textView.maxSize = containerSize
        textView.minSize = NSSize(width: 0, height: 0)
        textView.autoresizingMask = [.width]
        textView.textContainerInset = NSSize(width: 6, height: 8)
        textView.backgroundColor = Theme.editorBackground
        textView.insertionPointColor = .controlAccentColor
        textView.setAccessibilityLabel("Luau source editor")

        scrollView.documentView = textView
        scrollView.hasVerticalScroller = true
        scrollView.hasHorizontalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.borderType = .noBorder
        scrollView.drawsBackground = true
        scrollView.backgroundColor = Theme.editorBackground
        scrollView.verticalRulerView = ruler
        scrollView.hasVerticalRuler = true
        scrollView.rulersVisible = true

        minimap.textView = textView
        container.addSubview(scrollView)
        container.addSubview(minimap)
        scrollView.translatesAutoresizingMaskIntoConstraints = false
        minimap.translatesAutoresizingMaskIntoConstraints = false
        minimapWidth = minimap.widthAnchor.constraint(equalToConstant: 72)
        NSLayoutConstraint.activate([
            scrollView.leadingAnchor.constraint(equalTo: container.leadingAnchor),
            scrollView.topAnchor.constraint(equalTo: container.topAnchor),
            scrollView.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            scrollView.trailingAnchor.constraint(equalTo: minimap.leadingAnchor),
            minimap.topAnchor.constraint(equalTo: container.topAnchor),
            minimap.bottomAnchor.constraint(equalTo: container.bottomAnchor),
            minimap.trailingAnchor.constraint(equalTo: container.trailingAnchor),
            minimapWidth!,
        ])

        scrollView.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(self, selector: #selector(viewScrolled), name: NSView.boundsDidChangeNotification, object: scrollView.contentView)

        apply(settings: settings)
        loading = true
        textView.string = document.initialContent
        loading = false
        textView.undoManager?.removeAllActions()
        textDidChangeImmediate()
    }

    deinit { NotificationCenter.default.removeObserver(self) }

    private var minimapWidth: NSLayoutConstraint?

    var text: String { textView.string }

    func apply(settings: NXSettings) {
        self.settings = settings
        let font = Theme.codeFont(name: settings.editorFontName, size: CGFloat(settings.editorFontSize))
        textView.font = font
        textView.typingAttributes = [.font: font, .foregroundColor: Theme.editorText]
        textView.autoClose = settings.autoCloseBrackets
        textView.autoIndentEnabled = settings.autoIndent
        textView.indentUnit = settings.useTabs ? "\t" : String(repeating: " ", count: max(1, settings.indentWidth))
        let paragraph = NSMutableParagraphStyle()
        paragraph.tabStops = []
        paragraph.defaultTabInterval = CGFloat(max(1, settings.indentWidth)) * (font.maximumAdvancement.width > 0 ? font.maximumAdvancement.width : 8)
        textView.defaultParagraphStyle = paragraph
        minimap.isHidden = !settings.showMinimap
        minimapWidth?.constant = settings.showMinimap ? 72 : 0
        applyHighlighting()
        ruler.recomputeLines()
    }

    // MARK: Text changes

    func textDidChange(_ notification: Notification) {
        guard !loading else { return }
        if let doc = document { model.documents.documentDidChange(doc) }
        textDidChangeImmediate()
        scheduleCompletionIfUseful()
    }

    private func textDidChangeImmediate() {
        ruler.recomputeLines()
        minimap.update(text: textView.string)
        highlightWork?.cancel()
        let work = DispatchWorkItem { [weak self] in self?.applyHighlighting() }
        highlightWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.04, execute: work)

        diagnosticsWork?.cancel()
        guard settings.liveDiagnostics else { return }
        let diag = DispatchWorkItem { [weak self] in
            guard let self else { return }
            self.showDiagnostics(self.model.host.syntaxDiagnostics(source: self.textView.string), runtimeErrorLine: 0)
        }
        diagnosticsWork = diag
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4, execute: diag)
    }

    func textViewDidChangeSelection(_ notification: Notification) {
        updateBracketMatch()
        ruler.needsDisplay = true
        guard let doc = document else { return }
        let location = textView.selectedRange().location
        let line = ruler.lineIndex(forCharacter: location)
        let start = ruler.lineStarts.indices.contains(line) ? ruler.lineStarts[line] : 0
        if doc.cursorLine != line + 1 { doc.cursorLine = line + 1 }
        if doc.cursorColumn != location - start + 1 { doc.cursorColumn = location - start + 1 }
    }

    @objc private func viewScrolled(_ note: Notification) {
        ruler.needsDisplay = true
        minimap.needsDisplay = true
    }

    // MARK: Highlighting and diagnostics

    func applyHighlighting() {
        guard let storage = textView.textStorage else { return }
        let source = textView.string
        let data = model.host.highlight(source: source)
        let length = (source as NSString).length
        let font = textView.font ?? NSFont.monospacedSystemFont(ofSize: 13, weight: .regular)
        storage.beginEditing()
        let full = NSRange(location: 0, length: length)
        storage.setAttributes([.font: font, .foregroundColor: Theme.editorText], range: full)
        data.withUnsafeBytes { (raw: UnsafeRawBufferPointer) in
            let tokens = raw.bindMemory(to: NXTokenRange.self)
            for t in tokens {
                let loc = Int(t.location), len = Int(t.length)
                guard loc >= 0, len > 0, loc + len <= length, let kind = NXTokenKind(rawValue: t.kind) else { continue }
                storage.addAttribute(.foregroundColor, value: Theme.tokenColor(kind), range: NSRange(location: loc, length: len))
            }
        }
        applyDiagnosticAttributes(storage: storage, length: length)
        storage.endEditing()
    }

    func showDiagnostics(_ diags: [NXDiagnostic], runtimeErrorLine: Int) {
        diagnostics = diags
        var markers: [Int: NSColor] = [:]
        for d in diags where d.line > 0 {
            let color: NSColor = d.severity == "error" ? .systemRed : d.severity == "warning" ? .systemOrange : .systemBlue
            if markers[d.line] == nil || d.severity == "error" { markers[d.line] = color }
        }
        if runtimeErrorLine > 0 { markers[runtimeErrorLine] = .systemRed }
        ruler.markers = markers
        ruler.needsDisplay = true
        document?.diagnostics = diags
        if let storage = textView.textStorage {
            storage.beginEditing()
            applyDiagnosticAttributes(storage: storage, length: storage.length)
            storage.endEditing()
        }
    }

    private func applyDiagnosticAttributes(storage: NSTextStorage, length: Int) {
        let full = NSRange(location: 0, length: length)
        storage.removeAttribute(.underlineStyle, range: full)
        storage.removeAttribute(.underlineColor, range: full)
        storage.removeAttribute(.toolTip, range: full)
        for d in diagnostics {
            var r = d.range
            guard r.location != NSNotFound, r.location < length else { continue }
            r.length = max(1, min(r.length, length - r.location))
            let color: NSColor = d.severity == "error" ? .systemRed : d.severity == "warning" ? .systemOrange : .systemBlue
            storage.addAttributes([
                .underlineStyle: NSUnderlineStyle.thick.rawValue | NSUnderlineStyle.patternDot.rawValue,
                .underlineColor: color,
                .toolTip: "\(d.severity.capitalized): \(d.message)",
            ], range: r)
        }
    }

    private func updateBracketMatch() {
        guard let lm = textView.layoutManager else { return }
        for r in bracketRanges { lm.removeTemporaryAttribute(.backgroundColor, forCharacterRange: r) }
        bracketRanges = []
        guard settings.highlightMatchingBrackets, textView.selectedRange().length == 0 else { return }
        let ns = textView.string as NSString
        let caret = textView.selectedRange().location
        let match = model.host.matchingBracket(source: textView.string, location: caret)
        guard match >= 0, match < ns.length else { return }
        var mine = caret
        if caret < ns.length, "()[]{}".contains(ns.substring(with: NSRange(location: caret, length: 1))) { mine = caret }
        else if caret > 0 { mine = caret - 1 }
        bracketRanges = [NSRange(location: mine, length: 1), NSRange(location: match, length: 1)].filter { $0.location < ns.length }
        for r in bracketRanges { lm.addTemporaryAttribute(.backgroundColor, value: Theme.bracketMatch, forCharacterRange: r) }
    }

    // MARK: Completion

    private var lastCompletion: NXCompletionResult?

    func completionRange() -> NSRange {
        let result = model.host.completions(source: textView.string, cursor: UInt(textView.selectedRange().location))
        lastCompletion = result
        return result.replaceRange
    }

    func completionItems() -> [String] {
        let result = lastCompletion ?? model.host.completions(source: textView.string, cursor: UInt(textView.selectedRange().location))
        return result.items.filter { $0.kind != "snippet" }.map { $0.label }
    }

    private func scheduleCompletionIfUseful() {
        completionWork?.cancel()
        guard settings.autocomplete, textView.selectedRange().length == 0 else { return }
        let ns = textView.string as NSString
        let caret = textView.selectedRange().location
        guard caret > 0 else { return }
        let last = ns.substring(with: NSRange(location: caret - 1, length: 1))
        var wordLength = 0
        var i = caret - 1
        while i >= 0, let scalar = ns.substring(with: NSRange(location: i, length: 1)).unicodeScalars.first,
              CharacterSet.alphanumerics.contains(scalar) || scalar == "_" {
            wordLength += 1
            i -= 1
        }
        guard last == "." || last == ":" || wordLength >= 3 else { return }
        let work = DispatchWorkItem { [weak self] in
            guard let self, self.textView.window?.firstResponder === self.textView,
                  self.textView.selectedRange().location == caret else { return }
            if !self.completionItems().isEmpty { self.textView.complete(nil) }
        }
        completionWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.35, execute: work)
    }

    // MARK: Commands

    func format() {
        do {
            let formatted = try model.host.format(source: textView.string)
            guard formatted != textView.string else { return }
            replaceAll(with: formatted)
        } catch {
            model.present(error, title: "Could not format")
        }
    }

    /// Replaces the whole buffer as a single undoable edit.
    func replaceAll(with newText: String) {
        let full = NSRange(location: 0, length: (textView.string as NSString).length)
        let selection = textView.selectedRange()
        if textView.shouldChangeText(in: full, replacementString: newText) {
            textView.textStorage?.replaceCharacters(in: full, with: newText)
            textView.didChangeText()
            let length = (newText as NSString).length
            textView.setSelectedRange(NSRange(location: min(selection.location, length), length: 0))
        }
    }

    func goTo(line: Int) {
        let index = max(0, min(line - 1, ruler.lineStarts.count - 1))
        let location = ruler.lineStarts[index]
        let ns = textView.string as NSString
        let range = ns.lineRange(for: NSRange(location: min(location, ns.length), length: 0))
        textView.window?.makeFirstResponder(textView)
        textView.setSelectedRange(NSRange(location: range.location, length: 0))
        textView.scrollRangeToVisible(range)
        if range.length > 0 { textView.showFindIndicator(for: range) }
    }

    func adjustFontSize(by delta: Double) {
        model.updateSettings { $0.editorFontSize = min(40, max(8, $0.editorFontSize + delta)) }
    }
}

// MARK: - SwiftUI wrapper

/// Hosts the AppKit editor of the selected document. Switching tabs swaps the hosted view,
/// preserving each document's text view, selection and undo stack.
struct CodeEditorView: NSViewRepresentable {
    @ObservedObject var document: EditorDocument

    func makeNSView(context: Context) -> NSView {
        let host = NSView()
        embed(document.editor.container, in: host)
        return host
    }

    func updateNSView(_ host: NSView, context: Context) {
        let editor = document.editor.container
        if host.subviews.first !== editor {
            host.subviews.forEach { $0.removeFromSuperview() }
            embed(editor, in: host)
        }
    }

    private func embed(_ view: NSView, in host: NSView) {
        view.translatesAutoresizingMaskIntoConstraints = false
        host.addSubview(view)
        NSLayoutConstraint.activate([
            view.leadingAnchor.constraint(equalTo: host.leadingAnchor),
            view.trailingAnchor.constraint(equalTo: host.trailingAnchor),
            view.topAnchor.constraint(equalTo: host.topAnchor),
            view.bottomAnchor.constraint(equalTo: host.bottomAnchor),
        ])
        DispatchQueue.main.async { view.window?.makeFirstResponder(document.editor.textView) }
    }
}

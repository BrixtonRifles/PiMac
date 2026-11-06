import AppKit
import HFSKit
import UniformTypeIdentifiers

/// The outline view that lists a disk; Return renames, Delete deletes.
final class BrowserOutlineView: NSOutlineView {
    var contextMenu: (() -> NSMenu?)?

    override func keyDown(with event: NSEvent) {
        switch event.keyCode {
        case 36, 76: NSApp.sendAction(#selector(BrowserWindowController.renameSelected(_:)), to: nil, from: self)
        case 51, 117: NSApp.sendAction(#selector(BrowserWindowController.delete(_:)), to: nil, from: self)
        default: super.keyDown(with: event)
        }
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        let row = self.row(at: convert(event.locationInWindow, from: nil))
        if row >= 0 && !selectedRowIndexes.contains(row) { selectRowIndexes([row], byExtendingSelection: false) }
        if row < 0 { deselectAll(nil) }
        return contextMenu?()
    }
}

/// Lets an item be dragged to the Finder (as a file promise) and within or
/// between disk windows (as a reference to the item).
final class ItemPromise: NSFilePromiseProvider {
    static let itemType = NSPasteboard.PasteboardType("org.pimac.floppybrowser.item")
    var node: HFSNode { (userInfo as! [String: Any])["node"] as! HFSNode }
    var docUID: String { (userInfo as! [String: Any])["doc"] as! String }

    override func writableTypes(for pasteboard: NSPasteboard) -> [NSPasteboard.PasteboardType] {
        [ItemPromise.itemType] + super.writableTypes(for: pasteboard)
    }

    override func writingOptions(forType type: NSPasteboard.PasteboardType, pasteboard: NSPasteboard) -> NSPasteboard.WritingOptions {
        type == ItemPromise.itemType ? [] : super.writingOptions(forType: type, pasteboard: pasteboard)
    }

    override func pasteboardPropertyList(forType type: NSPasteboard.PasteboardType) -> Any? {
        type == ItemPromise.itemType ? "\(docUID):\(node.cnid)" : super.pasteboardPropertyList(forType: type)
    }
}

final class BrowserWindowController: NSWindowController, NSWindowDelegate, NSOutlineViewDataSource,
    NSOutlineViewDelegate, NSTextFieldDelegate, NSFilePromiseProviderDelegate, NSToolbarDelegate,
    NSMenuItemValidation, NSToolbarItemValidation {

    private let outline = BrowserOutlineView()
    private let status = NSTextField(labelWithString: "")
    private var doc: Document? { document as? Document }
    private var volume: HFSVolume? { doc?.volume }

    private enum Column: String, CaseIterable {
        case name = "Name", size = "Size", type = "Type", creator = "Creator", modified = "Date Modified"
        var id: NSUserInterfaceItemIdentifier { NSUserInterfaceItemIdentifier(rawValue) }
        var width: CGFloat { switch self { case .name: 300; case .size: 70; case .type, .creator: 62; case .modified: 170 } }
    }

    private let dateFormatter: DateFormatter = {
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        return f
    }()

    private lazy var icons: [String: NSImage] = [
        "folder": NSWorkspace.shared.icon(for: .folder),
        "app": NSWorkspace.shared.icon(for: .applicationBundle),
        "text": NSWorkspace.shared.icon(for: .plainText),
        "doc": NSWorkspace.shared.icon(for: .data),
        "system": NSWorkspace.shared.icon(for: .systemPreferencesPane),
    ]

    init() {
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 760, height: 480),
                              styleMask: [.titled, .closable, .miniaturizable, .resizable],
                              backing: .buffered, defer: false)
        window.minSize = NSSize(width: 520, height: 260)
        super.init(window: window)
        shouldCascadeWindows = true
        window.delegate = self
        buildWindow(window)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    private func buildWindow(_ window: NSWindow) {
        for c in Column.allCases {
            let col = NSTableColumn(identifier: c.id)
            col.title = c.rawValue
            col.width = c.width
            col.minWidth = c == .name ? 140 : 50
            if c == .size { col.headerCell.alignment = .right }
            outline.addTableColumn(col)
        }
        outline.outlineTableColumn = outline.tableColumns[0]
        outline.dataSource = self
        outline.delegate = self
        outline.allowsMultipleSelection = true
        outline.usesAlternatingRowBackgroundColors = true
        outline.columnAutoresizingStyle = .firstColumnOnlyAutoresizingStyle
        outline.autoresizesOutlineColumn = false
        outline.target = self
        outline.doubleAction = #selector(doubleClicked(_:))
        outline.registerForDraggedTypes([ItemPromise.itemType, .fileURL])
        outline.setDraggingSourceOperationMask(.copy, forLocal: false)
        outline.setDraggingSourceOperationMask([.move, .copy], forLocal: true)
        outline.draggingDestinationFeedbackStyle = .regular
        outline.contextMenu = { [weak self] in self?.makeContextMenu() }

        let scroll = NSScrollView()
        scroll.documentView = outline
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = false
        scroll.autohidesScrollers = true
        scroll.borderType = .noBorder

        status.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        status.textColor = .secondaryLabelColor
        status.lineBreakMode = .byTruncatingTail
        status.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)

        let content = NSView()
        for v in [scroll, status] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            content.addSubview(v)
        }
        NSLayoutConstraint.activate([
            scroll.topAnchor.constraint(equalTo: content.topAnchor),
            scroll.leadingAnchor.constraint(equalTo: content.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: content.trailingAnchor),
            scroll.bottomAnchor.constraint(equalTo: content.bottomAnchor, constant: -26),
            status.leadingAnchor.constraint(equalTo: content.leadingAnchor, constant: 10),
            status.trailingAnchor.constraint(equalTo: content.trailingAnchor, constant: -10),
            status.centerYAnchor.constraint(equalTo: content.bottomAnchor, constant: -13),
        ])
        window.contentView = content
        window.setContentBorderThickness(26, for: .minY)

        let toolbar = NSToolbar(identifier: "FloppyBrowserToolbar")
        toolbar.delegate = self
        toolbar.displayMode = .iconAndLabel
        window.toolbar = toolbar
        window.toolbarStyle = .unified
    }

    override func windowDidLoad() { reload() }

    override var document: AnyObject? {
        didSet { reload() }
    }

    // MARK: Reloading

    func reload(select: Set<UInt32>? = nil) {
        guard let volume = volume else { return }
        var expanded = Set<UInt32>(), selected = Set<UInt32>()
        for row in 0..<outline.numberOfRows {
            guard let n = outline.item(atRow: row) as? HFSNode else { continue }
            if outline.isItemExpanded(n) { expanded.insert(n.cnid) }
            if outline.isRowSelected(row) { selected.insert(n.cnid) }
        }
        let wanted = select ?? selected
        outline.reloadData()
        func expand(_ n: HFSNode) {
            for c in n.children where c.isFolder && expanded.contains(c.cnid) { outline.expandItem(c); expand(c) }
        }
        expand(volume.root)
        // Show newly selected items inside closed folders.
        if select != nil {
            for cnid in wanted {
                var chain = [HFSNode]()
                var p = volume.node(cnid: cnid)?.parent
                while let q = p, q !== volume.root { chain.insert(q, at: 0); p = q.parent }
                for c in chain { outline.expandItem(c) }
            }
        }
        var rows = IndexSet()
        for row in 0..<outline.numberOfRows {
            if let n = outline.item(atRow: row) as? HFSNode, wanted.contains(n.cnid) { rows.insert(row) }
        }
        outline.selectRowIndexes(rows, byExtendingSelection: false)
        if let first = rows.first { outline.scrollRowToVisible(first) }
        updateStatus()
    }

    private func updateStatus() {
        guard let volume = volume, let doc = doc else { return }
        let u = volume.usage()
        let items = volume.root.itemCount
        var s = "“\(volume.displayName)”: \(items) item\(items == 1 ? "" : "s"), \(kb(u.freeBytes)) available of \(kb(u.totalBytes))"
        if let note = doc.note { s += "  —  " + note }
        status.stringValue = s
    }

    private func kb(_ bytes: Int) -> String {
        let k = (bytes + 1023) / 1024
        return k >= 10000 ? String(format: "%.1f MB", Double(bytes) / 1048576) : "\(NumberFormatter.localizedString(from: k as NSNumber, number: .decimal)) KB"
    }

    // MARK: Data source

    func outlineView(_ outlineView: NSOutlineView, numberOfChildrenOfItem item: Any?) -> Int {
        guard let volume = volume else { return 0 }
        return ((item as? HFSNode) ?? volume.root).children.count
    }

    func outlineView(_ outlineView: NSOutlineView, child index: Int, ofItem item: Any?) -> Any {
        ((item as? HFSNode) ?? volume!.root).children[index]
    }

    func outlineView(_ outlineView: NSOutlineView, isItemExpandable item: Any) -> Bool {
        (item as? HFSNode)?.isFolder == true
    }

    func outlineView(_ outlineView: NSOutlineView, viewFor tableColumn: NSTableColumn?, item: Any) -> NSView? {
        guard let node = item as? HFSNode, let column = tableColumn.flatMap({ Column(rawValue: $0.identifier.rawValue) }) else { return nil }
        let cell = (outlineView.makeView(withIdentifier: column.id, owner: self) as? NSTableCellView) ?? makeCell(column)
        let tf = cell.textField!
        tf.textColor = node.isInvisible ? .tertiaryLabelColor : .labelColor
        switch column {
        case .name:
            tf.stringValue = node.displayName
            cell.imageView?.image = icon(for: node)
            cell.imageView?.alphaValue = node.isInvisible ? 0.45 : 1
            cell.toolTip = node.isInvisible ? "Invisible on the Mac" : nil
        case .size:
            tf.stringValue = node.isFolder ? "—" : kb(node.totalSize)
            tf.textColor = .secondaryLabelColor
        case .type:
            tf.stringValue = node.isFolder ? "" : node.type
        case .creator:
            tf.stringValue = node.isFolder ? "" : node.creator
        case .modified:
            tf.stringValue = dateFormatter.string(from: MacDate.date(node.modified))
            tf.textColor = .secondaryLabelColor
        }
        return cell
    }

    private func makeCell(_ column: Column) -> NSTableCellView {
        let cell = NSTableCellView()
        cell.identifier = column.id
        let tf = NSTextField(labelWithString: "")
        tf.translatesAutoresizingMaskIntoConstraints = false
        tf.lineBreakMode = .byTruncatingTail
        tf.delegate = self
        cell.addSubview(tf)
        cell.textField = tf
        if column == .name {
            let iv = NSImageView()
            iv.translatesAutoresizingMaskIntoConstraints = false
            iv.imageScaling = .scaleProportionallyUpOrDown
            cell.addSubview(iv)
            cell.imageView = iv
            NSLayoutConstraint.activate([
                iv.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 2),
                iv.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                iv.widthAnchor.constraint(equalToConstant: 16), iv.heightAnchor.constraint(equalToConstant: 16),
                tf.leadingAnchor.constraint(equalTo: iv.trailingAnchor, constant: 5),
            ])
        } else {
            tf.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 2).isActive = true
            if column == .size { tf.alignment = .right }
            if column == .type || column == .creator { tf.font = .monospacedSystemFont(ofSize: NSFont.systemFontSize - 1, weight: .regular) }
        }
        NSLayoutConstraint.activate([
            tf.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -2),
            tf.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        return cell
    }

    private func icon(for n: HFSNode) -> NSImage? {
        if n.isFolder { return icons["folder"] }
        if n.isApplication { return icons["app"] }
        switch n.type {
        case "TEXT", "ttro": return icons["text"]
        case "zsys", "FNDR", "INIT", "cdev", "gbly", "extn", "RDEV", "appe", "PRER", "thng", "dfil", "ffil", "tfil": return icons["system"]
        default: return icons["doc"]
        }
    }

    // MARK: Selection helpers

    private var selectedNodes: [HFSNode] {
        let nodes = outline.selectedRowIndexes.compactMap { outline.item(atRow: $0) as? HFSNode }
        // Leave out items inside other selected folders.
        return nodes.filter { n in !nodes.contains { $0 !== n && $0.isAncestor(of: n) } }
    }

    /// Where new items go: into a selected folder, next to a selected file, else the top level.
    private var targetFolder: HFSNode? {
        guard let volume = volume else { return nil }
        guard outline.selectedRowIndexes.count == 1, let n = outline.item(atRow: outline.selectedRow) as? HFSNode else { return volume.root }
        return n.isFolder ? n : (n.parent ?? volume.root)
    }

    // MARK: Actions

    @objc func doubleClicked(_ sender: Any?) {
        let row = outline.clickedRow
        guard row >= 0, let node = outline.item(atRow: row) as? HFSNode else { return }
        if node.isFolder {
            if outline.isItemExpanded(node) { outline.collapseItem(node) } else { outline.expandItem(node) }
        } else {
            getInfo(nil)
        }
    }

    @objc func newFolder(_ sender: Any?) {
        guard let doc = doc, let volume = volume, let parent = targetFolder else { return }
        var name = "untitled folder", i = 2
        while parent.child(named: MacName.encode(name)) != nil { name = "untitled folder \(i)"; i += 1 }
        let folder = HFSNode(name: MacName.encode(name), isFolder: true)
        doc.change("New Folder", select: { [folder.cnid] }) {
            try volume.add(folder, to: parent)
            parent.modified = MacDate.now
        }
        if outline.selectedRowIndexes.count == 1, (outline.item(atRow: outline.selectedRow) as? HFSNode) === folder {
            renameSelected(nil)
        }
    }

    @objc func addFiles(_ sender: Any?) {
        guard let window = window, let target = targetFolder else { return }
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.prompt = "Add"
        panel.message = "Choose files or folders to copy onto “\(target === volume?.root ? volume!.displayName : target.displayName)”."
        panel.beginSheetModal(for: window) { [weak self] r in
            if r == .OK { self?.importURLs(panel.urls, into: target) }
        }
    }

    @objc func exportSelected(_ sender: Any?) {
        guard let window = window else { return }
        let nodes = selectedNodes
        guard !nodes.isEmpty else { return }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Copy Here"
        panel.message = "Choose a folder to copy the selected items to."
        panel.beginSheetModal(for: window) { [weak self] r in
            guard r == .OK, let dir = panel.url else { return }
            do {
                let urls = try nodes.map { try MacFile.write($0, into: dir) }
                NSWorkspace.shared.activateFileViewerSelecting(urls)
            } catch {
                self?.doc?.presentErrorSheet(error)
            }
        }
    }

    @IBAction func delete(_ sender: Any?) {
        guard let doc = doc, let volume = volume else { return }
        let nodes = selectedNodes
        guard !nodes.isEmpty else { return }
        doc.change(nodes.count == 1 ? "Delete “\(nodes[0].displayName)”" : "Delete \(nodes.count) Items") {
            for n in nodes { volume.remove(n) }
        }
    }

    @objc func renameSelected(_ sender: Any?) {
        guard outline.selectedRowIndexes.count == 1 else { return }
        let row = outline.selectedRow
        guard let cell = outline.view(atColumn: 0, row: row, makeIfNecessary: true) as? NSTableCellView,
              let tf = cell.textField else { return }
        tf.isEditable = true
        window?.makeFirstResponder(tf)
        tf.currentEditor()?.selectAll(nil)
    }

    func controlTextDidEndEditing(_ notification: Notification) {
        guard let tf = notification.object as? NSTextField else { return }
        tf.isEditable = false
        let row = outline.row(for: tf)
        guard row >= 0, let node = outline.item(atRow: row) as? HFSNode, let doc = doc, let volume = volume else { return }
        let newName = tf.stringValue
        if newName == node.displayName { return }
        doc.change("Rename", select: { [node.cnid] }) {
            try volume.rename(node, to: try MacName.validated(newName))
            node.parent?.modified = MacDate.now
        }
        window?.makeFirstResponder(outline)
    }

    func control(_ control: NSControl, textView: NSTextView, doCommandBy selector: Selector) -> Bool {
        guard selector == #selector(NSResponder.cancelOperation(_:)), let tf = control as? NSTextField else { return false }
        let row = outline.row(for: tf)
        if row >= 0, let node = outline.item(atRow: row) as? HFSNode { tf.stringValue = node.displayName }
        tf.abortEditing()
        tf.isEditable = false
        window?.makeFirstResponder(outline)
        return true
    }

    @objc func renameDisk(_ sender: Any?) {
        guard let doc = doc, let volume = volume, let window = window else { return }
        let alert = NSAlert()
        alert.messageText = "Rename Disk"
        alert.informativeText = "A disk name can have up to 27 characters."
        let field = NSTextField(string: volume.displayName)
        field.frame = NSRect(x: 0, y: 0, width: 260, height: 22)
        alert.accessoryView = field
        alert.addButton(withTitle: "Rename")
        alert.addButton(withTitle: "Cancel")
        alert.window.initialFirstResponder = field
        alert.beginSheetModal(for: window) { r in
            guard r == .alertFirstButtonReturn else { return }
            let name = field.stringValue
            doc.change("Rename Disk") {
                try volume.rename(volume.root, to: try MacName.validated(name, maxLength: MacName.maxVolumeName))
            }
        }
    }

    @objc func getInfo(_ sender: Any?) {
        guard let doc = doc, let volume = volume, let window = window,
              outline.selectedRowIndexes.count == 1, let node = outline.item(atRow: outline.selectedRow) as? HFSNode else { return }
        let alert = NSAlert()
        alert.messageText = node.displayName
        var info = node.isFolder
            ? "Folder, \(node.itemCount) item\(node.itemCount == 1 ? "" : "s") (\(kb(node.totalSize)))"
            : "Data fork \(NumberFormatter.localizedString(from: node.data.count as NSNumber, number: .decimal)) bytes, resource fork \(NumberFormatter.localizedString(from: node.rsrc.count as NSNumber, number: .decimal)) bytes"
        info += "\nCreated \(dateFormatter.string(from: MacDate.date(node.created)))"
        info += "\nModified \(dateFormatter.string(from: MacDate.date(node.modified)))"
        if !node.path.isEmpty { info += "\nWhere: \(volume.displayName):\(node.path)" }
        alert.informativeText = info
        alert.icon = icon(for: node)

        let name = NSTextField(string: node.displayName)
        let type = NSTextField(string: node.type)
        let creator = NSTextField(string: node.creator)
        for f in [type, creator] { f.font = .monospacedSystemFont(ofSize: NSFont.systemFontSize, weight: .regular) }
        let locked = NSButton(checkboxWithTitle: "Locked", target: nil, action: nil)
        locked.state = node.flags & 1 != 0 ? .on : .off
        let invisible = NSButton(checkboxWithTitle: "Invisible on the Mac", target: nil, action: nil)
        invisible.state = node.isInvisible ? .on : .off
        var rows: [[NSView]] = [[NSTextField(labelWithString: "Name:"), name]]
        if !node.isFolder {
            rows += [[NSTextField(labelWithString: "Type:"), type], [NSTextField(labelWithString: "Creator:"), creator],
                     [NSGridCell.emptyContentView, locked]]
        }
        rows.append([NSGridCell.emptyContentView, invisible])
        let grid = NSGridView(views: rows)
        grid.column(at: 0).xPlacement = .trailing
        grid.rowSpacing = 8
        grid.column(at: 1).width = 220
        grid.frame = NSRect(origin: .zero, size: grid.fittingSize)
        alert.accessoryView = grid
        alert.addButton(withTitle: "OK")
        alert.addButton(withTitle: "Cancel")
        alert.window.initialFirstResponder = name
        alert.beginSheetModal(for: window) { r in
            guard r == .alertFirstButtonReturn else { return }
            doc.change("Change Info", select: { [node.cnid] }) {
                if name.stringValue != node.displayName {
                    try volume.rename(node, to: try MacName.validated(name.stringValue))
                }
                var fi = node.finderInfo
                if !node.isFolder {
                    fi.replaceSubrange(0..<4, with: try MacName.fourCCBytes(type.stringValue))
                    fi.replaceSubrange(4..<8, with: try MacName.fourCCBytes(creator.stringValue))
                    node.flags = locked.state == .on ? node.flags | 1 : node.flags & ~1
                }
                let flags = UInt16(fi[8]) << 8 | UInt16(fi[9])
                let newFlags = invisible.state == .on ? flags | 0x4000 : flags & ~0x4000
                fi[8] = UInt8(newFlags >> 8); fi[9] = UInt8(newFlags & 0xFF)
                node.finderInfo = fi
            }
        }
    }

    @objc func checkDisk(_ sender: Any?) {
        guard let doc = doc, let volume = volume, let window = window else { return }
        let alert = NSAlert()
        do {
            let original = HFSCheck.check(sectors: doc.image.sectors)
            let rebuilt = HFSCheck.check(sectors: try volume.build())
            if original.isEmpty {
                alert.messageText = "“\(volume.displayName)” appears to be OK."
                alert.informativeText = rebuilt.isEmpty ? "" : "But the changes to be saved have problems:\n" + rebuilt.prefix(10).joined(separator: "\n")
            } else {
                alert.messageText = "The disk image as last saved has \(original.count) problem\(original.count == 1 ? "" : "s")."
                alert.informativeText = original.prefix(10).joined(separator: "\n")
                    + (original.count > 10 ? "\n…" : "")
                    + (rebuilt.isEmpty ? "\n\nSaving rewrites the disk without them." : "")
            }
        } catch {
            alert.messageText = error.localizedDescription
        }
        alert.beginSheetModal(for: window)
    }

    @objc func showHelp(_ sender: Any?) {
        let alert = NSAlert()
        alert.messageText = "Floppy Browser"
        alert.informativeText = """
        Opens classic Mac floppy disk images (raw .img/.dsk, Disk Copy 4.2 and Disk Copy 6) so you can copy files on and off them.

        • Drag files or folders from the Finder onto the list (or onto a folder in it) to copy them onto the disk. MacBinary (.bin) and BinHex (.hqx) files are decoded.
        • Drag items from the list to the Finder to copy them off. They keep their resource forks, type and creator.
        • Drag within the list to move items between folders, or between windows to copy them to another disk.
        • Return renames; Delete deletes; Get Info (⌘I) changes the type, creator, locked and invisible settings.

        Nothing is written to the image until you choose File > Save, and every change can be undone.
        If the Mac shows generic icons for new files, rebuild its desktop: hold ⌘⌥ while it mounts the disk.
        """
        if let w = NSApp.keyWindow { alert.beginSheetModal(for: w) } else { alert.runModal() }
    }

    func validateMenuItem(_ item: NSMenuItem) -> Bool { validate(item.action) }
    func validateToolbarItem(_ item: NSToolbarItem) -> Bool { validate(item.action) }

    private func validate(_ action: Selector?) -> Bool {
        guard volume != nil else { return false }
        let count = outline.selectedRowIndexes.count
        switch action {
        case #selector(delete(_:)), #selector(exportSelected(_:)): return count > 0
        case #selector(renameSelected(_:)), #selector(getInfo(_:)): return count == 1
        default: return true
        }
    }

    // MARK: Adding items

    private enum Conflict { case replace, skip, stop }

    /// Asks what to do about a name that's taken; remembers "for all".
    private func resolveConflict(_ name: String, _ remembered: inout Conflict?) -> Conflict {
        if let r = remembered { return r }
        let alert = NSAlert()
        alert.messageText = "An item named “\(name)” already exists in this location. Do you want to replace it?"
        alert.informativeText = "Replacing it overwrites its current contents (you can undo this)."
        alert.addButton(withTitle: "Replace")
        alert.addButton(withTitle: "Skip")
        alert.addButton(withTitle: "Stop")
        alert.showsSuppressionButton = true
        alert.suppressionButton?.title = "Apply to all"
        let r = alert.runModal()
        let choice: Conflict = r == .alertFirstButtonReturn ? .replace : r == .alertSecondButtonReturn ? .skip : .stop
        if alert.suppressionButton?.state == .on && choice != .stop { remembered = choice }
        return choice
    }

    /// Puts `node` in `folder`, asking about a clash. Returns false to stop.
    private func place(_ node: HFSNode, in folder: HFSNode, moving: Bool, _ remembered: inout Conflict?,
                       _ added: inout Set<UInt32>) throws -> Bool {
        guard let volume = volume else { return false }
        if let existing = folder.child(named: node.name), existing !== node {
            if existing.isAncestor(of: node) {
                throw DiskError.invalidMove("“\(node.displayName)” can't replace a folder that contains it.")
            }
            switch resolveConflict(existing.displayName, &remembered) {
            case .replace: volume.remove(existing)
            case .skip: return true
            case .stop: return false
            }
        }
        if moving { try volume.move(node, to: folder) } else { try volume.add(node, to: folder); volume.clearFinderPosition(node) }
        folder.modified = MacDate.now
        added.insert(node.cnid)
        return true
    }

    func importURLs(_ urls: [URL], into folder: HFSNode) {
        guard let doc = doc else { return }
        var added = Set<UInt32>()
        var skipped = [String]()
        doc.change(urls.count == 1 ? "Add “\(urls[0].lastPathComponent)”" : "Add Items", select: { added }) {
            var remembered: Conflict?
            for url in urls {
                let node: HFSNode
                do { node = try MacFile.read(url) } catch {
                    skipped.append("\(url.lastPathComponent): \(error.localizedDescription)")
                    continue
                }
                if !(try place(node, in: folder, moving: false, &remembered, &added)) { break }
            }
        }
        if !skipped.isEmpty, let window = window {
            let alert = NSAlert()
            alert.messageText = skipped.count == 1 ? "One item couldn't be added." : "\(skipped.count) items couldn't be added."
            alert.informativeText = skipped.prefix(8).joined(separator: "\n")
            alert.beginSheetModal(for: window)
        }
    }

    private func moveOrCopy(_ nodes: [HFSNode], from source: Document, to folder: HFSNode) {
        guard let doc = doc else { return }
        let moving = source === doc
        var added = Set<UInt32>()
        doc.change(moving ? "Move" : "Copy", select: { added }) {
            var remembered: Conflict?
            for n in nodes {
                if !(try place(moving ? n : n.copy(), in: folder, moving: moving, &remembered, &added)) { break }
            }
        }
    }

    // MARK: Drag and drop

    func outlineView(_ outlineView: NSOutlineView, pasteboardWriterForItem item: Any) -> NSPasteboardWriting? {
        guard let node = item as? HFSNode, let doc = doc else { return nil }
        let type = node.isFolder ? UTType.folder.identifier : UTType.data.identifier
        let promise = ItemPromise(fileType: type, delegate: self)
        promise.userInfo = ["node": node, "doc": doc.uid]
        return promise
    }

    func filePromiseProvider(_ provider: NSFilePromiseProvider, fileNameForType fileType: String) -> String {
        MacName.toHost((provider as! ItemPromise).node.name)
    }

    func filePromiseProvider(_ provider: NSFilePromiseProvider, writePromiseTo url: URL,
                             completionHandler: @escaping (Error?) -> Void) {
        do {
            try MacFile.write((provider as! ItemPromise).node, at: MacFile.uniqueURL(url))
            completionHandler(nil)
        } catch {
            completionHandler(error)
        }
    }

    func operationQueue(for filePromiseProvider: NSFilePromiseProvider) -> OperationQueue { .main }

    /// Items dragged from one of our windows.
    private func draggedItems(_ info: NSDraggingInfo) -> (Document, [HFSNode])? {
        guard let items = info.draggingPasteboard.pasteboardItems else { return nil }
        var source: Document?
        var nodes = [HFSNode]()
        for item in items {
            guard let s = item.string(forType: ItemPromise.itemType) else { continue }
            let parts = s.split(separator: ":")
            guard parts.count == 2, let cnid = UInt32(parts[1]),
                  let d = Document.document(withUID: String(parts[0])), let n = d.volume.node(cnid: cnid) else { continue }
            source = d
            nodes.append(n)
        }
        guard let s = source, !nodes.isEmpty else { return nil }
        return (s, nodes.filter { n in !nodes.contains { $0 !== n && $0.isAncestor(of: n) } })
    }

    func outlineView(_ outlineView: NSOutlineView, validateDrop info: NSDraggingInfo, proposedItem item: Any?,
                     proposedChildIndex index: Int) -> NSDragOperation {
        guard let volume = volume else { return [] }
        var target = (item as? HFSNode) ?? volume.root
        if !target.isFolder { target = target.parent ?? volume.root }
        outlineView.setDropItem(target === volume.root ? nil : target, dropChildIndex: NSOutlineViewDropOnItemIndex)
        if let (source, nodes) = draggedItems(info) {
            if source === doc {
                for n in nodes where n === target || n.isAncestor(of: target) { return [] }
                if nodes.allSatisfy({ $0.parent === target }) { return [] }
                return .move
            }
            return .copy
        }
        if info.draggingPasteboard.canReadObject(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) {
            return .copy
        }
        return []
    }

    func outlineView(_ outlineView: NSOutlineView, acceptDrop info: NSDraggingInfo, item: Any?, childIndex index: Int) -> Bool {
        guard let volume = volume else { return false }
        let target = (item as? HFSNode) ?? volume.root
        if let (source, nodes) = draggedItems(info) {
            DispatchQueue.main.async { self.moveOrCopy(nodes, from: source, to: target) }
            return true
        }
        let urls = info.draggingPasteboard.readObjects(forClasses: [NSURL.self],
                                                       options: [.urlReadingFileURLsOnly: true]) as? [URL] ?? []
        guard !urls.isEmpty else { return false }
        // Asking about name clashes during the drop would stall the drag: do it just after.
        DispatchQueue.main.async { self.importURLs(urls, into: target) }
        return true
    }

    /// Testing aid: the move a drag within the window makes.
    func testMove(_ nodes: [HFSNode], to folder: HFSNode) { moveOrCopy(nodes, from: doc!, to: folder) }

    /// Testing aid: expands the named top-level folder and selects its first item.
    func testExpand(_ name: String) {
        guard let n = volume?.root.child(named: MacName.encode(name)) else { return }
        outline.expandItem(n)
        if let first = n.children.first { outline.selectRowIndexes([outline.row(forItem: first)], byExtendingSelection: false) }
    }

    // MARK: Context menu and toolbar

    private func makeContextMenu() -> NSMenu {
        let m = NSMenu()
        let count = outline.selectedRowIndexes.count
        if count == 1 {
            m.addItem(withTitle: "Get Info", action: #selector(getInfo(_:)), keyEquivalent: "")
            m.addItem(withTitle: "Rename", action: #selector(renameSelected(_:)), keyEquivalent: "")
        }
        if count > 0 {
            m.addItem(withTitle: "Copy to Folder…", action: #selector(exportSelected(_:)), keyEquivalent: "")
            m.addItem(withTitle: "Delete", action: #selector(delete(_:)), keyEquivalent: "")
            m.addItem(.separator())
        }
        m.addItem(withTitle: "New Folder", action: #selector(newFolder(_:)), keyEquivalent: "")
        m.addItem(withTitle: "Add Files…", action: #selector(addFiles(_:)), keyEquivalent: "")
        for i in m.items { i.target = self }
        return m
    }

    private struct ToolItem { let id: String; let label: String; let symbol: String; let action: Selector; let tip: String }
    private let toolItems: [ToolItem] = [
        ToolItem(id: "newFolder", label: "New Folder", symbol: "folder.badge.plus", action: #selector(newFolder(_:)), tip: "Make a new folder on the disk"),
        ToolItem(id: "add", label: "Add Files", symbol: "plus.rectangle.on.folder", action: #selector(addFiles(_:)), tip: "Copy files from this Mac onto the disk"),
        ToolItem(id: "export", label: "Copy Out", symbol: "square.and.arrow.up", action: #selector(exportSelected(_:)), tip: "Copy the selected items to a folder on this Mac"),
        ToolItem(id: "info", label: "Get Info", symbol: "info.circle", action: #selector(getInfo(_:)), tip: "Name, type, creator and flags"),
        ToolItem(id: "delete", label: "Delete", symbol: "trash", action: #selector(delete(_:)), tip: "Delete the selected items from the disk"),
    ]

    func toolbarDefaultItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        toolItems.prefix(3).map { NSToolbarItem.Identifier($0.id) } + [.flexibleSpace] + toolItems.suffix(2).map { NSToolbarItem.Identifier($0.id) }
    }

    func toolbarAllowedItemIdentifiers(_ toolbar: NSToolbar) -> [NSToolbarItem.Identifier] {
        toolItems.map { NSToolbarItem.Identifier($0.id) } + [.flexibleSpace, .space]
    }

    func toolbar(_ toolbar: NSToolbar, itemForItemIdentifier id: NSToolbarItem.Identifier,
                 willBeInsertedIntoToolbar flag: Bool) -> NSToolbarItem? {
        guard let t = toolItems.first(where: { $0.id == id.rawValue }) else { return nil }
        let item = NSToolbarItem(itemIdentifier: id)
        item.label = t.label
        item.paletteLabel = t.label
        item.toolTip = t.tip
        item.image = NSImage(systemSymbolName: t.symbol, accessibilityDescription: t.label)
        item.action = t.action
        item.target = self
        item.isBordered = true
        return item
    }
}

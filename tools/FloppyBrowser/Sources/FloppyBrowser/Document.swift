import AppKit
import HFSKit

/// One open disk image. Changes are made in memory (with undo) and written
/// back only when the user saves.
@objc(Document)
final class Document: NSDocument {
    static let typeID = "org.pimac.disk-image"

    private(set) var image: DiskImage!
    private(set) var volume: HFSVolume!
    /// Shown in the window's status bar (e.g. "converted from Disk Copy 6").
    var note: String?
    let uid = UUID().uuidString
    private var initializedBlank = false

    override class var autosavesInPlace: Bool { false }
    override class var readableTypes: [String] { [typeID] }
    override class var writableTypes: [String] { [typeID] }
    override class func isNativeType(_ type: String) -> Bool { true }

    convenience init(blankKilobytes kb: Int) throws {
        self.init()
        image = try DiskImage.blank(kilobytes: kb, name: "Untitled")
        volume = try HFSVolume(sectors: image.sectors)
        fileType = Document.typeID
        note = "New \(kb == 800 ? "800K" : "1.44 MB") disk"
    }

    override func read(from url: URL, ofType typeName: String) throws {
        var img = try DiskImage.load(url: url)
        do {
            volume = try HFSVolume(sectors: img.sectors)
            let problems = HFSCheck.check(sectors: img.sectors)
            note = problems.isEmpty ? nil
                : "This disk has \(problems.count == 1 ? "a minor problem" : "\(problems.count) minor problems") (see Disk > Check Disk). Saving rewrites it cleanly."
        } catch let error as DiskError where HFSVolume.looksBlank(img.sectors) && [1440, 1600, 2880].contains(img.sectors.count / 512) {
            _ = error
            let blank = try HFSVolume.blank(sectors: img.sectors.count / 512, name: "Untitled")
            img.sectors = try blank.build()
            volume = try HFSVolume(sectors: img.sectors)
            initializedBlank = true
            note = "This disk was blank, so it has been initialized. Save to keep it."
        }
        if img.format == .ndif {
            note = "Converted from a Disk Copy 6 image, which can't be written: Save makes a new raw image."
        }
        image = img
    }

    override func data(ofType typeName: String) throws -> Data {
        var img = image!
        img.sectors = try volume.build()
        img.setVolumeName(volume.name)
        if img.format == .ndif { img.format = .raw }
        image = img
        note = nil
        return Data(img.fileBytes())
    }

    override func makeWindowControllers() {
        if image.format == .ndif, let url = fileURL {
            // Never overwrite the Disk Copy 6 original: saving asks for a new file.
            fileURL = nil
            displayName = url.deletingPathExtension().lastPathComponent + " (converted)"
            updateChangeCount(.changeDone)
        }
        if initializedBlank { updateChangeCount(.changeDone) }
        addWindowController(BrowserWindowController())
    }

    override func revert(toContentsOf url: URL, ofType typeName: String) throws {
        try super.revert(toContentsOf: url, ofType: typeName)
        undoManager?.removeAllActions()
        reloadViews()
    }

    override func prepareSavePanel(_ savePanel: NSSavePanel) -> Bool {
        savePanel.isExtensionHidden = false
        savePanel.allowsOtherFileTypes = true
        return true
    }

    override func fileNameExtension(forType typeName: String, saveOperation: NSDocument.SaveOperationType) -> String? {
        image?.format == .diskCopy42 ? "image" : "img"
    }

    // MARK: Changes

    /// Makes a change as one undoable step. If it fails, or the result would not
    /// fit on the disk, everything is put back and the error is shown.
    func change(_ actionName: String, select: (() -> Set<UInt32>)? = nil, _ body: () throws -> Void) {
        let before = volume.snapshot()
        let freeBefore = volume.usage().freeBytes
        do {
            try body()
            try volume.checkFits(freeBefore: freeBefore)
        } catch {
            volume.restore(before)
            reloadViews()
            if !(error is CancellationError) { presentErrorSheet(error) }
            return
        }
        registerUndo(restoring: before, name: actionName)
        reloadViews(select: select?())
    }

    private func registerUndo(restoring snapshot: HFSVolume.Snapshot, name: String) {
        let current = volume.snapshot()
        undoManager?.registerUndo(withTarget: self) { doc in
            doc.volume.restore(snapshot)
            doc.registerUndo(restoring: current, name: name)
            doc.reloadViews()
        }
        undoManager?.setActionName(name)
    }

    func reloadViews(select: Set<UInt32>? = nil) {
        for wc in windowControllers { (wc as? BrowserWindowController)?.reload(select: select) }
    }

    func presentErrorSheet(_ error: Error) {
        if let window = windowControllers.first?.window {
            let alert = NSAlert()
            alert.messageText = (error as? LocalizedError)?.errorDescription ?? error.localizedDescription
            alert.alertStyle = .warning
            alert.beginSheetModal(for: window)
        } else {
            NSApp.presentError(error)
        }
    }

    static func document(withUID uid: String) -> Document? {
        NSDocumentController.shared.documents.lazy.compactMap { $0 as? Document }.first { $0.uid == uid }
    }
}

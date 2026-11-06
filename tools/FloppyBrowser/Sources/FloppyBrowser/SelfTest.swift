import AppKit
import HFSKit

/// FLOPPYBROWSER_SELFTEST=FOLDER runs the app's own change paths (the ones drag
/// and drop and the menus use) on a new disk, saves it as FOLDER/selftest.img and
/// writes FOLDER/selftest.txt. Used to test the app without clicking.
enum SelfTest {
    static func run(_ folder: String) {
        let dir = URL(fileURLWithPath: folder)
        var log = [String]()
        func check(_ ok: Bool, _ what: String) { log.append((ok ? "ok   " : "FAIL ") + what) }
        do {
            let doc = try NSDocumentController.shared.makeUntitledDocument(ofType: Document.typeID) as! Document
            NSDocumentController.shared.addDocument(doc)
            doc.makeWindowControllers()
            doc.showWindows()
            let wc = doc.windowControllers.first as! BrowserWindowController
            let vol = doc.volume!
            // Each user action is its own undo group; here there's no event loop between steps.
            let undo = doc.undoManager!
            undo.groupsByEvent = false

            // Add host files and a folder (as a drop from the Finder does).
            let src = dir.appendingPathComponent("selftest-src")
            try? FileManager.default.removeItem(at: src)
            try FileManager.default.createDirectory(at: src.appendingPathComponent("Folder/Inner"), withIntermediateDirectories: true)
            try Data("hello\r".utf8).write(to: src.appendingPathComponent("Hello.txt"))
            try Data("inner".utf8).write(to: src.appendingPathComponent("Folder/Inner/deep.txt"))
            try Data([1, 2, 3]).write(to: src.appendingPathComponent("Folder/data.bin"))
            undo.beginUndoGrouping()
            wc.importURLs([src.appendingPathComponent("Hello.txt"), src.appendingPathComponent("Folder")], into: vol.root)
            undo.endUndoGrouping()
            check(vol.root.children.map(\.displayName) == ["Folder", "Hello.txt"], "import: \(vol.root.children.map(\.displayName))")
            check(vol.root.child(named: Array("Hello.txt".utf8))?.type == "TEXT", "text file gets type TEXT")

            // Move (as a drag within the window does), then undo and redo it.
            let hello = vol.root.child(named: Array("Hello.txt".utf8))!
            let folder = vol.root.child(named: Array("Folder".utf8))!
            undo.beginUndoGrouping()
            wc.testMove([hello], to: folder)
            undo.endUndoGrouping()
            check(folder.child(named: hello.name) != nil && vol.root.child(named: hello.name) == nil, "move into folder")
            doc.undoManager?.undo()
            check(doc.volume.root.child(named: Array("Hello.txt".utf8)) != nil && doc.volume.root.child(named: Array("Folder".utf8)) != nil, "undo move")
            doc.undoManager?.redo()
            check(doc.volume.root.child(named: Array("Hello.txt".utf8)) == nil, "redo move")

            // A file that does not fit is refused and nothing changes.
            let big = src.appendingPathComponent("big.dat")
            try Data(count: 1_600_000).write(to: big)
            let before = doc.volume.root.itemCount
            undo.beginUndoGrouping()
            wc.importURLs([big], into: doc.volume.root)
            undo.endUndoGrouping()
            check(doc.volume.root.itemCount == before, "too-large file refused")
            for w in NSApp.windows where w.attachedSheet != nil { w.endSheet(w.attachedSheet!) }

            // Save, read back and check.
            let out = dir.appendingPathComponent("selftest.img")
            try? FileManager.default.removeItem(at: out)
            try doc.write(to: out, ofType: Document.typeID)
            let image = try DiskImage.load(url: out)
            check(HFSCheck.check(sectors: image.sectors).isEmpty, "saved disk checks clean")
            let back = try HFSVolume(sectors: image.sectors)
            check(back.root.child(named: Array("Folder".utf8))?.child(named: Array("Hello.txt".utf8))?.data == Array("hello\r".utf8), "saved contents")
            check(back.root.child(named: Array("Folder".utf8))?.child(named: Array("Inner".utf8))?.children.count == 1, "nested folder")

            // Copy out (as a drag to the Finder does).
            let exportDir = dir.appendingPathComponent("selftest-out")
            try? FileManager.default.removeItem(at: exportDir)
            try FileManager.default.createDirectory(at: exportDir, withIntermediateDirectories: true)
            try MacFile.write(back.root.child(named: Array("Folder".utf8))!, into: exportDir)
            check(FileManager.default.fileExists(atPath: exportDir.appendingPathComponent("Folder/Inner/deep.txt").path), "export folder")
            doc.updateChangeCount(.changeCleared)
            doc.close()
        } catch {
            log.append("FAIL error: \(error)")
        }
        try? (log.joined(separator: "\n") + "\n").write(to: dir.appendingPathComponent("selftest.txt"), atomically: true, encoding: .utf8)
        NSApp.terminate(nil)
    }
}

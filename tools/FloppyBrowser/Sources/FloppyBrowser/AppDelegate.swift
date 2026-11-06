import AppKit
import HFSKit

final class DocumentController: NSDocumentController {
    // Any file can be tried: disk images come with many extensions (.img, .image, .dsk...)
    // and the contents decide what they are.
    override func typeForContents(of url: URL) throws -> String { Document.typeID }
    override func documentClass(forType typeName: String) -> AnyClass? { Document.self }

    override func beginOpenPanel(_ openPanel: NSOpenPanel, forTypes types: [String]?,
                                 completionHandler: @escaping (Int) -> Void) {
        openPanel.message = "Choose a classic Mac floppy disk image (raw, Disk Copy 4.2 or Disk Copy 6)."
        super.beginOpenPanel(openPanel, forTypes: nil, completionHandler: completionHandler)
    }

    override func runModalOpenPanel(_ openPanel: NSOpenPanel, forTypes types: [String]?) -> Int {
        super.runModalOpenPanel(openPanel, forTypes: nil)
    }

    // File > New (Cmd-N) makes a blank 1.44 MB disk.
    override func makeUntitledDocument(ofType typeName: String) throws -> NSDocument {
        try Document(blankKilobytes: 1440)
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillFinishLaunching(_ notification: Notification) {
        NSApp.mainMenu = makeMainMenu()
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        if let dir = ProcessInfo.processInfo.environment["FLOPPYBROWSER_SELFTEST"] {
            DispatchQueue.main.async { SelfTest.run(dir) }
            return
        }
        // Testing aid: FLOPPYBROWSER_SNAPSHOT=out.png draws the front window to a PNG and quits.
        if let path = ProcessInfo.processInfo.environment["FLOPPYBROWSER_SNAPSHOT"] {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                if let w = NSApp.orderedWindows.first, let frame = w.contentView?.superview {
                    if let row = ProcessInfo.processInfo.environment["FLOPPYBROWSER_EXPAND"],
                       let wc = w.windowController as? BrowserWindowController { wc.testExpand(row) }
                    frame.layoutSubtreeIfNeeded()
                    if let rep = frame.bitmapImageRepForCachingDisplay(in: frame.bounds) {
                        frame.cacheDisplay(in: frame.bounds, to: rep)
                        try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
                    }
                }
                NSApp.terminate(nil)
            }
            return
        }
        // Opened on its own (not by opening a disk image): offer the Open panel.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
            if NSDocumentController.shared.documents.isEmpty {
                NSDocumentController.shared.openDocument(nil)
            }
        }
    }

    func applicationShouldOpenUntitledFile(_ sender: NSApplication) -> Bool { false }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows: Bool) -> Bool {
        if !hasVisibleWindows { NSDocumentController.shared.openDocument(nil) }
        return false
    }

    @objc func newDisk800(_ sender: Any?) {
        do {
            let doc = try Document(blankKilobytes: 800)
            NSDocumentController.shared.addDocument(doc)
            doc.makeWindowControllers()
            doc.showWindows()
        } catch {
            NSApp.presentError(error)
        }
    }

    private func makeMainMenu() -> NSMenu {
        let main = NSMenu()
        func submenu(_ title: String) -> NSMenu {
            let item = NSMenuItem(title: title, action: nil, keyEquivalent: "")
            let menu = NSMenu(title: title)
            item.submenu = menu
            main.addItem(item)
            return menu
        }
        @discardableResult
        func add(_ menu: NSMenu, _ title: String, _ action: Selector?, _ key: String = "",
                 _ mods: NSEvent.ModifierFlags = .command, target: AnyObject? = nil) -> NSMenuItem {
            let item = NSMenuItem(title: title, action: action, keyEquivalent: key)
            item.keyEquivalentModifierMask = mods
            item.target = target
            menu.addItem(item)
            return item
        }

        let appMenu = submenu("Floppy Browser")
        add(appMenu, "About Floppy Browser", #selector(NSApplication.orderFrontStandardAboutPanel(_:)))
        appMenu.addItem(.separator())
        add(appMenu, "Hide Floppy Browser", #selector(NSApplication.hide(_:)), "h")
        add(appMenu, "Hide Others", #selector(NSApplication.hideOtherApplications(_:)), "h", [.command, .option])
        add(appMenu, "Show All", #selector(NSApplication.unhideAllApplications(_:)))
        appMenu.addItem(.separator())
        add(appMenu, "Quit Floppy Browser", #selector(NSApplication.terminate(_:)), "q")

        let file = submenu("File")
        add(file, "New 1.44 MB Disk", #selector(NSDocumentController.newDocument(_:)), "n")
        add(file, "New 800K Disk", #selector(newDisk800(_:)), "n", [.command, .option], target: self)
        add(file, "Open…", #selector(NSDocumentController.openDocument(_:)), "o")
        let recent = NSMenuItem(title: "Open Recent", action: nil, keyEquivalent: "")
        let recentMenu = NSMenu(title: "Open Recent")
        add(recentMenu, "Clear Menu", #selector(NSDocumentController.clearRecentDocuments(_:)))
        recent.submenu = recentMenu
        file.addItem(recent)
        file.addItem(.separator())
        add(file, "Close", #selector(NSWindow.performClose(_:)), "w")
        add(file, "Save", #selector(NSDocument.save(_:)), "s")
        add(file, "Save As…", #selector(NSDocument.saveAs(_:)), "s", [.command, .shift])
        add(file, "Revert to Saved", #selector(NSDocument.revertToSaved(_:)))

        let edit = submenu("Edit")
        add(edit, "Undo", Selector(("undo:")), "z")
        add(edit, "Redo", Selector(("redo:")), "z", [.command, .shift])
        edit.addItem(.separator())
        add(edit, "Select All", #selector(NSResponder.selectAll(_:)), "a")
        add(edit, "Delete", #selector(BrowserWindowController.delete(_:)), "\u{8}")

        let disk = submenu("Disk")
        add(disk, "New Folder", #selector(BrowserWindowController.newFolder(_:)), "n", [.command, .shift])
        add(disk, "Add Files…", #selector(BrowserWindowController.addFiles(_:)), "a", [.command, .shift])
        add(disk, "Copy to Folder…", #selector(BrowserWindowController.exportSelected(_:)), "e", [.command, .shift])
        disk.addItem(.separator())
        add(disk, "Get Info", #selector(BrowserWindowController.getInfo(_:)), "i")
        add(disk, "Rename", #selector(BrowserWindowController.renameSelected(_:)))
        add(disk, "Rename Disk…", #selector(BrowserWindowController.renameDisk(_:)))
        disk.addItem(.separator())
        add(disk, "Check Disk", #selector(BrowserWindowController.checkDisk(_:)))

        let window = submenu("Window")
        add(window, "Minimize", #selector(NSWindow.performMiniaturize(_:)), "m")
        add(window, "Zoom", #selector(NSWindow.performZoom(_:)))
        window.addItem(.separator())
        add(window, "Bring All to Front", #selector(NSApplication.arrangeInFront(_:)))
        NSApp.windowsMenu = window

        let help = submenu("Help")
        add(help, "Floppy Browser Help", #selector(BrowserWindowController.showHelp(_:)), "?")
        NSApp.helpMenu = help
        return main
    }
}

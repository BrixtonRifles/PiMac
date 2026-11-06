// swift-tools-version:5.10
import PackageDescription

let package = Package(
    name: "FloppyBrowser",
    platforms: [.macOS(.v13)],
    targets: [
        // HFS volumes, disk image containers and Mac file conversion (no AppKit).
        .target(name: "HFSKit"),
        // The GUI app (assembled into "Floppy Browser.app" by build-app.sh).
        .executableTarget(name: "FloppyBrowser", dependencies: ["HFSKit"]),
        // Command-line tool: list, extract, add, check and rebuild images.
        .executableTarget(name: "hfstool", dependencies: ["HFSKit"]),
        .testTarget(name: "HFSKitTests", dependencies: ["HFSKit"]),
    ]
)

import Foundation
import HFSKit

let usage = """
usage: hfstool COMMAND ...
  ls IMAGE                      list everything on the disk
  check IMAGE                   check the disk's structure (like Disk First Aid)
  extract IMAGE PATH FOLDER     copy an item (Mac path, e.g. "System Folder:Finder") to FOLDER
  add IMAGE OUT FILE... [--to PATH]   copy macOS files/folders onto the disk, write OUT
  mkdir IMAGE OUT PATH          make a folder, write OUT
  rm IMAGE OUT PATH             delete an item, write OUT
  rebuild IMAGE OUT             read the disk and write it out again
  new 800|1440 NAME OUT         make a blank disk image
OUT must not exist yet. IMAGE is never changed.
"""

func fail(_ s: String) -> Never {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
    exit(1)
}

func open(_ path: String) throws -> (DiskImage, HFSVolume) {
    let image = try DiskImage.load(url: URL(fileURLWithPath: path))
    return (image, try HFSVolume(sectors: image.sectors))
}

func find(_ vol: HFSVolume, _ path: String) -> HFSNode? {
    var n = vol.root
    for part in path.split(separator: ":") where !part.isEmpty {
        guard let c = n.child(named: MacName.encode(String(part))) else { return nil }
        n = c
    }
    return n
}

func save(_ image: DiskImage, _ vol: HFSVolume, _ out: String) throws {
    if FileManager.default.fileExists(atPath: out) { fail("\(out) already exists") }
    var image = image
    image.sectors = try vol.build()
    image.setVolumeName(vol.name)
    if image.format == .ndif { image.format = .raw }
    try Data(image.fileBytes()).write(to: URL(fileURLWithPath: out), options: .withoutOverwriting)
    let problems = HFSCheck.check(sectors: image.sectors)
    if !problems.isEmpty { fail("wrote \(out), but the check found problems:\n" + problems.prefix(30).joined(separator: "\n")) }
    print("wrote \(out) (\(vol.usage().freeBytes / 1024) KB free)")
}

func list(_ n: HFSNode, _ depth: Int) {
    for c in n.children {
        let pad = String(repeating: "  ", count: depth)
        if c.isFolder {
            print("\(pad)\(c.displayName)/")
            list(c, depth + 1)
        } else {
            let codes = "\(c.type.padding(toLength: 4, withPad: " ", startingAt: 0)) \(c.creator.padding(toLength: 4, withPad: " ", startingAt: 0))"
            print("\(pad)\(c.displayName)  [\(codes)]  data \(c.data.count)  rsrc \(c.rsrc.count)\(c.isInvisible ? "  (invisible)" : "")")
        }
    }
}

let args = Array(CommandLine.arguments.dropFirst())
guard let cmd = args.first else { fail(usage) }
do {
    switch (cmd, args.count) {
    case ("ls", 2):
        let (image, vol) = try open(args[1])
        let u = vol.usage()
        print("\"\(vol.displayName)\" — \(image.format.rawValue), \(u.totalBytes / 1024) KB, \(u.freeBytes / 1024) KB free")
        list(vol.root, 1)
    case ("check", 2):
        let image = try DiskImage.load(url: URL(fileURLWithPath: args[1]))
        let problems = HFSCheck.check(sectors: image.sectors)
        if problems.isEmpty { print("\(args[1]): OK") } else {
            print("\(args[1]): \(problems.count) problem(s)")
            for p in problems.prefix(40) { print("  " + p) }
            exit(1)
        }
    case ("extract", 4):
        let (_, vol) = try open(args[1])
        guard let n = find(vol, args[2]) else { fail("no item \(args[2])") }
        print(try MacFile.write(n, into: URL(fileURLWithPath: args[3])).path)
    case ("add", 4...):
        let (image, vol) = try open(args[1])
        var files = Array(args[3...])
        var dest = vol.root
        if let t = files.firstIndex(of: "--to"), t + 1 < files.count {
            guard let d = find(vol, files[t + 1]), d.isFolder else { fail("no folder \(files[t + 1])") }
            dest = d
            files.removeSubrange(t...t + 1)
        }
        for f in files { try vol.add(try MacFile.read(URL(fileURLWithPath: f)), to: dest) }
        try vol.checkFits()
        try save(image, vol, args[2])
    case ("mkdir", 4):
        let (image, vol) = try open(args[1])
        let parts = args[3].split(separator: ":").map(String.init)
        guard let parent = find(vol, parts.dropLast().joined(separator: ":")), let last = parts.last else { fail("bad path") }
        try vol.add(HFSNode(name: try MacName.validated(last), isFolder: true), to: parent)
        try save(image, vol, args[2])
    case ("rm", 4):
        let (image, vol) = try open(args[1])
        guard let n = find(vol, args[3]), n !== vol.root else { fail("no item \(args[3])") }
        vol.remove(n)
        try save(image, vol, args[2])
    case ("rebuild", 3):
        let (image, vol) = try open(args[1])
        try save(image, vol, args[2])
    case ("new", 4):
        guard let kb = Int(args[1]), kb == 800 || kb == 1440 else { fail("size must be 800 or 1440") }
        let image = try DiskImage.blank(kilobytes: kb, name: args[2])
        try save(image, try HFSVolume(sectors: image.sectors), args[3])
    default:
        fail(usage)
    }
} catch {
    fail("error: \(error.localizedDescription)")
}

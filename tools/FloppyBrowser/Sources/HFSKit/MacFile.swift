import Foundation

/// Copying between macOS files (which keep resource forks and type/creator
/// codes as extended attributes) and items on an HFS disk.
public enum MacFile {
    static let finderInfoAttr = "com.apple.FinderInfo"
    static let resourceForkAttr = "com.apple.ResourceFork"

    // MARK: macOS -> disk

    /// Reads a macOS file or folder (recursively) as a new, unattached item.
    /// MacBinary (.bin) and BinHex (.hqx) files are decoded.
    public static func read(_ url: URL) throws -> HFSNode {
        let values = try url.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey, .creationDateKey, .contentModificationDateKey])
        let created = values.creationDate.map(MacDate.from) ?? MacDate.now
        let modified = values.contentModificationDate.map(MacDate.from) ?? created
        if values.isDirectory == true && !isPackage(url) {
            let folder = HFSNode(name: MacName.fromHost(url.lastPathComponent), isFolder: true)
            folder.created = created; folder.modified = modified
            let items = try FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: nil)
            for item in items.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) where !item.lastPathComponent.hasPrefix(".") {
                let child = try read(item)
                if folder.child(named: child.name) == nil { folder.insertSorted(child) }
            }
            return folder
        }
        if values.isDirectory == true {
            throw DiskError.unsupported("“\(url.lastPathComponent)” is a macOS app or package, which a classic Mac can't use.")
        }
        let data = [UInt8](try Data(contentsOf: url))
        let ext = url.pathExtension.lowercased()
        if ["bin", "macbin", "mbin"].contains(ext), let n = decodeMacBinary(data) { return n }
        if ext == "hqx" || data.starts(with: Array("(This file must be converted with BinHex".utf8)),
           let n = decodeBinHex(data) { return n }

        let node = HFSNode(name: MacName.fromHost(url.lastPathComponent), isFolder: false)
        node.data = data
        node.rsrc = xattr(url, resourceForkAttr) ?? []
        node.created = created; node.modified = modified
        if let fi = xattr(url, finderInfoAttr), fi.count >= 32, !fi[0..<8].allSatisfy({ $0 == 0 }) {
            node.finderInfo = Array(fi[0..<16])
            // Keep type, creator and the Finder flags that mean something on another disk.
            node.finderInfo.put16(8, node.finderInfo.u16(8) & (0x4000 | 0x0400 | 0x2000 | 0x1000 | 0x0800 | 0x0020))
            for i in 10..<16 { node.finderInfo[i] = 0 }
        } else {
            let (type, creator) = guessCodes(ext)
            node.finderInfo.replaceSubrange(0..<4, with: Array(type.utf8))
            node.finderInfo.replaceSubrange(4..<8, with: Array(creator.utf8))
        }
        return node
    }

    static func isPackage(_ url: URL) -> Bool {
        (try? url.resourceValues(forKeys: [.isPackageKey]).isPackage) == true
    }

    /// Type and creator for a file from a modern Mac that has none.
    public static func guessCodes(_ ext: String) -> (String, String) {
        switch ext {
        case "txt", "text", "readme", "md", "c", "h", "s", "a", "p", "pas": return ("TEXT", "ttxt")
        case "sit": return ("SIT!", "SIT!")
        case "sitx": return ("SITX", "SIT!")
        case "cpt": return ("PACT", "CPCT")
        case "hqx": return ("TEXT", "BnHq")
        case "zip": return ("ZIP ", "SITx")
        case "pict", "pct", "pic": return ("PICT", "ttxt")
        case "gif": return ("GIFf", "GKON")
        case "jpg", "jpeg": return ("JPEG", "GKON")
        case "mov": return ("MooV", "TVOD")
        case "aif", "aiff": return ("AIFF", "SCPL")
        case "image", "img", "dsk", "dc42": return ("dImg", "dCpy")
        case "rtf": return ("TEXT", "MSWD")
        case "html", "htm": return ("TEXT", "MOSS")
        default: return ("????", "????")
        }
    }

    // MARK: disk -> macOS

    /// Writes an item (recursively) into `folder` as a native macOS file, with
    /// its resource fork, type and creator. Returns the URL written.
    @discardableResult
    public static func write(_ node: HFSNode, into folder: URL, name: String? = nil) throws -> URL {
        let url = uniqueURL(folder.appendingPathComponent(name ?? MacName.toHost(node.name)))
        try write(node, at: url)
        return url
    }

    /// Writes an item at exactly `url` (which must not exist).
    public static func write(_ node: HFSNode, at url: URL) throws {
        let fm = FileManager.default
        if node.isFolder {
            try fm.createDirectory(at: url, withIntermediateDirectories: false)
            for c in node.children { try write(c, into: url) }
        } else {
            try Data(node.data).write(to: url, options: .withoutOverwriting)
            if !node.rsrc.isEmpty { try setXattr(url, resourceForkAttr, node.rsrc) }
            var fi = [UInt8](repeating: 0, count: 32)
            fi.replaceSubrange(0..<8, with: node.finderInfo[0..<8])
            // Keep "has custom icon", "has bundle" and similar; drop invisible, inited and position.
            fi.put16(8, node.finderFlags & (0x0400 | 0x2000 | 0x1000 | 0x0800 | 0x0020))
            if !fi[0..<10].allSatisfy({ $0 == 0 }) { try setXattr(url, finderInfoAttr, fi) }
        }
        try? fm.setAttributes([.creationDate: MacDate.date(node.created),
                               .modificationDate: MacDate.date(node.modified)], ofItemAtPath: url.path)
    }

    /// `url`, or "name 2", "name 3"... if it is taken.
    public static func uniqueURL(_ url: URL) -> URL {
        let fm = FileManager.default
        if !fm.fileExists(atPath: url.path) { return url }
        let dir = url.deletingLastPathComponent(), ext = url.pathExtension
        let stem = ext.isEmpty ? url.lastPathComponent : url.deletingPathExtension().lastPathComponent
        for i in 2... {
            let name = ext.isEmpty ? "\(stem) \(i)" : "\(stem) \(i).\(ext)"
            let u = dir.appendingPathComponent(name)
            if !fm.fileExists(atPath: u.path) { return u }
        }
        return url
    }

    // MARK: Extended attributes

    static func xattr(_ url: URL, _ name: String) -> [UInt8]? {
        url.withUnsafeFileSystemRepresentation { path -> [UInt8]? in
            guard let path = path else { return nil }
            let size = getxattr(path, name, nil, 0, 0, 0)
            guard size > 0 else { return nil }
            var buf = [UInt8](repeating: 0, count: size)
            let got = getxattr(path, name, &buf, size, 0, 0)
            return got > 0 ? Array(buf.prefix(got)) : nil
        }
    }

    static func setXattr(_ url: URL, _ name: String, _ value: [UInt8]) throws {
        let r = url.withUnsafeFileSystemRepresentation { path -> Int32 in
            guard let path = path else { return -1 }
            return setxattr(path, name, value, value.count, 0, 0)
        }
        if r != 0 {
            throw DiskError.unsupported("Couldn't save the Mac information (\(name)) for “\(url.lastPathComponent)”: \(String(cString: strerror(errno))).")
        }
    }

    // MARK: MacBinary

    public static func decodeMacBinary(_ d: [UInt8]) -> HFSNode? {
        guard d.count >= 128, d[0] == 0, d[74] == 0, d[82] == 0, (1...63).contains(d[1]) else { return nil }
        let dataLen = Int(d.u32(83)), rsrcLen = Int(d.u32(87))
        guard dataLen <= 0x7FFFFF, rsrcLen <= 0x7FFFFF else { return nil }
        let crc = d.u16(124)
        if crc != 0 && crc != crc16(d[0..<124]) { return nil }          // MacBinary II/III with a bad CRC
        if crc == 0 && !d[101..<126].allSatisfy({ $0 == 0 }) { return nil } // not MacBinary I either
        let rsrcAt = 128 + (dataLen + 127) / 128 * 128
        guard d.count >= rsrcAt + rsrcLen else { return nil }
        let node = HFSNode(name: MacName.shortened(d.bytes(2, Int(d[1])).map { $0 == 0x3A ? 0x2D : $0 }, maxLength: 31), isFolder: false)
        node.finderInfo.replaceSubrange(0..<8, with: d[65..<73])
        node.finderInfo.put16(8, (UInt16(d[73]) << 8 | UInt16(d[101])) & ~0x0100)
        node.data = Array(d[128..<128 + dataLen])
        node.rsrc = Array(d[rsrcAt..<rsrcAt + rsrcLen])
        if d.u32(91) != 0 { node.created = d.u32(91) }
        if d.u32(95) != 0 { node.modified = d.u32(95) }
        return node
    }

    // MARK: BinHex 4.0

    public static func decodeBinHex(_ d: [UInt8]) -> HFSNode? {
        let alphabet = Array("!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr".utf8)
        var value = [Int](repeating: -1, count: 256)
        for (i, c) in alphabet.enumerated() { value[Int(c)] = i }
        // The data runs from a ':' at the start of a line to the next ':'.
        var start: Int?
        for i in d.indices where d[i] == 0x3A && (i == 0 || d[i - 1] == 0x0A || d[i - 1] == 0x0D) { start = i + 1; break }
        guard let s = start else { return nil }
        var bits = 0, acc = 0
        var packed = [UInt8]()
        var i = s
        while i < d.count && d[i] != 0x3A {
            let c = d[i]; i += 1
            if c == 0x0A || c == 0x0D || c == 0x20 || c == 0x09 { continue }
            let v = value[Int(c)]
            guard v >= 0 else { return nil }
            acc = (acc << 6) | v; bits += 6
            if bits >= 8 { bits -= 8; packed.append(UInt8((acc >> bits) & 0xFF)) }
        }
        guard i < d.count else { return nil }
        // Run-length decoding: 0x90 n repeats the previous byte (n-1 more times); 0x90 0 is 0x90.
        var out = [UInt8]()
        out.reserveCapacity(packed.count * 2)
        var j = 0
        while j < packed.count {
            let b = packed[j]; j += 1
            if b == 0x90 && j < packed.count {
                let n = Int(packed[j]); j += 1
                if n == 0 { out.append(0x90) } else if let last = out.last { out += [UInt8](repeating: last, count: n - 1) }
            } else { out.append(b) }
        }
        guard out.count > 1 else { return nil }
        let nameLen = Int(out[0])
        let h = 1 + nameLen + 1 // header fields follow the name and a version byte
        guard nameLen >= 1, nameLen <= 63, out.count >= h + 20 else { return nil }
        guard crc16(out[0..<h + 18]) == out.u16(h + 18) else { return nil }
        let dataLen = Int(out.u32(h + 10)), rsrcLen = Int(out.u32(h + 14))
        let dataAt = h + 20, rsrcAt = dataAt + dataLen + 2
        guard out.count >= rsrcAt + rsrcLen + 2 else { return nil }
        guard crc16(out[dataAt..<dataAt + dataLen]) == out.u16(dataAt + dataLen),
              crc16(out[rsrcAt..<rsrcAt + rsrcLen]) == out.u16(rsrcAt + rsrcLen) else { return nil }
        let node = HFSNode(name: MacName.shortened(out.bytes(1, nameLen).map { $0 == 0x3A ? 0x2D : $0 }, maxLength: 31), isFolder: false)
        node.finderInfo.replaceSubrange(0..<8, with: out[h..<h + 8])
        node.finderInfo.put16(8, out.u16(h + 8) & ~0x0100)
        node.data = Array(out[dataAt..<dataAt + dataLen])
        node.rsrc = Array(out[rsrcAt..<rsrcAt + rsrcLen])
        return node
    }
}

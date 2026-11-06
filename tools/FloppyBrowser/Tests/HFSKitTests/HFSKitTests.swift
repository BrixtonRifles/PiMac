import XCTest
@testable import HFSKit

final class HFSKitTests: XCTestCase {
    static let floppies = FileManager.default.homeDirectoryForCurrentUser
        .appendingPathComponent("Documents/PiMac files/floppies")

    func assertSameTree(_ a: HFSNode, _ b: HFSNode, file: StaticString = #filePath, line: UInt = #line) {
        XCTAssertEqual(a.name, b.name, file: file, line: line)
        XCTAssertEqual(a.cnid, b.cnid, "\(a.path)", file: file, line: line)
        XCTAssertEqual(a.isFolder, b.isFolder, file: file, line: line)
        XCTAssertEqual(a.data, b.data, "\(a.path) data", file: file, line: line)
        XCTAssertEqual(a.rsrc, b.rsrc, "\(a.path) rsrc", file: file, line: line)
        XCTAssertEqual(a.finderInfo, b.finderInfo, "\(a.path)", file: file, line: line)
        XCTAssertEqual(a.extFinderInfo, b.extFinderInfo, file: file, line: line)
        XCTAssertEqual(a.created, b.created, file: file, line: line)
        XCTAssertEqual(a.modified, b.modified, file: file, line: line)
        XCTAssertEqual(a.flags & 0xFD, b.flags & 0xFD, file: file, line: line)
        XCTAssertEqual(a.children.count, b.children.count, "\(a.path) children", file: file, line: line)
        for (x, y) in zip(a.children, b.children) { assertSameTree(x, y, file: file, line: line) }
    }

    /// Every real floppy image: rebuilt without changes, it must check clean and hold the same items.
    func testRoundTripRealImages() throws {
        guard let files = try? FileManager.default.contentsOfDirectory(at: Self.floppies, includingPropertiesForKeys: nil) else {
            throw XCTSkip("no floppies folder")
        }
        var tested = 0
        for url in files where ["img", "image", "dsk"].contains(url.pathExtension) {
            guard let image = try? DiskImage.load(url: url), let vol = try? HFSVolume(sectors: image.sectors) else { continue }
            let built = try vol.build()
            XCTAssertEqual(built.count, image.sectors.count)
            XCTAssertEqual(HFSCheck.check(sectors: built), [], url.lastPathComponent)
            let again = try HFSVolume(sectors: built)
            assertSameTree(vol.root, again.root)
            XCTAssertEqual(Array(built[0..<1024]), Array(image.sectors[0..<1024]), "boot blocks")
            // Volume Finder info (blessed folder etc.) survives.
            XCTAssertEqual(Array(built[1024 + 92..<1024 + 124]), Array(image.sectors[1024 + 92..<1024 + 124]))
            tested += 1
        }
        XCTAssertGreaterThan(tested, 0)
    }

    func testBlankDiskAndManyFiles() throws {
        for kb in [800, 1440] {
            var image = try DiskImage.blank(kilobytes: kb, name: "Test Disk")
            XCTAssertEqual(image.sectors.count, kb * 1024)
            XCTAssertEqual(HFSCheck.check(sectors: image.sectors), [])
            let vol = try HFSVolume(sectors: image.sectors)
            XCTAssertEqual(vol.displayName, "Test Disk")
            XCTAssertTrue(vol.root.children.isEmpty)
            // Enough items to need two index levels (over 11 * ~4 leaves... use 700).
            let folder = HFSNode(name: Array("Lots".utf8), isFolder: true)
            try vol.add(folder, to: vol.root)
            for i in 0..<700 {
                let f = HFSNode(name: Array(String(format: "File %03d with a long name", i).utf8), isFolder: false)
                f.data = [UInt8(i & 0xFF)]
                try vol.add(f, to: i % 2 == 0 ? folder : vol.root)
            }
            image.sectors = try vol.build()
            XCTAssertEqual(HFSCheck.check(sectors: image.sectors), [])
            let again = try HFSVolume(sectors: image.sectors)
            assertSameTree(vol.root, again.root)
            let tree = try BTree(again.catalogFileForTests(), "catalog")
            XCTAssertGreaterThanOrEqual(tree.depth, 3, "should exercise index levels")
        }
    }

    func testNameOrderAndCaseInsensitivity() throws {
        let vol = try HFSVolume.blank(sectors: 1600, name: "Names")
        let names = ["zebra", "Apple", "apple pie", "Ärger", "Ábc", "abc", "_under", "1999", "™ Mark", "ƒolder"]
        for n in names {
            let f = HFSNode(name: MacName.encode(n), isFolder: false)
            try vol.add(f, to: vol.root)
        }
        XCTAssertThrowsError(try vol.add(HFSNode(name: MacName.encode("ZEBRA"), isFolder: false), to: vol.root))
        let built = try vol.build()
        XCTAssertEqual(HFSCheck.check(sectors: built), [])
        let again = try HFSVolume(sectors: built)
        XCTAssertEqual(Set(again.root.children.map { $0.displayName }), Set(names))
        for i in 1..<again.root.children.count {
            XCTAssertLessThan(MacName.compare(again.root.children[i - 1].name, again.root.children[i].name), 0)
        }
    }

    func testRenameMoveRemoveAndUndoSnapshot() throws {
        let vol = try HFSVolume.blank(sectors: 2880, name: "Edits")
        let a = HFSNode(name: Array("A".utf8), isFolder: true)
        let b = HFSNode(name: Array("B".utf8), isFolder: true)
        let f = HFSNode(name: Array("file".utf8), isFolder: false)
        f.data = Array(repeating: 7, count: 5000); f.rsrc = Array(repeating: 9, count: 700)
        try vol.add(a, to: vol.root); try vol.add(b, to: a); try vol.add(f, to: vol.root)
        let snap = vol.snapshot()
        XCTAssertThrowsError(try vol.move(a, to: b)) // into its own subfolder
        try vol.move(f, to: b)
        try vol.rename(f, to: Array("renamed".utf8))
        try vol.rename(vol.root, to: Array("New Name".utf8))
        let built = try vol.build()
        XCTAssertEqual(HFSCheck.check(sectors: built), [])
        let again = try HFSVolume(sectors: built)
        XCTAssertEqual(again.displayName, "New Name")
        XCTAssertEqual(again.root.children.first?.children.first?.children.first?.displayName, "renamed")
        XCTAssertEqual(again.root.children.first?.children.first?.children.first?.rsrc.count, 700)
        vol.restore(snap)
        XCTAssertEqual(vol.root.children.map { $0.displayName }, ["A", "file"])
        vol.remove(vol.root.children[0])
        let built2 = try vol.build()
        XCTAssertEqual(HFSCheck.check(sectors: built2), [])
        XCTAssertEqual(try HFSVolume(sectors: built2).root.children.map { $0.displayName }, ["file"])
    }

    func testDiskFull() throws {
        let vol = try HFSVolume.blank(sectors: 1600, name: "Full")
        let f = HFSNode(name: Array("big".utf8), isFolder: false)
        f.data = [UInt8](repeating: 1, count: 790 * 1024)
        try vol.add(f, to: vol.root)
        XCTAssertThrowsError(try vol.checkFits())
        f.data = [UInt8](repeating: 1, count: 700 * 1024)
        XCTAssertNoThrow(try vol.checkFits())
        XCTAssertEqual(HFSCheck.check(sectors: try vol.build()), [])
    }

    func testDiskCopy42Checksums() throws {
        var image = try DiskImage.blank(kilobytes: 800, name: "DC")
        image.format = .diskCopy42
        image.dc42DiskFormat = 1; image.dc42FormatByte = 0x22
        image.setVolumeName(Array("DC".utf8))
        let bytes = image.fileBytes()
        XCTAssertEqual(bytes.count, 84 + 819200)
        let back = try DiskImage.load(bytes: bytes)
        XCTAssertEqual(back.format, .diskCopy42)
        XCTAssertEqual(bytes.u32(0x48), DiskImage.dc42Checksum(bytes[84...]))
        XCTAssertEqual(back.sectors, image.sectors)
    }

    func testMacBinaryAndBinHex() throws {
        let name = Array("Hello File".utf8)
        let data: [UInt8] = Array("data fork \u{90}\u{90}\u{90} aaaaaaaaaaaaaaaaaaaaa".utf8)
        let rsrc: [UInt8] = Array(repeating: 0x90, count: 40) + [1, 2, 3]

        // MacBinary II
        var mb = [UInt8](repeating: 0, count: 128)
        mb[1] = UInt8(name.count); mb.replaceSubrange(2..<2 + name.count, with: name)
        mb.replaceSubrange(65..<73, with: Array("TEXTttxt".utf8))
        mb.put32(83, UInt32(data.count)); mb.put32(87, UInt32(rsrc.count))
        mb[122] = 129; mb[123] = 129
        mb.put16(124, crc16(mb[0..<124]))
        let pad = { (n: Int) in [UInt8](repeating: 0, count: (128 - n % 128) % 128) }
        let file = mb + data + pad(data.count) + rsrc + pad(rsrc.count)
        let n = try XCTUnwrap(MacFile.decodeMacBinary(file))
        XCTAssertEqual(n.name, name); XCTAssertEqual(n.data, data); XCTAssertEqual(n.rsrc, rsrc)
        XCTAssertEqual(n.type, "TEXT"); XCTAssertEqual(n.creator, "ttxt")

        // BinHex 4.0, built by a small encoder here.
        var raw: [UInt8] = [UInt8(name.count)] + name + [0] + Array("TEXTttxt".utf8) + [0, 0]
        var lens = [UInt8](repeating: 0, count: 8); lens.put32(0, UInt32(data.count)); lens.put32(4, UInt32(rsrc.count))
        raw += lens
        func crcBytes(_ b: [UInt8]) -> [UInt8] { let c = crc16(b[...]); return [UInt8(c >> 8), UInt8(c & 0xFF)] }
        raw += crcBytes(raw)
        raw += data + crcBytes(data) + rsrc + crcBytes(rsrc)
        var rle = [UInt8]()
        var i = 0
        while i < raw.count {
            let b = raw[i]
            var run = 1
            while i + run < raw.count && raw[i + run] == b && run < 255 { run += 1 }
            if b == 0x90 { rle += [0x90, 0]; if run > 1 { rle += [0x90, UInt8(run)] } }
            else if run > 2 { rle += [b, 0x90, UInt8(run)] }
            else { rle += [UInt8](repeating: b, count: run) }
            i += run
        }
        let alphabet = Array("!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr".utf8)
        XCTAssertEqual(alphabet.count, 64)
        var text = Array("(This file must be converted with BinHex 4.0)\r\r:".utf8)
        var acc = 0, bits = 0, col = 1
        for b in rle {
            acc = (acc << 8) | Int(b); bits += 8
            while bits >= 6 { bits -= 6; text.append(alphabet[(acc >> bits) & 63]); col += 1; if col == 64 { text.append(0x0D); col = 0 } }
        }
        if bits > 0 { text.append(alphabet[(acc << (6 - bits)) & 63]) }
        text.append(0x3A)
        let h = try XCTUnwrap(MacFile.decodeBinHex(text))
        XCTAssertEqual(h.name, name); XCTAssertEqual(h.data, data); XCTAssertEqual(h.rsrc, rsrc)
    }

    /// A Disk Copy 6 image made here: raw, ADC (all three codes) and zero chunks, map in the resource fork.
    func testDiskCopy6() throws {
        let vol = try HFSVolume.blank(sectors: 1600, name: "NDIF Test")
        let f = HFSNode(name: Array("pattern".utf8), isFolder: false)
        f.data = (0..<3000).map { UInt8(($0 / 7) & 0xFF) } + [UInt8](repeating: 0xAB, count: 900)
        try vol.add(f, to: vol.root)
        let sectors = try vol.build()
        for s in 100..<1598 { XCTAssertTrue(sectors[s * 512..<(s + 1) * 512].allSatisfy { $0 == 0 }) }

        func adc(_ src: ArraySlice<UInt8>) -> [UInt8] {
            var out = [UInt8](), i = src.startIndex
            while i < src.endIndex {
                var run = 0
                if i > src.startIndex { while i + run < src.endIndex && src[i + run] == src[i - 1] { run += 1 } }
                if run >= 4 { let n = min(run, 67); out += [0x40 | UInt8(n - 4), 0, 0]; i += n }
                else if run == 3 { out += [0x00, 0]; i += 3 }   // short match, length 3, distance 1
                else { let n = min(128, src.endIndex - i); out.append(0x80 | UInt8(n - 1)); out += src[i..<i + n]; i += n }
            }
            return out
        }
        var data = [UInt8]()
        var entries = [(UInt32, UInt32, UInt32)]()
        func chunk(_ first: Int, _ end: Int, _ kind: UInt32) {
            let offset = UInt32(data.count)
            switch kind {
            case 2: data += sectors[first * 512..<end * 512]
            case 0x83: data += adc(sectors[first * 512..<end * 512])
            default: break
            }
            entries.append((UInt32(first) << 8 | kind, offset, UInt32(data.count) - offset))
        }
        chunk(0, 4, 2); chunk(4, 100, 0x83); chunk(100, 1598, 0); chunk(1598, 1600, 2)
        entries.append((UInt32(1600) << 8 | 0xFF, UInt32(data.count), 0))
        var bcem = [UInt8](repeating: 0, count: 0x80)
        bcem.put32(0x44, 1600); bcem.put32(0x7C, UInt32(entries.count))
        for e in entries { var b = [UInt8](repeating: 0, count: 12); b.put32(0, e.0); b.put32(4, e.1); b.put32(8, e.2); bcem += b }
        // Resource fork: header, data (length + bcem), map with one type and one reference.
        let dataOff = 256
        var res = [UInt8](repeating: 0, count: dataOff)
        var len = [UInt8](repeating: 0, count: 4); len.put32(0, UInt32(bcem.count))
        res += len + bcem
        let mapOff = res.count
        var map = [UInt8](repeating: 0, count: 28 + 2 + 8 + 12)
        map.put16(24, 28); map.put16(26, UInt16(map.count))
        map.put16(28, 0); map.replaceSubrange(30..<34, with: Array("bcem".utf8)); map.put16(34, 0); map.put16(36, 10)
        map.put16(38, 128); map.put16(40, 0xFFFF); map.put32(42, 0)
        res += map
        res.put32(0, UInt32(dataOff)); res.put32(4, UInt32(mapOff)); res.put32(8, UInt32(4 + bcem.count)); res.put32(12, UInt32(map.count))

        let url = FileManager.default.temporaryDirectory.appendingPathComponent("ndif-\(UUID().uuidString).img")
        defer { try? FileManager.default.removeItem(at: url) }
        try Data(data).write(to: url)
        try MacFile.setXattr(url, MacFile.resourceForkAttr, res)
        let image = try DiskImage.load(url: url)
        XCTAssertEqual(image.format, .ndif)
        XCTAssertEqual(image.sectors, sectors)
        XCTAssertEqual(try HFSVolume(sectors: image.sectors).root.children.first?.data, f.data)
        XCTAssertEqual(image.fileBytes(), sectors) // saved as a raw image
    }

    func testHostRoundTrip() throws {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let f = HFSNode(name: MacName.encode("Résumé/1999™"), isFolder: false)
        f.data = Array("hello".utf8); f.rsrc = [1, 2, 3, 4]
        f.finderInfo.replaceSubrange(0..<8, with: Array("TEXTttxt".utf8))
        let url = try MacFile.write(f, into: dir)
        XCTAssertEqual(url.lastPathComponent, "Résumé:1999™")
        let back = try MacFile.read(url)
        XCTAssertEqual(back.name, f.name)
        XCTAssertEqual(back.data, f.data)
        XCTAssertEqual(back.rsrc, f.rsrc)
        XCTAssertEqual(back.type, "TEXT")
        XCTAssertEqual(back.creator, "ttxt")
        XCTAssertEqual(abs(Int(back.modified) - Int(f.modified)), 0)
    }
}

extension HFSVolume {
    func catalogFileForTests() throws -> [UInt8] {
        let built = try build()
        let m = built.bytes(1024, 512)
        let start = Int(m.u16(150)), count = Int(m.u16(152))
        let at = firstBlockSector * 512 + start * blockSize
        return Array(built[at..<at + count * blockSize])
    }
}

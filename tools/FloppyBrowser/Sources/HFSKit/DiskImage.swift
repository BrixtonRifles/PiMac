import Foundation

/// The file a disk lives in: a raw sector dump, a Disk Copy 4.2 image, or a
/// Disk Copy 6 (NDIF) image, which is read only (it is saved as a raw image).
public struct DiskImage {
    public enum Format: String {
        case raw = "Raw disk image"
        case diskCopy42 = "Disk Copy 4.2 image"
        case ndif = "Disk Copy 6 image"
    }

    public var format: Format
    /// The disk's sectors (512 bytes each).
    public var sectors: [UInt8]
    // Disk Copy 4.2 header fields kept on save.
    var dc42Name: [UInt8] = []
    var dc42Tags: [UInt8] = []
    var dc42DiskFormat: UInt8 = 0
    var dc42FormatByte: UInt8 = 0

    public init(format: Format, sectors: [UInt8]) {
        self.format = format
        self.sectors = sectors
    }

    /// A new, empty disk: `kilobytes` is 800 or 1440.
    public static func blank(kilobytes: Int, name: String) throws -> DiskImage {
        let volume = try HFSVolume.blank(sectors: kilobytes * 2, name: name)
        return DiskImage(format: .raw, sectors: try volume.build())
    }

    public static func load(url: URL) throws -> DiskImage {
        let data = try Data(contentsOf: url)
        // Disk Copy 4.2 first: Disk Copy 6 also adds a 'bcem' resource to 4.2 images.
        if let image = diskCopy42([UInt8](data)) { return image }
        if let ndif = try NDIF.decode(url: url, data: data) {
            return DiskImage(format: .ndif, sectors: ndif)
        }
        return try load(bytes: [UInt8](data))
    }

    public static func load(bytes d: [UInt8]) throws -> DiskImage {
        if let image = diskCopy42(d) { return image }
        if d.count >= 1536 && d.count % 512 == 0 { return DiskImage(format: .raw, sectors: d) }
        if d.count >= 1536 && d.count % 512 == 84 {
            throw DiskError.corrupt("it looks like a Disk Copy 4.2 image, but its header does not match its size.")
        }
        throw DiskError.unsupported("This file is not a disk image this app can open (it should be a raw disk image or a Disk Copy 4.2 or 6 image).")
    }

    static func diskCopy42(_ d: [UInt8]) -> DiskImage? {
        guard d.count >= 84 + 1024, d[0] <= 63, d.u16(0x52) == 0x0100 else { return nil }
        let dataSize = Int(d.u32(0x40)), tagSize = Int(d.u32(0x44))
        guard dataSize % 512 == 0, dataSize >= 1024, 84 + dataSize + tagSize == d.count else { return nil }
        var image = DiskImage(format: .diskCopy42, sectors: Array(d[84..<84 + dataSize]))
        image.dc42Name = d.bytes(1, Int(d[0]))
        image.dc42Tags = Array(d[(84 + dataSize)...])
        image.dc42DiskFormat = d[0x50]
        image.dc42FormatByte = d[0x51]
        return image
    }

    /// The disk-image checksum Disk Copy 4.2 uses: add each big-endian word, rotate right 1.
    static func dc42Checksum(_ d: ArraySlice<UInt8>) -> UInt32 {
        var sum: UInt32 = 0
        var i = d.startIndex
        while i + 1 < d.endIndex {
            sum = sum &+ (UInt32(d[i]) << 8 | UInt32(d[i + 1]))
            sum = (sum >> 1) | (sum << 31)
            i += 2
        }
        return sum
    }

    /// The bytes to write back to a file. NDIF images become raw images.
    public func fileBytes() -> [UInt8] {
        guard format == .diskCopy42 else { return sectors }
        var h = [UInt8](repeating: 0, count: 84)
        let name = Array(dc42Name.prefix(63))
        h[0] = UInt8(name.count)
        for (i, b) in name.enumerated() { h[1 + i] = b }
        h.put32(0x40, UInt32(sectors.count))
        h.put32(0x44, UInt32(dc42Tags.count))
        h.put32(0x48, DiskImage.dc42Checksum(sectors[...]))
        // The tag checksum leaves out the first sector's 12 tag bytes.
        h.put32(0x4C, dc42Tags.count > 12 ? DiskImage.dc42Checksum(dc42Tags[12...]) : 0)
        h[0x50] = dc42DiskFormat
        h[0x51] = dc42FormatByte
        h.put16(0x52, 0x0100)
        return h + sectors + dc42Tags
    }

    /// Disk Copy 4.2 keeps the disk's name in its header: keep it in step with the volume.
    public mutating func setVolumeName(_ name: [UInt8]) {
        if format == .diskCopy42 { dc42Name = name }
    }
}

/// Disk Copy 6 images: the block map lives in the resource fork ('bcem' 128).
enum NDIF {
    static func decode(url: URL, data: Data) throws -> [UInt8]? {
        let rsrcURL = url.appendingPathComponent("..namedfork/rsrc")
        guard let fork = try? [UInt8](Data(contentsOf: rsrcURL)), fork.count >= 16,
              let bcem = ResourceFork.find(fork, type: "bcem", id: 128) else { return nil }
        let src = [UInt8](data)
        try bcem.need(0, 0x80, "the Disk Copy 6 block map is too short.")
        let sectorCount = Int(bcem.u32(0x44))
        let count = Int(bcem.u32(0x7C))
        try bcem.need(0x80, count * 12, "the Disk Copy 6 block map is too short.")
        guard sectorCount > 0, sectorCount <= 4_194_304 else { throw DiskError.corrupt("bad sector count in the Disk Copy 6 header.") }
        var out = [UInt8](repeating: 0, count: sectorCount * 512)
        for k in 0..<count {
            let e = 0x80 + k * 12
            let start = bcem.u32(e), kind = start & 0xFF, sector = Int(start >> 8)
            if kind == 0xFF { break }
            guard k + 1 < count else { throw DiskError.corrupt("the Disk Copy 6 block map has no end.") }
            let next = Int(bcem.u32(e + 12) >> 8)
            let size = (next - sector) * 512
            guard size >= 0, sector * 512 + size <= out.count else { throw DiskError.corrupt("a Disk Copy 6 chunk lies outside the disk.") }
            let offset = Int(bcem.u32(e + 4)), length = Int(bcem.u32(e + 8))
            switch kind {
            case 0, 1: continue // zeros
            case 2:
                try src.need(offset, size, "a Disk Copy 6 chunk is missing.")
                out.replaceSubrange(sector * 512..<sector * 512 + size, with: src[offset..<offset + size])
            case 0x83:
                try src.need(offset, length, "a Disk Copy 6 chunk is missing.")
                let chunk = try adc(src[offset..<offset + length], size)
                out.replaceSubrange(sector * 512..<sector * 512 + size, with: chunk)
            default:
                throw DiskError.unsupported(String(format: "This Disk Copy 6 image uses a compression method (type %02X) this app can't read.", kind))
            }
        }
        return out
    }

    /// Apple Data Compression.
    static func adc(_ src: ArraySlice<UInt8>, _ outLen: Int) throws -> [UInt8] {
        var out = [UInt8]()
        out.reserveCapacity(outLen)
        var i = src.startIndex
        let bad = DiskError.corrupt("compressed data in the Disk Copy 6 image is damaged.")
        while i < src.endIndex && out.count < outLen {
            let b = src[i]; i += 1
            if b & 0x80 != 0 {
                let n = Int(b & 0x7F) + 1
                guard i + n <= src.endIndex else { throw bad }
                out += src[i..<i + n]; i += n
                continue
            }
            var n: Int, dist: Int
            if b & 0x40 != 0 {
                guard i + 2 <= src.endIndex else { throw bad }
                n = Int(b & 0x3F) + 4; dist = (Int(src[i]) << 8 | Int(src[i + 1])) + 1; i += 2
            } else {
                guard i < src.endIndex else { throw bad }
                n = Int((b & 0x3C) >> 2) + 3; dist = (Int(b & 3) << 8 | Int(src[i])) + 1; i += 1
            }
            guard dist <= out.count else { throw bad }
            for _ in 0..<n { out.append(out[out.count - dist]) }
        }
        guard out.count == outLen else { throw bad }
        return out
    }
}

/// Just enough of the resource fork format to find one resource.
enum ResourceFork {
    static func find(_ fork: [UInt8], type: String, id: Int16) -> [UInt8]? {
        guard fork.count >= 16 else { return nil }
        let dataOff = Int(fork.u32(0)), mapOff = Int(fork.u32(4)), mapLen = Int(fork.u32(12))
        guard mapOff + mapLen <= fork.count, mapLen >= 30 else { return nil }
        let map = Array(fork[mapOff..<mapOff + mapLen])
        let typeList = Int(map.u16(24))
        guard typeList + 2 <= map.count else { return nil }
        let types = Int(map.u16(typeList)) &+ 1
        let want = Array(type.utf8)
        for t in 0..<types {
            let at = typeList + 2 + t * 8
            guard at + 8 <= map.count else { return nil }
            guard map.bytes(at, 4) == want else { continue }
            let n = Int(map.u16(at + 4)) + 1, refs = typeList + Int(map.u16(at + 6))
            for j in 0..<n {
                let r = refs + j * 12
                guard r + 12 <= map.count else { return nil }
                guard Int16(bitPattern: map.u16(r)) == id else { continue }
                let at = dataOff + Int(map.u32(r + 4) & 0xFFFFFF)
                guard at + 4 <= fork.count else { return nil }
                let len = Int(fork.u32(at))
                guard at + 4 + len <= fork.count else { return nil }
                return Array(fork[at + 4..<at + 4 + len])
            }
        }
        return nil
    }
}

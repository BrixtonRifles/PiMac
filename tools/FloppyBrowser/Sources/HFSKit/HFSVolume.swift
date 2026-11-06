import Foundation

/// A file or folder on an HFS disk.
public final class HFSNode {
    public internal(set) var name: [UInt8]
    public internal(set) var cnid: UInt32
    public let isFolder: Bool
    public internal(set) weak var parent: HFSNode?
    /// Kept in the order the Mac's catalog sorts names.
    public internal(set) var children: [HFSNode] = []
    public var data: [UInt8] = []
    public var rsrc: [UInt8] = []
    /// FInfo (type, creator, Finder flags, location) or, for a folder, DInfo.
    public var finderInfo = [UInt8](repeating: 0, count: 16)
    /// FXInfo or DXInfo.
    public var extFinderInfo = [UInt8](repeating: 0, count: 16)
    /// filFlags for a file (bit 0 = locked), dirFlags for a folder.
    public var flags: UInt16 = 0
    public var created: UInt32
    public var modified: UInt32
    public var backedUp: UInt32 = 0

    public init(name: [UInt8], isFolder: Bool, cnid: UInt32 = 0) {
        self.name = name
        self.isFolder = isFolder
        self.cnid = cnid
        let now = MacDate.now
        created = now
        modified = now
    }

    public var displayName: String { MacName.string(name) }
    public var type: String { MacName.fourCC(finderInfo[0..<4]) }
    public var creator: String { MacName.fourCC(finderInfo[4..<8]) }
    public var finderFlags: UInt16 { finderInfo.u16(8) }
    public var isInvisible: Bool { finderFlags & 0x4000 != 0 }
    public var isApplication: Bool { !isFolder && finderInfo[0..<4].elementsEqual(Array("APPL".utf8)) }

    /// Data + resource fork bytes; for a folder, everything inside it.
    public var totalSize: Int {
        isFolder ? children.reduce(0) { $0 + $1.totalSize } : data.count + rsrc.count
    }
    /// Items inside, counting the contents of folders.
    public var itemCount: Int { children.reduce(0) { $0 + 1 + $1.itemCount } }

    public func child(named n: [UInt8]) -> HFSNode? { children.first { MacName.same($0.name, n) } }

    public func isAncestor(of node: HFSNode) -> Bool {
        var p = node.parent
        while let q = p { if q === self { return true }; p = q.parent }
        return false
    }

    /// The path from the disk's top level, e.g. "System Folder:Finder".
    public var path: String {
        var parts = [String]()
        var n: HFSNode? = self
        while let m = n, m.parent != nil { parts.insert(m.displayName, at: 0); n = m.parent }
        return parts.joined(separator: ":")
    }

    /// A copy of this item and everything inside it (CNIDs kept).
    public func copy() -> HFSNode {
        let c = HFSNode(name: name, isFolder: isFolder, cnid: cnid)
        c.data = data; c.rsrc = rsrc
        c.finderInfo = finderInfo; c.extFinderInfo = extFinderInfo
        c.flags = flags; c.created = created; c.modified = modified; c.backedUp = backedUp
        c.children = children.map { $0.copy() }
        for k in c.children { k.parent = c }
        return c
    }

    func insertSorted(_ node: HFSNode) {
        node.parent = self
        let i = children.firstIndex { MacName.compare($0.name, node.name) > 0 } ?? children.count
        children.insert(node, at: i)
    }

    func walk(_ body: (HFSNode) -> Void) {
        body(self)
        for c in children { c.walk(body) }
    }
}

/// An HFS volume held in memory. Changes are made to the tree of nodes;
/// `build()` writes a complete new volume (catalog, bitmap, file data) that
/// keeps the original's boot blocks, geometry, item IDs and Finder info.
public final class HFSVolume {
    public private(set) var root: HFSNode
    var nextCNID: UInt32
    let image: [UInt8]          // the original sectors (boot blocks are kept)
    let mdb: [UInt8]            // the original master directory block
    public let blockSize: Int   // allocation block size
    let firstBlockSector: Int   // drAlBlSt
    let bitmapSector: Int       // drVBMSt
    public let blockCount: Int  // drNmAlBlks
    let extentsBytes: Int       // size to give the extents B-tree
    let catalogBytes: Int       // the catalog's original size (it can grow)
    let catalogClump: Int

    public var name: [UInt8] { root.name }
    public var displayName: String { root.displayName }

    static let nodeSize = 512

    // MARK: Reading

    public init(sectors d: [UInt8]) throws {
        guard d.count >= 1536 else { throw DiskError.unsupported("This file is too small to be a disk image.") }
        let m = 1024
        let sig = d.u16(m)
        guard sig == 0x4244 else {
            if sig == 0xD2D7 {
                throw DiskError.unsupported("This is a 400K disk in the original Macintosh File System (MFS), which this app can't open.")
            }
            if d.u16(0) == 0x4552 {
                throw DiskError.unsupported("This is a hard disk image (it has a partition map). This app opens floppy disk images.")
            }
            if sig == 0x482B {
                throw DiskError.unsupported("This disk uses Mac OS Extended (HFS+), which this app can't open.")
            }
            if HFSVolume.looksBlank(d) {
                throw DiskError.unsupported("This disk has never been initialized (it is blank).")
            }
            throw DiskError.unsupported("This disk image does not hold a Mac (HFS) disk.")
        }
        if d.u16(m + 124) == 0x482B {
            throw DiskError.unsupported("This disk uses Mac OS Extended (HFS+), which this app can't open.")
        }
        mdb = d.bytes(m, 512)
        image = d
        blockCount = Int(mdb.u16(18))
        blockSize = Int(mdb.u32(20))
        firstBlockSector = Int(mdb.u16(28))
        bitmapSector = Int(mdb.u16(14))
        nextCNID = mdb.u32(30)
        guard blockSize >= 512, blockSize % 512 == 0, blockCount > 0,
              bitmapSector >= 3, firstBlockSector > bitmapSector,
              firstBlockSector * 512 + blockCount * blockSize <= d.count else {
            throw DiskError.corrupt("its volume information (the master directory block) does not make sense.")
        }
        extentsBytes = Int(mdb.u32(130))
        catalogBytes = Int(mdb.u32(146))
        catalogClump = Int(mdb.u32(78))

        // The extents overflow file: extra pieces of fragmented files.
        var overflow = [ExtentKey: [(Int, Int)]]()
        let xtFile = try HFSVolume.readFork(d, firstBlockSector: firstBlockSector, blockSize: blockSize, blockCount: blockCount,
                                            extents: mdb.bytes(134, 12), overflow: [:], fileID: 3, fork: 0,
                                            length: extentsBytes, what: "the extents file")
        for r in try BTree(xtFile, "extents").leafRecords() {
            let rec = Array(r)
            guard rec.count >= 20, rec[0] == 7 else { throw DiskError.corrupt("an extents record has the wrong size.") }
            overflow[ExtentKey(fork: rec[1], fileID: rec.u32(2), start: Int(rec.u16(6)))] = HFSVolume.extents(rec.bytes(8, 12))
        }

        let catalog = try HFSVolume.readFork(d, firstBlockSector: firstBlockSector, blockSize: blockSize, blockCount: blockCount,
                                             extents: mdb.bytes(150, 12), overflow: overflow, fileID: 4, fork: 0,
                                             length: catalogBytes, what: "the catalog")
        var folders = [UInt32: (node: HFSNode, parent: UInt32)]()
        var files = [(node: HFSNode, parent: UInt32)]()
        for r in try BTree(catalog, "catalog").leafRecords() {
            let rec = Array(r)
            try rec.need(0, 7, "a catalog record is too short.")
            let keyLen = Int(rec[0])
            let parent = rec.u32(2)
            let nameLen = Int(rec[6])
            guard keyLen >= 6, nameLen <= 31, 7 + nameLen <= keyLen + 1 else { throw DiskError.corrupt("a catalog key is malformed.") }
            let name = rec.bytes(7, nameLen)
            let v = (keyLen + 2) & ~1
            try rec.need(v, 2, "a catalog record is too short.")
            switch rec[v] {
            case 1: // folder
                try rec.need(v, 70, "a folder record is too short.")
                let node = HFSNode(name: name, isFolder: true, cnid: rec.u32(v + 6))
                node.flags = rec.u16(v + 2)
                node.created = rec.u32(v + 10); node.modified = rec.u32(v + 14); node.backedUp = rec.u32(v + 18)
                node.finderInfo = rec.bytes(v + 22, 16); node.extFinderInfo = rec.bytes(v + 38, 16)
                folders[node.cnid] = (node, parent)
            case 2: // file
                try rec.need(v, 102, "a file record is too short.")
                let node = HFSNode(name: name, isFolder: false, cnid: rec.u32(v + 20))
                node.flags = UInt16(rec[v + 2])
                node.finderInfo = rec.bytes(v + 4, 16)
                node.created = rec.u32(v + 44); node.modified = rec.u32(v + 48); node.backedUp = rec.u32(v + 52)
                node.extFinderInfo = rec.bytes(v + 56, 16)
                let what = "the file “\(MacName.string(name))”"
                node.data = try HFSVolume.readFork(d, firstBlockSector: firstBlockSector, blockSize: blockSize, blockCount: blockCount,
                                                   extents: rec.bytes(v + 74, 12), overflow: overflow, fileID: node.cnid, fork: 0,
                                                   length: Int(rec.u32(v + 26)), what: what)
                node.rsrc = try HFSVolume.readFork(d, firstBlockSector: firstBlockSector, blockSize: blockSize, blockCount: blockCount,
                                                   extents: rec.bytes(v + 86, 12), overflow: overflow, fileID: node.cnid, fork: 0xFF,
                                                   length: Int(rec.u32(v + 36)), what: what)
                files.append((node, parent))
            case 3, 4: break // threads: rebuilt from the tree
            default: throw DiskError.corrupt("the catalog has a record of unknown type \(rec[v]).")
            }
        }
        guard let top = folders[2] else { throw DiskError.corrupt("the disk's top-level folder is missing.") }
        root = top.node
        for (cnid, f) in folders where cnid != 2 {
            folders[f.parent]?.node.children.append(f.node)
            f.node.parent = folders[f.parent]?.node
        }
        for f in files {
            folders[f.parent]?.node.children.append(f.node)
            f.node.parent = folders[f.parent]?.node
        }
        root.walk { n in n.children.sort { MacName.compare($0.name, $1.name) < 0 } }
        var maxID: UInt32 = 15
        root.walk { maxID = Swift.max(maxID, $0.cnid) }
        nextCNID = Swift.max(nextCNID, maxID + 1)
    }

    /// An empty, newly initialized disk of `sectors` 512-byte sectors (1600 or 2880).
    public static func blank(sectors: Int, name: String) throws -> HFSVolume {
        let bitmapSectors = (sectors + 4095) / 4096
        let first = 3 + bitmapSectors
        let blocks = sectors - first - 2
        let small = sectors <= 1600
        var m = [UInt8](repeating: 0, count: 512)
        let now = MacDate.now
        m.put16(0, 0x4244)
        m.put32(2, now); m.put32(6, now)
        m.put16(10, 0x0100)
        m.put16(14, 3)
        m.put16(18, UInt16(blocks))
        m.put32(20, 512)
        m.put32(24, small ? 1024 * 4 : 1024 * 4)
        m.put16(28, UInt16(first))
        m.put32(30, 16)
        m.put32(74, small ? 3072 : 4096)
        m.put32(78, small ? 6144 : 8192)
        m.put32(130, small ? 3072 : 4096)
        m.put32(146, small ? 6144 : 8192)
        var d = [UInt8](repeating: 0, count: sectors * 512)
        d.replaceSubrange(1024..<1536, with: m)
        // An empty catalog and extents file are written by build(); describe them here.
        let vol = try HFSVolume(blankImage: d)
        vol.root.name = try MacName.validated(name, maxLength: MacName.maxVolumeName)
        return vol
    }

    private init(blankImage d: [UInt8]) throws {
        mdb = d.bytes(1024, 512)
        image = d
        blockCount = Int(mdb.u16(18)); blockSize = Int(mdb.u32(20))
        firstBlockSector = Int(mdb.u16(28)); bitmapSector = Int(mdb.u16(14))
        extentsBytes = Int(mdb.u32(130)); catalogBytes = Int(mdb.u32(146)); catalogClump = Int(mdb.u32(78))
        nextCNID = 16
        root = HFSNode(name: Array("untitled".utf8), isFolder: true, cnid: 2)
    }

    /// All zeros past the boot blocks: a disk that was never initialized.
    public static func looksBlank(_ d: [UInt8]) -> Bool {
        d.count >= 1536 && d[1024..<1536].allSatisfy { $0 == 0 }
    }

    struct ExtentKey: Hashable { let fork: UInt8; let fileID: UInt32; let start: Int }

    static func extents(_ r: [UInt8]) -> [(Int, Int)] {
        var out = [(Int, Int)]()
        for i in 0..<3 {
            let count = Int(r.u16(i * 4 + 2))
            if count > 0 { out.append((Int(r.u16(i * 4)), count)) }
        }
        return out
    }

    static func readFork(_ d: [UInt8], firstBlockSector: Int, blockSize: Int, blockCount: Int,
                         extents first: [UInt8], overflow: [ExtentKey: [(Int, Int)]],
                         fileID: UInt32, fork: UInt8, length: Int, what: String) throws -> [UInt8] {
        if length == 0 { return [] }
        let needBlocks = (length + blockSize - 1) / blockSize
        guard needBlocks <= blockCount else { throw DiskError.corrupt("\(what) is larger than the disk.") }
        var exts = extents(first)
        var have = exts.reduce(0) { $0 + $1.1 }
        while have < needBlocks {
            guard let more = overflow[ExtentKey(fork: fork, fileID: fileID, start: have)], !more.isEmpty else {
                throw DiskError.corrupt("part of \(what) is missing.")
            }
            exts += more
            have += more.reduce(0) { $0 + $1.1 }
        }
        var out = [UInt8]()
        out.reserveCapacity(needBlocks * blockSize)
        for (start, count) in exts where out.count < length {
            guard start + count <= blockCount else { throw DiskError.corrupt("\(what) lies outside the disk.") }
            let at = firstBlockSector * 512 + start * blockSize
            out += d[at..<at + count * blockSize]
        }
        return Array(out.prefix(length))
    }

    // MARK: Changing

    public func node(cnid: UInt32) -> HFSNode? {
        var found: HFSNode?
        root.walk { if $0.cnid == cnid { found = $0 } }
        return found
    }

    /// Adds a new item (and everything in it) to `folder`, giving it fresh IDs.
    public func add(_ node: HFSNode, to folder: HFSNode) throws {
        precondition(folder.isFolder)
        if folder.child(named: node.name) != nil { throw DiskError.nameExists(node.displayName) }
        node.walk { n in n.cnid = nextCNID; nextCNID += 1 }
        folder.insertSorted(node)
    }

    public func remove(_ node: HFSNode) {
        guard let p = node.parent else { return }
        p.children.removeAll { $0 === node }
        node.parent = nil
        p.modified = MacDate.now
    }

    public func rename(_ node: HFSNode, to name: [UInt8]) throws {
        if node === root {
            guard name.count <= MacName.maxVolumeName else { throw DiskError.badName("A disk name can have at most 27 characters.") }
            root.name = name
            return
        }
        guard let p = node.parent else { return }
        if let other = p.child(named: name), other !== node { throw DiskError.nameExists(MacName.string(name)) }
        p.children.removeAll { $0 === node }
        node.name = name
        p.insertSorted(node)
    }

    public func move(_ node: HFSNode, to folder: HFSNode) throws {
        precondition(folder.isFolder)
        if node === folder || node.isAncestor(of: folder) {
            throw DiskError.invalidMove("“\(node.displayName)” can't be moved into itself.")
        }
        if node.parent === folder { return }
        if folder.child(named: node.name) != nil { throw DiskError.nameExists(node.displayName) }
        remove(node)
        clearFinderPosition(node)
        folder.insertSorted(node)
        folder.modified = MacDate.now
    }

    /// Lets the Finder place the item afresh (clears its "inited" flag and icon position).
    public func clearFinderPosition(_ node: HFSNode) {
        var f = node.finderInfo
        f.put16(8, f.u16(8) & ~0x0100)
        f.put32(10, 0)
        node.finderInfo = f
    }

    public struct Snapshot {
        let root: HFSNode
        let nextCNID: UInt32
    }

    public func snapshot() -> Snapshot { Snapshot(root: root.copy(), nextCNID: nextCNID) }

    public func restore(_ s: Snapshot) {
        root = s.root.copy()
        nextCNID = s.nextCNID
    }

    // MARK: Space

    public struct Usage {
        public let usedBlocks: Int
        public let totalBlocks: Int
        public let blockSize: Int
        public var freeBytes: Int { Swift.max(0, totalBlocks - usedBlocks) * blockSize }
        public var totalBytes: Int { totalBlocks * blockSize }
    }

    public func usage() -> Usage {
        let layout = try? makeLayout()
        return Usage(usedBlocks: layout?.usedBlocks ?? blockCount, totalBlocks: blockCount, blockSize: blockSize)
    }

    /// Throws `diskFull` if the current contents would not fit on the disk;
    /// `freeBefore` (bytes free before the change) goes into the message.
    public func checkFits(freeBefore: Int = 0) throws {
        let l = try makeLayout()
        if l.usedBlocks > blockCount {
            throw DiskError.diskFull(neededKB: ((l.usedBlocks - blockCount) * blockSize + 1023) / 1024,
                                     freeKB: freeBefore / 1024)
        }
    }

    // MARK: Writing

    struct Layout {
        var records: [CatalogRecord]
        var leaves: [Range<Int>]
        var indexLevels: [[Range<Int>]] // per level above the leaves, ranges of the level below
        var catalogNodes: Int           // nodes in use
        var catalogBlocks: Int
        var extentsBlocks: Int
        var fixedBlocks: Int { catalogBlocks + extentsBlocks }
        var usedBlocks: Int
    }

    struct CatalogRecord {
        let parent: UInt32
        let name: [UInt8]
        var node: HFSNode?
        var isThread: Bool
        var size: Int { HFSVolume.leafKey(parent, name).count + (isThread ? 46 : node!.isFolder ? 70 : 102) }
    }

    static let indexRecordSize = 42

    static func leafKey(_ parent: UInt32, _ name: [UInt8]) -> [UInt8] {
        var k: [UInt8] = [UInt8(6 + name.count), 0, 0, 0, 0, 0, UInt8(name.count)]
        k.put32(2, parent)
        k += name
        if k.count % 2 == 1 { k.append(0) }
        return k
    }

    static func indexKey(_ parent: UInt32, _ name: [UInt8]) -> [UInt8] {
        var k = [UInt8](repeating: 0, count: 38)
        k[0] = 37
        k.put32(2, parent)
        k[6] = UInt8(name.count)
        for (i, b) in name.enumerated() { k[7 + i] = b }
        return k
    }

    func blocks(_ bytes: Int) -> Int { (bytes + blockSize - 1) / blockSize }

    func makeLayout() throws -> Layout {
        var records = [CatalogRecord]()
        func add(_ n: HFSNode, parent: UInt32) {
            records.append(CatalogRecord(parent: parent, name: n.name, node: n, isThread: false))
            if n.isFolder {
                records.append(CatalogRecord(parent: n.cnid, name: [], node: n, isThread: true))
                for c in n.children { add(c, parent: n.cnid) }
            }
        }
        add(root, parent: 1)
        records.sort { a, b in a.parent != b.parent ? a.parent < b.parent : MacName.compare(a.name, b.name) < 0 }

        // Pack the leaf records, then index levels until one node is left.
        func pack(_ sizes: [Int]) -> [Range<Int>] {
            var out = [Range<Int>](), start = 0, used = 16
            for (i, s) in sizes.enumerated() {
                if used + s + 2 > HFSVolume.nodeSize && i > start { out.append(start..<i); start = i; used = 16 }
                used += s + 2
            }
            out.append(start..<sizes.count)
            return out
        }
        let leaves = pack(records.map { $0.size })
        var levels = [[Range<Int>]]()
        var below = leaves.count
        while below > 1 {
            let level = pack([Int](repeating: HFSVolume.indexRecordSize, count: below))
            levels.append(level)
            below = level.count
        }
        let nodes = 1 + leaves.count + levels.reduce(0) { $0 + $1.count }
        guard nodes <= 2048 else { throw DiskError.unsupported("The disk has too many files for this app to write.") }
        let clump = Swift.max(blockSize, catalogClump / blockSize * blockSize)
        let needBytes = (nodes * HFSVolume.nodeSize + clump - 1) / clump * clump
        let catalogBlocks = blocks(Swift.max(catalogBytes, needBytes))
        let extentsBlocks = blocks(Swift.max(extentsBytes, HFSVolume.nodeSize))
        var used = catalogBlocks + extentsBlocks
        root.walk { n in if !n.isFolder { used += blocks(n.data.count) + blocks(n.rsrc.count) } }
        return Layout(records: records, leaves: leaves, indexLevels: levels, catalogNodes: nodes,
                      catalogBlocks: catalogBlocks, extentsBlocks: extentsBlocks, usedBlocks: used)
    }

    /// The whole volume, ready to be written back to the image.
    public func build() throws -> [UInt8] {
        let l = try makeLayout()
        guard l.usedBlocks <= blockCount else {
            throw DiskError.diskFull(neededKB: (l.usedBlocks - blockCount) * blockSize / 1024 + 1, freeKB: 0)
        }
        var d = image
        d.replaceSubrange(1024..<d.count, with: [UInt8](repeating: 0, count: d.count - 1024))
        let base = firstBlockSector * 512

        // Allocation: extents file, catalog, then each file's forks in order.
        var next = 0
        let xtStart = next; next += l.extentsBlocks
        let ctStart = next; next += l.catalogBlocks
        var forks = [UInt32: (data: Int, rsrc: Int)]()
        root.walk { n in
            guard !n.isFolder else { return }
            let dStart = next; next += blocks(n.data.count)
            let rStart = next; next += blocks(n.rsrc.count)
            forks[n.cnid] = (dStart, rStart)
            d.replaceSubrange(base + dStart * blockSize..<base + dStart * blockSize + n.data.count, with: n.data)
            d.replaceSubrange(base + rStart * blockSize..<base + rStart * blockSize + n.rsrc.count, with: n.rsrc)
        }

        // Volume bitmap.
        for b in 0..<next { d[bitmapSector * 512 + b / 8] |= 0x80 >> UInt8(b % 8) }

        // Extents file: an empty B-tree.
        let xtNodes = l.extentsBlocks * blockSize / HFSVolume.nodeSize
        let xtHeader = HFSVolume.headerNode(depth: 0, root: 0, records: 0, first: 0, last: 0, keyLen: 7,
                                            nodes: xtNodes, used: 1)
        d.replaceSubrange(base + xtStart * blockSize..<base + xtStart * blockSize + 512, with: xtHeader)

        // Catalog.
        var files = 0, folders = 0, rootFiles = 0, rootFolders = 0
        func body(_ r: CatalogRecord) -> [UInt8] {
            let n = r.node!
            if r.isThread {
                var t = [UInt8](repeating: 0, count: 46)
                t[0] = 3
                t.put32(10, n === root ? 1 : n.parent!.cnid)
                t[14] = UInt8(n.name.count)
                for (i, b) in n.name.enumerated() { t[15 + i] = b }
                return t
            }
            if n.isFolder {
                var v = [UInt8](repeating: 0, count: 70)
                v[0] = 1
                v.put16(2, n.flags)
                v.put16(4, UInt16(Swift.min(n.children.count, 0xFFFF)))
                v.put32(6, n.cnid)
                v.put32(10, n.created); v.put32(14, n.modified); v.put32(18, n.backedUp)
                v.replaceSubrange(22..<38, with: n.finderInfo)
                v.replaceSubrange(38..<54, with: n.extFinderInfo)
                return v
            }
            let f = forks[n.cnid]!
            var v = [UInt8](repeating: 0, count: 102)
            v[0] = 2
            v[2] = UInt8(n.flags & 0xFD) // no file thread record
            v.replaceSubrange(4..<20, with: n.finderInfo)
            v.put32(20, n.cnid)
            let db = blocks(n.data.count), rb = blocks(n.rsrc.count)
            v.put32(26, UInt32(n.data.count)); v.put32(30, UInt32(db * blockSize))
            v.put32(36, UInt32(n.rsrc.count)); v.put32(40, UInt32(rb * blockSize))
            v.put32(44, n.created); v.put32(48, n.modified); v.put32(52, n.backedUp)
            v.replaceSubrange(56..<72, with: n.extFinderInfo)
            if db > 0 { v.put16(74, UInt16(f.data)); v.put16(76, UInt16(db)) }
            if rb > 0 { v.put16(86, UInt16(f.rsrc)); v.put16(88, UInt16(rb)) }
            return v
        }
        for r in l.records where !r.isThread {
            if r.node!.isFolder { if r.node !== root { folders += 1; if r.parent == 2 { rootFolders += 1 } } }
            else { files += 1; if r.parent == 2 { rootFiles += 1 } }
        }

        let ctNodes = l.catalogBlocks * blockSize / HFSVolume.nodeSize
        var catalog = [UInt8](repeating: 0, count: l.catalogBlocks * blockSize)
        func putNode(_ i: Int, _ node: [UInt8]) { catalog.replaceSubrange(i * 512..<(i + 1) * 512, with: node) }
        // Leaves are nodes 1...n; index levels follow, bottom up.
        var levelFirstKeys = [(UInt32, [UInt8])]()
        let levelStart = 1
        for (i, range) in l.leaves.enumerated() {
            let recs = l.records[range].map { HFSVolume.leafKey($0.parent, $0.name) + body($0) }
            let f = i + 1 < l.leaves.count ? UInt32(levelStart + i + 1) : 0
            let b = i > 0 ? UInt32(levelStart + i - 1) : 0
            putNode(levelStart + i, HFSVolume.makeNode(kind: 0xFF, height: 1, fLink: f, bLink: b, records: recs))
            levelFirstKeys.append((l.records[range.first!].parent, l.records[range.first!].name))
        }
        var below = (start: levelStart, keys: levelFirstKeys)
        var nextNode = 1 + l.leaves.count
        var height: UInt8 = 1
        for level in l.indexLevels {
            height += 1
            var keys = [(UInt32, [UInt8])]()
            let start = nextNode
            for (i, range) in level.enumerated() {
                let recs = range.map { j -> [UInt8] in
                    var r = HFSVolume.indexKey(below.keys[j].0, below.keys[j].1)
                    r += [0, 0, 0, 0]
                    r.put32(38, UInt32(below.start + j))
                    return r
                }
                let f = i + 1 < level.count ? UInt32(start + i + 1) : 0
                let b = i > 0 ? UInt32(start + i - 1) : 0
                putNode(start + i, HFSVolume.makeNode(kind: 0, height: height, fLink: f, bLink: b, records: recs))
                keys.append(below.keys[range.first!])
            }
            below = (start, keys)
            nextNode += level.count
        }
        putNode(0, HFSVolume.headerNode(depth: UInt16(height), root: UInt32(below.start), records: l.records.count,
                                        first: 1, last: l.leaves.count, keyLen: 37, nodes: ctNodes, used: l.catalogNodes))
        d.replaceSubrange(base + ctStart * blockSize..<base + ctStart * blockSize + catalog.count, with: catalog)

        // Master directory block (and its copy near the end of the disk).
        var m = mdb
        m.put32(6, MacDate.now)
        m.put16(10, (m.u16(10) | 0x0100) & ~0x0200)
        m.put16(12, UInt16(rootFiles))
        m.put16(16, UInt16(next < blockCount ? next : 0))
        m.put32(30, nextCNID)
        m.put16(34, UInt16(blockCount - next))
        m.replaceSubrange(36..<64, with: [UInt8](repeating: 0, count: 28))
        m[36] = UInt8(root.name.count)
        for (i, b) in root.name.enumerated() { m[37 + i] = b }
        m.put32(70, m.u32(70) &+ 1)
        m.put16(82, UInt16(rootFolders))
        m.put32(84, UInt32(files))
        m.put32(88, UInt32(folders))
        if let blessed = Optional(m.u32(92)), blessed != 0, node(cnid: blessed)?.isFolder != true { m.put32(92, 0) }
        m.put32(130, UInt32(l.extentsBlocks * blockSize))
        m.replaceSubrange(134..<146, with: [UInt8](repeating: 0, count: 12))
        m.put16(136, UInt16(l.extentsBlocks))
        m.put32(146, UInt32(l.catalogBlocks * blockSize))
        m.replaceSubrange(150..<162, with: [UInt8](repeating: 0, count: 12))
        m.put16(150, UInt16(ctStart))
        m.put16(152, UInt16(l.catalogBlocks))
        m.replaceSubrange(162..<512, with: [UInt8](repeating: 0, count: 350))
        d.replaceSubrange(1024..<1536, with: m)
        d.replaceSubrange(d.count - 1024..<d.count - 512, with: m)
        return d
    }

    static func makeNode(kind: UInt8, height: UInt8, fLink: UInt32, bLink: UInt32, records: [[UInt8]]) -> [UInt8] {
        var n = [UInt8](repeating: 0, count: nodeSize)
        n.put32(0, fLink); n.put32(4, bLink)
        n[8] = kind; n[9] = height
        n.put16(10, UInt16(records.count))
        var off = 14
        for (i, r) in records.enumerated() {
            n.put16(nodeSize - 2 - 2 * i, UInt16(off))
            n.replaceSubrange(off..<off + r.count, with: r)
            off += r.count
        }
        n.put16(nodeSize - 2 - 2 * records.count, UInt16(off))
        precondition(off <= nodeSize - 2 * (records.count + 1))
        return n
    }

    static func headerNode(depth: UInt16, root: UInt32, records: Int, first: Int, last: Int, keyLen: UInt16,
                           nodes: Int, used: Int) -> [UInt8] {
        var h = [UInt8](repeating: 0, count: 106)
        h.put16(0, depth); h.put32(2, root); h.put32(6, UInt32(records))
        h.put32(10, UInt32(first)); h.put32(14, UInt32(last))
        h.put16(18, UInt16(nodeSize)); h.put16(20, keyLen)
        h.put32(22, UInt32(nodes)); h.put32(26, UInt32(nodes - used))
        var map = [UInt8](repeating: 0, count: 256)
        for i in 0..<used { map[i / 8] |= 0x80 >> UInt8(i % 8) }
        return makeNode(kind: 1, height: 0, fLink: 0, bLink: 0,
                        records: [h, [UInt8](repeating: 0, count: 128), map])
    }
}

/// Reading the B-trees (catalog and extents) of an HFS volume.
struct BTree {
    let file: [UInt8]
    let what: String
    let depth: Int, root: Int, recordCount: Int, firstLeaf: Int, lastLeaf: Int
    let keyLen: Int, nodeCount: Int, freeCount: Int

    init(_ file: [UInt8], _ what: String) throws {
        self.file = file
        self.what = what
        guard file.count >= 512, file[8] == 1 else { throw DiskError.corrupt("the \(what) B-tree has no header.") }
        depth = Int(file.u16(14)); root = Int(file.u32(16)); recordCount = Int(file.u32(20))
        firstLeaf = Int(file.u32(24)); lastLeaf = Int(file.u32(28))
        keyLen = Int(file.u16(34)); nodeCount = Int(file.u32(36)); freeCount = Int(file.u32(40))
        guard file.u16(32) == 512 else { throw DiskError.unsupported("The \(what) B-tree uses an unusual node size.") }
    }

    struct Node { let kind: UInt8; let height: Int; let fLink: Int; let bLink: Int; let records: [ArraySlice<UInt8>] }

    func node(_ n: Int) throws -> Node {
        guard n > 0, (n + 1) * 512 <= file.count else { throw DiskError.corrupt("the \(what) B-tree points outside itself.") }
        let s = Array(file[n * 512..<(n + 1) * 512])
        let count = Int(s.u16(10))
        guard count < 250 else { throw DiskError.corrupt("a \(what) B-tree node is malformed.") }
        var offs = [Int]()
        for i in 0...count { offs.append(Int(s.u16(510 - 2 * i))) }
        for i in 0..<count {
            guard offs[i] >= 14, offs[i] < offs[i + 1], offs[i + 1] <= 512 - 2 * (count + 1) else {
                throw DiskError.corrupt("a \(what) B-tree node is malformed.")
            }
        }
        let recs = (0..<count).map { s[offs[$0]..<offs[$0 + 1]] }
        return Node(kind: s[8], height: Int(s[9]), fLink: Int(s.u32(0)), bLink: Int(s.u32(4)), records: recs)
    }

    /// Every leaf record, following the leaf chain from the first leaf.
    func leafRecords() throws -> [ArraySlice<UInt8>] {
        var out = [ArraySlice<UInt8>]()
        var n = firstLeaf, seen = Set<Int>()
        while n != 0 {
            guard seen.insert(n).inserted else { throw DiskError.corrupt("the \(what) B-tree's leaves loop.") }
            let node = try self.node(n)
            guard node.kind == 0xFF else { throw DiskError.corrupt("the \(what) B-tree's leaf chain is broken.") }
            out += node.records
            n = node.fLink
        }
        return out
    }
}

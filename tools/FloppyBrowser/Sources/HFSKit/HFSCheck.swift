import Foundation

/// A consistency check of an HFS volume, in the spirit of Disk First Aid:
/// B-tree structure and order, threads, valences, volume counts and the
/// allocation bitmap. Returns a list of problems (empty = all good).
public enum HFSCheck {
    public static func check(sectors d: [UInt8]) -> [String] {
        var problems = [String]()
        do { try run(d, &problems) } catch { problems.append(error.localizedDescription) }
        return problems
    }

    struct CatKey { let parent: UInt32; let name: [UInt8] }

    static func catKey(_ r: ArraySlice<UInt8>) throws -> CatKey {
        let r = Array(r)
        try r.need(0, 7, "short catalog key")
        let n = Int(r[6])
        guard n <= 31 else { throw DiskError.corrupt("catalog key name too long") }
        try r.need(7, n, "short catalog key")
        return CatKey(parent: r.u32(2), name: r.bytes(7, n))
    }

    static func catCompare(_ a: ArraySlice<UInt8>, _ b: ArraySlice<UInt8>) throws -> Int {
        let x = try catKey(a), y = try catKey(b)
        if x.parent != y.parent { return x.parent < y.parent ? -1 : 1 }
        return MacName.compare(x.name, y.name)
    }

    static func extCompare(_ a: ArraySlice<UInt8>, _ b: ArraySlice<UInt8>) throws -> Int {
        let x = Array(a), y = Array(b)
        try x.need(0, 8, "short extents key"); try y.need(0, 8, "short extents key")
        if x.u32(2) != y.u32(2) { return x.u32(2) < y.u32(2) ? -1 : 1 }
        if x[1] != y[1] { return Int(x[1]) - Int(y[1]) }
        return Int(x.u16(6)) - Int(y.u16(6))
    }

    /// Walks a B-tree from its root; returns the leaf records in key order.
    static func walkTree(_ t: BTree, _ compare: (ArraySlice<UInt8>, ArraySlice<UInt8>) throws -> Int,
                         _ problems: inout [String]) throws -> [ArraySlice<UInt8>] {
        let name = t.what
        if t.depth == 0 {
            if t.root != 0 || t.recordCount != 0 || t.firstLeaf != 0 { problems.append("\(name): empty tree has a root or records") }
            return []
        }
        var leaves = [Int]()
        var used: Set<Int> = [0]
        var levelNodes = [Int: [Int]]()
        func descend(_ n: Int, _ height: Int, _ lower: ArraySlice<UInt8>?) throws -> ArraySlice<UInt8> {
            guard used.insert(n).inserted else { throw DiskError.corrupt("\(name): node \(n) is reached twice") }
            let node = try t.node(n)
            levelNodes[height, default: []].append(n)
            let wantKind: UInt8 = height == 1 ? 0xFF : 0
            if node.kind != wantKind || node.height != height {
                problems.append("\(name): node \(n) has kind \(node.kind) height \(node.height), expected \(wantKind)/\(height)")
            }
            guard !node.records.isEmpty else { throw DiskError.corrupt("\(name): node \(n) is empty") }
            for i in 1..<node.records.count where try compare(node.records[i - 1], node.records[i]) >= 0 {
                problems.append("\(name): keys out of order in node \(n)")
            }
            if let lower = lower, try compare(lower, node.records[0]) > 0 {
                problems.append("\(name): node \(n)'s first key sorts before its index key")
            }
            if height == 1 { leaves.append(n); return node.records[0] }
            for r in node.records {
                let rr = Array(r)
                let keyBytes = Int(rr[0]) + 1
                let at = (keyBytes + 1) & ~1
                guard keyBytes >= 7 || t.keyLen == 7, at + 4 <= rr.count else {
                    problems.append("\(name): bad index record in node \(n)"); continue
                }
                if t.keyLen == 37 && rr[0] != 37 { problems.append("\(name): index key in node \(n) is not full length") }
                _ = try descend(Int(rr.u32(at)), height - 1, r)
            }
            return node.records[0]
        }
        _ = try descend(t.root, t.depth, nil)

        // Sibling links at every level, and the leaf chain.
        for (h, nodes) in levelNodes {
            for (i, n) in nodes.enumerated() {
                let node = try t.node(n)
                let f = i + 1 < nodes.count ? nodes[i + 1] : 0, b = i > 0 ? nodes[i - 1] : 0
                if node.fLink != f || node.bLink != b { problems.append("\(name): node \(n) (height \(h)) has wrong sibling links") }
            }
        }
        if t.firstLeaf != leaves.first || t.lastLeaf != leaves.last { problems.append("\(name): header first/last leaf wrong") }
        var records = [ArraySlice<UInt8>]()
        for n in leaves { records += try t.node(n).records }
        if records.count != t.recordCount { problems.append("\(name): header says \(t.recordCount) records, found \(records.count)") }
        for i in records.indices.dropFirst() where try compare(records[i - 1], records[i]) >= 0 {
            problems.append("\(name): leaf records out of order at \(i)")
        }

        // Node map (in the header node; floppies never need map nodes).
        let header = try headerMap(t)
        var mapped = 0
        for i in 0..<t.nodeCount {
            let bit = i / 8 < header.count && header[i / 8] & (0x80 >> UInt8(i % 8)) != 0
            if bit { mapped += 1 }
            if used.contains(i) && !bit { problems.append("\(name): node \(i) is in use but free in the map") }
        }
        if t.nodeCount - mapped != t.freeCount { problems.append("\(name): header free count \(t.freeCount), map says \(t.nodeCount - mapped)") }
        if mapped != used.count { problems.append("\(name): map marks \(mapped) nodes, \(used.count) reachable") }
        return records
    }

    static func headerMap(_ t: BTree) throws -> [UInt8] {
        let f = t.file
        let mapStart = Int(f.u16(512 - 6)), mapEnd = Int(f.u16(512 - 8))
        guard mapStart < mapEnd, mapEnd <= 512 else { throw DiskError.corrupt("\(t.what): bad header map record") }
        return Array(f[mapStart..<mapEnd])
    }

    static func run(_ d: [UInt8], _ problems: inout [String]) throws {
        let vol = try HFSVolume(sectors: d) // geometry + readability of every fork
        let m = d.bytes(1024, 512)
        let base = vol.firstBlockSector * 512, bs = vol.blockSize, count = vol.blockCount

        var owner = [Int: String]()
        func claim(_ exts: [(Int, Int)], _ who: String) {
            for (s, c) in exts {
                guard s + c <= count else { problems.append("\(who): extent outside the disk"); continue }
                for b in s..<s + c {
                    if let o = owner[b] { problems.append("block \(b) used by both \(o) and \(who)") }
                    owner[b] = who
                }
            }
        }
        func read(_ exts: [(Int, Int)], _ length: Int) -> [UInt8] {
            var out = [UInt8]()
            for (s, c) in exts where s + c <= count { out += d[base + s * bs..<base + (s + c) * bs] }
            return Array(out.prefix(length))
        }

        let xtExt = HFSVolume.extents(m.bytes(134, 12))
        claim(xtExt, "extents file")
        if xtExt.reduce(0, { $0 + $1.1 }) * bs != Int(m.u32(130)) { problems.append("MDB: extents file size does not match its extents") }
        let xt = BTree(try BTree(read(xtExt, Int(m.u32(130))), "extents"))
        let xtRecords = try walkTree(xt, extCompare, &problems)
        var overflow = [HFSVolume.ExtentKey: [(Int, Int)]]()
        for r in xtRecords {
            let rr = Array(r)
            overflow[HFSVolume.ExtentKey(fork: rr[1], fileID: rr.u32(2), start: Int(rr.u16(6)))] = HFSVolume.extents(rr.bytes(8, 12))
        }
        func allExtents(_ first: [UInt8], _ id: UInt32, _ fork: UInt8) -> [(Int, Int)] {
            var e = HFSVolume.extents(first)
            var have = e.reduce(0) { $0 + $1.1 }
            while let more = overflow[HFSVolume.ExtentKey(fork: fork, fileID: id, start: have)], !more.isEmpty {
                e += more; have += more.reduce(0) { $0 + $1.1 }
            }
            return e
        }

        let ctExt = allExtents(m.bytes(150, 12), 4, 0)
        claim(ctExt, "catalog")
        if ctExt.reduce(0, { $0 + $1.1 }) * bs != Int(m.u32(146)) { problems.append("MDB: catalog size does not match its extents") }
        let ct = try BTree(read(ctExt, Int(m.u32(146))), "catalog")
        if ct.keyLen != 37 { problems.append("catalog: max key length \(ct.keyLen)") }
        let records = try walkTree(ct, catCompare, &problems)

        var folders = [UInt32: (parent: UInt32, name: [UInt8], valence: Int)]()
        var threads = [UInt32: (parent: UInt32, name: [UInt8])]()
        var children = [UInt32: Int]()
        var ids = Set<UInt32>()
        var files = 0, rootFiles = 0, rootFolders = 0
        let nextID = m.u32(30)
        for r in records {
            let rr = Array(r)
            let key = try catKey(r)
            let v = (Int(rr[0]) + 2) & ~1
            switch rr[v] {
            case 1:
                let id = rr.u32(v + 6)
                folders[id] = (key.parent, key.name, Int(rr.u16(v + 4)))
                if !ids.insert(id).inserted { problems.append("CNID \(id) used twice") }
                if id >= nextID { problems.append("folder ID \(id) >= next ID \(nextID)") }
                children[key.parent, default: 0] += 1
                if key.parent == 2 { rootFolders += 1 }
            case 2:
                let id = rr.u32(v + 20)
                if !ids.insert(id).inserted { problems.append("CNID \(id) used twice") }
                if id >= nextID { problems.append("file ID \(id) >= next ID \(nextID)") }
                files += 1
                children[key.parent, default: 0] += 1
                if key.parent == 2 { rootFiles += 1 }
                for (fork, extAt, lenAt, physAt) in [(UInt8(0), 74, 26, 30), (UInt8(0xFF), 86, 36, 40)] {
                    let e = allExtents(rr.bytes(v + extAt, 12), id, fork)
                    let blocks = e.reduce(0) { $0 + $1.1 }
                    let logical = Int(rr.u32(v + lenAt)), physical = Int(rr.u32(v + physAt))
                    if blocks * bs != physical || physical < logical {
                        problems.append("file \(MacName.string(key.name)): fork \(fork) sizes \(logical)/\(physical) vs \(blocks) blocks")
                    }
                    claim(e, "file \(MacName.string(key.name))")
                }
            case 3, 4:
                if !key.name.isEmpty { problems.append("thread record with a name in its key") }
                threads[key.parent] = (rr.u32(v + 10), rr.bytes(v + 15, Int(rr[v + 14])))
            default:
                problems.append("unknown record type \(rr[v])")
            }
        }
        for (id, f) in folders {
            guard let t = threads[id] else { problems.append("folder \(MacName.string(f.name)) has no thread"); continue }
            if t.parent != f.parent || !MacName.same(t.name, f.name) { problems.append("folder \(MacName.string(f.name)): thread mismatch") }
            if (children[id] ?? 0) != f.valence { problems.append("folder \(MacName.string(f.name)): valence \(f.valence), has \(children[id] ?? 0)") }
            if id != 2 && folders[f.parent] == nil { problems.append("folder \(MacName.string(f.name)): parent missing") }
        }
        for (id, _) in threads where folders[id] == nil {
            // File threads are optional and allowed; folder threads need a folder.
            if !records.contains(where: { r in let rr = Array(r); let v = (Int(rr[0]) + 2) & ~1
                return rr[v] == 2 && rr.u32(v + 20) == id }) { problems.append("thread for missing item \(id)") }
        }
        if folders[2] == nil { problems.append("no root folder") }
        if Int(m.u16(12)) != rootFiles { problems.append("MDB: drNmFls \(m.u16(12)), counted \(rootFiles)") }
        if Int(m.u16(82)) != rootFolders { problems.append("MDB: drNmRtDirs \(m.u16(82)), counted \(rootFolders)") }
        if Int(m.u32(84)) != files { problems.append("MDB: drFilCnt \(m.u32(84)), counted \(files)") }
        if Int(m.u32(88)) != folders.count - 1 { problems.append("MDB: drDirCnt \(m.u32(88)), counted \(folders.count - 1)") }
        if let rootName = folders[2]?.name, rootName != m.bytes(37, Int(m[36])) { problems.append("MDB: volume name differs from root folder") }

        // Bitmap.
        var free = 0
        for b in 0..<count {
            let set = d[vol.bitmapSector * 512 + b / 8] & (0x80 >> UInt8(b % 8)) != 0
            if !set { free += 1 }
            if set != (owner[b] != nil) {
                problems.append(set ? "block \(b) marked used but nothing owns it" : "block \(b) used by \(owner[b]!) but marked free")
            }
        }
        if free != Int(m.u16(34)) { problems.append("MDB: drFreeBks \(m.u16(34)), bitmap has \(free)") }
        _ = vol
    }
}

extension BTree {
    init(_ other: BTree) { self = other }
}

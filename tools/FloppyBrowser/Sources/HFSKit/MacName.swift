import Foundation

/// File names on HFS volumes: Mac Roman bytes, at most 31 (27 for a volume),
/// no colons, compared without regard to case as the Mac's HFS does.
public enum MacName {
    public static let maxFileName = 31
    public static let maxVolumeName = 27

    /// The order HFS sorts names in (Linux fs/hfs/string.c, from ARDI's code:
    /// RelString with case folded and diacriticals significant).
    static let caseOrder: [UInt8] = [
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F,
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F,
        0x20, 0x22, 0x23, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36,
        0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
        0x47, 0x48, 0x57, 0x59, 0x5D, 0x5F, 0x66, 0x68, 0x6A, 0x6C, 0x72, 0x74, 0x76, 0x78, 0x7A, 0x7E,
        0x8C, 0x8E, 0x90, 0x92, 0x95, 0x97, 0x9E, 0xA0, 0xA2, 0xA4, 0xA7, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD,
        0x4E, 0x48, 0x57, 0x59, 0x5D, 0x5F, 0x66, 0x68, 0x6A, 0x6C, 0x72, 0x74, 0x76, 0x78, 0x7A, 0x7E,
        0x8C, 0x8E, 0x90, 0x92, 0x95, 0x97, 0x9E, 0xA0, 0xA2, 0xA4, 0xA7, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3,
        0x4A, 0x4C, 0x5A, 0x60, 0x7B, 0x7F, 0x98, 0x4F, 0x49, 0x51, 0x4A, 0x4B, 0x4C, 0x5A, 0x60, 0x63,
        0x64, 0x65, 0x6E, 0x6F, 0x70, 0x71, 0x7B, 0x84, 0x85, 0x86, 0x7F, 0x80, 0x9A, 0x9B, 0x9C, 0x98,
        0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0x94, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0x4D, 0x81,
        0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0x55, 0x8A, 0xCC, 0x4D, 0x81,
        0xCD, 0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0x26, 0x27, 0xD4, 0x20, 0x49, 0x4B, 0x80, 0x82, 0x82,
        0xD5, 0xD6, 0x24, 0x25, 0x2D, 0x2E, 0xD7, 0xD8, 0xA6, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
        0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
        0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
    ]

    /// <0, 0 or >0 in the order the Mac's catalog B-tree keeps names.
    public static func compare(_ a: [UInt8], _ b: [UInt8]) -> Int {
        let n = Swift.min(a.count, b.count)
        for i in 0..<n {
            let d = Int(caseOrder[Int(a[i])]) - Int(caseOrder[Int(b[i])])
            if d != 0 { return d }
        }
        return a.count - b.count
    }

    public static func same(_ a: [UInt8], _ b: [UInt8]) -> Bool { compare(a, b) == 0 }

    /// A Mac Roman name for display.
    public static func string(_ bytes: [UInt8]) -> String {
        String(bytes: bytes, encoding: .macOSRoman) ?? String(decoding: bytes, as: UTF8.self)
    }

    /// Mac Roman bytes for a string; characters Mac Roman lacks become "_".
    public static func encode(_ s: String) -> [UInt8] {
        var out = [UInt8]()
        for ch in s.precomposedStringWithCanonicalMapping {
            if let d = String(ch).data(using: .macOSRoman), !d.isEmpty { out += d } else { out.append(0x5F) }
        }
        return out
    }

    /// A name typed by the user, checked. Throws for empty, too long or colon.
    public static func validated(_ s: String, maxLength: Int = maxFileName) throws -> [UInt8] {
        let b = encode(s)
        if b.isEmpty { throw DiskError.badName("A name can't be empty.") }
        if b.contains(0x3A) { throw DiskError.badName("Classic Mac names can't contain a colon (:).") }
        if b.count > maxLength { throw DiskError.badName("That name is too long: classic Mac names can have at most \(maxLength) characters.") }
        return b
    }

    /// A Mac name for a file name from macOS (where Finder's "/" is ":" on disk),
    /// shortened to fit, keeping a short extension.
    public static func fromHost(_ hostName: String, maxLength: Int = maxFileName) -> [UInt8] {
        var b = encode(hostName.replacingOccurrences(of: ":", with: "/"))
        b = b.map { $0 < 0x20 && $0 != 0x0D ? 0x5F : $0 }
        if b.isEmpty { b = Array("untitled".utf8) }
        return shortened(b, maxLength: maxLength)
    }

    static func shortened(_ b: [UInt8], maxLength: Int) -> [UInt8] {
        if b.count <= maxLength { return b }
        if let dot = b.lastIndex(of: 0x2E), b.count - dot <= 5, dot > 0 {
            let ext = Array(b[dot...])
            return Array(b[..<(maxLength - ext.count)]) + ext
        }
        return Array(b[..<maxLength])
    }

    /// The macOS file name for a Mac name ("/" is allowed on the Mac and is ":" on disk here).
    public static func toHost(_ macName: [UInt8]) -> String {
        var s = string(macName).replacingOccurrences(of: "/", with: ":")
        s = String(s.map { $0 == "\r" || $0 == "\0" ? "_" : $0 })
        if s.hasPrefix(".") { s = "_" + s.dropFirst() }
        return s.isEmpty ? "untitled" : s
    }

    /// A four-character code for display (Mac Roman; NULs shown as spaces).
    public static func fourCC(_ b: ArraySlice<UInt8>) -> String {
        if b.allSatisfy({ $0 == 0 }) { return "" }
        return string(b.map { $0 == 0 ? 0x20 : $0 })
    }

    /// Four bytes from up to four typed characters (padded with spaces; empty = zeros).
    public static func fourCCBytes(_ s: String) throws -> [UInt8] {
        if s.isEmpty { return [0, 0, 0, 0] }
        let b = encode(s)
        if b.count > 4 { throw DiskError.badName("Type and creator codes have exactly four characters.") }
        return b + [UInt8](repeating: 0x20, count: 4 - b.count)
    }
}

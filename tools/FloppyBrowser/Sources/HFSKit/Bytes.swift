import Foundation

/// Errors from reading or changing a disk image.
public enum DiskError: LocalizedError, Equatable {
    case unsupported(String)
    case corrupt(String)
    case diskFull(neededKB: Int, freeKB: Int)
    case nameExists(String)
    case badName(String)
    case invalidMove(String)

    public var errorDescription: String? {
        switch self {
        case .unsupported(let s): return s
        case .corrupt(let s): return "The disk image is damaged: \(s)"
        case .diskFull(let need, let free):
            return "There is not enough space on the disk (\(need) KB more is needed; \(free) KB is free)."
        case .nameExists(let n): return "An item named “\(n)” already exists in this folder."
        case .badName(let s): return s
        case .invalidMove(let s): return s
        }
    }
}

/// Big-endian access to byte arrays. Callers check bounds first (see `need`).
extension Array where Element == UInt8 {
    @inline(__always) func u16(_ o: Int) -> UInt16 { UInt16(self[o]) << 8 | UInt16(self[o + 1]) }
    @inline(__always) func u32(_ o: Int) -> UInt32 {
        UInt32(self[o]) << 24 | UInt32(self[o + 1]) << 16 | UInt32(self[o + 2]) << 8 | UInt32(self[o + 3])
    }
    @inline(__always) mutating func put16(_ o: Int, _ v: UInt16) {
        self[o] = UInt8(v >> 8); self[o + 1] = UInt8(v & 0xFF)
    }
    @inline(__always) mutating func put32(_ o: Int, _ v: UInt32) {
        self[o] = UInt8(v >> 24); self[o + 1] = UInt8((v >> 16) & 0xFF)
        self[o + 2] = UInt8((v >> 8) & 0xFF); self[o + 3] = UInt8(v & 0xFF)
    }
    /// Throws unless bytes o..<o+n exist.
    func need(_ o: Int, _ n: Int, _ what: @autoclosure () -> String) throws {
        if o < 0 || n < 0 || o > count - n { throw DiskError.corrupt(what()) }
    }
    func bytes(_ o: Int, _ n: Int) -> [UInt8] { Array(self[o..<o + n]) }
}

/// CRC-16/XMODEM (CCITT polynomial 0x1021, initial value 0), used by MacBinary and BinHex.
func crc16(_ data: ArraySlice<UInt8>, _ start: UInt16 = 0) -> UInt16 {
    var crc = start
    for b in data {
        crc ^= UInt16(b) << 8
        for _ in 0..<8 { crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1 }
    }
    return crc
}

/// Classic Mac dates: seconds since 1 Jan 1904, local time.
public enum MacDate {
    static let offset1904: Double = 2082844800

    public static func from(_ date: Date) -> UInt32 {
        let local = date.timeIntervalSince1970 + offset1904 + Double(TimeZone.current.secondsFromGMT(for: date))
        return UInt32(Swift.max(0, Swift.min(Double(UInt32.max), local.rounded(.down))))
    }

    public static func date(_ mac: UInt32) -> Date {
        let local = Double(mac) - offset1904
        let guess = Date(timeIntervalSince1970: local)
        return Date(timeIntervalSince1970: local - Double(TimeZone.current.secondsFromGMT(for: guess)))
    }

    public static var now: UInt32 { from(Date()) }
}

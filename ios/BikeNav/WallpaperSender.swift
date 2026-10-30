import Combine
import Foundation

/// Sends a rendered wallpaper to the board, one sector at a time.
///
/// 153,600 bytes is far more than the 128-byte packets everything else on this
/// link uses, so it goes as its own packet type in pieces the board can write
/// to flash without buffering the whole picture: one 4 KB sector, which is the
/// smallest thing flash can erase.
///
/// This runs ONLY while the app is open, and that is the point of the design.
/// The board stores what it receives and reads it at boot with no phone
/// involved, so a ride never waits on this and an expired sideload never
/// leaves the screen blank.
@MainActor
final class WallpaperSender: ObservableObject {
    enum Phase: Equatable {
        case idle
        case sending(Double)        // 0...1
        case done
        case failed(String)
    }

    @Published private(set) var phase: Phase = .idle

    /// One flash sector. The board erases and writes a sector at a time, so
    /// sending in anything else would mean it holding a part-sector somewhere.
    static let sectorBytes = 4096

    private var task: Task<Void, Never>?

    func cancel() {
        task?.cancel()
        task = nil
        phase = .idle
    }

    func send(_ pixels: Data, face: Wallpaper.Face, over link: BLELink) {
        cancel()
        guard pixels.count == Wallpaper.byteCount else {
            phase = .failed("Wrong size: \(pixels.count) bytes")
            return
        }
        guard link.state == .connected else {
            phase = .failed("Display not connected")
            return
        }

        phase = .sending(0)
        task = Task { [weak self] in
            let sectors = (pixels.count + Self.sectorBytes - 1) / Self.sectorBytes

            // Tell the board what is coming, so it can erase before anything
            // arrives rather than erasing under a write in flight.
            link.send(Packet.wallpaperBegin(face: face, sectors: UInt16(sectors),
                                            bytes: UInt32(pixels.count)), force: true)
            try? await Task.sleep(nanoseconds: 120_000_000)

            for s in 0..<sectors {
                if Task.isCancelled { return }
                let start = s * Self.sectorBytes
                let end = min(start + Self.sectorBytes, pixels.count)
                var offset = 0
                // Chunked well under the board's receive buffer. Paced rather
                // than blasted: this link also carries the turn the rider is
                // currently following, and a background picture must never be
                // the reason an instruction is late.
                while offset < end - start {
                    if Task.isCancelled { return }
                    let n = min(Packet.wallpaperChunkBytes, end - start - offset)
                    let slice = pixels.subdata(in: (start + offset)..<(start + offset + n))
                    link.send(Packet.wallpaperChunk(sector: UInt16(s),
                                                    offset: UInt16(offset),
                                                    bytes: slice), force: true)
                    offset += n
                    try? await Task.sleep(nanoseconds: 12_000_000)
                }
                await MainActor.run {
                    self?.phase = .sending(Double(s + 1) / Double(sectors))
                }
            }

            link.send(Packet.wallpaperEnd(), force: true)
            await MainActor.run { self?.phase = .done }
        }
    }
}

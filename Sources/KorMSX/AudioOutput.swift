import AVFoundation
import MSXCore

/// Plays the mono samples produced each frame through a ring buffer drained
/// by an AVAudioSourceNode on the audio thread (same design as CGC Apple).
final class AudioOutput: @unchecked Sendable {
    private let engine = AVAudioEngine()
    private let lock = NSLock()
    private let capacity = 16384
    private var ring = [Float](repeating: 0, count: 16384)
    private var readIndex = 0
    private var count = 0
    private var last: Float = 0
    private let maxLatency = 8192
    /// Samples held before (re)starting playback, to absorb 60 Hz burst jitter.
    static let targetFill = 2048
    private var primed = false

    var muted = false

    /// Samples currently queued, for the emulator's pacing.
    var fill: Int {
        lock.lock()
        defer { lock.unlock() }
        return count
    }

    init() {
        let format = AVAudioFormat(standardFormatWithSampleRate: Double(MSX_AUDIO_RATE), channels: 1)!
        let source = AVAudioSourceNode(format: format) { [unowned self] _, _, frameCount, bufferList in
            let buffers = UnsafeMutableAudioBufferListPointer(bufferList)
            guard let out = buffers.first?.mData?.assumingMemoryBound(to: Float.self) else { return noErr }
            self.pull(out, frames: Int(frameCount))
            return noErr
        }
        engine.attach(source)
        engine.connect(source, to: engine.mainMixerNode, format: format)
        try? engine.start()
    }

    func push(_ samples: UnsafeBufferPointer<Int16>) {
        lock.lock()
        defer { lock.unlock() }
        for s in samples {
            if count >= maxLatency {
                readIndex = (readIndex + 1) % capacity
                count -= 1
            }
            ring[(readIndex + count) % capacity] = muted ? 0 : Float(s) / 32768
            count += 1
        }
    }

    private func pull(_ out: UnsafeMutablePointer<Float>, frames: Int) {
        lock.lock()
        defer { lock.unlock() }
        if !primed && count >= Self.targetFill { primed = true }
        for i in 0..<frames {
            if primed && count > 0 {
                last = ring[readIndex]
                readIndex = (readIndex + 1) % capacity
                count -= 1
            } else {
                primed = false          // underrun: refill before resuming
                last *= 0.99            // fade instead of clicking
            }
            out[i] = last
        }
    }
}

import CoreAudio
import Foundation

/// Thin CoreAudio property helpers.
enum CA {
    static let virtualDeviceUID = "AnalogAudioDevice_UID"

    static func address(_ sel: AudioObjectPropertySelector,
                        _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    static func value<T: BitwiseCopyable>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector,
                         _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ initial: T) -> T? {
        var a = address(sel, scope)
        var v = initial
        var size = UInt32(MemoryLayout<T>.size)
        return AudioObjectGetPropertyData(id, &a, 0, nil, &size, &v) == noErr ? v : nil
    }

    static func array<T>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector,
                         _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ type: T.Type) -> [T] {
        var a = address(sel, scope)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return [] }
        let n = Int(size) / MemoryLayout<T>.stride
        let p = UnsafeMutablePointer<T>.allocate(capacity: n)
        defer { p.deallocate() }
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, p) == noErr else { return [] }
        return Array(UnsafeBufferPointer(start: p, count: Int(size) / MemoryLayout<T>.stride))
    }

    static func string(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
        var a = address(sel)
        var s: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, &s) == noErr, let s else { return nil }
        return s.takeRetainedValue() as String
    }

    static func set<T: BitwiseCopyable>(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector,
                       _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal, _ value: T) -> OSStatus {
        var a = address(sel, scope)
        var v = value
        return AudioObjectSetPropertyData(id, &a, 0, nil, UInt32(MemoryLayout<T>.size), &v)
    }

    // MARK: Devices

    static func allDevices() -> [AudioObjectID] {
        array(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, AudioObjectID.self)
    }

    static func uid(_ id: AudioObjectID) -> String? { string(id, kAudioDevicePropertyDeviceUID) }
    static func name(_ id: AudioObjectID) -> String { string(id, kAudioObjectPropertyName) ?? "Unknown device" }

    static func device(uid: String) -> AudioObjectID? {
        allDevices().first { self.uid($0) == uid }
    }

    static func hasOutput(_ id: AudioObjectID) -> Bool {
        !array(id, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, AudioStreamID.self).isEmpty
    }

    static func isAlive(_ id: AudioObjectID) -> Bool {
        (value(id, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, UInt32(0)) ?? 0) != 0
    }

    // MARK: Defaults

    static func defaultOutput() -> AudioObjectID? {
        value(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, AudioObjectID(0))
    }

    static func setDefaultOutput(_ id: AudioObjectID) {
        let system = AudioObjectID(kAudioObjectSystemObject)
        _ = set(system, kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, id)
        _ = set(system, kAudioHardwarePropertyDefaultSystemOutputDevice, kAudioObjectPropertyScopeGlobal, id)
    }

    // MARK: Format

    static func nominalRate(_ id: AudioObjectID) -> Double {
        value(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, Float64(0)) ?? 0
    }

    static func supportedRates(_ id: AudioObjectID) -> [Double] {
        array(id, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, AudioValueRange.self)
            .filter { $0.mMinimum == $0.mMaximum }
            .map { $0.mMinimum }
    }

    static func supports(_ id: AudioObjectID, rate: Double) -> Bool {
        array(id, kAudioDevicePropertyAvailableNominalSampleRates, kAudioObjectPropertyScopeGlobal, AudioValueRange.self)
            .contains { $0.mMinimum <= rate && rate <= $0.mMaximum }
    }

    /// Sets the nominal rate and waits (briefly) for the device to report it.
    @discardableResult
    static func setNominalRate(_ id: AudioObjectID, _ rate: Double) -> Bool {
        if nominalRate(id) == rate { return true }
        guard set(id, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, Float64(rate)) == noErr else { return false }
        for _ in 0..<40 {
            if nominalRate(id) == rate { return true }
            usleep(25_000)
        }
        return nominalRate(id) == rate
    }

    /// Picks the deepest integer physical format at the current rate for each output stream
    /// (24-bit on the QUAD), keeping its channel count. Float-only devices are left alone.
    static func useDeepestIntegerFormat(_ id: AudioObjectID) -> Int? {
        let rate = nominalRate(id)
        var deepest: Int?
        for stream in array(id, kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput, AudioStreamID.self) {
            guard let current = value(stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, AudioStreamBasicDescription())
            else { continue }
            let candidates = array(stream, kAudioStreamPropertyAvailablePhysicalFormats, kAudioObjectPropertyScopeGlobal, AudioStreamRangedDescription.self)
                .map(\.mFormat)
                .filter {
                    $0.mFormatID == kAudioFormatLinearPCM
                        && $0.mFormatFlags & kAudioFormatFlagIsFloat == 0
                        && $0.mChannelsPerFrame == current.mChannelsPerFrame
                        && ($0.mSampleRate == rate || $0.mSampleRate == 0)
                }
            guard var best = candidates.max(by: { $0.mBitsPerChannel < $1.mBitsPerChannel }) else {
                deepest = max(deepest ?? 0, Int(current.mBitsPerChannel))
                continue
            }
            best.mSampleRate = rate
            if best.mBitsPerChannel != current.mBitsPerChannel || best.mFormatFlags != current.mFormatFlags {
                _ = set(stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, best)
            }
            let now = value(stream, kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, AudioStreamBasicDescription())
            deepest = max(deepest ?? 0, Int(now?.mBitsPerChannel ?? best.mBitsPerChannel))
        }
        return deepest
    }

    // MARK: Listening

    static func listen(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector, queue: DispatchQueue = .main,
                       _ block: @escaping () -> Void) {
        var a = address(sel)
        AudioObjectAddPropertyListenerBlock(id, &a, queue) { _, _ in block() }
    }
}

/// An output device the user can send audio to.
struct OutputDevice: Equatable {
    let id: AudioObjectID
    let uid: String
    let name: String

    static func all() -> [OutputDevice] {
        CA.allDevices().compactMap { id in
            guard CA.hasOutput(id), let uid = CA.uid(id), uid != CA.virtualDeviceUID else { return nil }
            return OutputDevice(id: id, uid: uid, name: CA.name(id))
        }
    }
}

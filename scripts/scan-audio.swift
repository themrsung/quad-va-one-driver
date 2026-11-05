// Prints every output device's formats and controls, as CoreAudio reports them.
// swift scripts/scan-audio.swift [name-filter]
import CoreAudio
import Foundation

func addr(_ sel: AudioObjectPropertySelector, _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal,
          _ el: AudioObjectPropertyElement = kAudioObjectPropertyElementMain) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: el)
}
func string(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
    var a = addr(sel); var s: CFString? = nil; var size = UInt32(MemoryLayout<CFString?>.size)
    return AudioObjectGetPropertyData(id, &a, 0, nil, &size, &s) == noErr ? s as String? : nil
}
func array<T>(_ id: AudioObjectID, _ a0: AudioObjectPropertyAddress, _ t: T.Type) -> [T] {
    var a = a0; var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(id, &a, 0, nil, &size) == noErr, size > 0 else { return [] }
    let n = Int(size) / MemoryLayout<T>.stride
    let p = UnsafeMutablePointer<T>.allocate(capacity: n); defer { p.deallocate() }
    guard AudioObjectGetPropertyData(id, &a, 0, nil, &size, p) == noErr else { return [] }
    return Array(UnsafeBufferPointer(start: p, count: n))
}
func control(_ id: AudioObjectID, _ sel: AudioObjectPropertySelector, _ el: UInt32) -> String {
    var a = addr(sel, kAudioDevicePropertyScopeOutput, el)
    guard AudioObjectHasProperty(id, &a) else { return "none" }
    var settable: DarwinBoolean = false
    AudioObjectIsPropertySettable(id, &a, &settable)
    return settable.boolValue ? "settable" : "read-only"
}
let filter = CommandLine.arguments.dropFirst().first?.lowercased()
for dev in array(AudioObjectID(kAudioObjectSystemObject), addr(kAudioHardwarePropertyDevices), AudioDeviceID.self) {
    let streams = array(dev, addr(kAudioDevicePropertyStreams, kAudioDevicePropertyScopeOutput), AudioStreamID.self)
    guard !streams.isEmpty else { continue }
    let name = string(dev, kAudioObjectPropertyName) ?? "?"
    if let f = filter, !name.lowercased().contains(f) { continue }
    print("== \(name)")
    print("   UID: \(string(dev, kAudioDevicePropertyDeviceUID) ?? "?")")
    print("   transport: \(array(dev, addr(kAudioDevicePropertyTransportType), UInt32.self).first.map { String(format: "%c%c%c%c", ($0 >> 24) & 255, ($0 >> 16) & 255, ($0 >> 8) & 255, $0 & 255) } ?? "?")")
    let rates = array(dev, addr(kAudioDevicePropertyAvailableNominalSampleRates), AudioValueRange.self)
    print("   sample rates: \(rates.map { $0.mMinimum == $0.mMaximum ? "\(Int($0.mMinimum))" : "\(Int($0.mMinimum))–\(Int($0.mMaximum))" }.joined(separator: ", "))")
    print("   buffer frame range: \(array(dev, addr(kAudioDevicePropertyBufferFrameSizeRange), AudioValueRange.self).first.map { "\(Int($0.mMinimum))–\(Int($0.mMaximum))" } ?? "?")")
    print("   latency (frames): device \(array(dev, addr(kAudioDevicePropertyLatency, kAudioDevicePropertyScopeOutput), UInt32.self).first ?? 0), safety offset \(array(dev, addr(kAudioDevicePropertySafetyOffset, kAudioDevicePropertyScopeOutput), UInt32.self).first ?? 0)")
    for (i, s) in streams.enumerated() {
        let phys = array(s, addr(kAudioStreamPropertyAvailablePhysicalFormats), AudioStreamRangedDescription.self)
        let cur = array(s, addr(kAudioStreamPropertyPhysicalFormat), AudioStreamBasicDescription.self).first
        func desc(_ f: AudioStreamBasicDescription) -> String {
            let float = f.mFormatFlags & kAudioFormatFlagIsFloat != 0
            return "\(Int(f.mSampleRate)) Hz, \(f.mChannelsPerFrame) ch, \(f.mBitsPerChannel)-bit \(float ? "float" : "int")"
        }
        if let cur { print("   stream \(i) physical format now: \(desc(cur))") }
        let uniq = Array(Set(phys.map { desc($0.mFormat) })).sorted()
        print("   stream \(i) physical formats: \(uniq.joined(separator: " | "))")
    }
    for (label, el) in [("main", UInt32(0)), ("ch 1", UInt32(1)), ("ch 2", UInt32(2))] {
        print("   \(label): volume \(control(dev, kAudioDevicePropertyVolumeScalar, el)), mute \(control(dev, kAudioDevicePropertyMute, el))")
    }
}

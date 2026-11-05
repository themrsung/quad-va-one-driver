import AppKit
import CoreAudio

/// Keeps the engine running between the virtual device and the chosen output, through device
/// changes, sample rate changes, unplugging and sleep.
final class Router {
    enum State: Equatable {
        case starting
        case running(output: String, rate: Double, bits: Int?)
        case driverMissing
        case noOutputChosen
        case outputMissing(name: String)
        case brokerMissing
        case failed(String)
    }

    private(set) var state: State = .starting { didSet { if state != oldValue { onChange?() } } }
    var onChange: (() -> Void)?

    private struct Config: Equatable { let virtualID, outputID: AudioObjectID; let rate: Double }
    private var running: Config?
    private var timer: Timer?
    private var ticks = 0
    private var tookOverDefault = false

    private static weak var current: Router?

    func start() {
        Router.current = self
        aad_link_set_lost_handler { Router.current?.restart() }
        Settings.chooseInitialOutput()
        CA.listen(AudioObjectID(kAudioObjectSystemObject), kAudioHardwarePropertyDevices) { [weak self] in
            self?.check()
        }
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { [weak self] _ in
            DispatchQueue.main.asyncAfter(deadline: .now() + 2) { self?.restart() }
        }
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
        restart()
    }

    func stop() {
        aad_engine_stop()
        running = nil
        // Leave the system output somewhere audible.
        if let virt = CA.device(uid: CA.virtualDeviceUID), CA.defaultOutput() == virt,
           let uid = Settings.outputUID, let out = CA.device(uid: uid) {
            CA.setDefaultOutput(out)
        }
    }

    /// Settings changed in the menu.
    func apply() {
        if let uid = Settings.outputUID, let name = Settings.outputName {
            aad_engine_set_pulse(Settings.pulse(uid: uid, name: name) ? 1 : 0)
        }
        aad_engine_set_comfort(Settings.mode == .comfort ? 1 : 0, Settings.preset.params)
        onChange?()
    }

    func selectOutput(_ device: OutputDevice) {
        Settings.outputUID = device.uid
        Settings.outputName = device.name
        restart()
    }

    var virtualIsDefault: Bool {
        guard let virt = CA.device(uid: CA.virtualDeviceUID) else { return false }
        return CA.defaultOutput() == virt
    }

    func makeVirtualDefault() {
        if let virt = CA.device(uid: CA.virtualDeviceUID) { CA.setDefaultOutput(virt) }
        onChange?()
    }

    func status() -> AADStatus { aad_engine_status() }

    // MARK: -

    func restart() {
        aad_engine_stop()
        running = nil
        configure()
    }

    private func tick() {
        aad_engine_control_tick(1)
        ticks += 1
        if running == nil {
            if ticks % 5 == 0 { configure() } // retry, gently
        } else {
            check()
        }
    }

    /// Reconfigures if anything the running engine depends on has changed.
    private func check() {
        guard let cfg = running else { configure(); return }
        let virt = CA.device(uid: CA.virtualDeviceUID)
        let out = Settings.outputUID.flatMap { CA.device(uid: $0) }
        if virt != cfg.virtualID || out != cfg.outputID || !CA.isAlive(cfg.outputID)
            || CA.nominalRate(cfg.virtualID) != cfg.rate || CA.nominalRate(cfg.outputID) != cfg.rate {
            restart()
        }
    }

    private func configure() {
        guard let virt = CA.device(uid: CA.virtualDeviceUID) else { state = .driverMissing; return }
        guard let shared = aad_link() else { state = .brokerMissing; return }

        if !tookOverDefault {
            tookOverDefault = true
            if CA.defaultOutput() != virt { CA.setDefaultOutput(virt) }
        }
        guard let uid = Settings.outputUID else { state = .noOutputChosen; return }
        guard let out = CA.device(uid: uid), CA.isAlive(out) else {
            state = .outputMissing(name: Settings.outputName ?? uid)
            return
        }
        Settings.outputName = CA.name(out)

        // Same rate on both sides, so nothing is resampled. The virtual device follows the output
        // when the output can't do the virtual device's current rate.
        var rate = CA.nominalRate(virt)
        if !CA.supports(out, rate: rate) {
            let preferred: [Double] = [48000, 44100, 96000, 88200, 192000, 176400]
            guard let common = preferred.first(where: { CA.supports(out, rate: $0) && CA.supports(virt, rate: $0) }) else {
                state = .failed("No sample rate in common with \(CA.name(out))")
                return
            }
            CA.setNominalRate(virt, common)
            rate = CA.nominalRate(virt)
        }
        guard CA.setNominalRate(out, rate) else {
            state = .failed("Couldn't set \(CA.name(out)) to \(Int(rate)) Hz")
            return
        }
        let bits = CA.useDeepestIntegerFormat(out)

        let err = aad_engine_start(virt, out, shared)
        guard err == noErr else {
            state = .failed("Couldn't start audio (\(fourCC(err)))")
            return
        }
        running = Config(virtualID: virt, outputID: out, rate: rate)
        apply()
        state = .running(output: CA.name(out), rate: rate, bits: bits)
    }

    private func fourCC(_ s: OSStatus) -> String {
        let n = UInt32(bitPattern: s)
        let chars = [24, 16, 8, 0].map { Character(UnicodeScalar(UInt8((n >> $0) & 0xFF))) }
        return chars.allSatisfy { $0.isLetter || $0.isNumber || $0 == " " || $0 == "!" } ? "'\(String(chars))'" : "\(s)"
    }
}

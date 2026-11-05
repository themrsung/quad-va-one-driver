import Foundation

/// Everything the user chooses in the menu, kept in UserDefaults.
enum Settings {
    private static let d = UserDefaults.standard

    enum Mode: String { case passthrough, comfort }

    static var mode: Mode {
        get {
            let raw = d.string(forKey: "mode") ?? ""
            return raw == "enhance" ? .comfort : Mode(rawValue: raw) ?? .passthrough // "enhance": before the rename
        }
        set { d.set(newValue.rawValue, forKey: "mode") }
    }

    static var preset: Preset {
        get { Presets.preset(id: d.string(forKey: "preset") ?? "") ?? Presets.fallback }
        set { d.set(newValue.id, forKey: "preset") }
    }

    /// The downstream device, by UID, and its name for when it's unplugged.
    static var outputUID: String? {
        get { d.string(forKey: "outputUID") }
        set { d.set(newValue, forKey: "outputUID") }
    }

    static var outputName: String? {
        get { d.string(forKey: "outputName") }
        set { d.set(newValue, forKey: "outputName") }
    }

    /// Keep-alive pulse, per output device. On by default for QUAD devices only.
    static func pulse(uid: String, name: String) -> Bool {
        let all = d.dictionary(forKey: "pulse") as? [String: Bool] ?? [:]
        return all[uid] ?? name.localizedCaseInsensitiveContains("QUAD")
    }

    static func setPulse(_ on: Bool, uid: String) {
        var all = d.dictionary(forKey: "pulse") as? [String: Bool] ?? [:]
        all[uid] = on
        d.set(all, forKey: "pulse")
    }

    /// First launch: the QUAD if there is one, otherwise whatever the system output is now.
    static func chooseInitialOutput() {
        guard outputUID == nil else { return }
        let devices = OutputDevice.all()
        let pick = devices.first { $0.name.localizedCaseInsensitiveContains("QUAD") }
            ?? CA.defaultOutput().flatMap { id in devices.first { $0.id == id } }
        if let pick {
            outputUID = pick.uid
            outputName = pick.name
        }
    }
}

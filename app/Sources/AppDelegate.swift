import AppKit

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let router = Router()
    private var statusItem: NSStatusItem!
    private let menu = NSMenu()

    func applicationDidFinishLaunching(_ notification: Notification) {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        menu.delegate = self
        statusItem.menu = menu
        router.onChange = { [weak self] in self?.updateIcon() }
        router.start()
        updateIcon()
    }

    func applicationWillTerminate(_ notification: Notification) {
        router.stop()
    }

    // MARK: Icon

    private func updateIcon() {
        let symbol: String
        switch router.state {
        case .running: symbol = Settings.mode == .comfort ? "hifispeaker.fill" : "hifispeaker"
        case .starting: symbol = "hifispeaker"
        default: symbol = "exclamationmark.triangle"
        }
        let image = NSImage(systemSymbolName: symbol, accessibilityDescription: "Analog Audio")
        image?.isTemplate = true
        statusItem.button?.image = image
    }

    // MARK: Menu

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()

        for line in statusLines() {
            let item = NSMenuItem(title: line, action: nil, keyEquivalent: "")
            item.isEnabled = false
            menu.addItem(item)
        }
        if case .running = router.state, !router.virtualIsDefault {
            menu.addItem(item("Send System Audio Through Analog audio device", #selector(makeDefault)))
        }
        menu.addItem(.separator())

        // Mode
        let passthrough = item("Passthrough", #selector(choosePassthrough))
        passthrough.state = Settings.mode == .passthrough ? .on : .off
        menu.addItem(passthrough)

        let comfort = NSMenuItem(title: Settings.mode == .comfort ? "Comfort: \(Settings.preset.title)" : "Comfort",
                                 action: nil, keyEquivalent: "")
        comfort.state = Settings.mode == .comfort ? .on : .off
        let profiles = NSMenu()
        profiles.addItem(presetItem(.plain))
        profiles.addItem(.separator())
        for group in Presets.groups {
            let groupItem = NSMenuItem(title: group, action: nil, keyEquivalent: "")
            let presets = NSMenu()
            for preset in Presets.inGroup(group) { presets.addItem(presetItem(preset)) }
            if Settings.mode == .comfort && Settings.preset.group == group { groupItem.state = .on }
            groupItem.submenu = presets
            profiles.addItem(groupItem)
        }
        comfort.submenu = profiles
        menu.addItem(comfort)
        menu.addItem(.separator())

        // Output
        let devices = OutputDevice.all()
        let chosen = Settings.outputUID
        let outputTitle = devices.first { $0.uid == chosen }?.name ?? Settings.outputName.map { "\($0) (missing)" } ?? "none"
        let output = NSMenuItem(title: "Output: \(outputTitle)", action: nil, keyEquivalent: "")
        let outputs = NSMenu()
        for device in devices {
            let d = item(device.name, #selector(chooseOutput(_:)))
            d.representedObject = device.uid
            d.state = device.uid == chosen ? .on : .off
            outputs.addItem(d)
        }
        if let chosen, !devices.contains(where: { $0.uid == chosen }) {
            let missing = NSMenuItem(title: "\(Settings.outputName ?? chosen) (missing)", action: nil, keyEquivalent: "")
            missing.state = .on
            missing.isEnabled = false
            outputs.addItem(missing)
        }
        output.submenu = outputs
        menu.addItem(output)

        if let uid = chosen {
            let name = Settings.outputName ?? ""
            let pulse = item("Keep-Alive Pulse for This Output", #selector(togglePulse))
            pulse.state = Settings.pulse(uid: uid, name: name) ? .on : .off
            menu.addItem(pulse)
        }
        menu.addItem(.separator())

        let login = item("Start at Login", #selector(toggleLogin))
        login.state = LoginItem.isEnabled ? .on : .off
        menu.addItem(login)
        menu.addItem(item("Quit Analog Audio", #selector(quit), key: "q"))
    }

    private func statusLines() -> [String] {
        switch router.state {
        case .starting: return ["Starting…"]
        case .driverMissing: return ["Analog audio device isn't installed"]
        case .noOutputChosen: return ["Choose an output device"]
        case .outputMissing(let name): return ["\(name) isn't connected"]
        case .brokerMissing: return ["The broker isn't running (reinstall with ./install.sh)"]
        case .failed(let why): return [why]
        case let .running(output, rate, bits):
            let s = router.status()
            var first = "→ \(output) · \(formatRate(rate))"
            if let bits { first += " · \(bits)-bit" }
            let ms = s.sampleRate > 0 ? s.fillFrames / s.sampleRate * 1000 : 0
            let ppm = (s.rateAdjust - 1) * 1e6
            var second = String(format: "buffer %.1f ms · clock %+.0f ppm", ms, ppm)
            if s.underruns > 0 { second += " · \(s.underruns) dropouts" }
            if s.pulsing != 0 { second += " · pulse" }
            return [first, second]
        }
    }

    private func formatRate(_ rate: Double) -> String {
        rate.truncatingRemainder(dividingBy: 1000) == 0 ? "\(Int(rate / 1000)) kHz" : String(format: "%.1f kHz", rate / 1000)
    }

    private func presetItem(_ preset: Preset) -> NSMenuItem {
        let p = item(preset.name, #selector(choosePreset(_:)))
        p.representedObject = preset.id
        p.state = Settings.mode == .comfort && Settings.preset.id == preset.id ? .on : .off
        return p
    }

    private func item(_ title: String, _ action: Selector, key: String = "") -> NSMenuItem {
        let i = NSMenuItem(title: title, action: action, keyEquivalent: key)
        i.target = self
        return i
    }

    // MARK: Actions

    @objc private func choosePassthrough() {
        Settings.mode = .passthrough
        router.apply()
    }

    @objc private func choosePreset(_ sender: NSMenuItem) {
        guard let id = sender.representedObject as? String, let preset = Presets.preset(id: id) else { return }
        Settings.preset = preset
        Settings.mode = .comfort
        router.apply()
    }

    @objc private func chooseOutput(_ sender: NSMenuItem) {
        guard let uid = sender.representedObject as? String,
              let device = OutputDevice.all().first(where: { $0.uid == uid }) else { return }
        router.selectOutput(device)
    }

    @objc private func togglePulse() {
        guard let uid = Settings.outputUID else { return }
        Settings.setPulse(!Settings.pulse(uid: uid, name: Settings.outputName ?? ""), uid: uid)
        router.apply()
    }

    @objc private func makeDefault() { router.makeVirtualDefault() }

    @objc private func toggleLogin() { LoginItem.setEnabled(!LoginItem.isEnabled) }

    @objc private func quit() { NSApp.terminate(nil) }
}

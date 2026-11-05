/// Comfort profiles. Every one applies the gain curve in Engine/dsp.h; "plain" is only that, the
/// rest add expansion. Levels are RMS dBFS; see `AADPreset` for what each field does.
/// First-pass tuning: upward expansion restores some of what the master's compression took away,
/// with makeup gain set so the boost lands under the limiter instead of into it.
struct Preset {
    let group: String
    let name: String
    let params: AADPreset

    var id: String { group.isEmpty ? name : "\(group)/\(name)" }
    var title: String { group.isEmpty ? name : "\(group) / \(name)" }

    /// The gain curve alone.
    static let plain = Preset(group: "", name: "plain", params: AADPreset(
        expand: 0, upThresholdDb: 0, upRatio: 1, maxBoostDb: 0, downThresholdDb: -100, downRatio: 1,
        attackMs: 10, releaseMs: 100, makeupDb: 0))

    private init(group: String, name: String, params: AADPreset) {
        self.group = group
        self.name = name
        self.params = params
    }

    init(_ group: String, _ name: String,
         up: Float, ratio: Float, maxBoost: Float,
         down: Float = -60, downRatio: Float = 1,
         attack: Float, release: Float, makeup: Float) {
        self.group = group
        self.name = name
        params = AADPreset(expand: 1, upThresholdDb: up, upRatio: ratio, maxBoostDb: maxBoost,
                           downThresholdDb: down, downRatio: downRatio,
                           attackMs: attack, releaseMs: release, makeupDb: makeup)
    }
}

enum Presets {
    static let groups = ["music", "film", "speech", "experimental"]

    static let all: [Preset] = [
        Preset.plain,
        // Music: pop is mastered loudest, so it gets the most expansion; early music the least.
        Preset("music", "k-pop", up: -22, ratio: 1.35, maxBoost: 5, down: -50, downRatio: 1.3, attack: 10, release: 150, makeup: -5),
        Preset("music", "j-pop", up: -22, ratio: 1.30, maxBoost: 4.5, down: -50, downRatio: 1.3, attack: 10, release: 150, makeup: -4.5),
        Preset("music", "us-pop", up: -20, ratio: 1.30, maxBoost: 4, down: -50, downRatio: 1.2, attack: 10, release: 120, makeup: -4),
        Preset("music", "global-pop", up: -20, ratio: 1.25, maxBoost: 4, down: -50, downRatio: 1.2, attack: 10, release: 150, makeup: -4),
        Preset("music", "modern", up: -24, ratio: 1.20, maxBoost: 4, down: -55, downRatio: 1.2, attack: 12, release: 200, makeup: -3.5),
        Preset("music", "classical", up: -30, ratio: 1.10, maxBoost: 3, down: -55, downRatio: 1.2, attack: 30, release: 400, makeup: -2),
        Preset("music", "baroque", up: -28, ratio: 1.12, maxBoost: 3, down: -58, downRatio: 1.15, attack: 15, release: 250, makeup: -2.5),
        Preset("music", "pre-baroque", up: -30, ratio: 1.08, maxBoost: 2, down: -60, downRatio: 1.1, attack: 25, release: 400, makeup: -1.5),
        // Film: TV mixes are flattened for living rooms; cinema mixes keep more range already.
        Preset("film", "tv", up: -26, ratio: 1.20, maxBoost: 4, down: -55, downRatio: 1.2, attack: 15, release: 250, makeup: -3.5),
        Preset("film", "cinema", up: -30, ratio: 1.30, maxBoost: 6, down: -55, downRatio: 1.2, attack: 10, release: 300, makeup: -5),
        // Speech: light upward expansion, and a downward expander that pushes room noise down.
        Preset("speech", "podcast", up: -24, ratio: 1.10, maxBoost: 2, down: -45, downRatio: 2.0, attack: 5, release: 120, makeup: -1.5),
        Preset("speech", "news", up: -24, ratio: 1.08, maxBoost: 2, down: -48, downRatio: 1.6, attack: 5, release: 150, makeup: -1.5),
        // Experimental: deliberately exaggerated.
        Preset("experimental", "propaganda", up: -26, ratio: 1.5, maxBoost: 8, down: -40, downRatio: 2.0, attack: 3, release: 80, makeup: -6),
        Preset("experimental", "war-cry", up: -20, ratio: 1.8, maxBoost: 10, down: -45, downRatio: 1.5, attack: 1, release: 60, makeup: -8),
        Preset("experimental", "dramatic", up: -32, ratio: 1.5, maxBoost: 9, down: -50, downRatio: 1.4, attack: 20, release: 500, makeup: -7),
        Preset("experimental", "tragic", up: -34, ratio: 1.25, maxBoost: 6, down: -48, downRatio: 1.8, attack: 40, release: 900, makeup: -5),
        Preset("experimental", "super-hdr", up: -30, ratio: 2.0, maxBoost: 14, down: -50, downRatio: 2.0, attack: 2, release: 200, makeup: -12),
    ]

    static func preset(id: String) -> Preset? { all.first { $0.id == id } }
    static func inGroup(_ group: String) -> [Preset] { all.filter { $0.group == group } }
    static let fallback = Preset.plain
}

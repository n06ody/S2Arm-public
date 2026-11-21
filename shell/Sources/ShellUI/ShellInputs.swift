// S2Arm modification: expose the host callback for persistent game reordering.
import Foundation
import CoreGraphics

/// The caller supplies display data. These types never load, validate, or store game data.
public enum GameState: Equatable, Sendable {
    case playable
    case needsGameData
    case needsAppUpdate
    /// The game's program on this device does not match this app (built for another version, signed from another computer or Apple ID,
    /// or installed only partly). One state for all three: the fix is the same, install this game again from the computer.
    case needsGameUpdate
    /// Installed with S2Arm 0.5 (the program lived inside that app; installing the 0.6 app replaced it): saves and resources are still here,
    /// the computer pushes this game's program once and it plays. Its own state so the card can say why and what to do.
    case needsLegacyUpdate
    /// S2Arm 0.6.0: this game is signed on this iPhone before it opens, and the iPhone has no certificate that can sign it.
    /// Its primary action gets one right here (`ShellActions.onGetSigningCert`); once imported the game is playable again.
    case needsCertificate(CertificateNeed)
    /// S2Arm 0.6.0: the game's data is on this iPhone but its program (the game's .s2arm file) isn't yet, such as when only the
    /// .img was put in. Its primary action picks the .s2arm here (`ShellActions.onImportProgram`).
    case needsProgram
    /// `reason` is one short line for under the title, such as "Game files damaged". `detail` is a plain sentence
    /// for the player (what happened, what to do), shown under the title and in the How to fix sheet. `technical`
    /// is the underlying message, shown only as small secondary Details in the sheet.
    case broken(reason: String, detail: String = "", technical: String = "")
    /// On its way from the computer: copied over the cable, then unpacked here. Its cover is greyed until it can be played.
    case installing(InstallStage)
}

/// Why a game needs a signing certificate on this iPhone. Each has its own words; the fix is the same: get the
/// certificate of the app that installed S2Arm (AltStore or SideStore) and import it here.
public enum CertificateNeed: Equatable, Sendable {
    /// No certificate on this iPhone yet.
    case missing
    /// Only certificates of another team (Apple ID) than the one S2Arm was installed with.
    case otherTeam
    /// The certificate S2Arm was installed with has been replaced or revoked, so the one here no longer works.
    case replaced
    /// Signed in a way this iPhone doesn't accept, and there is no certificate here to sign it again.
    case oldSignature
}

/// Where an arriving game is. `fraction` (0...1) is how far this phase has got; nil when it is not known yet.
public struct InstallStage: Equatable, Sendable {
    public enum Phase: Equatable, Sendable {
        /// Being copied from the computer: the cable must stay connected.
        case transferring
        /// Copied; being unpacked on this device.
        case installing
        /// The copying stopped part way, such as when the cable came out. Connecting the computer again continues it.
        case interrupted
        /// The unpacking did not finish. Play tries it again.
        case failed
    }
    public var phase: Phase
    public var fraction: Double?
    /// While installing, which part is under way (single-file game data): 1 unpacking what was sent, 2 taking the small files
    /// out of the game data file, 3 checking the files on this device; 4 waiting for another game to finish installing, 5 the host is
    /// organizing the game folders (S2Arm). 0: not told, shown as plain Installing.
    public var step: Int
    /// Why the install did not finish, in one plain sentence from the host; nil: the general explanation.
    public var reason: String?
    public init(_ phase: Phase, fraction: Double? = nil, step: Int = 0, reason: String? = nil) {
        self.phase = phase; self.fraction = fraction.map { min(1, max(0, $0)) }; self.step = step; self.reason = reason
    }
}

/// The version's "What's new" (0.6.0 on, decided 11-19): the pages docs/site/feature.json marks for iPhone, in the player's
/// language, the same words and pictures as the computer's. The host reads it from the app (Feature/feature.json).
public struct FeatureIntro: Equatable, Sendable {
    public struct Page: Equatable, Sendable {
        public var picture: Artwork?
        public var kicker: String, title: String, what: String, how: String, whereText: String
        /// Column headings this page renames (what / how / where → its own words, such as "Good to know").
        public var labels: [String: String]
        public init(picture: Artwork?, kicker: String, title: String, what: String, how: String, whereText: String, labels: [String: String] = [:]) {
            self.picture = picture; self.kicker = kicker; self.title = title; self.what = what; self.how = how; self.whereText = whereText; self.labels = labels
        }
    }
    public var id: String
    public var badge: String
    public var pages: [Page]
    public init(id: String, badge: String, pages: [Page]) { self.id = id; self.badge = badge; self.pages = pages }
}

/// The game's saved moment (instant snapshot, 0.6.0): when it was saved and the picture of that moment. One per game.
public struct SavedMoment: Equatable, Sendable {
    public var savedAt: Date
    /// The frame at the saved moment (thumb.png, else the full frame). Nil when the snapshot has no picture.
    public var picture: Artwork?
    /// The space it takes on this device, in bytes (what deleting it frees).
    public var bytes: Int64
    public init(savedAt: Date, picture: Artwork? = nil, bytes: Int64 = 0) { self.savedAt = savedAt; self.picture = picture; self.bytes = bytes }
}

public struct GameTile: Identifiable, Equatable, Sendable {
    public var id: String
    public var title: String
    /// One short line under the title, such as the version.
    public var subtitle: String
    /// The game's 16:9 key art, such as its eShop banner. Covers, Cover Flow cards and the hero all use it.
    public var banner: Artwork?
    /// The square NACP icon. Only a fallback without a banner: shown whole inside a 16:9 card, never stretched.
    public var icon: Artwork?
    public var state: GameState
    /// Already localized, such as "Played today". Nil leaves the game out of Recently played.
    public var recentlyPlayed: String?
    /// Installed (or updated) and not opened since: a blue dot before its name, as iOS marks a new app.
    public var isNew: Bool
    /// S-grade enhancements are built into this game (community patches / cheats in its cpatch.tsv): the cover lights a gold S (v0.4.0).
    public var sgrade: Bool
    /// A complete instant snapshot to go back to: the library lists it under "Continue from this moment". Nil: none.
    public var moment: SavedMoment?
    public init(id: String, title: String, subtitle: String = "", banner: Artwork? = nil, icon: Artwork? = nil,
                state: GameState = .playable, recentlyPlayed: String? = nil, isNew: Bool = false, sgrade: Bool = false, moment: SavedMoment? = nil) {
        self.id = id
        self.title = title
        self.subtitle = subtitle
        self.banner = banner
        self.icon = icon
        self.state = state
        self.recentlyPlayed = recentlyPlayed
        self.isNew = isNew
        self.sgrade = sgrade
        self.moment = moment
    }
}

public struct SettingOption: Identifiable, Equatable, Sendable {
    public var id: String
    public var label: String
    /// The label is a text of the game's launcher.json: it is shown in the player's language when the catalog has it.
    public init(_ value: String, _ label: String) { self.init(value, label, in: ShellStrings.bundle) }
    init(_ value: String, _ label: String, in bundle: Bundle) { id = value; self.label = ShellStrings.setting(label, bundle: bundle) }
}

public enum SettingKind: Equatable, Sendable {
    case toggle(on: String, off: String)
    case choice([SettingOption])
    case range(min: Double, max: Double, step: Double, unit: String)
}

public struct SettingRow: Identifiable, Equatable, Sendable {
    public var id: String
    public var label: String
    public var note: String
    public var kind: SettingKind
    public var value: String
    /// The caller resolves dependencies and locks before supplying rows.
    public var enabled: Bool
    /// The value is the shell settings' choice, because the game has none of its own.
    public var fromShell: Bool
    /// Label and note are texts of the game's launcher.json: shown in the player's language when the catalog has them.
    public init(id: String, label: String, note: String = "", kind: SettingKind, value: String, enabled: Bool = true, fromShell: Bool = false) {
        self.init(id: id, label: label, note: note, kind: kind, value: value, enabled: enabled, fromShell: fromShell, in: ShellStrings.bundle)
    }
    init(id: String, label: String, note: String = "", kind: SettingKind, value: String, enabled: Bool = true, fromShell: Bool = false, in bundle: Bundle) {
        self.id = id
        self.label = ShellStrings.setting(label, bundle: bundle)
        self.note = ShellStrings.setting(note, bundle: bundle)
        self.kind = kind
        self.value = value
        self.enabled = enabled
        self.fromShell = fromShell
    }
}

public struct SettingSection: Identifiable, Equatable, Sendable {
    public var id: String
    public var title: String
    public var rows: [SettingRow]
    /// The id stays the section's name in launcher.json, the title is that name in the player's language when the catalog has it.
    public init(id: String, title: String, rows: [SettingRow]) { self.init(id: id, title: title, rows: rows, in: ShellStrings.bundle) }
    init(id: String, title: String, rows: [SettingRow], in bundle: Bundle) {
        self.id = id
        self.title = ShellStrings.setting(title, bundle: bundle)
        self.rows = rows
    }
}

public struct GameDetail: Identifiable, Equatable, Sendable {
    public var id: String { game.id }
    public var game: GameTile
    public var sections: [SettingSection]
    public var dataSource: String
    public var dataNote: String
    public var cloudSaves: String?
    public var cloudNote: String
    public var dlc: String
    public var hasModifiedSettings: Bool
    /// The data or cloud-saves folder is the shell settings' one, not one the player picked for this game.
    public var dataFromShell: Bool
    public var cloudFromShell: Bool
    /// The shell settings have a cloud-saves folder this game does not use now, so the game can follow it.
    public var cloudCanUseShell: Bool
    public init(game: GameTile, sections: [SettingSection] = [], dataSource: String = "", dataNote: String = "",
                cloudSaves: String? = nil, cloudNote: String = "", dlc: String = "", hasModifiedSettings: Bool = false,
                dataFromShell: Bool = false, cloudFromShell: Bool = false, cloudCanUseShell: Bool = false) {
        self.game = game
        self.sections = sections
        self.dataSource = dataSource
        self.dataNote = dataNote
        self.cloudSaves = cloudSaves
        self.cloudNote = cloudNote
        self.dlc = dlc
        self.hasModifiedSettings = hasModifiedSettings
        self.dataFromShell = dataFromShell
        self.cloudFromShell = cloudFromShell
        self.cloudCanUseShell = cloudCanUseShell
    }
}

public struct ControllerInfo: Identifiable, Equatable, Sendable {
    public var id: String
    public var name: String
    public var player: String
    /// Nil means the controller does not report battery level.
    public var batteryPercent: Int?
    public var charging: Bool
    public init(id: String, name: String, player: String, batteryPercent: Int? = nil, charging: Bool = false) {
        self.id = id
        self.name = name
        self.player = player
        self.batteryPercent = batteryPercent
        self.charging = charging
    }
}

/// Which kind of controller a family of glyphs belongs to, from GameController's `productCategory`.
/// The buttons are read by label, as on the Mac: GameController's buttonA is the cross on a PlayStation controller,
/// A at the bottom on an Xbox controller and A on the right on a Nintendo controller, so each confirms with its own.
public enum ControllerFamily: Equatable, Sendable {
    case xbox, playStation, nintendo, neutral
    public init(productCategory: String?) {
        let c = (productCategory ?? "").lowercased().replacingOccurrences(of: "-", with: "")
        if c.contains("dual") || c.contains("playstation") { self = .playStation }
        else if c.contains("xbox") { self = .xbox }
        else if c.contains("nintendo") || c.contains("switch") || c.contains("joycon") { self = .nintendo }
        else { self = .neutral }
    }
}

/// What sent the last input, so the hints show that device's own glyphs.
public enum InputDevice: Equatable, Sendable {
    case controller(ControllerFamily)
    case keyboard
}

/// One event from the host's input adapter: a press, or a button or direction let go. `sequence` orders them.
public struct ShellInput: Equatable, Sendable {
    public enum Action: Sendable { case up, down, left, right, activate, back, settings, controllers }
    public var sequence: Int
    public var action: Action
    public var device: InputDevice
    /// The button or direction of `action` came up. Nothing acts on it; it only restarts the window for the system's
    /// echo of a controller button (`SystemEchoWindow`).
    public var released: Bool
    public init(sequence: Int, action: Action, device: InputDevice = .controller(.neutral), released: Bool = false) {
        self.sequence = sequence
        self.action = action
        self.device = device
        self.released = released
    }
}

/// The host's input events in order, the last `kept` of them. The host appends each press and release; the library acts
/// on every event it has not seen yet. One value would keep only the last event written between two view updates, so a
/// press could be lost, or a release, which left a button counted as held (`SystemEchoWindow`).
public struct ShellInputLog: Equatable, Sendable {
    public static let kept = 64
    public private(set) var events: [ShellInput] = []
    public init() {}
    /// The sequence of the last event, 0 with none yet.
    public var last: Int { events.last?.sequence ?? 0 }
    public mutating func append(_ action: ShellInput.Action, device: InputDevice = .controller(.neutral), released: Bool = false) {
        events.append(ShellInput(sequence: last + 1, action: action, device: device, released: released))
        if events.count > Self.kept { events.removeFirst(events.count - Self.kept) }
    }
    /// The events after `sequence`, oldest first.
    public func since(_ sequence: Int) -> [ShellInput] { events.filter { $0.sequence > sequence } }
}

/// A controller element the button test lights up, named by its place on an Xbox controller (GameController's names).
public enum PadElement: String, CaseIterable, Sendable {
    case a, b, x, y, leftBumper, rightBumper, leftTrigger, rightTrigger, leftStick, rightStick, view, menu, home, up, down, left, right
}

/// A controller's whole state for the button test: what is held, the sticks from -1 to 1 (up is positive) and the
/// triggers from 0 to 1. The host sends it only while the test is open.
public struct PadSnapshot: Equatable, Sendable {
    public var name: String
    public var pressed: Set<PadElement>
    public var leftStick: CGPoint
    public var rightStick: CGPoint
    public var leftTrigger: Double
    public var rightTrigger: Double
    public init(name: String = "", pressed: Set<PadElement> = [], leftStick: CGPoint = .zero, rightStick: CGPoint = .zero,
                leftTrigger: Double = 0, rightTrigger: Double = 0) {
        self.name = name
        self.pressed = pressed
        self.leftStick = leftStick
        self.rightStick = rightStick
        self.leftTrigger = leftTrigger
        self.rightTrigger = rightTrigger
    }
}

/// All effects leave the package through these closures. The game ID is always explicit.
/// An optional closure the host leaves nil has no handler: the button for it is not shown, so nothing on screen does nothing.
public struct ShellActions {
    /// Inserts the dragged games before an ID, or at the end when the destination is nil.
    public var onReorderGames: (([String], String?) -> Bool)? = nil
    public var onSelect: (String) -> Void
    public var onPlay: (String) -> Void
    public var onChange: (_ gameID: String, _ settingID: String, _ value: String) -> Void
    public var onPickGameData: (String) -> Void
    public var onUseAppData: ((String) -> Void)?
    public var onPickCloudSaves: ((String) -> Void)?
    public var onDisableCloudSaves: ((String) -> Void)?
    public var onResetDefaults: ((String) -> Void)?
    public var onShowLog: ((String) -> Void)?
    public var onAppUpdate: (() -> Void)?
    public var onBack: () -> Void
    public var onPickShellData: (() -> Void)?
    public var onPickShellCloud: (() -> Void)?
    public var onUseShellCloud: ((String) -> Void)?
    public var onClearShellData: (() -> Void)?
    public var onClearShellCloud: (() -> Void)?
    /// S2Arm: settings → "Report a problem" (the host packs the logs, uploads them and shows a short code). Nil hides the row.
    public var onReportProblem: (() -> Void)?
    /// S2Arm: settings → "Signing certificate" (0.6.0): the certificate this iPhone signs imported games with ("" = none yet),
    /// importing a .p12 (the host picks the file and asks for its password) and getting SideStore's. All nil hides the group.
    public var signingCertStatus: (() -> String)? = nil
    public var onImportSigningCert: (() -> Void)? = nil
    public var onSideStoreSigningCert: (() -> Void)? = nil
    /// S2Arm: a game's "Get certificate…" (state `.needsCertificate`): SideStore when it is installed, otherwise picking a .p12.
    public var onGetSigningCert: (() -> Void)? = nil
    /// S2Arm: "Import .s2arm…" on a game that needs its program (`.needsProgram`, or an older game `.needsLegacyUpdate`): pick the file here.
    public var onImportProgram: ((String) -> Void)? = nil
    public var onGlobalPadMapChange: ((String, String) -> Void)?
    public var onGlobalPadMapReset: (() -> Void)?
    /// The button test opened (true) or closed: the host sends `PadSnapshot`s only in between.
    public var onButtonTest: (Bool) -> Void
    /// Deletes the game's data on this device and takes it off the list. `saves`: its saves go too.
    public var onDeleteGame: ((_ gameID: String, _ saves: Bool) -> Void)?
    /// Opens the game's save management (export / import saves) before it starts. nil: no item in the card's menu.
    public var onManageSaves: ((_ gameID: String) -> Void)?
    /// S2Arm: "Export this game…" on a playable game (0.6.3): the host writes its .s2arm and .img and hands them to the Files
    /// app. Nil: no item in the card's menu.
    public var onExportGame: ((_ gameID: String) -> Void)? = nil
    /// S2Arm: "Add to Home Screen…" on a playable game: after the library's three-step guide, the host opens the game's page
    /// in Safari, which the player adds to the Home Screen; its icon then opens the game directly. Nil: no item in the menu.
    public var onAddToHome: ((_ gameID: String) -> Void)? = nil
    /// Instant snapshot (0.6.0): whether this game has a complete snapshot to go back to, and going back to it (starts the
    /// game restored to that moment). Both nil: no snapshot item anywhere in the library.
    public var snapshotAvailable: ((_ gameID: String) -> Bool)? = nil
    public var onRestoreSnapshot: ((_ gameID: String) -> Void)? = nil
    /// The version's "What's new" for iPhone. The host shows it once over the library (after the first-run guide) and
    /// the library's settings offer it again; nil: this version has none, so no settings row either.
    public var featureIntro: FeatureIntro? = nil
    /// Deletes the game's saved moment and frees its space (the library asks the player first). Nil: no Delete item.
    public var onDeleteSnapshot: ((_ gameID: String) -> Void)? = nil
    public init(onSelect: @escaping (String) -> Void = { _ in }, onPlay: @escaping (String) -> Void = { _ in },
                onChange: @escaping (String, String, String) -> Void = { _, _, _ in },
                onPickGameData: @escaping (String) -> Void = { _ in }, onUseAppData: ((String) -> Void)? = nil,
                onPickCloudSaves: ((String) -> Void)? = nil, onDisableCloudSaves: ((String) -> Void)? = nil,
                onResetDefaults: ((String) -> Void)? = nil, onShowLog: ((String) -> Void)? = nil,
                onAppUpdate: (() -> Void)? = nil, onBack: @escaping () -> Void = {},
                onPickShellData: (() -> Void)? = nil, onPickShellCloud: (() -> Void)? = nil,
                onUseShellCloud: ((String) -> Void)? = nil, onClearShellData: (() -> Void)? = nil,
                onClearShellCloud: (() -> Void)? = nil,
                onGlobalPadMapChange: ((String, String) -> Void)? = nil,
                onGlobalPadMapReset: (() -> Void)? = nil, onButtonTest: @escaping (Bool) -> Void = { _ in },
                onDeleteGame: ((String, Bool) -> Void)? = nil, onManageSaves: ((String) -> Void)? = nil) {
        self.onSelect = onSelect
        self.onPlay = onPlay
        self.onChange = onChange
        self.onPickGameData = onPickGameData
        self.onUseAppData = onUseAppData
        self.onPickCloudSaves = onPickCloudSaves
        self.onDisableCloudSaves = onDisableCloudSaves
        self.onResetDefaults = onResetDefaults
        self.onShowLog = onShowLog
        self.onAppUpdate = onAppUpdate
        self.onBack = onBack
        self.onPickShellData = onPickShellData
        self.onPickShellCloud = onPickShellCloud
        self.onUseShellCloud = onUseShellCloud
        self.onClearShellData = onClearShellData
        self.onClearShellCloud = onClearShellCloud
        self.onGlobalPadMapChange = onGlobalPadMapChange
        self.onGlobalPadMapReset = onGlobalPadMapReset
        self.onButtonTest = onButtonTest
        self.onDeleteGame = onDeleteGame
        self.onManageSaves = onManageSaves
    }
}

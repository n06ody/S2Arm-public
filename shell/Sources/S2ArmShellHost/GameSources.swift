import Foundation
import S2ArmShellBridge
import ShellUI

/// 外部游戏来源（11-22 维护者「游戏库设置可以指定其他目录也作为游戏来源」；玩家用 Type-C 外接 NVMe 硬盘放电脑导出的 .img）：
///   库设置「游戏来源」里加的文件夹（系统文件夹选择器，书签存 ShellSettingsStore.gameSources，可加多个、可移除）。
///   打开库、回前台、库里有款离线时：解析书签、开始访问（本进程内一直访问，游戏读外部 .img 也靠这份访问权）、交宿主扫
///   （ios/app.m → runtime/src/pack_apply.c s2a_img_scan_source：完整 .img 以软链接接进这款的资源包，外部文件不挪不改；存档、派生件都在 App 内）。
///   文件夹打不开（外接盘没插）：不动已接的款，库按软链接打不开显示「没有找到游戏所在位置」（宿主装机状态 5），插回后下次扫自己恢复
enum GameSources {
    /// 本进程里已开始访问的文件夹路径（来源号 → 路径）：同一路径不重复开始访问；访问不停（进程结束系统收回）。只在 queue 上读写
    nonisolated(unsafe) private static var opened: [String: String] = [:]
    nonisolated(unsafe) private static var lastFail: [String: String] = [:]   // 同一来源同一原因只记一次日志（离线时每几秒重扫一次）
    static let queue = DispatchQueue(label: "dev.s2arm.game-sources", qos: .utility)

    /// 扫全部来源（后台）。skip：库里删过的游戏号，例行扫描不把它们链回来。done 在主线程上回，带新链 / 改链的款数
    static func rescan(skip: [String], store: ShellSettingsStore = .current, done: (@MainActor @Sendable (Int) -> Void)? = nil) {
        let list = store.gameSources
        guard !list.isEmpty else { return }
        let skipList = skip.joined(separator: ",")
        queue.async {
            var linked = 0
            for source in list {
                guard let path = open(source, store: store) else { continue }
                let n = skipList.isEmpty ? s2a_shell_scan_game_source(path, source.id, nil) : s2a_shell_scan_game_source(path, source.id, skipList)
                if n > 0 { linked += Int(n) }
            }
            if let done { DispatchQueue.main.async { MainActor.assumeIsolated { done(linked) } } }
        }
    }

    /// 解析书签、开始访问、试读一次目录：能用回路径，离线 / 授权失效回 nil（记一行原因，同一原因不重复记）
    private static func open(_ source: ShellSettingsStore.GameSource, store: ShellSettingsStore) -> String? {
        var stale = false
        let url: URL
        do { url = try CloudSaves.resolveBookmark(source.bookmark, stale: &stale) }
        catch { note(source, bookmark: true, error.localizedDescription); return nil }
        if opened[source.id] != url.path {
            let scoped = url.startAccessingSecurityScopedResource()
            NSLog("S2Arm 游戏来源：%@ 开始访问 %@（访问授权 %@）", source.name, url.path, scoped ? "拿到" : "没拿到")
            opened[source.id] = url.path
        }
        do { _ = try FileManager.default.contentsOfDirectory(atPath: url.path) }
        catch { note(source, bookmark: false, error.localizedDescription); return nil }
        if lastFail.removeValue(forKey: source.id) != nil { NSLog("S2Arm 游戏来源：%@ 又能打开了", source.name) }
        if stale, let fresh = try? url.bookmarkData() {   // 书签过期（盘重新挂载等）：换成新的
            store.storeGameSources(store.gameSources.map { $0.id == source.id ? .init(id: $0.id, name: $0.name, bookmark: fresh) : $0 })
        }
        return url.path
    }
    /// bookmark：书签解析不出（外接盘没插，或重新签名后授权失效）；否则是解析得出、文件夹打不开
    private static func note(_ source: ShellSettingsStore.GameSource, bookmark: Bool, _ error: String) {
        let key = (bookmark ? "b:" : "o:") + error
        guard lastFail[source.id] != key else { return }
        lastFail[source.id] = key
        if bookmark { NSLog("S2Arm 游戏来源：【黄】%@ 现在用不了（书签解析不出：外接盘没插，或重新签名后授权失效：%@），它的游戏在库里置灰", source.name, error) }
        else { NSLog("S2Arm 游戏来源：【黄】%@ 现在用不了（文件夹打不开：%@），它的游戏在库里置灰", source.name, error) }
    }

    /// 收下玩家刚选的文件夹：存书签、加进列表，立刻扫一次（不带删过的名单：玩家主动加文件夹，删过的款回到库里）
    @MainActor static func add(_ url: URL, store: ShellSettingsStore = .current, done: @escaping @MainActor @Sendable (Int) -> Void) {
        let scoped = url.startAccessingSecurityScopedResource()
        let mark: Data
        do { mark = try url.bookmarkData() }
        catch { NSLog("S2Arm 游戏来源：【红】%@ 书签没存上：%@", url.lastPathComponent, error.localizedDescription); if scoped { url.stopAccessingSecurityScopedResource() }; return }
        if store.gameSources.contains(where: { $0.bookmark == mark }) { NSLog("S2Arm 游戏来源：%@ 已经在列表里", url.lastPathComponent); return }
        let source = ShellSettingsStore.GameSource(id: UUID().uuidString.lowercased(), name: url.lastPathComponent, bookmark: mark)
        store.storeGameSources(store.gameSources + [source])
        NSLog("S2Arm 游戏来源：加了 %@（%@）", source.name, url.path)
        queue.async {
            opened[source.id] = url.path
            let n = s2a_shell_scan_game_source(url.path, source.id, nil)
            NSLog("S2Arm 游戏来源：%@ 首次扫描接上 %d 款", source.name, n)
            DispatchQueue.main.async { MainActor.assumeIsolated { done(Int(max(n, 0))) } }
        }
    }

    /// 玩家移除一个来源：从它接来的款删掉 App 内的资源包（外部文件不动），从列表拿掉
    @MainActor static func remove(_ id: String, store: ShellSettingsStore = .current, done: @escaping @MainActor @Sendable () -> Void) {
        let name = store.gameSources.first(where: { $0.id == id })?.name ?? id
        store.storeGameSources(store.gameSources.filter { $0.id != id })
        queue.async {
            let n = s2a_shell_forget_game_source(id)
            opened[id] = nil; lastFail[id] = nil
            NSLog("S2Arm 游戏来源：移除 %@，从它接来的 %d 款拿掉（外部文件不动）", name, n)
            DispatchQueue.main.async { MainActor.assumeIsolated { done() } }
        }
    }
}

#if canImport(UIKit) && !os(tvOS)   // tvOS 没有文件夹选择器：电视上不出「游戏来源」
import UIKit
import UniformTypeIdentifiers
/// 库设置「游戏来源 → 选择文件夹…」：系统文件夹选择器（同云存档那个：起始在 S2Arm 的 Documents，外接盘从左栏「位置」点进去）
@MainActor final class GameSourcePicker: NSObject, UIDocumentPickerDelegate {
    static var current: GameSourcePicker?
    var picked: ((URL) -> Void)?
    static func present(picked: @escaping (URL) -> Void) {
        if let appID = SigningIdentity.current { NSLog("S2Arm 游戏来源：【黄】签名的 App ID %@ 与包名不一致，系统可能不肯交出选的文件夹", appID) }
        let picker = CloudFolderPicker.makePicker()
        let me = GameSourcePicker(); me.picked = picked; current = me; picker.delegate = me
        CloudFolderPicker.presentLogged(picker, from: topController(), who: "game-source")
    }
    func documentPicker(_ controller: UIDocumentPickerViewController, didPickDocumentsAt urls: [URL]) {
        defer { Self.current = nil }
        NSLog("[s2a] 选文件界面：结束（game-source），选了 %d 项", urls.count)
        if let url = urls.first { picked?(url) }
    }
    func documentPickerWasCancelled(_ controller: UIDocumentPickerViewController) { NSLog("[s2a] 选文件界面：结束（game-source），取消"); Self.current = nil }
}
#endif

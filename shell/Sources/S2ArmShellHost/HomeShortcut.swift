// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import CoreGraphics
import Foundation
import ImageIO
import ShellUI
import UniformTypeIdentifiers
#if canImport(UIKit)
import UIKit
#endif

/// 游戏库长按「添加到主屏幕…」：在 Safari 里打开官网启动页 s2arm.com/play/launch，玩家用 Safari「添加到主屏幕」把这款放上主屏。
/// 游戏号、名字、封面小图都放在链接的 # 片段里：片段不发给服务器（官网看不到玩家有哪些游戏，封面也不上官网），
/// 页面用 JS 读片段，把封面设成主屏图标；从主屏打开时直接跳 s2arm://play/<游戏号>（与主屏小组件同一条路，app.m 接）。
/// 页面带离线缓存（sw.js），添加后第一次打开有网即可，之后没网也能直进。
enum HomeShortcut {
    static let page = "https://s2arm.com/play/launch"
    /// iPhone 主屏图标是 180×180 像素：封面居中裁成正方形再缩到这么大，JPEG 压成十几 KB 放进链接
    static let side = 180

    static func iconJPEG(_ image: CGImage, side: Int = side, quality: Double = 0.7) -> Data? {
        let w = image.width, h = image.height, s = max(1, min(w, h))
        guard let square = image.cropping(to: CGRect(x: (w - s) / 2, y: (h - s) / 2, width: s, height: s)),
              let ctx = CGContext(data: nil, width: side, height: side, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
        ctx.interpolationQuality = .high
        ctx.draw(square, in: CGRect(x: 0, y: 0, width: side, height: side))
        let out = NSMutableData()
        guard let small = ctx.makeImage(), let dest = CGImageDestinationCreateWithData(out, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dest, small, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
        return CGImageDestinationFinalize(dest) ? out as Data : nil
    }

    /// 片段按 URLSearchParams 的规矩写（页面用它读）：只留字母数字和 -._~，其余（含 + / = 空格）一律百分号转义，
    /// 免得 + 被读成空格
    static func url(id: String, name: String, icon: Data?) -> URL? {
        var keep = CharacterSet.alphanumerics.intersection(CharacterSet(charactersIn: Unicode.Scalar(0)...Unicode.Scalar(127)))
        keep.insert(charactersIn: "-._~")
        func enc(_ s: String) -> String { s.addingPercentEncoding(withAllowedCharacters: keep) ?? "" }
        var fragment = "id=\(enc(id))&n=\(enc(name))"
        if let icon { fragment += "&i=" + enc(icon.base64EncodedString()) }
        return URL(string: page + "#" + fragment)
    }
}

extension S2ArmShellHostModel {
    func openHomeShortcut(_ id: String) {
        guard let detail = games.first(where: { $0.id == id }) else { return }
        let tile = detail.game
        let icon = (tile.icon ?? tile.banner).flatMap { HomeShortcut.iconJPEG($0.cgImage) }
        guard let url = HomeShortcut.url(id: id, name: tile.title, icon: icon) else { return }
        #if canImport(UIKit) && !os(tvOS)
        UIApplication.shared.open(url)
        #endif
    }
}

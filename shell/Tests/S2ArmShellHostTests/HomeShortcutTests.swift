// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import CoreGraphics
import Foundation
import ImageIO
import Testing
@testable import S2ArmShellHost

/// 游戏库「添加到主屏幕…」打开的官网启动页链接：游戏信息全在 # 片段里（不发给服务器），页面按 URLSearchParams 读回原样
struct HomeShortcutTests {
    /// 按 URLSearchParams 的规矩解析片段（+ 当空格、%xx 解码）：和官网页面 play/launch 读法一致
    func searchParams(_ fragment: String) -> [String: String] {
        var out: [String: String] = [:]
        for pair in fragment.split(separator: "&") {
            let kv = pair.split(separator: "=", maxSplits: 1).map { String($0).replacingOccurrences(of: "+", with: " ").removingPercentEncoding ?? "" }
            out[kv[0]] = kv.count > 1 ? kv[1] : ""
        }
        return out
    }

    @Test func fragmentCarriesGameAndServerSeesOnlyThePage() throws {
        let name = "Zelda: Tears + Kingdom & 王国之泪 = 1"
        let icon = Data((0..<3000).map { UInt8(truncatingIfNeeded: $0 &* 37) })   // base64 里必有 + / =
        let url = try #require(HomeShortcut.url(id: "0100F2C0115B6000", name: name, icon: icon))
        #expect(url.host == "s2arm.com")
        #expect(url.path == "/play/launch")
        #expect(url.query == nil)   // 请求里只有路径：游戏号、名字、封面都不到服务器
        let fragment = try #require(url.fragment(percentEncoded: true))
        #expect(!fragment.contains("+"))   // + 原样留着会被页面读成空格
        let p = searchParams(fragment)
        #expect(p["id"] == "0100F2C0115B6000")
        #expect(p["n"] == name)
        #expect(Data(base64Encoded: p["i"] ?? "") == icon)
    }

    @Test func noCoverMeansNoIconParameter() throws {
        let url = try #require(HomeShortcut.url(id: "0100000000010000", name: "A", icon: nil))
        #expect(searchParams(url.fragment(percentEncoded: true) ?? "")["i"] == nil)
    }

    @Test func coverBecomesSquareHomeScreenJPEG() throws {
        let ctx = try #require(CGContext(data: nil, width: 400, height: 300, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                         bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue))
        ctx.setFillColor(red: 0, green: 0.6, blue: 0.2, alpha: 1); ctx.fill(CGRect(x: 0, y: 0, width: 400, height: 300))
        let cover = try #require(ctx.makeImage())
        let jpeg = try #require(HomeShortcut.iconJPEG(cover))
        #expect(jpeg.starts(with: [0xFF, 0xD8]))   // JPEG 文件头
        let src = try #require(CGImageSourceCreateWithData(jpeg as CFData, nil))
        let img = try #require(CGImageSourceCreateImageAtIndex(src, 0, nil))
        #expect(img.width == 180 && img.height == 180)
        #expect(jpeg.count < 20_000)   // 放进链接：十几 KB 以内
    }
}

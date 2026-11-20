// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import SwiftUI
import ShellUI

/// 「提高内存上限」权限的「了解详情」页（游戏库顶提示条上点开，11-16 维护者）：按装法写能不能在自签时带上这项权限、要做什么。
///   每条结论有出处（11-16 查证）：启动器（pack/dev_sign.py 只核这一项、Windows 签名用的 Impactor 2.7.2 源码按程序自带权限申请能力，
///   实测可用内存 8177 MB）；AltStore Classic 2.2 发布说明写明支持这项权限；SideStore 0.7.0 签名会丢（官方 issue #1616 未修）；
///   Sideloadly 官方没说免费账号会保留，自定义权限要付费开发者账号，按「暂不支持」写；GetMoreRam 官方说明给 App ID 补上这项能力后重新安装。
///   11-17：启动器装的也有丢的（当天 8 份诊断包名都是 io.s2arm.games.<团队>）→ 启动器一条补「重装后还在就用 GetMoreRam」；
///   补「自己的开发者证书」一条（描述文件里要带这一项）；GetMoreRam 补「同一个 Apple ID 自签装上、占一个免费名额」（社区说明）
///   iOS 只依赖这一项（increased-memory-limit），不教扩展虚拟寻址（那项只有付费账号能签，iOS 用不到）
struct IMLGuideView: View {
    @Environment(\.dismiss) private var dismiss

    enum Verdict { case keeps, drops, unsupported, fixes }
    struct Way: Identifiable { let id: String; let name: String; let verdict: Verdict; let steps: String }

    static var ways: [Way] {
        [Way(id: "launcher", name: shellText("S2Arm launcher (recommended)"), verdict: .keeps,
             steps: shellText("In S2Arm on your Mac or Windows PC, click “Renew S2Arm” on the S2Arm card first in the library. It signs with your Apple ID and keeps this permission, even with a free Apple ID. If you still see the notice after reinstalling, add it with GetMoreRam below, then click “Renew S2Arm” again.")),
         Way(id: "cert", name: shellText("Your own developer certificate"), verdict: .fixes,
             steps: shellText("If you sign with your own .p12 certificate and provisioning profile: on the Apple Developer website, turn on “Increased Memory Limit” for S2Arm’s App ID, regenerate the provisioning profile, then sign and install S2Arm again with the new profile. A profile without it can’t carry this permission.")),
         Way(id: "altstore", name: "AltStore Classic", verdict: .keeps,
             steps: shellText("Use AltStore Classic 2.2 or later and install S2Arm again (a refresh is not enough). A free Apple ID works.")),
         Way(id: "sidestore", name: "SideStore", verdict: .drops,
             steps: shellText("SideStore (0.7.0 and earlier) drops this permission when it signs. Add it with GetMoreRam below, then install S2Arm again in SideStore.")),
         Way(id: "sideloadly", name: "Sideloadly", verdict: .unsupported,
             steps: shellText("Not supported for now: Sideloadly doesn’t say it keeps this permission with a free Apple ID. Its custom entitlement option needs a paid Apple Developer account.")),
         Way(id: "getmoreram", name: "GetMoreRam", verdict: .fixes,
             steps: shellText("Already installed? Sideload GetMoreRam with the same Apple ID you used for S2Arm (it takes one of a free account’s app slots; delete it afterwards), sign in, open App IDs and tap Refresh, choose S2Arm, tap “Add Increased Memory Limit”, then install S2Arm again with the same tool."))]
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text(shellText("S2Arm needs the “Increased Memory Limit” permission. Without it, large games can run out of memory and close. Whether your copy keeps it depends on how S2Arm was signed."))
                        .font(.system(size: 15)).foregroundStyle(.white.opacity(0.85)).fixedSize(horizontal: false, vertical: true)
                    ForEach(Self.ways) { way in row(way) }
                }
                .padding(20)
            }
            .background(Color(red: 0.055, green: 0.055, blue: 0.06).ignoresSafeArea())
            .navigationTitle(shellText("Keep “Increased Memory Limit” when signing"))
            #if os(iOS)
            .navigationBarTitleDisplayMode(.inline)
            #endif
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button(shellText("Done")) { dismiss() }.accessibilityIdentifier("iml.guide.done") } }
        }
        .preferredColorScheme(.dark)
        .accessibilityIdentifier("iml.guide")
    }

    private func row(_ way: Way) -> some View {
        let (symbol, color, label): (String, Color, String) = switch way.verdict {
        case .keeps: ("checkmark.circle.fill", Color(red: 0.24, green: 0.94, blue: 0.66), shellText("Keeps it"))
        case .drops: ("xmark.circle.fill", Color(red: 1, green: 0.36, blue: 0.48), shellText("Drops it"))
        case .unsupported: ("questionmark.circle.fill", .white.opacity(0.5), shellText("Not supported for now"))
        case .fixes: ("plus.circle.fill", Color(red: 1, green: 0.78, blue: 0.28), shellText("Adds it back"))
        }
        return VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: symbol).foregroundStyle(color).accessibilityHidden(true)
                Text(way.name).font(.system(size: 16, weight: .semibold)).foregroundStyle(.white)
                Text(label).font(.system(size: 12, weight: .semibold)).foregroundStyle(color)
                    .padding(.horizontal, 8).padding(.vertical, 2).overlay(Capsule().strokeBorder(color.opacity(0.6)))
            }
            Text(way.steps).font(.system(size: 14)).foregroundStyle(.white.opacity(0.75)).fixedSize(horizontal: false, vertical: true)
        }
        .padding(14).frame(maxWidth: .infinity, alignment: .leading)
        .background(Color.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14))
        .accessibilityElement(children: .combine).accessibilityIdentifier("iml.guide." + way.id)
    }
}

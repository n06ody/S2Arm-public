// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 S2Arm contributors
import SwiftUI

/// S2Arm: the three steps to put a game on the Home Screen, shown before the host opens the game's page in Safari.
/// Safari's own menus do the adding; each step shows the symbol the player looks for there.
struct AddToHomeGuideView: View {
    let name: String
    let go: () -> Void
    let close: () -> Void

    private let steps: [(symbol: String, text: String.LocalizationValue)] = [
        ("ellipsis.circle", "In Safari, tap ••• at the bottom, then Share."),
        ("square.and.arrow.up", "Tap View More, then Add to Home Screen."),
        ("plus.app", "Tap Add. The icon then opens this game directly, even offline."),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 22) {
            HStack {
                Text(ShellStrings.text("Add to Home Screen")).font(ShellFonts.font(22, weight: .semibold))
                Spacer()
                Button(action: close) { Image(systemName: "xmark.circle.fill").font(.system(size: 26)).foregroundStyle(.secondary) }
                    .buttonStyle(.plain).accessibilityLabel(ShellStrings.text("Close"))
            }
            if !name.isEmpty { Text(name).font(ShellFonts.font(17, weight: .medium)).foregroundStyle(ShellPalette.cream) }
            ForEach(Array(steps.enumerated()), id: \.offset) { index, step in
                HStack(alignment: .top, spacing: 14) {
                    ZStack {
                        RoundedRectangle(cornerRadius: 12).fill(Color.white.opacity(0.12)).frame(width: 52, height: 52)
                        Image(systemName: step.symbol).font(.system(size: 24, weight: .medium))
                    }
                    VStack(alignment: .leading, spacing: 4) {
                        Text(verbatim: "\(index + 1)").font(ShellFonts.font(13, weight: .bold)).foregroundStyle(.secondary)
                        Text(ShellStrings.text(step.text)).font(ShellFonts.font(16)).fixedSize(horizontal: false, vertical: true)
                    }
                }
            }
            Spacer(minLength: 0)
            Button(action: go) {
                Text(ShellStrings.text("Open in Safari")).font(ShellFonts.font(17, weight: .semibold))
                    .frame(maxWidth: .infinity).padding(.vertical, 14)
                    .background(RoundedRectangle(cornerRadius: 14).fill(ShellPalette.cream)).foregroundStyle(.black)
            }
            .buttonStyle(.plain)
        }
        .padding(24)
        .presentationDetents([.medium, .large])
    }
}

import SwiftUI

struct TasksView: View {
    @EnvironmentObject var store: Store
    private var attention: [TaskRecord] {store.visibleTasks.filter {!$0.active && (["failed","interrupted"].contains($0.state) || $0.hasWarnings || $0.hasErrors)}}
    // 成功出的 iPhone 包不进任务历史：在「已出好的 iPhone 包」按游戏成卡（PackShelf.swift）
    private var history: [TaskRecord] {store.visibleTasks.filter {!$0.active && !["failed","interrupted"].contains($0.state) && !$0.hasWarnings && !$0.hasErrors && !($0.operation == "ipa" && $0.state == "completed")}}
    var body: some View {
        WorkspacePage {
            WorkspaceHeading(title:"编译任务",subtitle:"",symbol:"checklist") {
                Button(L("打开日志")) {store.reveal(store.logsRoot)}.platformButtonStyle()
            }
            HStack(spacing:8) {
                StatusPill(title:"进行中 \(store.activeTasks.count)",symbol:"circle.dotted")
                StatusPill(title:"等待中 \(store.queue.count)",symbol:"clock",color:.secondary)
                Spacer()
            }
            if store.visibleTasks.isEmpty && store.queue.isEmpty {
                GlassPanel {
                    ContentUnavailableView(L("暂无任务"),systemImage:"checklist",description:Text(L("导入后，文件齐全的游戏会自动加入准备队列；也可在详情中手动准备。")))
                        .frame(minHeight:160)
                }
            }
            if let hero = store.completionHeroTask {PackCompletionHero(task:hero)}
            ForEach(store.visibleTasks.filter { $0.active && $0.state != "queued" }) {task in TaskCard(task:task,logLines:12,gameHeader:true)}   // 排队的只在下面「等待队列」里列：大卡片的「已用」是从入队算起，与在跑的任务并排像同一进度（11-01）
            if !store.queue.isEmpty {
                GlassPanel(title:"等待队列",symbol:"clock") {
                    ForEach(Array(store.queue.enumerated()),id:\.element) { i,id in
                        HStack(spacing:12) {
                            Text("\(i+1)").font(.caption.monospacedDigit()).foregroundStyle(.secondary).frame(width:24)
                            Text(store.games.first(where:{$0.id==id})?.name ?? id).lineLimit(1)
                            Spacer()
                            Button(L("移出队列")) {store.dequeue(id)}.controlSize(.small)
                        }.padding(.vertical,4)
                    }
                }
            }
            if !attention.isEmpty {
                GlassPanel {
                    ForEach(attention) {task in
                        EndedTaskRow(task:task)
                        if task.id != attention.last?.id {Divider()}
                    }
                }
            }
            if !store.packShelf.isEmpty { PackShelfPanel(entries:store.packShelf) }
            if !history.isEmpty {
                GlassPanel {
                    CompactDisclosure(title:"历史任务",subtitle:"\(history.count)",symbol:"clock.arrow.circlepath") {
                        LazyVStack(spacing:10) {ForEach(history) {task in EndedTaskRow(task:task)}}
                    }
                }
            }
        }
        .disclosureGroupStyle(GlassDisclosureStyle())
        .task { store.refreshPackShelf() }
    }
}
/// 结束的任务：一行一条（图标 · 操作 · 游戏 · 结果说明 · 结束时间 · 动作）；失败的带「详情」弹出层看诊断，不展开占地方
struct EndedTaskRow: View {
    @EnvironmentObject var store: Store
    let task: TaskRecord
    @State private var showDetail = false
    private var icon: (String, Color) {
        if task.state == "failed" { return ("xmark.octagon.fill", .red) }
        if task.hasErrors { return ("exclamationmark.triangle.fill", .red) }
        if task.hasWarnings { return ("exclamationmark.triangle.fill", .orange) }
        if task.state == "completed" { return ("checkmark.circle.fill", .teal) }
        return ("minus.circle", .secondary)
    }
    private var summary: String {
        if let d = task.diagnostics?.first { return L(d.summary) }
        if let m = task.message, !m.isEmpty { return L(m) }
        return ""
    }
    var body: some View {
        HStack(spacing:12) {
            Image(systemName:icon.0).foregroundStyle(icon.1).font(.title3).frame(width:22)
            VStack(alignment:.leading, spacing:2) {
                HStack(spacing:8) {
                    Text(task.title).font(.callout.weight(.semibold))
                    if let name = store.games.first(where:{ $0.id == task.gameId })?.name { Text(name).font(.callout).foregroundStyle(.secondary).lineLimit(1) }
                }
                if !summary.isEmpty { Text(summary).font(.caption).foregroundStyle(.secondary).lineLimit(2).truncationMode(.tail).help(summary) }
            }
            Spacer(minLength:12)
            if let t = task.ended ?? task.started {
                Text(Date(timeIntervalSince1970:t).formatted(date:.omitted, time:.shortened)).font(.caption.monospacedDigit()).foregroundStyle(.tertiary)
            }
            Text(task.stateName).font(.caption.weight(.medium)).foregroundStyle(task.state == "failed" || task.hasErrors ? .red : task.hasWarnings ? .orange : .secondary).frame(minWidth:56, alignment:.trailing)
            HStack(spacing:6) {
                TaskLogDeleteButton(task:task)
                if ["failed","interrupted"].contains(task.state) || task.hasWarnings || task.hasErrors || task.space != nil {
                    Button(L("详情")) { showDetail = true }.buttonStyle(.borderless)
                        .popover(isPresented:$showDetail, arrowEdge:.bottom) {
                            ScrollView {TaskCard(task:task).padding(12)}.frame(width:600).frame(maxHeight:460)
                        }
                }
                if task.state == "completed", let path = task.outputPath { Button(L("查看产物")) { store.reveal(URL(fileURLWithPath:path)) }.buttonStyle(.borderless) }
                if ["failed","interrupted","cancelled"].contains(task.state), task.gameId != nil { Button(L("重试")) { store.retry(task) }.buttonStyle(.borderless).disabled(store.executionBusy) }
            }
        }.padding(.vertical,6).contentShape(Rectangle())
    }
}

/// 实时计数：阶段里还没做完的逐项计数各一格（名字、已完成 / 总数、细进度条、每分钟速度与预计还要多久），数字每秒跟着跳
struct LiveCountersView: View {
    let counters: [TaskCounter]
    var body: some View {
        LazyVGrid(columns:[GridItem(.adaptive(minimum:260), spacing:12)], alignment:.leading, spacing:12) {
            ForEach(counters) { c in
                VStack(alignment:.leading, spacing:6) {
                    Text(L(c.name)).font(.caption.weight(.medium)).foregroundStyle(.secondary).lineLimit(1)
                    HStack(alignment:.firstTextBaseline, spacing:4) {
                        Text(Int(c.done).formatted()).font(.title2.weight(.semibold).monospacedDigit()).contentTransition(.numericText()).animation(.default, value:c.done)
                        Text("/ " + Int(c.total).formatted()).font(.callout.monospacedDigit()).foregroundStyle(.secondary)
                        Spacer()
                        Text("\(Int(c.fraction * 100))%").font(.callout.monospacedDigit().weight(.medium)).foregroundStyle(.teal)
                    }
                    ProgressView(value:c.fraction).tint(.teal)
                    HStack {
                        if let r = c.rate { Text(L("约 ") + Int((r * 60).rounded()).formatted() + L(" 个 / 分钟")) } else { Text(L("刚开始，速度稍后出来")) }
                        Spacer()
                        if let rem = c.remaining { Text(L("还要约 ") + durationText(rem)) }
                    }.font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                }.padding(12).frame(maxWidth:.infinity, alignment:.leading)
                    .background(Color.teal.opacity(0.06), in:RoundedRectangle(cornerRadius:12, style:.continuous))
            }
        }
    }
}

/// 日志末几行：每 2 秒读一次这条任务的日志尾巴，进度条不动的时候也看得出确实在做
struct TaskLogTail: View {
    @EnvironmentObject var store: Store
    let task: TaskRecord
    var lines = 3
    var showsHeading = true
    var body: some View {
        TimelineView(.periodic(from:.now, by:2)) { _ in
            let tail = store.logTail(for:task, lines:lines)
            if !tail.isEmpty {
                VStack(alignment:.leading, spacing:3) {
                    if showsHeading {Label(L("实时日志"), systemImage:"waveform.path.ecg").font(.caption2.weight(.semibold)).foregroundStyle(.secondary)}
                    ForEach(Array(tail.enumerated()), id:\.offset) { _, l in
                        Text(l).font(.caption2.monospaced()).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                    }
                }.padding(10).frame(maxWidth:.infinity, alignment:.leading)
                    .background(.quaternary.opacity(0.5), in:RoundedRectangle(cornerRadius:10, style:.continuous))
            }
        }
    }
}

/// 左栏「当前任务」：游戏名、第几步、主计数（着色器 n / 总数）与百分比，细进度条；点一下到编译任务页
struct SidebarTaskCard: View {
    @EnvironmentObject var store: Store
    let task: TaskRecord
    var body: some View {
        Button { store.selection = "tasks" } label: {
            VStack(alignment:.leading, spacing:7) {
                HStack(spacing:6) {
                    ProgressView().controlSize(.mini)
                    Text(task.title).font(.caption.weight(.semibold)).lineLimit(1)
                }
                if let name = store.games.first(where:{ $0.id == task.gameId })?.name { Text(name).font(.caption).lineLimit(1) }
                let step = task.milestone.flatMap { m in task.milestones?.first(where:{ $0.id == m })?.label } ?? task.stage
                if let step, !step.isEmpty { Text(L(step)).font(.caption2).foregroundStyle(.secondary).lineLimit(1) }
                // 主计数 = 最近一次更新的那个（作业的进度消息「<名字> n/总数」与任务卡同口径），没对上取第一个
                if let c = task.counters?.first(where:{ (task.message ?? "").hasPrefix($0.name) }) ?? task.counters?.first {
                    ProgressView(value:c.fraction).tint(.teal)
                    Text("\(Int(c.done).formatted()) / \(Int(c.total).formatted()) · \(Int(c.fraction * 100))%").font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                }
            }.padding(12).frame(maxWidth:.infinity, alignment:.leading).contentShape(Rectangle())
        }.buttonStyle(.plain).glassSurface(16)
    }
}

/// 一行放不下就折行的横排（内容摘要、阶段图例：英文比中文长，固定宽度的横排会把整栏撑出窗口）
struct FlowRow: Layout {
    var spacing: CGFloat = 14, lineSpacing: CGFloat = 6
    private func lines(_ width: CGFloat, _ subviews: Subviews) -> [[(Int, CGSize)]] {
        var out: [[(Int, CGSize)]] = [[]], x: CGFloat = 0
        for (i, v) in subviews.enumerated() {
            let sz = v.sizeThatFits(.unspecified)
            if x > 0 && x + sz.width > width { out.append([]); x = 0 }
            out[out.count - 1].append((i, sz)); x += sz.width + spacing
        }
        return out
    }
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) -> CGSize {
        let ls = lines(proposal.width ?? .infinity, subviews)
        let w = ls.map { l in l.reduce(0) { $0 + $1.1.width } + spacing * CGFloat(max(0, l.count - 1)) }.max() ?? 0
        let h = ls.map { l in l.map(\.1.height).max() ?? 0 }.reduce(0, +) + lineSpacing * CGFloat(max(0, ls.count - 1))
        return CGSize(width: min(w, proposal.width ?? w), height: h)
    }
    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize, subviews: Subviews, cache: inout ()) {
        var y = bounds.minY
        for l in lines(bounds.width, subviews) {
            var x = bounds.minX; let lh = l.map(\.1.height).max() ?? 0
            for (i, sz) in l { subviews[i].place(at: CGPoint(x: x, y: y + (lh - sz.height) / 2), proposal: ProposedViewSize(sz)); x += sz.width + spacing }
            y += lh + lineSpacing
        }
    }
}

// MARK: - 正在编译的这款游戏（10-26 维护者：编译任务页要看得到在打哪款——名称、图标、版本、DLC、包大小）

/// 游戏的内容摘要：本体版本 · 更新 · DLC · 原始包大小（· 目标 iPhone）；窄了折行
struct GameContentLine: View {
    let game: Game
    var target: IPhoneTarget? = nil
    private var items: [(String, String)] {
        let base = game.packages.first { $0.type == "base" }
        let updates = game.packages.filter { $0.type == "update" }.sorted { $0.version < $1.version }
        let dlc = game.packages.filter { $0.type == "dlc" }.count
        var out: [(String, String)] = []
        if let base { out.append(("shippingbox", L("本体 ") + (base.display.isEmpty ? "v\(base.version)" : base.display))) }
        if let last = updates.last { out.append(("arrow.triangle.2.circlepath", L("更新 ") + (last.display.isEmpty ? "v\(last.version)" : last.display))) }
        if dlc > 0 { out.append(("puzzlepiece.extension", "DLC \(dlc)")) }
        let bytes = game.sourceBytes
        if bytes > 0 { out.append(("internaldrive", ByteCountFormatter.string(fromByteCount:bytes, countStyle:.file))) }
        return out
    }
    var body: some View {
        let device = target.map { [("iphone", $0.label)] } ?? []
        FlowRow(spacing:14, lineSpacing:4) { ForEach(items + device, id:\.1) { Label($0.1, systemImage:$0.0).lineLimit(1) } }
            .font(.caption).foregroundStyle(.secondary).labelStyle(.titleAndIcon)
    }
}

/// 进行中任务卡顶部：大图标 + 游戏名 + 版本 + 内容摘要；右侧是在做什么、已用多久
struct TaskGameHeader: View {
    @EnvironmentObject var store: Store
    let game: Game
    let task: TaskRecord
    var body: some View {
        HStack(alignment:.center, spacing:16) {
            GameCover(game:game).frame(width:64, height:64)
                .clipShape(RoundedRectangle(cornerRadius:14, style:.continuous))
                .shadow(color:.black.opacity(0.16), radius:6, y:3).accessibilityHidden(true)
            VStack(alignment:.leading, spacing:6) {
                HStack(alignment:.firstTextBaseline, spacing:8) {
                    Text(game.name).font(.title2.weight(.semibold)).lineLimit(1).truncationMode(.tail)
                    if !game.display.isEmpty { Text(game.display).font(.callout.monospacedDigit()).foregroundStyle(.secondary) }
                }
                GameContentLine(game:game, target: task.operation == "ipa" ? store.iphoneTarget : nil)
            }
            Spacer(minLength:12)
            VStack(alignment:.trailing, spacing:4) {
                Label(task.title, systemImage: task.operation == "ipa" ? "iphone.gen3" : "macbook").font(.callout.weight(.medium))
                if let s = task.started {
                    TimelineView(.periodic(from:.now, by:1)) { _ in
                        Text(L("已用 ") + durationText(Date().timeIntervalSince1970 - s)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    }
                }
            }
        }
        .accessibilityElement(children:.combine)
    }
}

// MARK: - 出包成功（10-26 维护者：成功页缺仪式感和信息量）
//   最近一次打 iPhone 包成功、玩家还没收起时，主栏最上方是这一屏：这款游戏、用了多久、包里是什么、各阶段花在哪、接下来三步怎么装

extension Store {
    /// 这次出包之后、同一款游戏成功的「推到 iPhone」（完成页第 1 步据此打勾）
    func installTask(after ipa: TaskRecord) -> (task: TaskRecord, device: String?)? {
        guard let t = tasks.filter({ $0.operation == "install" && $0.state == "completed" && $0.gameId == ipa.gameId && ($0.ended ?? 0) >= (ipa.ended ?? 0) })
            .max(by: { ($0.ended ?? 0) < ($1.ended ?? 0) }) else { return nil }
        return (t, t.device)
    }
    private static let heroDismissKey = "tasks.completionHeroDismissed"
    /// 最近一次结束的打 iPhone 包任务——成功了、且没被收起才给
    var completionHeroTask: TaskRecord? {
        let ipa = visibleTasks.filter { $0.operation == "ipa" && !$0.active }
        guard let t = ipa.max(by: { ($0.ended ?? 0) < ($1.ended ?? 0) }), t.state == "completed",
              t.id != UserDefaults.standard.string(forKey:Store.heroDismissKey) else { return nil }
        return t
    }
    func dismissCompletionHero(_ task: TaskRecord) {
        UserDefaults.standard.set(task.id, forKey:Store.heroDismissKey); objectWillChange.send()
    }
}

/// IPA 与资源包的实际大小（资源包读 index.tsv：每行「路径 字节 sha256」）；后台读一次
struct PackFacts: Equatable {
    var ipaBytes: Int64?; var packFiles: Int?; var packBytes: Int64?; var packURL: URL?
    static func read(ipa: String?) -> PackFacts {
        guard let ipa else { return PackFacts() }
        let url = URL(fileURLWithPath:ipa); var f = PackFacts()
        f.ipaBytes = (try? FileManager.default.attributesOfItem(atPath:ipa)[.size] as? NSNumber)?.int64Value
        let dir = url.deletingLastPathComponent()
        if let name = try? FileManager.default.contentsOfDirectory(atPath:dir.path).first(where: { $0.hasSuffix("资源包") }) {
            let pack = dir.appendingPathComponent(name); f.packURL = pack
            if let text = try? String(contentsOf:pack.appendingPathComponent("index.tsv"), encoding:.utf8) {
                var n = 0, b: Int64 = 0
                for line in text.split(separator:"\n") { let c = line.split(separator:"\t"); if c.count >= 2, let s = Int64(c[1]) { n += 1; b += s } }
                f.packFiles = n; f.packBytes = b
            }
        }
        return f
    }
}

/// 各阶段用时：一根按时长分段的条 + 图例（里程碑开始时刻差；最后一段到任务结束）
struct StageTimeline: View {
    let task: TaskRecord
    private var segments: [(String, Double)] {
        guard let plan = task.milestones, let started = task.milestoneStarted, let end = task.ended else { return [] }
        let seen = plan.compactMap { m in started[m.id].map { (m.label, $0) } }.sorted { $0.1 < $1.1 }
        return seen.enumerated().compactMap { i, s in
            let next = i + 1 < seen.count ? seen[i + 1].1 : end
            return next > s.1 ? (s.0, next - s.1) : nil
        }
    }
    // 拆成小函数：混着 CGFloat / Double 的长表达式在负载高时 Swift 类型检查超时（10-26 本机负载 20 编译红）
    private func shade(_ i: Int, _ n: Int) -> Double { 0.35 + 0.55 * Double(n - i) / Double(n) }
    private func barWidth(_ full: CGFloat, _ n: Int, _ part: Double, _ total: Double) -> CGFloat {
        let room = full - CGFloat(n - 1) * 2
        return max(3, room * CGFloat(part / total))
    }
    var body: some View {
        let segs = segments, total = segs.reduce(0) { $0 + $1.1 }
        if total > 0 {
            VStack(alignment:.leading, spacing:8) {
                GeometryReader { g in
                    HStack(spacing:2) {
                        ForEach(Array(segs.enumerated()), id:\.offset) { i, s in
                            Rectangle().fill(Brand.accent.opacity(shade(i, segs.count)))
                                .frame(width: barWidth(g.size.width, segs.count, s.1, total))
                                .help(L(s.0) + " " + durationText(s.1))
                        }
                    }.clipShape(Capsule())
                }.frame(height:10)
                FlowRow(spacing:16, lineSpacing:4) {
                    ForEach(Array(segs.enumerated()), id:\.offset) { i, s in
                        HStack(spacing:5) {
                            Circle().fill(Brand.accent.opacity(shade(i, segs.count))).frame(width:7, height:7)
                            Text(L(s.0)).foregroundStyle(.secondary)
                            Text(durationText(s.1)).monospacedDigit()
                        }.lineLimit(1)
                    }
                }.font(.caption)
            }
            .accessibilityElement(children:.combine)
        }
    }
}

struct PackCompletionHero: View {
    @EnvironmentObject var store: Store
    let task: TaskRecord
    @State private var facts = PackFacts()
    @State private var sealed = 0
    private var game: Game? { store.games.first { $0.id == task.gameId } }
    var body: some View {
        let ev = store.packEvidence(for:task)
        VStack(alignment:.leading, spacing:12) {
            HStack(alignment:.top, spacing:12) {
                ZStack(alignment:.bottomTrailing) {
                    Group { if let game { GameCover(game:game) } else { Color.teal.opacity(0.12) } }
                        .frame(width:64, height:64).clipShape(RoundedRectangle(cornerRadius:14, style:.continuous))
                        .shadow(color:.black.opacity(0.22), radius:6, y:3)
                    // 带警告（只出通用模式、追加内容没解出等，审计 P1-74）：不画完成章，用任务列表同一个橙色三角
                    Image(systemName:task.hasWarnings ? "exclamationmark.triangle.fill" : "checkmark.seal.fill").font(.system(size:24))
                        .symbolRenderingMode(.palette).foregroundStyle(.white, task.hasWarnings ? Color.orange : Brand.accent)
                        .background(Circle().fill(.background).padding(4))
                        .symbolEffect(.bounce, value:sealed)
                        .offset(x:6, y:6)
                }.accessibilityHidden(true)
                VStack(alignment:.leading, spacing:10) {
                    Text(game?.name ?? L("这款游戏")).font(.headline).fixedSize(horizontal:false, vertical:true)
                    FlowRow(spacing:8, lineSpacing:2) {
                        Text(task.hasWarnings ? task.stateName : L("iPhone 包已出好")).fontWeight(.semibold).foregroundStyle(task.hasWarnings ? Color.orange : Brand.accent)
                        if let v = game?.display, !v.isEmpty { Text("·"); Text(v).monospacedDigit() }
                        if let s = task.started, let e = task.ended { Text("·"); Text(L("用时 ") + durationText(e - s)).monospacedDigit() }
                        if let e = task.ended { Text("·"); Text(Date(timeIntervalSince1970:e).formatted(date:.omitted, time:.shortened) + L(" 完成")) }
                    }.font(.callout).foregroundStyle(.secondary)
                    if let game { GameContentLine(game:game) }
                    ForEach(task.diagnostics?.filter(\.isWarning) ?? []) { TaskDiagnosticCard(diagnostic:$0) }
                    Grid(alignment:.leadingFirstTextBaseline, horizontalSpacing:14, verticalSpacing:6) {
                        fact("sparkles.tv", L("画面模式"), ev.mode.title, tint: { if case .direct = ev.mode { return Brand.accent } else { return .secondary } }())
                        if let t = store.iphoneTarget { fact("iphone.gen3", L("目标设备"), t.label) }
                        if let b = facts.ipaBytes { fact("app.badge", "IPA", ByteCountFormatter.string(fromByteCount:b, countStyle:.file)) }
                        if let n = facts.packFiles, let b = facts.packBytes { fact("folder", L("资源包"), n.formatted() + L(" 个文件 · ") + ByteCountFormatter.string(fromByteCount:b, countStyle:.file)) }
                        if let id = ev.buildID, !id.isEmpty {
                            GridRow {
                                Image(systemName:"number").foregroundStyle(.secondary).frame(width:18)
                                Text(L("包号")).foregroundStyle(.secondary)
                                HStack(spacing:6) {
                                    Text(id).font(.callout.monospaced()).textSelection(.enabled)
                                    Button { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(id, forType:.string); store.notice = L("包号已复制。回报问题时请填上它。") } label: { Image(systemName:"doc.on.doc") }
                                        .buttonStyle(.borderless).help(L("复制包号")).accessibilityLabel(L("复制包号"))
                                }
                            }
                        }
                    }.font(.callout)
                }
                Spacer(minLength:0)
                Button { store.dismissCompletionHero(task) } label: { Image(systemName:"xmark").font(.callout.weight(.semibold)).padding(6) }
                    .buttonStyle(.borderless).foregroundStyle(.secondary).help(L("收起")).accessibilityLabel(L("收起出包结果"))
            }
            StageTimeline(task:task)
            Divider()
            // 装机主路 = Xcode 登录的 Apple ID 签名 + 数据线装 + 自动拷资源包（10-26 维护者 定）；AltStore 作备选，说明在装机步骤页
            let installed = store.installTask(after:task)
            VStack(alignment:.leading, spacing:8) {
                Text(L("接下来两步")).font(.headline)
                step(1, installed.map { $0.task.unpackPending ?? (L("已推到 iPhone") + ($0.device.map { "「\($0)」" } ?? "") + Store.packPushedClause($0.task.packFiles)) }
                        ?? L("用数据线连上 iPhone，一键装好：用你在 Xcode 里登录的 Apple ID 签名，资源包一起拷进去")) {
                    if installed == nil {
                        HStack(spacing:10) {
                            if !InstallGuideView.xcodeInstallVerified { VerifyingBadge().help(L("这条路还在真机验证，装不上或打不开时请改用下面的 AltStore。")) }   // 与装机步骤页同一开关（审计 P2-7）
                            Button(L("改用 AltStore")) { store.showInstallGuide = true }.buttonStyle(.borderless)
                            Button { if let game { store.requestInstall(game) } } label: { Label(L("推到 iPhone"), systemImage:"iphone.and.arrow.forward") }
                                .platformButtonStyle(prominent:true).controlSize(.small).disabled(game == nil || store.executionBusy)
                        }
                    } else {
                        Image(systemName:"checkmark.circle.fill").font(.title3).foregroundStyle(Brand.accent)
                    }
                }
                step(2, L("打开游戏：第一次会先导入游戏资源（几分钟），保持在前台、别锁屏，导入完会自动进入游戏")) { EmptyView() }
                Text(L("免费 Apple ID 签的 App 7 天过期：到时连上 iPhone 点游戏库第一格「S2Arm」里的「续签 S2Arm」，手机上的游戏不用重新推。"))
                    .font(.caption).foregroundStyle(.secondary)
            }
        }
        .padding(WorkspaceLayout.cardInset).frame(maxWidth:.infinity, alignment:.leading)
        .background(Brand.accent.opacity(0.05), in:RoundedRectangle(cornerRadius:14, style:.continuous))
        .glassSurface(WorkspaceLayout.cardRadius)
        .task(id:task.id) { let ipa = task.ipaPath; facts = await Task.detached { PackFacts.read(ipa:ipa) }.value; sealed += 1 }
    }
    private func fact(_ icon: String, _ name: String, _ value: String, tint: Color = .primary) -> some View {
        GridRow {
            Image(systemName:icon).foregroundStyle(.secondary).frame(width:18)
            Text(name).foregroundStyle(.secondary)
            Text(value).foregroundStyle(tint).fixedSize(horizontal:false, vertical:true).textSelection(.enabled)
        }
    }
    private func step<A: View>(_ n: Int, _ text: String, @ViewBuilder action: () -> A) -> some View {
        HStack(alignment:.center, spacing:12) {
            Text("\(n)").font(.callout.weight(.bold).monospacedDigit()).foregroundStyle(Brand.accent)
                .frame(width:26, height:26).background(Brand.accent.opacity(0.12), in:Circle())
            Text(text).font(.callout).fixedSize(horizontal:false, vertical:true)
            Spacer(minLength:12)
            action()
        }
    }
}

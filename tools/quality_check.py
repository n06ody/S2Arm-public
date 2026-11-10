#!/usr/bin/env python3
"""移植质量快检（维护者 10-29 06:2x：五个问题——画面 / 帧率 / 满屏 / 直连默认启动 / 能耗，用已有真机局产物、最简单的办法自动判；AI 看图也算自动化）：
  quality_check.py <局目录> --report <报告目录> [--game <款>] [--root <产物根>] [--device <机型>] [--contributor <署名>]
      贡献者一条命令（10-29 维护者：开发者自查 / 自纠 / 自修）：局目录 = 自己的一场局（iPhone 真机局 tools/dev_run.sh 的 out_*，或 Mac 局
      runtime/run_mac.sh / 维护方 entry_check.sh 的局目录）；证据（截图 / frames.tsv / gfx_mode.txt / 功耗 plist / host.log）拷进 <报告目录>/evidence/
      并脱敏（家目录 / 用户名 / App 容器路径 / UUID / 邮箱 / IP / 钥匙样字符串），再对证据副本判五项 → report.md（可直接贴 issue）+ report.json
      （s2a.quality-report.v1：工具指纹、每份证据 sha256、程序号 / 构建号、--root 给了时带能力内容哈希）。维护方拿证据目录重跑得同一份指纹
      入账：tools/caps_ledger.py ingest <报告目录>（docs/名单_验证账.tsv）
  quality_check.py <供数表.tsv | 局目录…> [--out <表.tsv>] [--no-ai]
      供数表 = 基础设施线「每款最新一场真机局」表（列：款 / 作业目录 / …，# 开头是注释）；局目录 = tools/dev_run.sh 的 out_*
      每款一行打到标准输出（--out 另写 TSV）：款 / 画面 / 帧率 / 满屏 / 直连默认启动 / 能耗（过 / 不过 / 没数据）/ 读数 / 截图 / 局目录
  quality_check.py --selftest-ai <应判花屏的截图> <应判正常的截图> [<画风对照截图>…]   AI 判官正反对照：花屏那张必须判异常（把握 ≥ 0.8）、
      正常那张必须判正常、画风对照（暗部 / 剪影 / 泛光等美术风格）不许判成有把握的异常，否则退出 1

五项口径（每项只回答一个问题，判不了就写「没数据」，不猜）：
  画面：AI 一次看这局的截图（shots_ap 到 MARK / 中途 / 收局 + shots，最多 4 张），逐张答有没有花屏、贴图错、黑块、闪烁残影、UI 错位；
        任一张「异常」→ 不过；全部「正常」→ 过；有「拿不准」且没有异常 → 没数据（读数写拿不准的那张）。不用像素阈值
  帧率：tools/score_cells.py 的帧率格（MARK 后 ≥ 55 秒窗口，平均帧率 ≥ 游戏自己要的帧率 × 99% 且慢帧 ≤ 1%；目标帧率取 host.log 帧节拍行）；
        score_cells 在未满屏时把帧率 / 能耗清成未测，快检五问各自独立、照判，读数注「未满屏局」
  满屏：score_cells.fullscreen——直连出图层宽高比与 iOS 壳屏幕比差 ≤ 1%（host.log 两行，机械精确）。截图是从出图层截的
        （16:9 出图层截出来就是 16:9，黑边在层外、截图里看不到；10-29 马车8 / 动森截图 2346×1320），所以不能拿截图黑列判；
        host.log 没这两行时退回截图：截图比例与 iPhone 屏同（≥ 2.0）才数左右纯黑列（每列 ≥ 97% 像素 max(R,G,B) ≤ 12，两侧都 ≥ 1% = 黑边），
        只看 AI 判为游戏画面的截图（菜单页按设计 16:9 居中留边不算）
  直连默认启动：图形模式（gfx_mode.txt 首行，没有就取 host.log「图形模式：」行）按缺省定成「直连」且 frames.tsv ≥ 1000 帧 → 过（与普查「直连能跑」同线）；
        「通用」或直连出图不足 1000 帧 → 不过；S2A_GFX 环境变量指定的模式 → 没数据（不是默认启动）。不看 AI 判没判到游戏画面（那是进局一问，10-31 维护者拆开）
  能耗：score_cells 的窗口功率（pwh → pw1 整机均值）≤ 门槛（游戏要 60 帧 ≤ 4.8 W，其余 ≤ 3.7 W），不看热档；
        帧率格不过时瓦数达标也判不过（维护者 10-22：降频省电但掉了帧不算成果），读数注「帧率未保持」；没到 MARK / 窗口不完整 → 没数据
看图判官（S2A_QC_PROVIDER 指定，缺省按顺序挑第一个可用的）：openrouter（环境 OPENROUTER_API_KEY，或维护方机器的 ~/.s2arm/.secrets/openrouter.key；
  模型 S2A_QC_MODELS）/ anthropic（环境 ANTHROPIC_API_KEY，模型 S2A_QC_ANTHROPIC_MODEL）/ claude-cli（本机 claude 命令行，自己的 Claude 账号）；
  不指定时按 relay（自建 OpenAI 兼容中转站，地址 S2A_QC_RELAY_URL、钥匙 ~/.s2arm/.secrets/relay.key；没设地址就不用）→ openrouter（1 小时内回过 402 就跳过，
  标记与驾驶员共用）→ anthropic → claude-cli（只在没有中转站钥匙时：我方机器的 CLI 走同一个中转站）逐档问，任何一档失败（402 额度用完 / 401 / 429 / 5xx / 网络 / 中转站丢图）落下一档；
  一个都没有 → 画面 / 直连默认启动两项「没数据」（读数写明），其余照判。钥匙只从环境 / 文件读，不打印、不写进报告
AI 回答按截图内容 sha256 缓存在 S2A_QC_CACHE（缺省 ~/.cache/s2arm-qc，同一张图不重复问）"""
import base64, csv, datetime, hashlib, io, json, os, plistlib, re, shutil, subprocess, sys, tempfile, urllib.error, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from PIL import Image
import score_cells
from score_cells import cells, window_power, window_power_note, w_max, rd
from game_fps import target as game_fps

CACHE = os.path.expanduser(os.environ.get('S2A_QC_CACHE', '~/.cache/s2arm-qc'))
OUR_KEY = os.path.expanduser('~/.s2arm/.secrets/openrouter.key')
RELAY_KEY = os.path.expanduser('~/.s2arm/.secrets/relay.key')
OR402 = os.path.expanduser(os.environ.get('S2A_OR_402_MARK', '~/.cache/s2a/openrouter_402'))   # OpenRouter 回过 402 的标记（tests/autoplay/vision_jev.py / vision_or.py 共用），1 小时内跳过
MODELS = os.environ.get('S2A_QC_MODELS', 'anthropic/claude-sonnet-5.5,moonshotai/kimi-k3').split(',')
PROMPT = ('你是游戏移植的画面质检员。下面 {n} 张是同一款 Switch 游戏移植到 iPhone 后的真机截图（按顺序编号 1..{n}）。逐张回答：\n'
          '1. 游戏画面：这张是不是已在可玩的游戏画面（角色 / 赛道 / 地图 / 战斗等可操作场景算是；文字冒险 / 视觉小说的剧情对话页、且画面上有游戏自己的操作提示'
          '（如 AUTO / SKIP / LOG / MENU、选项按钮）也算是——这类游戏的玩法就是读对话、做选择；标题页、主菜单、读盘、没有操作提示的过场字幕、系统提示框算否）；\n'
          '2. 画质：有没有渲染错误——花屏 / 噪点、贴图错或缺、不该有的黑块或色块、闪烁残影、UI 错位或被裁。正常的美术风格、暗场景、特效、游戏自带黑边不算错。\n'
          '只有确信是渲染错误才判异常；可能是美术风格（前景剪影、暗角、单色调场景、雾 / 泛光）时判拿不准。\n'
          '只回一行 JSON：{{"图": [{{"号": 1, "游戏画面": "是|否", "画质": "正常|异常|拿不准", "把握": 0.0-1.0, "问题": "异常或拿不准时写看到了什么，正常写空"}}, ...]}}')
# 复核（维护者 10-29：拿不准的对照同款 Ryujinx 截图）：首轮判异常 / 拿不准的截图 + 同款 Ryujinx 参考图（画面线 visual/truth/<款>/<检查点>/ryu_*.png，
#   场景多半不同，用来辨这款的美术风格）再问一次，复核结论替换首轮。10-29 丝之歌前景剪影蘑菇 / 暗角首轮判异常，实为美术风格
TRUTH = os.path.expanduser(os.environ.get('S2A_QC_TRUTH', '~/.s2arm/run/s2arm/visual/truth'))   # 没有就不复核（贡献者机器）
PROMPT2 = ('前 {m} 张是同一款 Switch 游戏在电脑模拟器 Ryujinx 上正确渲染的参考截图（场景可能不同，只用来认这款游戏的美术风格）；'
           '后 {n} 张是移植到 iPhone 后的真机截图（编号 1..{n}），首轮质检怀疑有渲染错误：{why}\n'
           '对照参考图的风格，逐张判断真机截图的问题是渲染错误，还是这款游戏本来的美术风格。参考图与真机截图场景不同时，'
           '不要因为某处细节在参考图里看不到就判异常；只有花屏 / 噪点 / 撕裂 / 残留色块这类任何画风都不会有的错误才判异常。'
           '只回一行 JSON：{{"图": [{{"号": 1, "画质": "正常|异常|拿不准", "把握": 0.0-1.0, "问题": "异常或拿不准时写看到了什么，正常写空"}}, ...]}}')

# 定论（11-01 维护者：画面「拿不准」= 没数据，fx 批进局 10 款 7 款落在这；判官要给出过 / 不过）：首轮 + 复核后仍拿不准（或把握不足的异常）的那几张，
#   连同本局其余截图（同一局不同时刻：渲染错误多半跨帧都在，转场 / 黑场一闪就过）再问一次，题面收窄成一张清单、只许答正常或异常
PROMPT3 = ('下面 {n} 张是同一款 Switch 游戏移植到 iPhone 后同一局里不同时刻的截图（编号 1..{n}），其中 {sus} 号前面的质检拿不准：{why}\n'
           '只回答它们画质「正常」还是「异常」，不许答拿不准。按下面清单判，清单里没有的都算正常：\n'
           '异常（任何画风都不会有）：① 横 / 竖条带或矩形色块切断画面内容、与周围不连续（撕裂、旧帧残留、错位的色带）；② 拉长的尖刺三角面、炸开的几何；'
           '③ 品红 / 棋盘格 / 纯色块顶替了本该有贴图的东西；④ 有 HUD / 字幕但整个游戏世界没画出来（全黑或全单色，且不是淡入淡出、读盘）；⑤ 花屏噪点铺满大片区域；⑥ 角色 / 物体本该在的位置被一个不透明、边缘锐利的纯黑或纯色轮廓（圆盘、方块、剪影）顶替，看不到它的贴图与细节——影子是贴地、半透明、边缘柔和的，立在角色位置、把角色整个盖住的纯黑圆盘不是影子。\n'
           '正常：暗场景 / 夜景（看得见灯光或轮廓）、景深 / 动态模糊、标题或菜单背景虚化、网点 / 棋盘式抖动半透明、复古像素风与街机外框、GAME OVER / 演示画面、'
           '触屏用的大按钮 / 鼓面等覆盖层、渐变光晕、黑边、淡入淡出。\n'
           '其余张只当参照（同一局的别的时刻），也一并回答。只回一行 JSON：{{"图": [{{"号": 1, "画质": "正常|异常", "清单": "①–⑤ 中的编号，正常写空", "问题": "异常时写看到了什么，正常写空"}}, ...]}}')


def settle(ps, ans):   # 首轮 / 复核后仍拿不准的几张 → 定论（就地改 ans，返回问了几张；不问返回 0）
    sus = [i for i, a in enumerate(ans) if a.get('画质') == '拿不准' or (a.get('画质') == '异常' and float(a.get('把握') or 0) < 0.8)]
    if not sus: return 0
    why = '；'.join(f'{i + 1} 号：{ans[i].get("问题", "")}' for i in sus)
    a3 = ai_judge(ps, PROMPT3.format(n=len(ps), sus='、'.join(str(i + 1) for i in sus), why=why))
    for i in sus:
        q = a3[i].get('画质')
        if q in ('正常', '异常'): ans[i] = dict(ans[i], 画质=q, 把握=1.0, 问题=a3[i].get('问题', '') + (f'（清单 {a3[i].get("清单")}）' if a3[i].get('清单') else ''), 定论=a3[i].get('模型', ''))
    return len(sus)


def refs(name, k=2):   # 同款 Ryujinx 参考图，每个检查点取一张，最多 k 张
    d = os.path.join(TRUTH, name)
    if not os.path.isdir(d): return []
    out = []
    for cp in sorted(os.listdir(d)):
        ps = sorted(f for f in os.listdir(os.path.join(d, cp)) if f.startswith('ryu_') and f.endswith('.png')) if os.path.isdir(os.path.join(d, cp)) else []
        if ps: out.append(os.path.join(d, cp, ps[0]))
    return out[:k]


def shots(o):   # 这局要看的截图，最多 4 张：iPhone 局 shots_ap 的到 MARK / 中途在前，再补最新的（收局 / 卡住）；
    #   Mac 局（run_mac.sh / entry_check 局目录，或 one_game 日志目录）截图在局目录顶层 ap_*.png 与 vision/vNN.png
    ap = os.path.join(o, 'shots_ap'); sh = os.path.join(o, 'shots')
    order = lambda d: sorted((os.path.join(d, f) for f in os.listdir(d) if f.endswith('.png')), key=os.path.getmtime) if os.path.isdir(d) else []
    if order(ap) or order(sh):   # 看有没有图、不看有没有目录：Mac 局（mac_health）局目录里也有 shots/，但常是空的，截图在顶层 ap_*.png
        pick = [p for k in ('ap_mark', 'ap_mid') for p in order(ap) if os.path.basename(p).startswith(k)]
        rest = [p for p in order(ap) + order(sh) if p not in pick]
        return (pick + rest[::-1])[:4]
    top = lambda *n: [os.path.join(o, x) for x in n if os.path.exists(os.path.join(o, x))]
    vd = os.path.join(o, 'vision')
    vis = sorted((os.path.join(vd, x) for x in os.listdir(vd) if re.fullmatch(r'v\d+\.png', x)), key=lambda q: int(re.sub(r'\D', '', os.path.basename(q)))) if os.path.isdir(vd) else []
    # 到了 MARK（进局 / 中途 / 收局截图够两张）就只看这几张：导航途中的 vision/ 截图常拍到读盘 / 转场黑场，混进来 AI 只能答「拿不准」
    #   （11-01 已过关 11 款对照：画面 6 / 9 款没数据，多是 v05 / v30 整张黑这类导航图）；没到 MARK 才拿最后两张导航图兜底看卡在哪
    inside = top('ap_mark.png', 'ap_mid.png', 'ap_end.png')
    cand = inside if len(inside) >= 2 else top('ap_mark.png', 'ap_mid.png') + vis[-2:] + top('last_vision.png', 'ap_end.png', 'ap_stuck.png')
    out, seen = [], set()
    for q in cand:
        h = hashlib.sha256(open(q, 'rb').read()).hexdigest()
        if h not in seen: seen.add(h); out.append(q)
    return out[:4]


# 整幅单色（11-01 维护者转运维线 c1031x：异度3 黑屏修复前收局 / 路线截图整幅纯黑，AI 判「拿不准」记成没数据）：看得见的坏画面用像素直接判，不交 AI。
#   只看本该在出游戏画面的四张：到 MARK / 中途 / 收局 / 卡住（局目录顶层或 shots_ap/ 下的 ap_mark / ap_mid / ap_end / ap_stuck）。
#   导航途中的 vision/ 截图与路线 shots/ 截图会拍到读盘 / 跳过过场的黑场（异度3 修复后那局 skip0_before 就是整幅黑），不算。
#   主色 = RGB 各按 5 位（32 级）量化后像素最多的一格；占比 ≥ SOLID_MIN 即整幅单色。小块纯色（如 10×8 像素的纯绿块）不在此列：HUD 指示灯也是纯色，像素规则分不开，仍交 AI
# 读数门（11-01 无参考判错调研 M4）：整幅单色本身分不开「画了没出来」和「加载 / 黑场」，用 frames.tsv 同一时刻的每帧绘制数分：
#   绘制 ≥ max(50, 0.25 × MARK 后中位) → 不过（画了没出来，异度3 黑屏修复前收局黑屏时每帧绘制 804）；绘制 < 5 → 过（加载黑场）；其间或没有读数 → 拿不准（画面记没数据）。
#   各张对应的时刻：ap_mark = MARK 帧前后 15 帧，ap_mid = MARK 之后全段，ap_end / ap_stuck = 末 30 帧（收局时在显示的）。局限：截图前的亮度序列拿不到，淡出到黑判不出
SOLID_MIN = 0.985
def solid(p):   # 截图 → (主色占比, (R, G, B) 该格中心值)
    import numpy as np
    a = np.asarray(Image.open(p).convert('RGB')) // 8; k = a[..., 0].astype(int) * 1024 + a[..., 1].astype(int) * 32 + a[..., 2]
    v, c = np.unique(k, return_counts=True); i = int(c.argmax())
    return float(c[i]) / k.size, (int(v[i]) // 1024 * 8 + 4, int(v[i]) // 32 % 32 * 8 + 4, int(v[i]) % 32 * 8 + 4)


SOLID_SHOTS = ('ap_mark.png', 'ap_mid.png', 'ap_end.png', 'ap_stuck.png')
def solid_shots(o): return [p for d in (o, os.path.join(o, 'shots_ap')) for n in SOLID_SHOTS for p in [os.path.join(d, n)] if os.path.exists(p)]
def draws_at(o):   # 局目录 → {截图名: 那一刻的每帧绘制中位}，加上 '门'：max(50, 0.25 × MARK 后中位)；没有 frames.tsv / draw_n 列 → {}
    import statistics
    rows = [l.split('\t') for l in rd(o, 'frames.tsv').split('\n') if l.strip()]
    if len(rows) < 2 or 'draw_n' not in rows[0] or 'frame' not in rows[0]: return {}
    i, j = rows[0].index('draw_n'), rows[0].index('frame'); fr = []
    for r in rows[1:]:
        try: fr.append((int(r[j]), int(r[i])))
        except (IndexError, ValueError): pass
    if not fr: return {}
    m = re.search(r'S2A 标记 MARK：帧 (\d+)', rd(o, 'host.log')); mk = int(m.group(1)) if m else None
    med = lambda xs: statistics.median(xs) if xs else None
    after = [d for f, d in fr if mk is None or f >= mk]
    out = {'门': max(50, 0.25 * (med(after) or 0)), 'ap_end.png': med([d for _, d in fr[-30:]]), 'ap_stuck.png': med([d for _, d in fr[-30:]])}
    if mk is not None: out['ap_mark.png'] = med([d for f, d in fr if abs(f - mk) <= 15]); out['ap_mid.png'] = med(after)
    return out
def solid_judge(o):   # 局目录 → (不过 [...], 拿不准 [...])：四张里整幅单色的，按读数门分；加载黑场（绘制 < 5）不列
    bad, unsure, dr = [], [], draws_at(o)
    for p in solid_shots(o):
        r, c = solid(p)
        if r < SOLID_MIN: continue
        n = os.path.basename(p); d = dr.get(n); w = f'{n}：整幅单色 RGB{c} 占 {r:.1%}'
        if d is None: unsure.append(w + '，没有绘制读数')
        elif d >= dr['门']: bad.append(w + f'，同一时刻每帧绘制 {d:.0f} ≥ {dr["门"]:.0f}（画了没出来）')
        elif d >= 5: unsure.append(w + f'，每帧绘制 {d:.0f} 在加载 5 与门 {dr["门"]:.0f} 之间')
    return bad, unsure


MAC_SCREEN_W = 2346   # Mac 验收 / 复判 / 健康局的全能力屏宽（tools/mac_health.sh 缺省，17 Pro Max 横屏比例 × 1080）


def ident(o):   # 局目录 → (程序号, 构建号)：host.log「程序号 …」「构建号 <32 位>」，读不到给空
    hl = rd(o, 'host.log')
    pid = re.search(r'程序号 ([0-9A-Fa-f]{16})', hl); bid = re.search(r'构建号 ([0-9a-f]{32})', hl)
    return (pid.group(1).upper() if pid else ''), (bid.group(1) if bid else '')


def brightness_hold(hl):   # 宿主心跳行「亮度保持：设定 X，实际最小 A 最大 B，重设 N 次」（ios/app.m，设了 S2A_BRIGHTNESS 才打）→ (读数, 是否无效)
    """10-30 维护者：测试设备开着自动亮度，起局设的 0.5 会被拉回 0.2；整局实际亮度偏离设定超过 0.05 → 这局整机瓦数不是该亮度口径，判局无效"""
    m = re.findall(r'亮度保持：设定 ([\d.]+)，实际最小 ([\d.]+) 最大 ([\d.]+)', hl)
    if not m: return '', False
    t, lo, hi = (float(x) for x in m[-1])
    bad = abs(lo - t) > 0.05 or abs(hi - t) > 0.05
    return f'亮度 设定 {t:.2f}、实际 {lo:.2f}–{hi:.2f}' + ('（偏离 > 0.05，局无效）' if bad else ''), bad


def first_encounter(hl):   # 宿主收局行「首遇现建：MARK 后提交线程现建管线 N 条，最长 x ms，合计 y ms」→ 帧率读数的标注（只标注，不改帧率项判法）
    """10-29 画面线 / 维护者：删链接预编，改读数监控首遇卡顿；最长 > 16.7 ms 标「首遇卡」，局目录回画面线补预热种子表。没有这行 → 空串"""
    m = re.findall(r'首遇现建：MARK 后提交线程现建管线 (\d+) 条，最长 ([\d.]+) ms，合计 ([\d.]+) ms', hl)
    if not m: return ''
    n, mx, tot = m[-1]
    return f'；首遇现建 {n} 条，最长 {mx} ms，合计 {tot} ms' + ('（首遇卡）' if float(mx) > 16.7 else '')


def edge_black(path):   # → (左黑列占宽比, 右黑列占宽比)
    im = Image.open(path).convert('RGB'); w, h = im.size
    small = im.resize((w // 4 or 1, h // 4 or 1)) if w >= 800 else im; sw, shh = small.size; px = small.load()
    def black(x): return sum(max(px[x, y]) <= 12 for y in range(shh)) >= 0.97 * shh
    l = 0
    while l < sw and black(l): l += 1
    r = 0
    while r < sw - l and black(sw - 1 - r): r += 1
    return l / sw, r / sw


def game_drew_sides(hl):
    """宽屏包装层认定两侧黑是游戏自己画的（host.log「出图两侧黑是游戏画的…不判未满屏」说明行）且全局没有一条【红】出图未满屏：
    此时截图上两侧扩展带是纯色块不是缺内容，band_judge 的「纯色块占比」空洞判据不采信（11-01 满屏画面线：丝之歌 3 行说明行、0 条【红】被判满屏不过）"""
    return '出图两侧黑是游戏画的' in hl and score_cells.fs_reds(hl)[0] == 0   # 界面页（MARK 前）的红不算（维护者 11-02，见 score_cells.fs_reds）


def band_judge(paths):
    """加宽截图逐张过 widen_accept.band_checks（两侧扩展带空洞 / 16:9 边界接缝）→ (不过原因列表, 各张读数)"""
    import widen_accept as wa
    wa.ensure_deps(); bad, rd = [], []
    for p in paths:
        b, r = wa.band_checks(Image.open(p).convert('RGB'))
        bad += [f'{os.path.basename(p)}：{x}' for x in b]; rd.append(f'{os.path.basename(p)} {r}')
    return bad, '；'.join(rd)


def bars(path):
    l, r = edge_black(path)
    return l >= 0.01 and r >= 0.01, l, r


def _img_url(path):   # 缩到宽 1280 的 PNG（2867 宽的原图对判花屏没多出信息，省流量）
    im = Image.open(path).convert('RGB')
    if im.width > 1280: im = im.resize((1280, round(im.height * 1280 / im.width)))
    b = io.BytesIO(); im.save(b, 'PNG')
    return 'data:image/png;base64,' + base64.b64encode(b.getvalue()).decode()


def providers():   # 看图判官后端顺序：S2A_QC_PROVIDER 指定只用那一个；否则可用的按 relay → openrouter（没 402 标记）→ anthropic → claude-cli 依次
    p = os.environ.get('S2A_QC_PROVIDER')
    if p: return [p]
    or402 = os.path.exists(OR402) and datetime.datetime.now().timestamp() - os.path.getmtime(OR402) < 3600
    return [n for n, ok in (('relay', bool(os.environ.get('S2A_QC_RELAY_URL')) and os.path.exists(RELAY_KEY)), ('openrouter', not or402 and (os.environ.get('OPENROUTER_API_KEY') or os.path.exists(OUR_KEY))),
                            ('anthropic', os.environ.get('ANTHROPIC_API_KEY')),
                            ('claude-cli', not (os.environ.get('S2A_QC_RELAY_URL') and os.path.exists(RELAY_KEY)) and shutil.which('claude'))) if ok]   # 有中转站钥匙不排 CLI（11-02 维护者：我方机器的 CLI 走同一个中转站，只多开销）


def provider():   # 第一档（报告里记判官名）；'' = 没有
    return (providers() or [''])[0]


def _png(path):   # 缩到宽 1280 的 PNG 字节
    im = Image.open(path).convert('RGB')
    if im.width > 1280: im = im.resize((1280, round(im.height * 1280 / im.width)))
    b = io.BytesIO(); im.save(b, 'PNG'); return b.getvalue()


def _post(req, what):   # 发请求；失败一律换成带原因的 RuntimeError（HTTP 状态 + 服务端说明前 200 字 / 网络错误），钥匙不进消息
    try: return json.load(urllib.request.urlopen(req, timeout=180))
    except urllib.error.HTTPError as e:
        try: body = e.read().decode('utf-8', 'replace')
        except Exception: body = ''
        raise RuntimeError(f'{what} HTTP {e.code}：{scrub(" ".join(body.split()))[:200] or e.reason}') from None
    except (urllib.error.URLError, TimeoutError, OSError) as e:
        raise RuntimeError(f'{what} 连不上：{getattr(e, "reason", e)}') from None


def _ask(imgs, prompt):   # 逐档逐个候选模型问一次 → 依次产出 (模型, 回答原文)；某档 / 某个模型不可用时产出 (原因, None)、接着问下一个
    pvs = providers()
    if not pvs: raise RuntimeError('没有看图判官（设 OPENROUTER_API_KEY / ANTHROPIC_API_KEY，或装好 claude 命令行）')
    for pv in pvs:
        try: yield from _ask_one(pv, imgs, prompt)
        except (RuntimeError, OSError, KeyError, ValueError) as e: yield f'{pv}：{e}', None   # 这一档整个不可用（402 额度 / 网络 / 丢图…）→ 下一档


def _ask_one(pv, imgs, prompt):
    if pv == 'openrouter':
        tok = (os.environ.get('OPENROUTER_API_KEY') or open(OUR_KEY).read()).strip()
        content = [{'type': 'image_url', 'image_url': {'url': _img_url(q)}} for q in imgs] + [{'type': 'text', 'text': prompt}]
        for i, m in enumerate(MODELS):
            body = {'model': m, 'max_tokens': 3000, 'reasoning': {'effort': 'low'}, 'messages': [{'role': 'user', 'content': content}]}
            req = urllib.request.Request(os.environ.get('S2A_QC_OR_URL', 'https://openrouter.ai/api/v1/chat/completions'), data=json.dumps(body).encode(),
                                         headers={'Authorization': 'Bearer ' + tok, 'Content-Type': 'application/json'})
            try: r = _post(req, f'OpenRouter {m}')
            except RuntimeError as e:
                if 'HTTP 402' in str(e): os.makedirs(os.path.dirname(OR402), exist_ok=True); open(OR402, 'w').close()   # 额度用完：记标记，1 小时内驾驶员和判官都跳过 OpenRouter
                if any(f'HTTP {c}' in str(e) for c in (400, 403, 404)) and i + 1 < len(MODELS): yield str(e), None; continue   # 这个模型不可用 → 换模型；402 等钥匙级错误整档失败
                raise
            yield m, r['choices'][0]['message']['content'] or ''
    elif pv == 'relay':   # 模型 claude-sonnet-5（11-02 实测 claude-sonnet-5-5 那条渠道约 70% 丢图，见 tests/autoplay/vision_or.py）；丢图判据留作兜底（同 vision_or.py）：回的输入 token 不到「题面字节 ÷ 3.5 + 每张图 500」就当丢图，题面尾加请求号重问，最多 S2A_QC_RELAY_TRIES 次，仍丢 → 这一档失败
        rk = open(os.environ.get('S2A_QC_RELAY_KEY', RELAY_KEY)).read().strip(); m = os.environ.get('S2A_QC_RELAY_MODEL', 'claude-sonnet-5')
        url = os.environ['S2A_QC_RELAY_URL']; est = len(prompt.encode()) / 3.5 + 500 * len(imgs)
        imgc = [{'type': 'image_url', 'image_url': {'url': _img_url(q)}} for q in imgs]; tries = int(os.environ.get('S2A_QC_RELAY_TRIES', '2'))
        for t in range(tries):
            text = prompt if t == 0 else f'{prompt}\n（请求号 {os.getpid()}-{t}）'
            req = urllib.request.Request(url, data=json.dumps({'model': m, 'max_tokens': 3000, 'messages': [{'role': 'user', 'content': imgc + [{'type': 'text', 'text': text}]}]}).encode(),
                                         headers={'Authorization': 'Bearer ' + rk, 'Content-Type': 'application/json'})
            r = _post(req, f'中转站 {m}'); pt = (r.get('usage') or {}).get('prompt_tokens')
            if isinstance(pt, (int, float)) and pt < est: continue
            yield f'中转站/{m}', r['choices'][0]['message']['content'] or ''; return
        raise RuntimeError(f'中转站 {m} 丢图 {tries} 次')
    elif pv == 'anthropic':
        m = os.environ.get('S2A_QC_ANTHROPIC_MODEL', 'claude-sonnet-5-5')
        content = [{'type': 'image', 'source': {'type': 'base64', 'media_type': 'image/png', 'data': base64.b64encode(_png(q)).decode()}} for q in imgs] + [{'type': 'text', 'text': prompt}]
        req = urllib.request.Request('https://api.anthropic.com/v1/messages', data=json.dumps({'model': m, 'max_tokens': 3000, 'messages': [{'role': 'user', 'content': content}]}).encode(),
                                     headers={'x-api-key': os.environ['ANTHROPIC_API_KEY'], 'anthropic-version': '2023-06-01', 'content-type': 'application/json'})
        r = _post(req, f'Anthropic {m}')
        yield m, ''.join(c.get('text', '') for c in r.get('content', []) if c.get('type') == 'text')
    elif pv == 'claude-cli':   # 自己的 Claude：图片缩好写进临时目录，让 CLI 用 Read 读
        with tempfile.TemporaryDirectory(prefix='s2arm-qc-') as d:
            ps = []
            for i, q in enumerate(imgs, 1):
                f = os.path.join(d, f'{i}.png'); open(f, 'wb').write(_png(q)); ps.append(f)
            ask = prompt + '\n\n图片按编号依次是（用 Read 工具读）：\n' + '\n'.join(f'{i}. {f}' for i, f in enumerate(ps, 1))
            cmd = ['claude', '-p', ask, '--allowedTools', 'Read', '--add-dir', d, '--strict-mcp-config', '--no-session-persistence', '--output-format', 'text']   # 不读 MCP、不存会话（11-02：省 CLI 启动开销）
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            inj = [k for k in ('ANTHROPIC_API_KEY', 'ANTHROPIC_BASE_URL', 'ANTHROPIC_AUTH_TOKEN') if os.environ.get(k)]
            if r.returncode and inj:   # 10-29 演练：嵌套在别的 Claude 会话里跑时继承了桌面端注入的 ANTHROPIC_*，命令行报「Invalid API key」；去掉再试一次（用它自己的登录）
                r = subprocess.run(cmd, capture_output=True, text=True, timeout=600, env={k: v for k, v in os.environ.items() if k not in inj})
            last = next((l for l in reversed((r.stderr or r.stdout or '').splitlines()) if l.strip()), '')
            yield ('claude-cli', r.stdout) if r.returncode == 0 else (f'claude-cli 退出 {r.returncode}：{scrub(last.strip())[:200]}', None)
    else:
        raise RuntimeError(f'没有看图判官 {pv}（设 OPENROUTER_API_KEY / ANTHROPIC_API_KEY，或装好 claude 命令行）')


ANDROID_NOTE = ('\n注意：这批是安卓整屏截图（adb screencap），画面上叠着触屏虚拟手柄的半透明按键 / 摇杆、两侧可能有系统黑边，'
                '这些是外壳的覆盖层，不算渲染错误、不算 UI 错位。')


def ai_judge(paths, prompt=None, pre=(), cached_only=False):   # → [{号, 游戏画面, 画质, 问题}]，与 paths 同序；pre = 排在前面的参考图（不作答）；读缓存，缺的一次问完
    os.makedirs(CACHE, exist_ok=True); prompt = prompt or PROMPT.format(n=len(paths))
    key = hashlib.sha256(b''.join(hashlib.sha256(open(p, 'rb').read()).digest() for p in list(pre) + list(paths)) + prompt.encode()).hexdigest()[:24]
    cf = os.path.join(CACHE, key + '.json')
    if os.path.exists(cf): return json.load(open(cf))
    if cached_only: return None   # 只读缓存（普查汇总循环里用，不发问）
    last = ''
    for m, txt in _ask(list(pre) + list(paths), prompt):
        if txt is None: last = m; continue   # m 此时是失败原因
        mm = re.search(r'\{.*\}', txt, re.S)
        try: ans = json.loads(mm.group(0))['图'] if mm else None
        except (ValueError, KeyError): ans = None
        if not ans or len(ans) != len(paths): last = f'{m} 回答格式不对：{txt[:80]!r}'; continue
        ans = [dict(a, 模型=m) for a in sorted(ans, key=lambda a: int(a.get('号', 0)))]
        json.dump(ans, open(cf + f'.{os.getpid()}', 'w'), ensure_ascii=False); os.replace(cf + f'.{os.getpid()}', cf)   # 内容寻址 + 原子替换：并发同键写的是同一份结论
        return ans
    raise RuntimeError(f'AI 判官没给出可用回答（{last}）')


def fs_layer(o, hl, ask=True):
    """满屏自动分层（11-02 维护者：待分层压到 0）：MARK 后留黑边段里的截图（判据存的 fit_pil_<k>.png、自动玩家截图，局目录里有的）交看图判官，
    按题面第 1 问「是不是已在可玩的游戏画面」→ {文件名: 局内 | 界面页}。判官不可用 / 没截图 → 空（段仍待分层，不猜）。
    ask=False 只读判官缓存（普查汇总：五问已经问过的同一组图直接用，没问过的仍待分层）"""
    names = sorted({x for g in score_cells.fs_pillar(hl) if g['局内'] and not g['盖住MARK'] for x in g['截图']})
    paths = [os.path.join(o, x) for x in names if os.path.isfile(os.path.join(o, x))]
    if not paths: return {}
    try: ans = ai_judge(paths, cached_only=not ask)
    except RuntimeError as e: print(f'【黄】满屏分层看图判官失败（段仍待分层）：{e}', file=sys.stderr); return {}
    if ans is None: return {}
    return {os.path.basename(p): ('局内' if a.get('游戏画面') == '是' else '界面页') for p, a in zip(paths, ans)}


def check_android(o, name, ai=True):
    """安卓局目录（安卓线 K3）：android.json（机型 / 包名 / 屏幕 / 窗口秒数 / 启动命令）、shots_ap/NN_SSSs.png（整屏截图，含虚拟手柄覆盖层与系统黑边）、
    frames.tsv（frame / present_ns / present_us / wall_us / disp_us / disp_seq，direct-present 帧日志，采集窗口内；disp_us = v5 确认上屏间隔按 seq 对号、未确认留空）、power.tsv（t + run_game.POWER_FIELDS + level）、
    logcat.txt、gfx_mode.txt。没有自动玩家：开窗即已在可玩画面（android.json mark），整窗口就是读数窗口。五项：
      画面 / 到游戏画面：同 iPhone，AI 看截图（题面说明覆盖层不算错）
      帧率：先判采集窗口覆盖——frame_window_us 窗口内帧 span 覆盖 <95% → 「窗口覆盖不足」没数据；
        再要间隔窗口 ≥ 55 秒、平均 ≥ 目标 × 99%、慢帧（> 1.5 倍预算）≤ 1%——disp_us 已确认读数优先，无确认退回 wall_us；目标取 tools/fps_truth.tsv（程序号从包名 org.s2arm.game.t<程序号> 取），没有按 60
      满屏：截图就是整屏，直接数左右纯黑列（同 bars()；只看 AI 判为游戏画面的，没有判官时看全部）
      直连默认启动：启动命令带了 --es s2arm.graphics_mode 就不算默认启动（没数据）；直连 / 通用认 logcat「direct provider prepared」/「Gpu LoadShaders」，
        其次 gfx_mode.txt 首行；直连且有游戏画面 → 过，通用 → 不过
      能耗：power.tsv = t + run_game.POWER_FIELDS + level（run_game.py 写）。整窗口每行 status=Discharging
        且供电旗标无 true 才算；放电符号由本机放电样本推出（非零全同号），不一致 → 没数据；
        电池侧梯形积分（boot_us 同窗，run_game.power_battery_window）算出瓦数 → 「未证」
        （诊断读数，无设备校准，不套 iPhone 整机门槛）；接过电 / 状态未证 / 窗口不齐 → 没数据"""
    import json as _json
    meta = _json.load(open(os.path.join(o, 'android.json'), encoding='utf-8')) if os.path.exists(os.path.join(o, 'android.json')) else {}
    ps = shots(o); out = {'款': name, '局目录': o, '截图': ' '.join(os.path.relpath(x, o) for x in ps)}; read = []
    m = re.search(r't([0-9a-fA-F]{16})', meta.get('package', '')); pid = m.group(1).upper() if m else ''
    ans = None
    if ps and ai:
        try: ans = ai_judge(ps, PROMPT.format(n=len(ps)) + ANDROID_NOTE)
        except Exception as e:
            out['判官问题'] = str(e); read.append(f'AI 判官失败：{e}'); print(f'【黄】看图判官失败（画面 / 直连默认启动记没数据）：{e}', file=sys.stderr)
    game = []
    if ans:
        sure = lambda a: a.get('画质') == '异常' and float(a.get('把握') or 0) >= 0.8
        bad = [f'{os.path.basename(x)}：{a.get("问题", "")}' for x, a in zip(ps, ans) if sure(a)]
        unsure = [f'{os.path.basename(x)}：{a.get("问题", "")}' for x, a in zip(ps, ans) if a.get('画质') in ('异常', '拿不准') and not sure(a)]
        out['画面'] = '不过' if bad else '没数据' if unsure else '过'
        read.append('画面 ' + ('异常 ' + '；'.join(bad) if bad else '拿不准 ' + '；'.join(unsure) if unsure else f'{len(ps)} 张都正常'))
        game = [x for x, a in zip(ps, ans) if a.get('游戏画面') == '是']
    else: out['画面'] = '没数据'
    # 帧率：先算采集窗口覆盖——帧表只装窗口内帧，但块首帧晚到 / 尾帧早停都会留空档；
    # 覆盖 <95% 不能拿手里这段冒充整窗统计（8E 首局：61.1s 窗内只覆盖 56.4s 曾误判窗口成立）
    rows = list(csv.DictReader(open(os.path.join(o, 'frames.tsv'), encoding='utf-8', errors='replace'), delimiter='\t')) if os.path.exists(os.path.join(o, 'frames.tsv')) else []
    # disp_us = v5 displayTiming 已确认上屏间隔（未确认/旧版记录留空）；没有就退回
    # wall_us（CPU present 间隔，契约同款回退）——真实确认读数优先，不混口径。
    disp = [int(r['disp_us']) / 1000 for r in rows if (r.get('disp_us') or '').isdigit() and int(r['disp_us']) > 0]
    win = disp or [int(r['wall_us']) / 1000 for r in rows if (r.get('wall_us') or '').isdigit() and int(r['wall_us']) > 0]
    import fps_truth
    t = fps_truth.load().get(pid.lower()) if pid else None; fps = float(t['目标帧率']) if t else 60.0; bud = 1000.0 / fps
    short = None
    fw = meta.get('frame_window_us')
    if rows and isinstance(fw, list) and len(fw) == 2:
        try:
            w0, w1 = int(fw[0]), int(fw[1])
            t_first = int(rows[0]['present_ns']) // 1000 - int(rows[0].get('present_us') or rows[0]['disp_us'])
            t_last = int(rows[-1]['present_ns']) // 1000
            if w1 > w0 and t_last > t_first:
                span = t_last - t_first
                if span / (w1 - w0) < 0.95:
                    short = (f'帧率 窗口覆盖不足：{(w1 - w0) / 1e6:.1f}s 窗口帧覆盖 {span / 1e6:.1f}s'
                             f'（{span / (w1 - w0):.0%}，首部空 {max(0, t_first - w0) / 1e6:.1f}s、'
                             f'尾部空 {max(0, w1 - t_last) / 1e6:.1f}s）')
        except (ValueError, KeyError):
            pass
    if short:
        out['帧率'] = '没数据'; read.append(short)
    elif sum(win) < 55000: out['帧率'] = '没数据'; read.append(f'帧率 窗口不足 55 秒：{sum(win) / 1000:.0f} 秒、{len(win)} 帧')
    else:
        avg = len(win) * 1000 / sum(win); slow = sum(w > 1.5 * bud for w in win) / len(win) * 100; med = sorted(win)[len(win) // 2]
        out['帧率'] = '过' if avg >= fps * 0.99 and slow <= 1 else '不过'
        read.append(f'帧率 要 {fps:g}（{"帧率真值表" if t else "缺省 60"}）；disp_us：平均 {avg:.2f} 帧/秒，中位 {med:.1f} ms，慢帧 {slow:.2f}%，{len(win)} 帧')
    # 满屏：整屏截图直接数黑列
    look = game or ([] if ans else ps)
    if look:
        bb = [(x,) + bars(x) for x in look]; hit = [b for b in bb if b[1]]
        out['满屏'] = ('局内红' if game else '待分层') if hit else '过'   # 黑边落在 AI 判为游戏画面的截图上才是局内红（11-02 三档）
        read.append('满屏（整屏截图）' + ('黑边 ' + '；'.join(f'{os.path.basename(x)} 左 {l:.1%} 右 {r:.1%}' for x, _, l, r in hit) if hit else f'{len(look)} 张左右无黑边'))
    else: out['满屏'] = '没数据'
    # 直连默认启动
    lc = rd(o, 'logcat.txt'); gm = 'direct' if 'direct provider prepared' in lc else 'generic' if 'Gpu LoadShaders' in lc else ''
    if not gm:
        g0 = (rd(o, 'gfx_mode.txt').strip().split('\n') or [''])[0]; gm = 'direct' if g0.startswith('直连') else 'generic' if g0.startswith('通用') else ''
    if '--es s2arm.graphics_mode' in meta.get('launch', ''): out['直连默认启动'] = '没数据'; read.append('直连默认启动 这局显式指定了图形模式（--es s2arm.graphics_mode），不算默认启动')
    elif gm == 'generic': out['直连默认启动'] = '不过'
    elif gm == 'direct': out['直连默认启动'] = '过' if game else '没数据' if ans is None else '不过'
    else: out['直连默认启动'] = '没数据'; read.append('直连默认启动 logcat / gfx_mode.txt 认不出图形模式（日志要取 <包名>:game 子进程）')
    # 能耗（兄弟项目 power-comparison 口径）：块边界 = frame_window_us，分析窗剔首 15s /
    #   尾 5s；窗内每行须物理放电（status=Discharging 且 plugged=0）、屏幕交互、非省电、
    #   thermal_status<3、电流/电压/电量计读数合法、符号一致、采样间隔 ≤3s、边界全覆盖——
    #   任一门不过整块作废；再跑整局电量计交叉核对，不互证 → 积分不采信。积出来的瓦数是
    #   电池侧估计、未经外部表计校准 → 记「未证」，不套整机门槛。
    pw = list(csv.DictReader(open(os.path.join(o, 'power.tsv'), encoding='utf-8', errors='replace'), delimiter='\t')) if os.path.exists(os.path.join(o, 'power.tsv')) else []
    if not pw:
        out['能耗'] = '没数据'
        psd = meta.get('power_sampler') or {}
        if psd.get('disabled'): read.append(f"能耗 本局未开采样（{psd.get('reason') or '--no-power-sampler'}），不报瓦数")
    else:
        for _p in ('android', os.path.join('android', 'tools')):
            _ap = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), _p)
            if _ap not in sys.path: sys.path.insert(0, _ap)
        import run_game  # android/tools：设备侧功耗判读与运行器同源（#163 后 tools/android_power.py 是 host 采集）
        def _i(r, k):
            try: return int(str(r.get(k, '')).strip())
            except ValueError: return None
        boots = [_i(r, 'boot_us') for r in pw]
        fw2 = meta.get('frame_window_us')
        begin, end = (fw2[0], fw2[1]) if isinstance(fw2, list) and len(fw2) == 2 else (
            (boots[0], boots[-1]) if boots and all(b is not None for b in boots) else (None, None))
        res = run_game.power_battery_window(pw, begin, end)
        if res.get('averageW') is None:
            out['能耗'] = '没数据'; read.append(f"能耗 {res.get('reason', '窗口不成立')}")
        else:
            co = run_game.power_charge_corroboration(pw)
            line = (f"能耗 电池侧均值 {res['averageW']:.2f} W × {res['durationSeconds']:.0f}s"
                    f"（电流符号 {res.get('rawCurrentSign')}、{res.get('samples')} 采样"
                    + (f"、电量计 {res['chargeCounterW']:.2f} W 比值 {res['chargeToCurrentRatio']:.2f}"
                       if res.get('chargeCounterW') is not None else "") + "）")
            if co.get('corroborated'):
                out['能耗'] = '未证'
                read.append(line + f"；电量计交叉核对过（{co.get('method', '')}），仍非外部校准")
            else:
                out['能耗'] = '没数据'
                read.append(line + f"；电量计交叉核对不成立（{co.get('error') or co.get('method', '')}），积分不采信")
    read.append(f'安卓 {meta.get("serial_model", "")} {meta.get("soc", "")} Android {meta.get("android", "")}，模式 {gm or "—"}，游戏画面 {len(game)}/{len(ps)} 张')
    out['读数'] = '｜'.join(read)
    return out


def metrics(o, hl, pw):
    """数值读数（10-29 维护者 60 帧退回规则，tools/caps_policy.py fps60_measured 读验证账里的这两列）：
    fps60 = 这局 host.log 的包内能力清单 60 帧开关（开 / 关 / -）；fpsAvg = MARK 后 ≥ 55 秒窗口的持续平均帧率（score_cells.fps_window 同一窗口，不足 55 秒 = None）；
    watts = 同窗口整机功率（score_cells.window_power）"""
    m = re.search(r'能力清单：60 帧(开|关)', hl); mk = re.search(r'S2A 标记 MARK：帧 (\d+)', hl)
    try: win = score_cells.fps_window(o, score_cells.frame_mark(o, int(mk.group(1)) if mk else None))[2]   # 重启局起帧取重启后那段
    except Exception: win = []
    ok = sum(win) >= 55000
    # 自适应（10-29 维护者）：接了宿主帧步的局（runtime/src/fps.c「接宿主帧步」行）按实际稳住的档判——hold60 = 窗口里 60 档帧（间隔 ≤ 1.5 × 16.7 ms）的占比
    # widen = 这局宽屏放没放（runtime/src/widen.c 名单门结论；10-30 #996：验证账按它配开 / 关对照，caps_policy.widen_measured 自动强信号）
    wd = '关' if ('宽屏：不放（来源' in hl or '本款不在宽屏已验证名单' in hl) else '开' if ('宽屏：放开（来源' in hl or '本款在宽屏已验证名单' in hl) else '-'
    return {'fps60': m.group(1) if m else '-', 'fpsAvg': round(len(win) * 1000 / sum(win), 2) if ok else None,
            'watts': round(pw, 2) if pw is not None else None,
            'adaptive': '接宿主帧步' in hl, 'hold60': round(sum(w <= 25.0 for w in win) / len(win), 3) if ok else None, 'widen': wd}


def check(o, name, ai=True):
    if os.path.exists(os.path.join(o, 'android.json')): return check_android(o, name, ai)
    hl = rd(o, 'host.log'); ps = shots(o); gm = (rd(o, 'gfx_mode.txt').strip().split('\n') or [''])[0]
    if not gm:   # 手动起局拉回的目录（贡献者文档 3.3）没有执行器写的 gfx_mode.txt：同执行器 tools/dev_run.sh，取 host.log「图形模式：」行
        m = re.search(r'图形模式：((?:直连|通用)[^\n]*)', hl); gm = m.group(1) if m else ''
    frames = [l for l in rd(o, 'frames.tsv').split('\n')[1:] if l.strip()]
    out = {'款': name, '局目录': o, '截图': ' '.join(os.path.relpath(p, o) for p in ps)}
    read = []
    # AI 看图（画面 / 游戏画面）
    ans = None
    if ps and ai:
        try: ans = ai_judge(ps)
        except Exception as e:   # 判官失败要写明原因（10-29 冷启动演练卡点：只记「没数据」看不出是钥匙、网络还是模型的事）
            out['判官问题'] = f'{type(e).__name__}：{e}' if not isinstance(e, RuntimeError) else str(e)
            read.append(f'AI 判官失败：{out["判官问题"]}'); print(f'【黄】看图判官失败（画面 / 直连默认启动记没数据）：{out["判官问题"]}', file=sys.stderr)
    if ans:
        sus = [i for i, a in enumerate(ans) if a.get('画质') in ('异常', '拿不准')]; rf = refs(name)
        if sus and rf:   # 复核：可疑的几张 + 同款 Ryujinx 参考图
            why = '；'.join(f'{j + 1} 号：{ans[i].get("问题", "")}' for j, i in enumerate(sus))
            try:
                a2 = ai_judge([ps[i] for i in sus], PROMPT2.format(m=len(rf), n=len(sus), why=why), rf)
                for j, i in enumerate(sus): ans[i] = dict(ans[i], 画质=a2[j].get('画质'), 把握=a2[j].get('把握'), 问题=a2[j].get('问题', ''), 复核='对照 Ryujinx ' + ' '.join(os.path.relpath(r, TRUTH) for r in rf))
                read.append(f'复核 {len(sus)} 张（对照 Ryujinx 参考图 {len(rf)} 张）')
            except Exception as e: read.append(f'复核失败：{e}')
        try:
            k = settle(ps, ans)
            if k: read.append(f'定论 {k} 张（同局 {len(ps)} 张一起看，只许正常 / 异常）')
        except Exception as e: read.append(f'定论失败：{e}')
        sure = lambda a: a.get('画质') == '异常' and float(a.get('把握') or 0) >= 0.8   # 把握不足的异常算拿不准（10-29 丝之歌剪影 / 八方2 泛光误判）
        bad = [f'{os.path.basename(p)}：{a.get("问题", "")}（把握 {a.get("把握")}）' for p, a in zip(ps, ans) if sure(a)]
        unsure = [f'{os.path.basename(p)}：{a.get("问题", "")}（把握 {a.get("把握")}）' for p, a in zip(ps, ans) if a.get('画质') in ('异常', '拿不准') and not sure(a)]
        out['画面'] = '不过' if bad else '没数据' if unsure else '过'
        read.append('画面 ' + ('异常 ' + '；'.join(bad) if bad else '拿不准 ' + '；'.join(unsure) if unsure else f'{len(ps)} 张都正常') + f'（{ans[0].get("模型", "")}）')
        game = [p for p, a in zip(ps, ans) if a.get('游戏画面') == '是']
    else:
        out['画面'] = '没数据'; game = []
        if not ps: read.append('没有截图')
    sb, su = solid_judge(o)
    if sb: out['画面'] = '不过'; read.append('画面（像素 + 读数）' + '；'.join(sb)); game = [p for p in game if not any(x.startswith(os.path.basename(p) + '：') for x in sb)]
    elif su:
        if out['画面'] == '过': out['画面'] = '没数据'
        read.append('画面（像素，拿不准）' + '；'.join(su))
    # 满屏：host.log 出图层比 vs 屏比；判不了才退回屏幕比例的游戏画面截图
    ft, fw = score_cells.fs_tier(hl, fs_layer(o, hl) if ai else None)   # 三档 + 自动分层（留黑边段的截图交看图判官判局内 / 界面页）
    fsok, fswhy = (None if ft is None else ft == '过'), (fw if ft in (None, '过') else f'{ft}：{fw}')
    wide = [p for p in game if (lambda s: s[0] / s[1] >= 2.0)(Image.open(p).size)]
    if fsok is not None:
        out['满屏'] = ft; read.append('满屏 ' + fswhy)   # 过 / 局内红 / 待分层（11-02 维护者三档）
        if fsok and wide:   # 宽高比过还要画面过（10-30 维护者：铺满不等于没缺东西）：AI 判为游戏画面的加宽截图逐张过两侧空洞 / 接缝
            bad, rr = band_judge(wide)
            if bad and game_drew_sides(hl): read.append('满屏画面 两侧纯色块 = 游戏自画（宽屏包装层说明行 + 无【红】出图未满屏），空洞判据不采信'); bad = []
            if bad: out['满屏'] = '局内红'   # AI 判为游戏画面的截图两侧空洞 / 接缝
            read.append('满屏画面 ' + ('；'.join(bad) if bad else f'{len(wide)} 张两侧无空洞、无接缝（{rr}）'))
    elif wide:
        bb = [(p,) + bars(p) for p in wide]; hit = [x for x in bb if x[1]]
        out['满屏'] = '局内红' if hit else '过'   # wide 只取 AI 判为游戏画面的截图
        read.append('满屏（截图）' + ('黑边 ' + '；'.join(f'{os.path.basename(p)} 左 {l:.1%} 右 {r:.1%}' for p, _, l, r in hit) if hit else f'{len(wide)} 张游戏画面左右无黑边'))
    else: out['满屏'] = '没数据'; read.append('满屏 ' + fswhy)
    # 截图全是整幅单色：铺没铺满看不出来，出图层比说「过」也不算（11-01 究极4 / 异度3 全黑截图记了满屏过）
    if sb and len(sb) == len(solid_shots(o)) and out['满屏'].startswith('过'):   # 四张都「画了没出来」
        out['满屏'] = '没数据'; read.append('满屏改没数据：本该出游戏画面的截图都是整幅单色，判不了铺满')
    # 满屏格口径（11-01 维护者）：Mac 局按 2346 宽 + 满屏能力开才作数；1920 宽（16:9 原比例）出图本来就满，「过」不说明任何事（第八批异度3 1920 宽、启用 0 点记了满屏过）。
    #   读数写实际屏宽与宽屏启用点数；Mac 局屏宽不是 2346 → 满屏记「不作数」。iPhone 局屏宽随机型（壳按横屏比例给），不走这条
    ws = [int(x) for x in re.findall(r'宽屏：\S+ \d+ 个点，启用 \d+ 个，屏幕宽 (\d+)', hl)]; wn = sum(int(x) for x in re.findall(r'宽屏：\S+ \d+ 个点，启用 (\d+) 个，屏幕宽', hl))
    if ws: read.append(f'屏宽 {ws[0]}、宽屏启用 {wn} 点')
    if 'iOS 壳' not in hl and (not ws or ws[0] != MAC_SCREEN_W) and out.get('满屏') in ('过', '不过', '局内红', '待分层'):
        out['满屏'] = '不作数'; read.append(f'满屏不作数：Mac 局屏宽 {ws[0] if ws else "未知（host.log 没有宽屏行）"}，不是全能力口径 {MAC_SCREEN_W}')
    # 直连默认启动：只问「包按缺省走了直连、局里直连后端在出图」，从 host.log 模式行 + frames.tsv 判；到没到游戏画面是进局那一问，不绑在这里
    #   （10-31 维护者：惊奇 1.0.0 无存档局直连在画 5405 帧，只因路线锚不中没进局，这项被记成没数据 / 不过）。直连运行中出错不切回通用（10-26），异常红行只记读数
    anom = len(re.findall(r'【红】直连异常', hl))
    if not gm: out['直连默认启动'] = '没数据'
    elif 'S2A_GFX' in gm: out['直连默认启动'] = '没数据'; read.append('直连默认启动 这局用环境变量指定了图形模式，不算默认启动')
    elif gm.startswith('通用'): out['直连默认启动'] = '不过'
    elif not frames: out['直连默认启动'] = '没数据' if not rd(o, 'frames.tsv') else '不过'
    else: out['直连默认启动'] = '过' if len(frames) >= DIRECT_MIN_FRAMES else '不过'
    read.append(f'模式 {gm[:2] or "—"}，{len(frames)} 帧（直连能跑要 ≥ {DIRECT_MIN_FRAMES}），直连异常红行 {anom} 种，游戏画面 {len(game)}/{len(ps)} 张')
    # 帧率 / 能耗（score_cells 同一口径）
    try:
        keep = score_cells.fullscreen; score_cells.fullscreen = lambda h: (None, '')   # 五问独立：帧率 / 能耗不因未满屏清空（满屏单列）
        try: cl, reading, notes = cells(o, name)
        finally: score_cells.fullscreen = keep
        fps = game_fps(hl)[0]
        if fsok is False and cl.get('帧率') in ('绿', '红'): notes['帧率'] = '未满屏局；' + notes.get('帧率', '')
    except Exception as e:
        cl, notes, fps = {}, {}, None; read.append(f'帧率能耗读不出：{e}')
    m = {'绿': '过', '红': '不过'}
    out['帧率'] = m.get(cl.get('帧率'), '没数据')
    if cl: read.append('帧率 ' + notes.get('帧率', '') + first_encounter(hl))
    pw = window_power(o); mt = metrics(o, hl, pw); h60 = mt['hold60'] if mt.get('adaptive') else None
    bnote, bbad = brightness_hold(hl)   # 亮度保持：整局实际亮度偏离设定 > 0.05 = 整机瓦数不可比，能耗判没数据
    if bnote: read.append(bnote)
    if h60 is not None:   # 自适应款：帧率项照判（不写成 60 帧过），读数如实写 60 档占比；能耗按主档位的线判（60 档过半 4.8 W，否则 30 帧线 3.7 W），不因帧率不过连带判红
        read.append(f'帧率 自适应，60 档占比 {h60 * 100:.0f}%')
        if pw is None or bbad: out['能耗'] = '没数据'
        else:
            lim = score_cells.W_MAX60 if h60 >= 0.5 else score_cells.W_MAX
            out['能耗'] = '过' if pw <= lim else '不过'; read.append(f'能耗 {pw:.2f} W（自适应：按 {"60" if h60 >= 0.5 else "30"} 档线 ≤ {lim}；{window_power_note(o)}）')
        if pw is None and window_power_note(o): read.append(f'能耗 {window_power_note(o)}')
    else:
        if pw is None or bbad or cl.get('帧率') not in ('绿', '红'): out['能耗'] = '没数据'
        else: out['能耗'] = '过' if pw <= w_max(fps) and cl.get('帧率') == '绿' else '不过'
        if pw is not None: read.append(f'能耗 {pw:.2f} W（要 ≤ {w_max(fps)}；{window_power_note(o)}）' + ('，帧率未保持' if pw <= w_max(fps) and cl.get('帧率') == '红' else ''))
        elif window_power_note(o): read.append(f'能耗 {window_power_note(o)}')   # 漏样率超限：局无效，读数写原因
    bl = re.findall(r'\bbrightness ([0-9.]+)', hl)   # 实际屏幕亮度（直连出图统计行「brightness 0.21」，取最后一次；10-30 测量口径修法 3：整机瓦数含屏幕）
    if pw is not None: read.append(f'亮度 {bl[-1]}' if bl else '亮度 读不到')
    out['metrics'] = mt
    out['读数'] = '｜'.join(read)
    return out


DIRECT_MIN_FRAMES = 1000   # 直连默认启动「直连能跑」的出图帧数线（同 tools/census/census.py 直连局口径）
COLS = ['款', '画面', '帧率', '满屏', '直连默认启动', '能耗', '读数', '截图', '局目录']
ITEMS = ['画面', '帧率', '满屏', '直连默认启动', '能耗']
EVIDENCE = ('host.log', 'frames.tsv', 'gfx_mode.txt', 'pw0.plist', 'pwh.plist', 'pw1.plist', 'android.json', 'power.tsv', 'logcat.txt')   # 后三个：安卓局（K3）   # 判五项要读的文本 / 功耗原件（截图另取 shots()）
SCRUB = [(re.compile(r'/(?:private/)?var/(?:mobile/)?(?:containers|Containers)/[^\s"\']*'), '<app容器>'),
         (re.compile(r'/(?:private/)?var/folders/[^/\s"\']+/[^/\s"\']+'), '<临时目录>'), (re.compile(r'/Volumes/[^/\s"\']+'), '<外接盘>'),
         (re.compile(r'/(?:private/)?tmp(?:/[^/\s"\'（）：，；、]+)*/([^/\s"\'（）：，；、]+)'), r'<临时目录>/\1'),   # 10-29 演练：/private/tmp/claude-501/-Users-<名>-.s2arm/<UUID>/…（只留文件名）
         (re.compile(r'-Users-[^-/\s"\']+'), '-Users-<用户>'),   # 用户名被短横线拼进目录名（Claude Code 会话目录等）
         (re.compile(r'/Users/[^/\s"\']+'), '~'), (re.compile(r'/home/[^/\s"\']+'), '~'),
         (re.compile(r'~(?:/[^/\s"\'（）：，；、]+)*/([^/\s"\'（）：，；、]+)'), r'<本机路径>/\1'),   # 家目录下的路径只留文件名（10-29 冷启动演练：Mac 局 host.log 整段 ~/.s2arm/... 暴露本机目录结构）
         (re.compile(r'\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}\b'), '<uuid>'),
         (re.compile(r'\b[0-9A-Fa-f]{8}-[0-9A-Fa-f]{16}\b'), '<设备号>'),
         (re.compile(r'[\w.+-]+@[\w-]+\.[\w.-]+'), '<邮箱>'), (re.compile(r'\b\d{1,3}(?:\.\d{1,3}){3}\b'), '<ip>'),
         (re.compile(r'\b(?:sk|pk|rk)-[A-Za-z0-9_-]{16,}'), '<钥匙>'), (re.compile(r'(?i)(api[_-]?key|token|secret|password)\s*[=:]\s*\S+'), r'\1=<略>')]


def scrub(text):   # 报告与证据文本脱敏：家目录路径（只留文件名）/ 临时目录 / 外接盘 / App 容器 / UUID / 设备号 / 邮箱 / IP / 钥匙样字符串
    for rx, sub in SCRUB: text = rx.sub(sub, text)
    return text


def sha(path):
    return hashlib.sha256(open(path, 'rb').read()).hexdigest()


def report(o, out, name=None, root=None, device='', contributor=''):
    """一场局 → <out>/evidence/（脱敏副本）+ report.json + report.md；返回报告 dict。判定在证据副本上做，维护方重跑得同一份结论与指纹"""
    ev = os.path.join(out, 'evidence'); os.makedirs(os.path.join(ev, 'shots'), exist_ok=True)
    for f in EVIDENCE:
        src = os.path.join(o, f)
        if not os.path.exists(src): continue
        if f.endswith('.plist'):   # 功耗 plist 只留判定用的四个字段（原件带电池序列号等设备身份，10-29 冒充贡献者验收查出）
            try:
                io = plistlib.load(open(src, 'rb')).get('IORegistry', {}); pt = io.get('PowerTelemetryData', {})
                plistlib.dump({'IORegistry': {'IsCharging': io.get('IsCharging', False), 'FullyCharged': io.get('FullyCharged', False),
                               'PowerTelemetryData': {k: pt[k] for k in ('AccumulatedSystemLoad', 'SystemLoadAccumulatorCount') if k in pt}}},
                              open(os.path.join(ev, f), 'wb'))
            except Exception: pass   # 读不出的功耗原件不带（能耗记没数据）
        else: open(os.path.join(ev, f), 'w', encoding='utf-8').write(scrub(open(src, encoding='utf-8', errors='replace').read()))
    os.makedirs(os.path.join(ev, 'shots_ap'), exist_ok=True)
    for i, q in enumerate(shots(o)):   # 截图统一放 shots_ap/，按选中次序编号（到 MARK 那张保留名字，复判时同样排在前面）
        nm = os.path.basename(q) if re.match(r'(ap_mark|ap_mid|z\d+_)', os.path.basename(q)) else f'z{i}_{os.path.basename(q)}'   # 复跑证据目录时名字不再叠前缀
        shutil.copyfile(q, os.path.join(ev, 'shots_ap', nm))
    os.rmdir(os.path.join(ev, 'shots')) if not os.listdir(os.path.join(ev, 'shots')) else None
    pid, bid = ident(ev)
    if not pid and os.path.exists(os.path.join(ev, 'android.json')):   # 安卓局：程序号从包名 org.s2arm.game.t<程序号> 取，没有构建号
        mm = re.search(r't([0-9a-fA-F]{16})', open(os.path.join(ev, 'android.json'), encoding='utf-8').read()); pid = mm.group(1).upper() if mm else ''
    name = name or pid or os.path.basename(os.path.abspath(o))
    r = check(ev, name)
    files = sorted(os.path.relpath(os.path.join(d, f), out) for d, _, fs in os.walk(ev) for f in fs)
    caps = {}
    if root:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import caps_policy
        caps = caps_policy.cap_hashes(root)
    tool = os.path.abspath(__file__)
    j = {'schema': 's2a.quality-report.v1', 'created': datetime.date.today().isoformat(),
         'tool': {'name': 'tools/quality_check.py', 'sha256': sha(tool), 'judge': provider() or '无'},
         'game': {'name': name, 'programId': pid, 'buildId': bid}, 'run': {'kind': 'android' if os.path.exists(os.path.join(ev, 'android.json')) else 'iphone' if os.path.exists(os.path.join(ev, 'pw0.plist')) or 'iOS 壳' in rd(ev, 'host.log') else 'mac',
         'device': device}, 'contributor': contributor, 'capHashes': caps,
         'checks': {k: r.get(k, '没数据') for k in ITEMS}, 'reading': scrub(r.get('读数', '')), 'metrics': r.get('metrics') or {},
         'judgeError': r.get('判官问题', ''),
         'evidence': [dict({'path': f, 'sha256': sha(os.path.join(out, f)), 'bytes': os.path.getsize(os.path.join(out, f))}, **({'private': True} if f.startswith('evidence/shots_ap/') else {})) for f in files]}
    j['fingerprint'] = hashlib.sha256(json.dumps({k: j[k] for k in ('tool', 'game', 'checks', 'evidence', 'capHashes', 'metrics')}, sort_keys=True, ensure_ascii=False).encode()).hexdigest()[:16]   # metrics 进指纹：退回规则读它，改了要复算不过
    json.dump(j, open(os.path.join(out, 'report.json'), 'w', encoding='utf-8'), ensure_ascii=False, indent=1)
    md = [f'## S2Arm 移植质量快检：{name}', '',
          f'程序号 `{pid or "—"}`　构建号 `{bid or "—"}`　局：{j["run"]["kind"]}{("（" + device + "）") if device else ""}　判官：{j["tool"]["judge"]}　报告指纹 `{j["fingerprint"]}`', '',
          '| 项 | 结论 |', '|---|---|'] + [f'| {k} | {j["checks"][k]} |' for k in ITEMS] + ['', '**读数**：', '']
    if j['judgeError']: md += [f'**看图判官失败**（画面 / 直连默认启动记没数据）：{j["judgeError"]}', '']
    md += [f'- {x}' for x in j['reading'].split('｜') if x]
    if caps: md += ['', '能力内容哈希：' + '　'.join(f'{k} `{v}`' for k, v in caps.items())]
    md += ['', '<details><summary>证据（{} 份，sha256 前 12 位）</summary>'.format(len(files)), ''] + [f'- `{e["path"]}` `{e["sha256"][:12]}` {e["bytes"]} 字节' for e in j['evidence']] + ['', '</details>', '',
           f'工具 `tools/quality_check.py`（sha256 `{j["tool"]["sha256"][:12]}`）生成；复算：`python3 tools/quality_check.py <报告目录>/evidence --report <新目录>`；'
           '入账：`python3 tools/caps_ledger.py ingest <报告目录>`。证据已脱敏（路径 / 用户名 / 设备号 / 邮箱 / IP / 钥匙）。']
    md += ['', '贴 issue：正文贴本文件，附件用 `attach.zip`（report.json + 证据，**不含游戏截图**——游戏截图不外发，`evidence/shots_ap/` 只留在本机，'
           '指纹已记在 report.json，维护方抽查时再私下要）。']
    open(os.path.join(out, 'report.md'), 'w', encoding='utf-8').write('\n'.join(md) + '\n')
    import zipfile
    with zipfile.ZipFile(os.path.join(out, 'attach.zip'), 'w', zipfile.ZIP_DEFLATED) as z:   # 公开附件：不含 private（截图）条目
        for f in ['report.md', 'report.json'] + [e['path'] for e in j['evidence'] if not e.get('private')]: z.write(os.path.join(out, f), f)
    return j


def jobs(args):   # 供数表 / 局目录 → [(款, 局目录)]
    out = []
    for a in args:
        if os.path.isdir(a): out.append((os.path.basename(os.path.dirname(os.path.abspath(a))).split('_')[0], a)); continue
        for line in open(a, encoding='utf-8', errors='replace'):   # 供数表 gfx_mode 列按字节截断过，只用前两列
            if line.startswith('#') or not line.strip(): continue
            f = line.rstrip('\n').split('\t')
            if f[0] == '款' or len(f) < 2: continue
            out.append((f[0], f[1]))
    return out


def main(a):
    if not a or a[0] in ('-h', '--help'):   # 10-29 演练：--help 原来当局目录读、抛 Traceback
        print(__doc__.strip()); return 0
    if a[0] != '--selftest-ai' and not os.path.exists(a[0]):
        print(f'🔴 局目录 / 供数表不存在：{a[0]}（用法：python3 tools/quality_check.py --help）', file=sys.stderr); return 2
    if a and a[0] == '--selftest-ai':
        bad, good = ai_judge([a[1]])[0], ai_judge([a[2]])[0]; art = [(x, ai_judge([x])[0]) for x in a[3:]]
        sure = lambda r: r.get('画质') == '异常' and float(r.get('把握') or 0) >= 0.8
        ok = sure(bad) and good.get('画质') == '正常' and not any(sure(r) for _, r in art)
        print(f'AI 判官正反对照：花屏 → {bad.get("画质")} 把握 {bad.get("把握")}（{bad.get("问题", "")}）；正常 → {good.get("画质")}'
              + ''.join(f'；画风 {os.path.basename(x)} → {r.get("画质")} 把握 {r.get("把握")}' for x, r in art) + f' → {"过" if ok else "不过"}')
        return 0 if ok else 1
    if '--report' in a:   # 贡献者一条命令
        opt = lambda k: a[a.index(k) + 1] if k in a else None
        j = report(a[0], opt('--report'), opt('--game'), opt('--root'), opt('--device') or '', opt('--contributor') or '')
        print(f'快检报告：{os.path.join(opt("--report"), "report.md")}（指纹 {j["fingerprint"]}）｜' + '　'.join(f'{k} {j["checks"][k]}' for k in ITEMS))
        return 0
    ai = '--no-ai' not in a; a = [x for x in a if x != '--no-ai']
    out = None
    if '--out' in a: i = a.index('--out'); out = a[i + 1]; del a[i:i + 2]
    rows = [check(o, n, ai) for n, o in jobs(a)]
    for r in rows: print('\t'.join(r.get(c, '') for c in COLS))
    if out:
        with open(out, 'w', encoding='utf-8') as f:
            w = csv.writer(f, delimiter='\t', lineterminator='\n'); w.writerow(COLS); w.writerows([[r.get(c, '') for c in COLS] for r in rows])
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

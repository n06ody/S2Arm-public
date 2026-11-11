#!/usr/bin/env python3
"""tools/quality_check.py 机械部分单测（不调 AI）：
  ① 截图左右各 10% 纯黑 → 黑边；满幅 → 无黑边
  ② 满屏按 host.log 出图层比：1.777 vs 屏 2.173 → 不过；2.172 vs 2.173 → 过（截图怎么样都不看）
  ②b Duo 内屏：屏比按「视图 宽x高 点」取（nativeBounds 报外屏）；没有视图段的旧日志退回屏幕像素
  ③ gfx_mode 首行「通用」→ 直连默认启动 不过；没截图 → 画面 没数据
  ④ 供数表：# 注释、表头跳过，字节截断的中文列不报错
  ⑤ 报告模式：证据脱敏、功耗 plist 只留四字段、指纹可复算、复跑证据目录结论不变（不调看图判官）
  ⑧ 演练补修：会话临时目录 / -Users-<名>- / var/folders 哈希脱敏；claude-cli 只报末行并脱敏；--help 退 0、局目录不存在退 2
  ⑨ 安卓局目录：帧率按 disp_us、整屏截图数黑列判满屏、能耗只在整窗证了放电才电池侧积分记「未证」（接过电 / 未知态记「没数据」）、显式指定图形模式不算默认启动、logcat 认直连 / 通用
  ⑩ 首遇现建行：最长 > 16.7 ms 标「首遇卡」，≤ 16.7 不标，没有这行空串（取最后一行）
  ⑬ Mac 局目录有空的 shots/：截图仍取顶层 ap_*.png（判有没有图，不判有没有目录）
  ⑭ 直连默认启动只看模式行 + 出图帧数（不调看图判官、没截图也判）：直连 1000 帧过、999 帧不过、S2A_GFX 指定没数据、没有 frames.tsv 没数据
  ⑮ 整幅单色 + 读数门（不交 AI）：收局全黑且同刻绘制 ≥ 门 → 画面不过；绘制 < 5（加载黑场）→ 不判；其间 / 没读数 → 拿不准；导航 vision/、路线 shots/、纹理 → 不判
  ⑰ 满屏格口径：Mac 局屏宽不是 2346 → 不作数（读数写屏宽与宽屏启用点数）；iPhone 局（有 iOS 壳行）不走这条
  ⑳ 定论轮：首轮 / 复核后仍拿不准或把握不足的异常，连同局内其余截图再问一次，只收正常 / 异常（假判官，不调 AI）
  ㉑ 判官逐档备用（11-02 维护者：中转站主力）：中转站丢图 1 次后带图 → 照出结论、不碰 OpenRouter；一直丢 → OpenRouter 402 一次写标记 → 判官失败写原因；
     标记新 → 档序只剩 relay；没中转站钥匙才排 claude-cli；请求体都带图；S2A_QC_PROVIDER 指定只问那一档
  ⑦ 判官失败写明原因（没判官 / claude-cli 退出码 + 报错）；截图标 private、attach.zip 不含截图、截图不在照样可入账"""
import os, sys, tempfile
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
from PIL import Image
import quality_check as q

bad = 0
def chk(name, want, got):
    global bad
    ok = want == got; bad += not ok
    print(f'{"🟢" if ok else "🔴"} {name}' + ('' if ok else f'：期望「{want}」→ 真结果「{got}」'))

T = tempfile.mkdtemp()
im = Image.new('RGB', (1000, 460), (90, 120, 60))
for x in list(range(100)) + list(range(900, 1000)):
    for y in range(460): im.putpixel((x, y), (0, 0, 0))
im.save(f'{T}/bar.png'); Image.new('RGB', (1000, 460), (90, 120, 60)).save(f'{T}/full.png')
chk('① 两侧纯黑 → 黑边', True, q.bars(f'{T}/bar.png')[0]); chk('① 满幅 → 无黑边', False, q.bars(f'{T}/full.png')[0])

fe = '[s2a] 首遇现建：MARK 后提交线程现建管线 3 条，最长 2.0 ms，合计 4.0 ms\n[s2a] 首遇现建：MARK 后提交线程现建管线 5 条，最长 21.4 ms，合计 40.2 ms\n'
chk('⑩ 最长 21.4 > 16.7 → 首遇卡（取最后一行）', '；首遇现建 5 条，最长 21.4 ms，合计 40.2 ms（首遇卡）', q.first_encounter(fe))
chk('⑩ 最长 16.7 不标', '；首遇现建 2 条，最长 16.7 ms，合计 20.0 ms', q.first_encounter('[s2a] 首遇现建：MARK 后提交线程现建管线 2 条，最长 16.7 ms，合计 20.0 ms'))
chk('⑩ 没有这行 → 空串', '', q.first_encounter('[s2a] 别的行'))

chk('⑪ 亮度保持 0.50 全程 0.49–0.51 → 有效', ('亮度 设定 0.50、实际 0.49–0.51', False), q.brightness_hold('[s2a] 亮度保持：设定 0.50，实际最小 0.49 最大 0.51，重设 3 次'))
chk('⑪ 实际最小 0.20（被自动亮度拉回）→ 局无效', ('亮度 设定 0.50、实际 0.20–0.50（偏离 > 0.05，局无效）', True), q.brightness_hold('[s2a] 亮度保持：设定 0.50，实际最小 0.20 最大 0.50，重设 9 次'))
chk('⑪ 没设亮度（无此行）→ 空、有效', ('', False), q.brightness_hold('[s2a] 别的行'))

def job(name, aspect, mode):
    o = os.path.join(T, name, 'out_1'); os.makedirs(o)
    open(f'{o}/host.log', 'w').write(f'直连：出图层已挂到根视图 2346x1320 像素（宽高比 {aspect}）\niOS 壳：x 屏幕 2867x1320 像素\n')
    open(f'{o}/gfx_mode.txt', 'w').write(mode + '\n'); open(f'{o}/frames.tsv', 'w').write('frame\twall_us\n1\t16000\n')
    return o
r = q.check(job('a', '1.777', '直连（依据 x）'), 'a', ai=False); chk('② 出图层 16:9 → 满屏局内红（11-02 三档）', '局内红', r['满屏'])
r = q.check(job('b', '2.172', '直连（依据 x）'), 'b', ai=False); chk('② 出图层同屏比 → 满屏过', '过', r['满屏'])
r = q.check(job('c', '2.172', '通用（依据 x）'), 'c', ai=False); chk('③ 通用 → 直连默认启动不过', '不过', r['直连默认启动'])
# ②b Duo 内屏（11-04 满屏补丁线）：nativeBounds 报外屏 2034x1398（1.455），视图 951x669 点（1.422）才是实际那块屏 → 屏比按视图取
import score_cells
DUO = '[s2a] iOS 壳：朝向 横屏（4），视图 951x669 点，屏幕 2034x1398 像素，屏幕宽（1080 高对应）1534\n'
chk('②b Duo 内屏 出图层 1.422 = 视图比 → 过', '过', score_cells.fs_tier(DUO + '[s2a] 直连：出图层已挂到根视图 2853x2007 像素（宽高比 1.422，居中）\n')[0])
chk('②b Duo 内屏 出图层 1.455（按外屏出）→ 局内红', '局内红', score_cells.fs_tier(DUO + '[s2a] 直连：出图层已挂到根视图 2853x1961 像素（宽高比 1.455，居中）\n')[0])
chk('②b 没有视图段的旧日志 → 仍按屏幕像素', '局内红', score_cells.fs_tier('[s2a] iOS 壳：x 屏幕 2034x1398 像素\n[s2a] 直连：出图层已挂到根视图 2853x2007 像素（宽高比 1.422）\n')[0])
chk('③ 没截图 → 画面没数据', '没数据', r['画面'])
open(f'{T}/t.tsv', 'wb').write('# 注释\n款\t作业目录\tgfx\nx\t/a/out_1\t直连（依据 包\xe9\n'.encode('utf-8') + b'\xe9\x80\n')
chk('④ 供数表解析', [('x', '/a/out_1')], q.jobs([f'{T}/t.tsv'])[:1])
# ⑤ 报告模式（贡献者一条命令）：证据脱敏、功耗 plist 只留四字段、指纹可复算（caps_ledger verify）、复跑证据目录得同样的证据路径与结论
import plistlib, json
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools')); import caps_ledger
os.environ['S2A_QC_PROVIDER'] = 'none'   # 不调看图判官：画面 / 直连默认启动记没数据，其余照判
R = os.path.join(T, 'run'); os.makedirs(os.path.join(R, 'shots_ap'))
HOMEP, DEV, MAIL = '/' + 'Users' + '/alice', '00008130' + '-' + '001A2B3C4D5E6F70', 'a' + '@' + 'b.com'
TMPP, VOL, TILDE = '/var/' + 'folders/nw/1t1m5nc1/C/x.data', '/Vol' + 'umes/新加卷/游戏.xci', '~' + '/.s2arm/run/s2arm/census/w/game.tsv'   # 10-29 冷启动演练：Mac 局整段家目录路径 / 临时目录 / 外接盘   # 运行时拼：仓库文本里不放完整的路径 / 设备号 / 邮箱样（公开仓扫描）
open(f'{R}/host.log', 'w').write('程序号 0100152000022000\n构建号 158c0c7399aabd7bf83795c2ef3484c7\n路径 ' + HOMEP + '/S2Arm/x 设备 ' + DEV + ' '
                                 '容器 /private/var/containers/Bundle/Application/DB757E4D-2CD9-4C1F-A263-C227D60EB38C/S2Arm.app 邮箱 ' + MAIL + ' ip 10.1.2.3 key sk-or-v1-abcdefabcdefabcdef1234\n'
                                 '直连：出图层已挂到根视图 2867x1320 像素（宽高比 2.172）\niOS 壳：x 屏幕 2867x1320 像素\n' + '缓存 ' + TMPP + ' 源 ' + VOL + ' 参数（' + TILDE + '）\n')
open(f'{R}/gfx_mode.txt', 'w').write('直连（依据 x）\n'); open(f'{R}/frames.tsv', 'w').write('frame\twall_us\n1\t16000\n')
plistlib.dump({'IORegistry': {'Serial': 'FQ1HN2007UL', 'IsCharging': True, 'PowerTelemetryData': {'AccumulatedSystemLoad': 10, 'SystemLoadAccumulatorCount': 2, 'Other': 1}}}, open(f'{R}/pw0.plist', 'wb'))
im = Image.new('RGB', (64, 30)); im.putdata([(x * 4, y * 8, 9) for y in range(30) for x in range(64)]); im.save(f'{R}/shots_ap/ap_mark.png')   # 渐变：纯色会被 ⑮ 整幅单色判画面不过
Image.new('RGB', (64, 30), (200, 9, 9)).save(f'{R}/shots_ap/end.png')
O = os.path.join(T, 'rep'); j = q.report(R, O, 'mk8d', device='iPhone', contributor='alice')
ev = open(f'{O}/evidence/host.log').read()
chk('⑤ host.log 脱敏（用户名 / 设备号 / 容器 / 邮箱 / IP / 钥匙 / 临时目录 / 外接盘 / 家目录路径都不剩）', [], [x for x in (HOMEP, DEV, 'DB757E4D', MAIL, '10.1.2.3', 'sk-or-v1', 'folders/nw', '新加卷', '.s2arm') if x in ev])
chk('⑤ 家目录路径只留文件名', True, '<本机路径>/game.tsv' in ev)
chk('⑤ 身份照读出', ('0100152000022000', '158c0c7399aabd7bf83795c2ef3484c7'), (j['game']['programId'], j['game']['buildId']))
io = plistlib.load(open(f'{O}/evidence/pw0.plist', 'rb'))['IORegistry']
chk('⑤ 功耗 plist 只留四字段（无 Serial）', (False, ['AccumulatedSystemLoad', 'SystemLoadAccumulatorCount']), ('Serial' in io, sorted(io['PowerTelemetryData'])))
chk('⑤ 指纹可复算（caps_ledger verify 无问题）', [], caps_ledger.verify(O)[1])
chk('⑤ 满屏按出图层比判过、画面没判官记没数据', ('过', '没数据'), (j['checks']['满屏'], j['checks']['画面']))
chk('⑤ report.md 带指纹、五项表', True, j['fingerprint'] in open(f'{O}/report.md').read() and '| 直连默认启动 |' in open(f'{O}/report.md').read())
j2 = q.report(os.path.join(O, 'evidence'), os.path.join(T, 'rep2'), 'mk8d')
chk('⑤ 复跑证据目录：证据路径与结论不变', ([e['path'] for e in j['evidence']], j['checks']), ([e['path'] for e in j2['evidence']], j2['checks']))
# ⑥ 手动局目录（没有 gfx_mode.txt）：图形模式从 host.log「图形模式：」行读
M = os.path.join(T, 'manual'); os.makedirs(M)
open(f'{M}/host.log', 'w').write('[s2a] 图形模式：通用（依据 x）\n'); open(f'{M}/frames.tsv', 'w').write('frame\twall_us\n1\t16000\n')
chk('⑥ 手动局：没有 gfx_mode.txt 也认出通用 → 直连默认启动不过', '不过', q.check(M, 'm', ai=False)['直连默认启动'])
# ⑦ 冷启动演练卡点（10-29）：判官失败写明原因；游戏截图不随公开附件外发
import zipfile, shutil
chk('⑦ 没判官：report.json judgeError 写明原因', True, '没有看图判官' in j['judgeError'])
chk('⑦ 没判官：report.md 写明原因', True, '看图判官失败' in open(f'{O}/report.md').read())
chk('⑦ 截图条目标 private', True, all(e.get('private') for e in j['evidence'] if e['path'].startswith('evidence/shots_ap/')) and any(e['path'].startswith('evidence/shots_ap/') for e in j['evidence']))
names = zipfile.ZipFile(f'{O}/attach.zip').namelist()
chk('⑦ attach.zip 有报告与证据、没有截图', (True, True, []), ('report.json' in names, 'evidence/host.log' in names, [n for n in names if 'shots_ap' in n]))
shutil.rmtree(f'{O}/evidence/shots_ap')
chk('⑦ 截图不在（只留本机）入账复核照过', [], caps_ledger.verify(O)[1])
fake = os.path.join(T, 'bin'); os.makedirs(fake); open(f'{fake}/claude', 'w').write('#!/bin/sh\necho "登录已过期，请重新登录" >&2\nexit 1\n'); os.chmod(f'{fake}/claude', 0o755)
os.environ['S2A_QC_PROVIDER'] = 'claude-cli'; os.environ['PATH'] = fake + os.pathsep + os.environ['PATH']; os.environ['S2A_QC_CACHE'] = os.path.join(T, 'qc_cache')
r7 = q.check(R, 'mk8d')
chk('⑦ claude-cli 失败：原因带退出码与它的报错', True, 'claude-cli 退出 1' in r7.get('判官问题', '') and '登录已过期' in r7.get('判官问题', ''))
# ⑧ 演练卡点补修（10-29 生态线补发细节）：/private/tmp 会话目录与 -Users-<名>- 拼名、/var/folders 本机哈希都要换掉；claude-cli 只报末行并脱敏；--help / 局目录不存在
import subprocess
NAME = 'ali' + 'ce'
drill = ('/private/' + 'tmp/claude-501/-Users-' + NAME + '-.s2arm/0d6f1c2a-1111-2222-3333-444455556666/scratchpad/drill_out/run1/host.log '
         '/private/var/' + 'folders/nw/1t1m5nc109z/T/x.data')
t8 = q.scrub(drill)
chk('⑧ 会话临时目录 + -Users-<名>- + var/folders 哈希都不剩', (False, False, False), (NAME in t8, '1t1m5nc' in t8, 'claude-501' in t8))
open(f'{fake}/claude', 'w').write('#!/bin/sh\necho "第一行无关" >&2\necho "Invalid API key · 路径 ' + '/' + 'Users/' + NAME + '/x" >&2\nexit 1\n')
r8 = q.check(R, 'mk8d')
chk('⑧ claude-cli 只报报错末行且脱敏', (True, False, False), ('Invalid API key' in r8.get('判官问题', ''), '第一行无关' in r8.get('判官问题', ''), NAME in r8.get('判官问题', '')))
qc = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'quality_check.py')
h = subprocess.run([sys.executable, qc, '--help'], capture_output=True, text=True)
chk('⑧ --help 打印用法、退出 0', (0, True), (h.returncode, '用法' in h.stdout or 'quality_check.py' in h.stdout))
m = subprocess.run([sys.executable, qc, os.path.join(T, '没有这个局')], capture_output=True, text=True)
chk('⑧ 局目录不存在：一行中文报错、退出 2、没有 Traceback', (2, True, False), (m.returncode, '不存在' in m.stderr, 'Traceback' in m.stderr))
# ⑨ 安卓局目录（安卓线 K3）：有 android.json 走安卓分支，不调 AI
import json as _json
A = os.path.join(T, 'android'); os.makedirs(os.path.join(A, 'shots_ap'))
PWR_F = ('boot_us', 'status', 'current_ua', 'voltage_uv', 'energy_uwh', 'charge_uah', 'temp_tenths_c',
         'source', 'ac_powered', 'usb_powered', 'wireless_powered', 'dock_powered',
         'plugged', 'interactive', 'power_save', 'thermal_status')
def adir(launch, logcat, plugged, frame_ms=16.6, secs=60):
    _json.dump({'package': 'org.s2arm.game.t01001b300b9be000', 'serial_model': 'X', 'soc': 'S', 'android': '16', 'launch': launch}, open(f'{A}/android.json', 'w'))
    n = int(secs * 1000 / frame_ms); open(f'{A}/frames.tsv', 'w').write('frame\tpresent_ns\tpresent_us\twall_us\tdisp_us\tdisp_seq\n' + ''.join(f'{i}\t0\t{int(frame_ms * 1000)}\t{int(frame_ms * 1000)}\t{int(frame_ms * 1000)}\t{i}\n' for i in range(1, n + 1)))
    # power.tsv = t + run_game.POWER_FIELDS + level：plugged=1 → Charging + 供电旗标 true（接过电）；
    #   放电行四路旗标 false→plugged=0、交互屏、非省电、热档 0、电量计每拍 -222µAh（800mA 口径）
    st, pw, pl = ('Charging', 'true', '1') if plugged else ('Discharging', 'false', '0')
    rows = ''.join(f'{i}\t{10_000_000 + i * 1_000_000}\t{st}\t-800000\t4000000\t\t{5_000_000 - i * 222}\t350\tsysfs\t{pw}\t{pw}\t{pw}\t{pw}\t{pl}\t1\t0\t0\t80\n' for i in range(60))
    open(f'{A}/power.tsv', 'w').write('t\t' + '\t'.join(PWR_F) + '\tlevel\n' + rows)
    open(f'{A}/logcat.txt', 'w').write(logcat)
    Image.new('RGB', (1000, 460), (90, 120, 60)).save(f'{A}/shots_ap/00_000s.png')
adir('am start -n x/y', 'direct provider prepared: driver=x\n', 0)
r9 = q.check(A, 'android', ai=False)
chk('⑨ 安卓：帧率按 disp_us 判过、满屏整屏截图无黑边判过', ('过', '过'), (r9['帧率'], r9['满屏']))
chk('⑨ 安卓：整窗放电 → 4000 mV × 0.8 A = 3.20 W 电池侧积分记「未证」（未经设备校准，不套 iPhone 门槛）',
    (True, '未证'), ('3.20 W' in r9['读数'], r9['能耗']))
adir('am start -n x/y --es s2arm.graphics_mode direct', '', 1, frame_ms=20)
r9 = q.check(A, 'android', ai=False)
chk('⑨ 安卓：显式指定图形模式 → 直连默认启动没数据；接过电 → 能耗没数据；50 帧 → 帧率不过', ('没数据', '没数据', '不过'), (r9['直连默认启动'], r9['能耗'], r9['帧率']))
adir('am start -n x/y', 'Gpu LoadShaders\n', 0)
chk('⑨ 安卓：logcat 认出通用 → 直连默认启动不过', '不过', q.check(A, 'android', ai=False)['直连默认启动'])
# ⑩ 自适应款（10-29 维护者：按实际稳住的档用对应能耗线；读数如实写 60 档占比，不写成 60 帧过）。期望手算：MARK 帧 1 之后 60 档帧 = n60 + 4（前置 5 帧里 MARK 之后的 4 帧），
#   占比 = (n60 + 4) / (n60 + n30 + 4)：1440 / 2160 → 40%（主档 30 → 线 3.7，4.2 W 不过）；2880 / 720 → 80%（主档 60 → 线 4.8，4.2 W 过）；不接帧步的同一份 → 帧率不过连带能耗不过、读数无「自适应」
import plistlib
def fsrun(adaptive, n60, n30, watts):
    o = tempfile.mkdtemp()
    open(f'{o}/host.log', 'w').write('[s2a] S2A 标记 MARK：帧 1\n' + ('[s2a] 帧率表：接宿主帧步（直连）\n' if adaptive else '') + '[s2a] 图形模式：直连（依据 x）\n')
    rows, f = ['frame\twall_us\tdisp_us'], 1
    for us, c in ((16667, 5), (16667, n60), (33333, n30)):
        for _ in range(c): rows.append(f'{f}\t{us}\t{us}'); f += 1
    open(f'{o}/frames.tsv', 'w').write('\n'.join(rows) + '\n')
    for nm, acc, cnt in (('pwh.plist', 0, 0), ('pw1.plist', int(watts * 1000 * 100), 100)):
        plistlib.dump({'IORegistry': {'PowerTelemetryData': {'AccumulatedSystemLoad': acc, 'SystemLoadAccumulatorCount': cnt}}}, open(f'{o}/{nm}', 'wb'))
    return q.check(o, 'fs', ai=False)
r = fsrun(True, 1440, 2160, 4.2)
chk('⑩ 自适应 60 档 40%、4.2 W：读数写占比、按 30 档线 3.7 → 能耗不过；帧率不写成过', ('不过', True, True, '不过'),
    (r['能耗'], '自适应，60 档占比 40%' in r['读数'], '按 30 档线 ≤ 3.7' in r['读数'], r['帧率']))
r = fsrun(True, 2880, 720, 4.2)
chk('⑩ 自适应 60 档 80%、4.2 W：按 60 档线 4.8 → 能耗过', ('过', True), (r['能耗'], '60 档占比 80%' in r['读数']))
r = fsrun(False, 1440, 2160, 4.2)
chk('⑩ 不接帧步的同一份：帧率不过连带能耗不过、读数无自适应', ('不过', False), (r['能耗'], '自适应' in r['读数']))
# ⑫ 本局满屏（10-30 #996）：新写法「宽屏：放开 / 不放（来源：…）」与旧写法都认；都没有 = -
mw = lambda t: q.metrics(tempfile.mkdtemp(), t, None)['widen']
chk('⑫ metrics.widen：新放开 / 新不放 / 旧在名单 / 旧不在名单 / 没有', ['开', '关', '开', '关', '-'],
    [mw('宽屏：放开（来源：包内能力清单开），屏幕宽 2346'), mw('宽屏：不放（来源：包内能力清单关），整局按 16:9'), mw('宽屏：本款在宽屏已验证名单，屏幕宽 2346'),
     mw('宽屏：本款不在宽屏已验证名单（…），整局按 16:9'), mw('无')])
M = tempfile.mkdtemp(); os.makedirs(f'{M}/shots')   # Mac 局（mac_health）：shots/ 空目录 + 顶层 ap_*.png
for n, c in (('ap_mark.png', 9), ('ap_end.png', 200)): Image.new('RGB', (8, 4), (c, c, c)).save(f'{M}/{n}')   # 颜色不同：同内容的图会去重
chk('⑬ Mac 局 shots/ 空、截图在顶层 → 取顶层 ap_mark / ap_end', ['ap_mark.png', 'ap_end.png'], [os.path.basename(x) for x in q.shots(M)])
def dj(n, mode, nf):   # Mac 局：只有 host.log 模式行（没有 gfx_mode.txt）+ nf 帧 frames.tsv（nf=None 不写）
    o = tempfile.mkdtemp(); open(f'{o}/host.log', 'w').write(f'[s2a] 图形模式：{mode}；程序号 0100000000000000）\n')
    if nf is not None: open(f'{o}/frames.tsv', 'w').write('frame\twall_us\n' + ''.join(f'{i}\t16667\n' for i in range(1, nf + 1)))
    return q.check(o, n, ai=False)['直连默认启动']
chk('⑭ 直连默认启动：缺省直连 1000 帧 / 999 帧 / S2A_GFX 指定 / 没有 frames.tsv', ['过', '不过', '没数据', '没数据'],
    [dj('a', '直连（依据 包里带本宿主口径的直连库', 1000), dj('b', '直连（依据 包里带本宿主口径的直连库', 999), dj('c', '直连（依据 环境变量 S2A_GFX=direct', 1500), dj('d', '直连（依据 包里带本宿主口径的直连库', None)])
import random
def sj(n, files, draws=None):   # 局目录：host.log（MARK 在帧 10）+ 指定截图 + 可选 frames.tsv（40 帧、每帧绘制 draws）；files = {相对路径: 'black' | 'tex'}
    o = tempfile.mkdtemp(); open(f'{o}/host.log', 'w').write('[s2a] 图形模式：直连（依据 x）\nS2A 标记 MARK：帧 10\n'); open(f'{o}/gfx_mode.txt', 'w').write('直连\n')
    if draws is not None: open(f'{o}/frames.tsv', 'w').write('frame\twall_us\tdraw_n\n' + ''.join(f'{i}\t16667\t{draws}\n' for i in range(1, 41)))
    rnd = random.Random(1)
    for rel, kind in files.items():
        os.makedirs(os.path.dirname(f'{o}/{rel}'), exist_ok=True); im = Image.new('RGB', (320, 180), (0, 0, 0))
        if kind == 'tex': im.putdata([(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(320 * 180)])
        im.save(f'{o}/{rel}')
    return o
o = sj('k', {'ap_stuck.png': 'black'}, draws=800); r = q.check(o, 'k', ai=False); b, u = q.solid_judge(o)
chk('⑮ 收局全黑 + 同刻每帧绘制 800（画了没出来，仿异度3 黑屏）→ 画面不过', ('不过', 1, 0), (r['画面'], len(b), len(u)))
chk('⑮ 不过读数写占比与绘制数', True, b[0].startswith('ap_stuck.png：整幅单色 RGB(4, 4, 4) 占 100.0%，同一时刻每帧绘制 800 ≥ 200'))
chk('⑮ 收局全黑 + 每帧绘制 2（加载黑场）→ 不判', ([], []), q.solid_judge(sj('l', {'ap_stuck.png': 'black'}, draws=2)))
chk('⑮ 收局全黑 + 每帧绘制 20（加载 5 与门 50 之间）→ 拿不准', (0, 1), tuple(len(x) for x in q.solid_judge(sj('m', {'ap_stuck.png': 'black'}, draws=20))))
o = sj('n', {'ap_stuck.png': 'black'}); r = q.check(o, 'n', ai=False)
chk('⑮ 收局全黑 + 没有 frames.tsv → 拿不准（画面没数据，读数写明）', ('没数据', True), (r['画面'], '没有绘制读数' in r.get('读数', '')))
chk('⑮ 只有导航 vision/v04.png 全黑、收局是纹理 → 不判', ([], []), q.solid_judge(sj('v', {'vision/v04.png': 'black', 'ap_end.png': 'tex'}, draws=800)))
chk('⑮ 路线 shots/ 截图全黑（跳过过场的黑场）→ 不判', ([], []), q.solid_judge(sj('s', {'shots/skip0.png': 'black', 'ap_mark.png': 'tex'}, draws=800)))
chk('⑮ 纹理画面 → 不判', ([], []), q.solid_judge(sj('t', {'ap_mark.png': 'tex', 'ap_end.png': 'tex'}, draws=800)))
V = tempfile.mkdtemp(); os.makedirs(f'{V}/vision')
for i, n in enumerate(('ap_mark.png', 'ap_mid.png', 'ap_end.png', 'vision/v01.png', 'vision/v02.png', 'vision/v03.png')): Image.new('RGB', (8, 4), (i * 30, i * 30, i * 30)).save(f'{V}/{n}')
chk('⑯ 到了 MARK（ap_mark / ap_mid / ap_end 齐）→ 只看这三张，不混导航 vision/', ['ap_mark.png', 'ap_mid.png', 'ap_end.png'], [os.path.basename(x) for x in q.shots(V)])
os.remove(f'{V}/ap_mid.png'); os.remove(f'{V}/ap_end.png')
chk('⑯ 只有 ap_mark 一张 → 照旧补最后两张导航图兜底', ['ap_mark.png', 'v02.png', 'v03.png'], [os.path.basename(x) for x in q.shots(V)])
def mw(n, w, en):   # Mac 局：满屏判据已开、没报未满屏（满屏会判过），宽屏行屏幕宽 w、启用 en 点
    o = tempfile.mkdtemp(); open(f'{o}/host.log', 'w').write(f'[s2a] 图形模式：直连（依据 x）\n[s2a] 宽屏：main 49 个点，启用 {en} 个，屏幕宽 {w}，启用表 t\n[dm] [nvn-mtl] [画] 满屏判据 已开\n')
    open(f'{o}/gfx_mode.txt', 'w').write('直连\n'); return q.check(o, n, ai=False)
r = mw('a', 1920, 0)
chk('⑰ Mac 局 1920 宽（原比例、启用 0 点）：满屏「过」改不作数，读数写屏宽与点数', ('不作数', True, True), (r['满屏'], '屏宽 1920、宽屏启用 0 点' in r['读数'], '满屏不作数：Mac 局屏宽 1920' in r['读数']))
r = mw('b', 2346, 12)
chk('⑰ Mac 局 2346 宽（全能力）：满屏照判过，读数写屏宽与点数', ('过', True), (r['满屏'], '屏宽 2346、宽屏启用 12 点' in r['读数']))
# ⑱ 满屏冤判（11-01 满屏画面线）：丝之歌 host.log 里 3 行「出图两侧黑是游戏画的…不判未满屏」说明行、0 条【红】→ 两侧纯色块不是缺内容，空洞判据不采信；有一条【红】出图未满屏则照旧
SILK = """[dm] [nvn-mtl] 出图两侧黑是游戏画的（宽屏包装层认定两侧本帧被放宽内容画过，如转场压黑的场景上盖 16:9 读盘页，不判未满屏）（第 9774 次出图起）：画面 2346x1080 两侧各 213 像素黑边
[dm] [nvn-mtl] 出图两侧黑是游戏画的（宽屏包装层认定两侧本帧被放宽内容画过，如转场压黑的场景上盖 16:9 读盘页，不判未满屏）（第 23625 次出图起）：画面 2346x1080 两侧各 213 像素黑边
[dm] [nvn-mtl] 出图两侧黑是游戏画的（宽屏包装层认定两侧本帧被放宽内容画过，如转场压黑的场景上盖 16:9 读盘页，不判未满屏）（第 29715 次出图起）：画面 2346x1080 两侧各 213 像素黑边
"""
chk('⑱ 丝之歌 3 行说明行、无【红】→ game_drew_sides 为真（满屏画面空洞判据不采信）', True, q.game_drew_sides(SILK))
chk('⑱ 同样的说明行外加一条【红】出图未满屏 → 为假（照旧判）', False, q.game_drew_sides(SILK + '[dm] [nvn-mtl] 【红】出图未满屏（内容，第 680 次出图起，累计第 1 次）：画面 2346x1080 左侧到第 360 列起明显变暗\n'))
chk('⑱ 没有说明行（只有「未满屏」字样的别的话）→ 为假', False, q.game_drew_sides('[x] 这局没报未满屏\n'))
# ⑲ 局内 16:9 留黑边（11-02 维护者：马里奥和索尼克东京局内被认成原比例页、五问记满屏过；局内任一时刻没铺满就红）：手算期望——
#    判据开 + 回退说明行、无【红】、没 MARK → 全算局内、不过；同一段在 MARK（帧 845）后才离开 → 不过；整段在 MARK 前（第 300 次起、第 700 次离开）→ 过、算界面页；
#    MARK 后换成「两侧黑是游戏画的」→ 段在那里结束；直连出图层宽高比对了、局内有原比例页段 → 照样不过（原先比例对了就直接判过）
FB = '[dm] [nvn-mtl] [画] 满屏判据 已开\n[dm] [nvn-mtl] 出图原比例回退（纯 2D 帧放宽后两侧游戏都没画到，整帧退回 16:9 居中；五问记「原比例回退」、不记满屏过）（第 1632 次出图起）：画面 2346x1080 两侧各 213 像素黑边\n'
import score_cells as sc
chk('⑲ 回退说明行、没 MARK → 一段局内、fullscreen 不过、说明写留黑边', (1, False, True), (lambda r: (len(sc.fs_pillar(FB)), r[0], '留黑边' in r[1]))(sc.fullscreen(FB)))
PG = lambda n: f'[dm] [nvn-mtl] 出图原比例居中（16:9 原比例页，宽屏包装层认定纯 CPU 供图、没有更宽内容可补，按 维护者 规则留黑边，不判未满屏）（第 {n} 次出图起）：画面 2346x1080 两侧各 213 像素黑边\n'
LV = lambda n: f'[dm] [nvn-mtl] 出图离开原比例页（第 {n} 次出图起）：画面 2346x1080 两侧各 213 像素黑边（之后黑带照常判）\n'
GD = lambda n: f'[dm] [nvn-mtl] 出图两侧黑是游戏画的（宽屏包装层认定两侧本帧被放宽内容画过）（第 {n} 次出图起）：画面 2346x1080 两侧各 213 像素黑边\n'
MK0 = '[s2a] S2A 标记 MARK：帧 845\n'
ON0 = '[dm] [nvn-mtl] [画] 满屏判据 已开\n'
chk('⑲ 原比例页第 300 次起、第 700 次离开、MARK 845 → 过（界面页）', True, sc.fullscreen(ON0 + PG(300) + LV(700) + MK0)[0])
chk('⑲ 原比例页第 300 次起、第 900 次才离开、MARK 845 → 不过', False, sc.fullscreen(ON0 + PG(300) + MK0 + LV(900))[0])
chk('⑲ MARK 后第 1000 次起原比例页、第 1200 次换成两侧是游戏画的 → 一段、起点 1000', [('出图原比例居中', 1000, 1200)], [(g['种类'], g['起'], g['止']) for g in sc.fs_pillar(ON0 + MK0 + PG(1000) + GD(1200))])
DL = '[s2a] 直连：出图层已挂到根视图 2346x1080 像素（宽高比 2.172）\n[s2a] iOS 壳：x 屏幕 2346x1080 像素\n'
chk('⑲ 直连出图层比例对、局内原比例页 → 不过（原先直接判过）', False, sc.fullscreen(DL + ON0 + MK0 + PG(1000))[0])
chk('⑲ 直连出图层比例对、判据开、局内没事 → 过', True, sc.fullscreen(DL + ON0 + MK0)[0])
# ⑲c 三档（11-02 维护者：过 / 局内红 / 待分层）：手算期望——MARK 帧 845；
#    段 300–900 盖住 MARK → 局内红；MARK 后段 1000–1200（中间一张自动玩家截图 ap_mid.png）→ 待分层、说明带起止与截图；
#    没 MARK 的【红】→ 待分层；MARK 后的【红】→ 局内红；整段在 MARK 前 → 过
SH = '自动玩家：截图 /x/run/ap_mid.png（2346x1080）\n'
chk('⑲c 段盖住 MARK → 局内红', '局内红', sc.fs_tier(ON0 + PG(300) + MK0 + LV(900))[0])
t = sc.fs_tier(ON0 + MK0 + PG(1000) + SH + LV(1200))
chk('⑲c MARK 后段（没盖住 MARK）→ 待分层，说明带 1000–1200 与截图', ('待分层', True), (t[0], '第 1000–1200 次出图' in t[1] and 'ap_mid.png' in t[1]))
R180 = '[dm] [nvn-mtl] 【红】出图未满屏（内容，第 180 次出图起，累计第 1 次）：画面 2346x1080 左右两边 16:9 外侧 213 像素黑带\n'
chk('⑲c 没 MARK 的【红】→ 待分层', '待分层', sc.fs_tier(ON0 + R180)[0])
chk('⑲c MARK 后的【红】→ 局内红', '局内红', sc.fs_tier(ON0 + MK0 + R13360)[0] if 'R13360' in globals() else sc.fs_tier(ON0 + MK0 + '[dm] [nvn-mtl] 【红】出图未满屏（内容，第 13360 次出图起，累计第 2 次）：x\n')[0])
chk('⑲c 整段在 MARK 前 → 过', '过', sc.fs_tier(ON0 + PG(300) + LV(700) + MK0)[0])
# ⑲d 自动分层（11-02 维护者：神之天平外传局内留黑边必须判局内红）：手算期望——MARK 845 后段 1000–1200，段内判据存图 fit_pil_1.png；
#    判官答「游戏画面 是」→ 局内红；答「否」→ 这段归界面页、整局过且说明记界面页 1 段；不给分层 → 待分层；fs_pillar 认判据存图行
PIL = '[dm] [nvn-mtl] 满屏判据第 1 次留黑边段的第一帧（第 1000 次出图）存到 /x/run/shots/fit_pil_1.png\n'
HL4 = ON0 + MK0 + PG(1000) + PIL + LV(1200)
chk('⑲d fs_pillar 认判据存图行', ['fit_pil_1.png'], sc.fs_pillar(HL4)[0]['截图'])
chk('⑲d 截图判局内 → 局内红', '局内红', sc.fs_tier(HL4, {'fit_pil_1.png': '局内'})[0])
t = sc.fs_tier(HL4, {'fit_pil_1.png': '界面页'})
chk('⑲d 截图判界面页 → 过、说明记界面页 1 段', ('过', True), (t[0], '界面页留黑边 1 段' in t[1]))
chk('⑲d 不给分层 → 待分层', '待分层', sc.fs_tier(HL4)[0])
import tempfile
_d = tempfile.mkdtemp(); open(os.path.join(_d, 'fit_pil_1.png'), 'wb').write(b'x')
_keep2 = q.ai_judge; q.ai_judge = lambda ps, prompt=None, pre=(), cached_only=False: [{'游戏画面': '是'} for _ in ps]
chk('⑲d fs_layer：局目录里的段内截图交判官、是 → 局内', {'fit_pil_1.png': '局内'}, q.fs_layer(_d, HL4))
q.ai_judge = lambda ps, prompt=None, pre=(), cached_only=False: (_ for _ in ()).throw(RuntimeError('没有判官'))
chk('⑲d fs_layer：判官失败 → 空（不猜）', {}, q.fs_layer(_d, HL4))
q.ai_judge = _keep2
chk('⑲d fs_layer ask=False：判官缓存里没有这组图 → 空、不发问', {}, q.fs_layer(_d, HL4, ask=False))
# ⑲b 界面页一层（维护者 11-02 07:2x「菜单页、暂停页等页面满屏是另一层的问题，不是不能开满屏的阻塞项」）：手算期望——MARK 在帧 845；
#    红在第 627 次出图（MARK 前，选角色页）→ 局内 0、界面页 1，满屏过且说明写界面页；红在第 13360 次（MARK 后）→ 不过；没有 MARK 行 → 红全算局内、不过
ON = '[dm] [nvn-mtl] [画] 满屏判据 已开\n'
MK = '[s2a] S2A 标记 MARK：帧 845\n'
R627 = '[dm] [nvn-mtl] 【红】出图未满屏（内容，第 627 次出图起，累计第 1 次）：画面 2346x1080 右侧第 2137 列起明显变暗\n'
R627U = R627 + '[dm] [nvn-mtl] 出图恢复满屏（内容，第 700 次出图起，累计第 1 次）\n'   # 界面页那条红在 MARK 前恢复（11-02 起没恢复、延续进局内的算局内，见 ⑲e）
R13360 = '[dm] [nvn-mtl] 【红】出图未满屏（内容，第 13360 次出图起，累计第 2 次）：画面 2346x1080 右侧 16:9 边界第 2133 列接缝贯穿 99% 行\n'
chk('⑲b MARK 前一条红 → fs_reds (0, 1)', (0, 1), sc.fs_reds(ON + R627U + MK))
# ⑲e 跨 MARK 的红（11-02 塞尔达：MARK 前起的接缝红一路没恢复、局内压扁）：手算期望——MARK 845；红第 627 次起、没有恢复行 → 局内 1；
#    第 700 次恢复（早于 MARK）→ 界面页 1；第 900 次才恢复（晚于 MARK）→ 局内 1；恢复的是另一种类（几何）→ 不算恢复、局内 1
RC = lambda kind, n: f'[dm] [nvn-mtl] 出图恢复满屏（{kind}，第 {n} 次出图起，累计第 1 次）\n'
chk('⑲e MARK 前起、没恢复 → 局内', (1, 0), sc.fs_reds(ON + R627 + MK))
chk('⑲e MARK 前起、MARK 前恢复 → 界面页', (0, 1), sc.fs_reds(ON + R627 + RC('内容', 700) + MK))
chk('⑲e MARK 前起、MARK 后才恢复 → 局内', (1, 0), sc.fs_reds(ON + R627 + MK + RC('内容', 900)))
chk('⑲e 恢复的是别的种类（几何）→ 不算恢复、局内', (1, 0), sc.fs_reds(ON + R627 + RC('几何', 700) + MK))

chk('⑲b MARK 前一条红 → fullscreen 过、说明写界面页', (True, True), (lambda r: (r[0], '界面页未满屏 1 条' in r[1]))(sc.fullscreen(ON + R627U + MK)))
chk('⑲b MARK 后一条红 → fullscreen 不过', False, sc.fullscreen(ON + MK + R13360)[0])
chk('⑲b 前后各一条 → fs_reds (1, 1)、不过', ((1, 1), False), (sc.fs_reds(ON + R627U + MK + R13360), sc.fullscreen(ON + R627U + MK + R13360)[0]))
chk('⑲b 没有 MARK 行 → 红算局内、不过', ((1, 0), False), (sc.fs_reds(ON + R627), sc.fullscreen(ON + R627)[0]))
# ⑳ 定论轮（11-01 维护者：c1101fx 进局 10 款 7 款画面「拿不准」= 没数据）：手算期望——首轮 / 复核后 [正常, 拿不准, 异常 把握 0.6, 异常 把握 0.9]，
#    只有 2、3 号该再问（拿不准 + 把握不足的异常），同局 4 张一起送；假判官答 [正常, 异常, 正常, 正常] → 2 号改异常（把握记 1.0）、3 号改正常，1、4 号不动
_calls = []
def _fake(ps, prompt=None, pre=(), cached_only=False):
    _calls.append((list(ps), prompt)); return _fake.ret
_keep = q.ai_judge; q.ai_judge = _fake
_fake.ret = [{'画质': '正常'}, {'画质': '异常', '问题': '天空横条带', '清单': '①'}, {'画质': '正常'}, {'画质': '正常'}]
A = [{'画质': '正常'}, {'画质': '拿不准', '问题': '条纹'}, {'画质': '异常', '把握': 0.6}, {'画质': '异常', '把握': 0.9, '问题': '黑块'}]
k = q.settle(['1.png', '2.png', '3.png', '4.png'], A)
chk('⑳ 拿不准 + 把握不足的异常共 2 张再问、同局 4 张一起送', (2, 4, True), (k, len(_calls[0][0]), '2、3 号' in _calls[0][1]))
chk('⑳ 定论后：1 正常、2 异常（把握 1.0）、3 正常、4 原样异常 0.9', [('正常', None), ('异常', 1.0), ('正常', 1.0), ('异常', 0.9)], [(a['画质'], a.get('把握')) for a in A])
_calls.clear(); B = [{'画质': '正常'}, {'画质': '异常', '把握': 0.95}]
chk('⑳ 没有该再问的 → 不调判官、返回 0', (0, 0), (q.settle(['1.png', '2.png'], B), len(_calls)))
_fake.ret = [{'画质': '拿不准'}]; C = [{'画质': '拿不准'}]
chk('⑳ 判官违规仍答拿不准 → 原样留拿不准（不臆造结论）', '拿不准', (q.settle(['1.png'], C), C[0]['画质'])[1])
q.ai_judge = _keep
# ㉑ 判官逐档备用（11-02 维护者：中转站做主力、OpenRouter 回过 402 就 1 小时内跳过、本机 claude CLI 兜底）：手算期望——
#    档序 relay → openrouter（没 anthropic 钥匙；有中转站钥匙不排 claude-cli，我方 CLI 走同一中转站）；中转站前 DROP 次回 prompt_tokens=10（< 题面字节 ÷ 3.5 + 500）判丢图、重问，之后回 99999 → 采纳；
#    丢 1 次 → 第 2 次采纳、不碰 OpenRouter；一直丢 → 问满 2 次 → OpenRouter 1 次 402（写标记）→ 报判官失败；标记新 → 档序只剩 relay；
#    没有中转站钥匙 → openrouter（标记新则跳过）→ claude-cli，假 CLI 答出；每个请求体都带 data:image/png
import threading, http.server
_srv = {'or': 0, 'relay': 0, 'img': [], 'drop': 1}
class _H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        b = self.rfile.read(int(self.headers['Content-Length'])).decode(); _srv['img'].append('data:image/png;base64,' in b)
        if self.path.startswith('/or'):
            _srv['or'] += 1; self.send_response(402); self.end_headers(); self.wfile.write(b'{"error":{"message":"Insufficient credits"}}'); return
        _srv['relay'] += 1; pt = 10 if _srv['relay'] <= _srv['drop'] else 99999
        out = json.dumps({'choices': [{'message': {'content': '{"图":[{"号":1,"游戏画面":"是","画质":"正常"}]}'}}], 'usage': {'prompt_tokens': pt}})
        self.send_response(200); self.end_headers(); self.wfile.write(out.encode())
_hs = http.server.HTTPServer(('127.0.0.1', 0), _H); threading.Thread(target=_hs.serve_forever, daemon=True).start(); _u = f'http://127.0.0.1:{_hs.server_port}'
open(f'{T}/k', 'w').write('x\n')
_env = dict(os.environ); os.environ.pop('S2A_QC_PROVIDER', None)
os.environ.update(OPENROUTER_API_KEY='x', S2A_QC_OR_URL=_u + '/or', S2A_QC_RELAY_URL=_u + '/relay', S2A_QC_RELAY_KEY=f'{T}/k')
q.CACHE = os.path.join(T, 'qc21'); q.RELAY_KEY = f'{T}/k'; q.OR402 = os.path.join(T, 'or402'); os.environ.pop('ANTHROPIC_API_KEY', None)   # CACHE / 标记路径在导入时定，改环境不生效
fk = os.path.join(T, 'bin21'); os.makedirs(fk); open(f'{fk}/claude', 'w').write('#!/bin/sh\necho \'{"图":[{"号":1,"游戏画面":"否","画质":"正常"}]}\'\n'); os.chmod(f'{fk}/claude', 0o755)
os.environ['PATH'] = fk + os.pathsep + _env['PATH']
chk('㉑ 不指定时的档序（中转站在前、有中转站钥匙不排 CLI）', ['relay', 'openrouter'], q.providers())
a = q.ai_judge([f'{R}/shots_ap/ap_mark.png'])
chk('㉑ 中转站丢图 1 次后第 2 次采纳、不碰 OpenRouter、结论照出', (0, 2, '中转站/claude-sonnet-5', '是'), (_srv['or'], _srv['relay'], a[0]['模型'], a[0]['游戏画面']))
_srv.update(drop=99, relay=0); q.CACHE = os.path.join(T, 'qc21b')   # 换空缓存：同图同题面不读上一问的结论
try: q.ai_judge([f'{R}/shots_ap/ap_mark.png']); e21 = ''
except RuntimeError as e: e21 = str(e)
chk('㉑ 中转站一直丢图（问满 2 次）→ OpenRouter 402 只问 1 次、写标记 → 判官失败写明原因', (2, 1, True, True), (_srv['relay'], _srv['or'], os.path.exists(q.OR402), 'HTTP 402' in e21))
chk('㉑ 每个发出的请求体都带图', (5, True), (len(_srv['img']), all(_srv['img'])))
chk('㉑ 402 标记 1 小时内 → 档序只剩 relay', ['relay'], q.providers())
q.RELAY_KEY = os.path.join(T, '无钥匙'); q.CACHE = os.path.join(T, 'qc21c'); a = q.ai_judge([f'{R}/shots_ap/ap_mark.png'])
chk('㉑ 没有中转站钥匙 → 跳过 openrouter（标记新）→ claude-cli 结论照出', (['claude-cli'], 'claude-cli', '否'), (q.providers(), a[0]['模型'], a[0]['游戏画面']))
os.environ['S2A_QC_PROVIDER'] = 'claude-cli'; chk('㉑ 指定 S2A_QC_PROVIDER 只问那一档', ['claude-cli'], q.providers())
_hs.shutdown(); os.environ.clear(); os.environ.update(_env)
print('快检单测全绿' if not bad else '快检单测有红'); sys.exit(1 if bad else 0)

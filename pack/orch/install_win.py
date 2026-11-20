#!/usr/bin/env python3
"""Windows 签名装机 + 推资源包，一条命令、不用点（11-06 Windows 端到端线：维护者「3 款纯 Windows 一键出包、装机、进局，维护者 不用动手」）。
签名装机用玩家自己装的 Impactor 同版本官方命令行 plumesign（维护者 11-04/11-05 定：不内嵌、不冒充，用原版；账号是玩家在 Impactor 里登过的那份，
  plumesign 与图形版共用 %APPDATA%\\PlumeImpactor 账号存档，登过一次就不再要密码）；推资源包用两端共用的 tools/push_pack.py（Windows 上走 s2a-devpush：house_arrest + AFC）。

  python -X utf8 -m pack.orch.install_win <ipa> <资源包目录> --udid <手机 UDID> [--name <桌面显示名>] [--plumesign <路径>] [--devpush <路径>] [--full]

步骤：① 按 UDID 在本机设备服务（usbmuxd）里找设备编号（plumesign 的 --udid 实际比的是设备编号，不是 UDID；USB 优先，Wi-Fi 也认）；
  ② plumesign sign --apple-id --register-and-install（登记设备、注册包名、签名、装机）并 -o 存一份签好的 ipa，从里面读装机后的实际包名（免费账号会加团队号后缀）；
  ③ tools/push_pack.py 按实际包名推资源包（同 Mac 启动器、dev_run 一份规则）。
退出码：0 装好并推完；1 失败（最后一行【红】带共用错误码，见 pack/jobs/errors.py）；2 用法；75 缺 plumesign / 设备服务里没这台手机。
实测（11-06 Windows 出包机 + 测试设备2，Wi-Fi）：金刚 c3 ipa 46 MB 签名 + 装机 48 秒。"""
import argparse, glob, os, plistlib, re, shutil, socket, struct, subprocess, sys, tempfile, time, zipfile
from pathlib import Path

R = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(R / 'pack/jobs')); sys.path.insert(0, str(R))
import impactor   # noqa: E402  找玩家装的 Impactor、报错 → 共用错误码


class Fail(Exception):
    def __init__(self, msg, code=1, err=None): super().__init__(msg); self.code = code; self.err = err


def plumesign_path(given=None):
    """plumesign 位置：指定（发版随包那份由调用方传）> S2A_PLUMESIGN > PATH > 玩家 Impactor 同目录。
    不再去桌面 / 下载文件夹找（11-11 装机链路审查）：那一档会执行下载目录里任何叫 plumesign* 的程序，发版本来就随包带了"""
    exe = 'plumesign.exe' if os.name == 'nt' else 'plumesign'
    cands = [given, os.environ.get('S2A_PLUMESIGN'), shutil.which('plumesign')]
    st = impactor.status()
    if st.get('path'): cands += glob.glob(os.path.join(glob.escape(os.path.dirname(st['path'])), 'plumesign*'))
    for c in cands:
        if c and os.path.isfile(c) and (os.name != 'nt' or c.lower().endswith('.exe')): return c
    return None


def _mux_list():
    """本机设备服务（usbmux）ListDevices 的原始设备表"""
    if os.name == 'nt': s = socket.create_connection(('127.0.0.1', 27015), 5)
    else: s = socket.socket(socket.AF_UNIX); s.settimeout(5); s.connect('/var/run/usbmuxd')
    with s:
        p = plistlib.dumps({'MessageType': 'ListDevices', 'ClientVersionString': 'S2Arm-install', 'ProgName': 'S2Arm-install', 'kLibUSBMuxVersion': 3})
        s.sendall(struct.pack('<IIII', 16 + len(p), 1, 8, 1) + p)
        h = b''
        while len(h) < 16: h += s.recv(16 - len(h))
        n = struct.unpack('<I', h[:4])[0] - 16; b = b''
        while len(b) < n:
            c = s.recv(n - len(b))
            if not c: break
            b += c
    return plistlib.loads(b).get('DeviceList', [])


def mux_devices():
    """本机设备服务列出的设备 → [(UDID, 连接方式, 设备编号)]"""
    out = []
    for d in _mux_list():
        pr = d.get('Properties', {}); out.append((pr.get('SerialNumber'), pr.get('ConnectionType'), d.get('DeviceID')))
    return out


APPLE_PID_IPAD = 0x12AB   # 苹果 USB 产品号：iPad 0x12AB（iPhone 0x12A8，本机实测两台 iPhone 都是 0x12A8）


def device_kind(udid):
    """这台用数据线连着的设备是不是 iPad（#487：入口写「装到 iPad」）→ "ipad" / "iphone"；读不到产品号（只在 Wi-Fi 上、设备服务连不上）→ ""。
    只认 USB 产品号正好是 iPad 的才算 iPad，其余照旧按 iPhone 写"""
    if not udid:
        return ""
    pids = {d.get('Properties', {}).get('ProductID') for d in _mux_list()
            if d.get('Properties', {}).get('SerialNumber') == udid and d.get('Properties', {}).get('ConnectionType') == 'USB'}
    pids.discard(None)
    return "" if not pids else "ipad" if APPLE_PID_IPAD in pids else "iphone"


def device_id(udid, devs):
    hit = [d for d in devs if d[0] == udid]
    hit.sort(key=lambda d: d[1] != 'USB')   # 同一台既插线又在 Wi-Fi：走 USB
    return hit[0][2] if hit else None


def bundle_id_of(ipa):
    with zipfile.ZipFile(ipa) as z:
        n = next(x for x in z.namelist() if re.fullmatch(r'Payload/[^/]+\.app/Info\.plist', x))
        return plistlib.loads(z.read(n))['CFBundleIdentifier']


IML_KEY = 'com.apple.developer.kernel.increased-memory-limit'


def iml_facts(ipa):
    """签好的 ipa 里主 App 的描述文件（embedded.mobileprovision）：带不带「提高内存上限」、描述文件名、应用号 → dict；读不到 → None。
    plumesign 签主 App 用的权限就是描述文件里的那份（v2.7.2 signer.rs sign_single_bundle：merge_entitlements 只补钥匙串组），
    描述文件里没有 = 装上就丢（宿主首启复核【红】[IML]，诊断 #7c32b3 等一族 4 份，丢在哪一步原先看不到）"""
    try:
        with zipfile.ZipFile(ipa) as z:
            n = next((x for x in z.namelist() if re.fullmatch(r'Payload/[^/]+\.app/embedded\.mobileprovision', x)), None)
            if not n:
                return None
            raw = z.read(n)
        a, b = raw.find(b'<?xml'), raw.rfind(b'</plist>')
        prof = plistlib.loads(raw[a:b + 8]) if a >= 0 and b > a else {}
    except (OSError, zipfile.BadZipFile, plistlib.InvalidFileException, ValueError):
        return None
    ent = prof.get('Entitlements') or {}
    return dict(iml=bool(ent.get(IML_KEY)), name=str(prof.get('Name') or ''), app_id=str(ent.get('application-identifier') or ''),
                team=str((prof.get('TeamIdentifier') or [''])[0]))


def cert_not_after(der):
    """X.509 证书（DER）→ 到期时刻（epoch 秒）；读不了 → None"""
    import calendar
    try:
        _, s, e = _tlv(der, 0)
        _, ts, te = _children(der, s, e)[0]
        tbs = _children(der, ts, te)
        if tbs[0][0] == 0xA0: tbs = tbs[1:]   # 版本号 [0]
        _, vs, ve = tbs[3]
        na = _children(der, vs, ve)[1]
        w = der[na[1]:na[2]].decode('ascii')
        w = ('20' + w if na[0] == 0x17 else w).rstrip('Z')   # UTCTime YYMMDDhhmmssZ / GeneralizedTime YYYYMMDDhhmmssZ
        return float(calendar.timegm(time.strptime(w[:14], '%Y%m%d%H%M%S')))
    except (IndexError, ValueError, UnicodeDecodeError):
        return None


def signed_expiry(ipa):
    """签好的 ipa → {expires: 到期时刻 epoch 秒, days: 描述文件有效天数}；读不到 → None（#1772）。
    到期取主 App 描述文件 ExpirationDate；描述文件里只列了一张开发证书时，再和它的到期取较早的（证书先到期，App 也就打不开了）。
    列了不止一张时认不出是哪张签的，只按描述文件。days 是苹果这次实际给的期限（免费账号 7 天、付费一年），调用方据此判免费 / 付费，不再估"""
    import calendar
    try:
        with zipfile.ZipFile(ipa) as z:
            n = next((x for x in z.namelist() if re.fullmatch(r'Payload/[^/]+\.app/embedded\.mobileprovision', x)), None)
            if not n:
                return None
            raw = z.read(n)
        a, b = raw.find(b'<?xml'), raw.rfind(b'</plist>')
        prof = plistlib.loads(raw[a:b + 8]) if a >= 0 and b > a else {}
        made, until = prof.get('CreationDate'), prof.get('ExpirationDate')
        if not (made and until):
            return None
        made, exp = calendar.timegm(made.utctimetuple()), float(calendar.timegm(until.utctimetuple()))
    except (OSError, zipfile.BadZipFile, plistlib.InvalidFileException, ValueError, AttributeError):
        return None
    certs = prof.get('DeveloperCertificates') or []
    if len(certs) == 1 and cert_not_after(certs[0]):
        exp = min(exp, cert_not_after(certs[0]))
    return dict(expires=exp, days=round((calendar.timegm(until.utctimetuple()) - made) / 86400))


def iml_line(ipa):
    """装机日志 / 进度里的一行：签出的 S2Arm 带不带「提高内存上限」。丢了的那行以「【IML 丢了】」开头（作业层据此记诊断、黄字提示玩家）"""
    f = iml_facts(ipa)
    if f is None:
        return '【IML】读不到签好的包里的描述文件，没核「提高内存上限」'  # 非玩家句
    where = f'描述文件「{f["name"]}」、应用号 {f["app_id"]}、团队 {f["team"]}'  # 非玩家句
    return f'【IML】签好的 S2Arm 带「提高内存上限」（{where}）' if f['iml'] else f'【IML 丢了】签好的 S2Arm 描述文件里没有「提高内存上限」（{where}）'  # 非玩家句


def legacy_case_bundle(bundle):
    """容器包名的旧写法：0.2.1 及以前 Windows 由 plumesign 自己在包名后接「.大写团队号」（io.s2arm.games.<大写团队号>），0.2.2 起和 Mac 统一小写。
    苹果的包名不分大小写，这个 Apple ID 早先注册过大写那个时，新写法去注册会被拒（9401），plumesign 查已有 App ID 却分大小写、认不出旧的。
    不是容器包名或团队号里没有字母 → None"""
    m = re.fullmatch(r'(io\.s2arm\.games\.)([a-z0-9]{10})', bundle or '')
    return m[1] + m[2].upper() if m and m[2] != m[2].upper() else None


WIDGET_TAKEN = re.compile(r"Identifier '[^']+\.widget' is not available", re.I)   # 9401 被拒的是小组件扩展的包名（pack/widget_ext.py SUFFIX）


def strip_plugins(src, dst):
    """另存一份去掉扩展（Payload/*.app/PlugIns/）的 ipa → 原包里有没有扩展。没有就不写 dst（同 pack/jobs/install.py ipa_strip_widget）"""
    with zipfile.ZipFile(src) as z:
        names = z.infolist()
        drop = [i for i in names if re.match(r'Payload/[^/]+\.app/PlugIns/', i.filename)]
        if not drop: return False
        with zipfile.ZipFile(dst, 'w', zipfile.ZIP_DEFLATED) as o:
            for i in names:
                if i not in drop: o.writestr(i, z.read(i), compress_type=i.compress_type)
    return True


def plumesign_env():
    """起 plumesign 的环境：日志级别固定 info，不继承玩家机上的 RUST_LOG（#585：环境里有 RUST_LOG=warn，info 级的「Installation complete」
    被压掉，装好了、退出码 0 也判成失败）。判装机成功靠这行，级别得由我们定"""
    return impactor.plume_env()   # S2A_PLUME_HOME 设了（Mac 通用壳）→ XDG_CONFIG_HOME：账号存档与私钥放启动器私有目录


def sign_install(exe, ipa, dev_id, name, log, out_ipa, bundle=None, register=True):
    """register=False：只签名出 out_ipa、不登记设备也不装（设备已在团队里、plumesign 却认不出时用，见 ALREADY_REGISTERED），装机另走 install_signed"""
    cmd = [exe, 'sign', '--package', str(ipa), '--apple-id'] + (['--register-and-install', '--udid', str(dev_id)] if register else []) + ['-o', str(out_ipa)]
    if name: cmd += ['--custom-name', name]
    # 包名由调用方定（install.container_bundle_id，和 Mac 一致的小写团队号）：不给时 plumesign 自己在包里的包名后接「.大写团队号」，
    #   它查已有 App ID 分大小写，同一团队在 Mac 注册过的小写包名认不出、去新建又撞 Apple 9401（包名不分大小写已被占）。给了它就原样用、不再接团队号
    if bundle: cmd += ['--custom-identifier', bundle]
    p = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', env=plumesign_env())
    text = p.stdout + p.stderr
    Path(log).write_text(text, encoding='utf-8')
    if p.returncode or (register and 'Installation complete' not in text) or (not register and not Path(out_ipa).is_file()):
        tail = [ln for ln in text.splitlines() if ln.strip() and 'DEBUG' not in ln][-1:] or ['（没有输出）']
        code = impactor.explain(text)
        raise Fail(f'签名装机失败（{code}）：{tail[0][-300:]}', 1, code)


# 第二次起装同一台手机（#800）：plumesign 登记设备前按设备号查团队里有没有，比较是精确字符串（同版 v2.7.2 源码
#   crates/plume_core/src/developer/qh/devices.rs `dev.device_number == *device_udid`）。Windows 的设备服务报的是不带横线的 24 位，
#   苹果那边存的是带横线的 25 位，永远对不上 → 每次都去登记 → 苹果回 35「already exists on this team」，签名装机整个中止（Mac 的设备号带横线，不中招）。
#   设备其实已经在团队里了，这次签出的描述文件会带上它：认出这句就改成「只签名不登记」再用 device --install 装签好的包
ALREADY_REGISTERED = re.compile(r'already exists on this team', re.I)
RETRY_WAIT = 3   # 连不上签名服务器时重试前等几秒（单测置 0）


# 签名前核开发证书（11-17 #1450 之后读 PlumeImpactor v2.7.2 源码发现：plumesign sign --apple-id 要新建证书时撞上额度（苹果 7460），
#   不问就撤掉证书列表里的第一张——可能是玩家 Xcode / AltStore 用的那张；crates/plume_core/src/utils/certificate.rs request_new_certificate，
#   commands/sign.rs 传的确认回调是 None）。只用确定的事实判：苹果列表里有一张证书的公钥 = 本机 <数据目录>/keys/<团队>/key.pem 的、机器名 = plumesign 缺省的 AltStore → 不会新建、什么都不撤；
#   没有，但列表里也没有别人的开发证书 → 新建也撤不到别人；没有、又有别人的 → 交调用方问玩家（它会撤哪张：列表第一张）。上限判不准（同一类型 maxActive 1
#   却有 3 张在用），不按张数判。只读：plumesign account certificates，不切账号、不动证书
def _tlv(b, i):
    """DER 一项 → (标签, 内容起, 内容止)"""
    tag, n = b[i], b[i + 1]
    i += 2
    if n & 0x80:
        k = n & 0x7F
        n = int.from_bytes(b[i:i + k], 'big'); i += k
    return tag, i, i + n


def _children(b, start, end):
    out = []
    while start < end:
        t, s, e = _tlv(b, start)
        out.append((t, s, e)); start = e
    return out


def cert_modulus_and_start(der):
    """X.509 证书（DER）→ (RSA 模数, 生效时刻文字 YYYY-MM-DD)；不是 RSA / 读不了 → (None, '')"""
    try:
        _, s, e = _tlv(der, 0)
        _, ts, te = _children(der, s, e)[0]
        tbs = _children(der, ts, te)
        if tbs[0][0] == 0xA0: tbs = tbs[1:]   # 版本号 [0]
        vt, vs, ve = tbs[3]
        nb = _children(der, vs, ve)[0]
        when = der[nb[1]:nb[2]].decode('ascii', 'replace')
        # UTCTime（0x17）YYMMDDhhmmssZ；GeneralizedTime（0x18）YYYYMMDDhhmmssZ
        date = f'20{when[0:2]}-{when[2:4]}-{when[4:6]}' if nb[0] == 0x17 else f'{when[0:4]}-{when[4:6]}-{when[6:8]}'
        _, ks, ke = tbs[5]
        _, bs, be = _children(der, ks, ke)[1]   # BIT STRING
        _, rs, re_ = _tlv(der, bs + 1)           # 跳过「未用位数」那一字节
        _, ms, me = _children(der, rs, re_)[0]
        return int.from_bytes(der[ms:me], 'big'), date
    except (IndexError, ValueError):
        return None, ''


def key_modulus(pem):
    """PKCS#8 私钥 PEM（plumesign keys/<团队>/key.pem）→ RSA 模数；读不了 → None"""
    import base64
    try:
        body = ''.join(l for l in pem.splitlines() if l and not l.startswith('-----'))
        der = base64.b64decode(body)
        _, s, e = _tlv(der, 0)
        kids = _children(der, s, e)
        _, os_, oe = kids[2]                      # OCTET STRING → RSAPrivateKey
        _, rs, re_ = _tlv(der, os_)
        return int.from_bytes(der[slice(*_children(der, rs, re_)[1][1:])], 'big')
    except (IndexError, ValueError):
        return None


CERT_BLOCK = re.compile(r'Cert \{(.*?)\n\s*\},?\s*(?=\n\s*Cert \{|\n\s*\]|$)', re.S)


def parse_certs(text):
    """plumesign account certificates 的输出（Rust Debug 形式的 Cert 列表，同版 v2.7.2）→ [{name, machine, der}]，顺序同苹果列表"""
    out = []
    for m in CERT_BLOCK.finditer(text):
        body = m.group(1)
        name = re.search(r'\bname: "([^"]*)"', body)
        machine = re.search(r'machine_name: Some\(\s*"([^"]*)"', body)
        content = re.search(r'cert_content: Some\(\s*\[([\d,\s]*)\]', body)
        der = bytes(int(x) for x in re.findall(r'\d+', content.group(1))) if content else b''
        out.append(dict(name=name.group(1) if name else '', machine=machine.group(1) if machine else '', der=der))
    return out


PLUME_MACHINE_NAME = 'AltStore'   # plumesign v2.7.2 sign 不传机器名时的缺省（crates/plume_core/src/utils/certificate.rs MACHINE_NAME）


def cert_check(exe, team, appdata=None, log=None, run=subprocess.run):
    """签名前核：要不要问玩家（见上）→ None（不用问）| {name, machine, created}（plumesign 这次会新建证书、列表里有别人的；满了会撤的那张 = 第一张）。
    列不出来（没联网 / 会话过期 / 工具旧）→ None：照原来签（判不了就不加阻碍，记日志）"""
    base = impactor.data_base(appdata)
    key = Path(base) / 'PlumeImpactor' / 'keys' / team / 'key.pem'
    mine = key_modulus(key.read_text(encoding='utf-8')) if key.is_file() else None
    try:
        p = run([exe, 'account', 'certificates', '--team', team], capture_output=True, text=True, encoding='utf-8', errors='replace', env=plumesign_env(), timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        if log: Path(log).write_text(f'列证书失败：{e}\n', encoding='utf-8')
        return None
    text = (p.stdout or '') + (p.stderr or '')
    if log: Path(log).write_text(re.sub(r'\[(?:\s*\d+,){20,}[\s\d,]*\]', '[…]', text), encoding='utf-8')
    if p.returncode: return None
    certs = parse_certs(text)
    rows = [(c, *cert_modulus_and_start(c['der'])) for c in certs]
    # 本机这把私钥的证书还在、且机器名是 plumesign 找的那个：不新建、不撤。plumesign 认「自己的」要两样都对上
    #   （certificate.rs find_certificate：machine_name == MACHINE_NAME 且公钥相同），只比公钥会漏掉「公钥对、名字不对 → 它照样新建」
    if mine is not None and any(m == mine and c['machine'] == PLUME_MACHINE_NAME for c, m, _ in rows): return None
    others = [(c, d) for c, m, d in rows if m is not None and m != mine]
    if not others: return None   # 一张别人的都没有：新建也撤不到别人
    first, created = rows[0][0], rows[0][2]
    return dict(name=first['name'], machine=first['machine'], created=created)


def plume_key_path(team, appdata=None):
    """plumesign 存私钥的地方（同版 v2.7.2 apps/plumesign/src/main.rs get_data_path）：Windows %APPDATA%，别的系统 $XDG_CONFIG_HOME 或 ~/.config；下面 PlumeImpactor/keys/<团队>/key.pem"""
    base = impactor.data_base(appdata)
    return Path(base) / 'PlumeImpactor' / 'keys' / team / 'key.pem'


def pkcs1_der(pem):
    """私钥 PEM → RSAPrivateKey（PKCS#1）DER。key.pem 是 PKCS#8（BEGIN PRIVATE KEY）：取出里面 OCTET STRING 包着的那份；已是 PKCS#1（BEGIN RSA PRIVATE KEY）就原样。
    读不了 → None。手机端 SecKeyCreateWithData 收的就是 PKCS#1（11-17 真机验过）"""
    import base64
    try:
        body = ''.join(l for l in pem.splitlines() if l and not l.startswith('-----'))
        der = base64.b64decode(body)
        if 'BEGIN RSA PRIVATE KEY' in pem:
            return der
        _, s, e = _tlv(der, 0)
        tag, os_, oe = _children(der, s, e)[2]
        if tag != 0x04:
            return None
        inner = der[os_:oe]
        return inner if inner[:1] == b'\x30' else None
    except (IndexError, ValueError):
        return None


def plume_identity(exe, team, appdata=None, log=None, run=subprocess.run):
    """手机自签要的身份（0.6.0 通用壳，「私钥怎么交给 App」）：本机 plumesign 私钥 + 苹果列表里它的那张证书（公钥对上、机器名 AltStore，
    就是 plumesign 自己会复用的那张，同 cert_check）→ {key: PKCS#1 DER, cert: DER, created, key_fp}；没有私钥 / 列不出 / 列表里没有对应证书 → None。
    只读：只列证书，不新建、不撤。key_fp = 私钥文件的 SHA-256 前 16 位（调用方据此判「交过了」，不必每次联网列证书）"""
    import hashlib
    key = plume_key_path(team, appdata)
    if not key.is_file():
        return None
    pem = key.read_text(encoding='utf-8')
    mine, der = key_modulus(pem), pkcs1_der(pem)
    if mine is None or der is None:
        return None
    try:
        p = run([exe, 'account', 'certificates', '--team', team], capture_output=True, text=True, encoding='utf-8', errors='replace', env=plumesign_env(), timeout=120)
    except (OSError, subprocess.SubprocessError) as e:
        if log: Path(log).write_text(f'列证书失败：{e}\n', encoding='utf-8')
        return None
    if p.returncode:
        return None
    for c in parse_certs((p.stdout or '') + (p.stderr or '')):
        m, created = cert_modulus_and_start(c['der'])
        if m == mine and c['machine'] == PLUME_MACHINE_NAME:
            return dict(key=der, cert=c['der'], created=created, key_fp=hashlib.sha256(pem.encode('utf-8')).hexdigest()[:16])
    return None


def key_fingerprint(team, appdata=None):
    """本机 plumesign 私钥文件的 SHA-256 前 16 位；没有 → None（不联网）"""
    import hashlib
    key = plume_key_path(team, appdata)
    return hashlib.sha256(key.read_text(encoding='utf-8').encode('utf-8')).hexdigest()[:16] if key.is_file() else None


def sign_dylibs(exe, shell_root, dylibs, bundle, log):
    """通用壳程序包（pack/jobs/program_pack.py win_signer，11-16）：plumesign 没有单签一个文件的命令，就拿预编壳（shell_root = 壳的 App 内容）
    去掉 PlugIns/（小组件扩展：带上它 plumesign 会为扩展另注册 App ID）、Frameworks/ 放进这几个 dylib，打成小 IPA，只签不装（sign_install register=False），
    再把签好的 dylib 原地换回。bundle = 这台手机上已装 S2Arm 的包名，经 --custom-identifier 原样交给 plumesign：不接团队号、不新建 App ID
    （11-16 Windows 出包机实测：不给它就新注册了主 App + 扩展 2 个 App ID）。签好的 dylib 代码字节不变，只换文件头长度字段与末尾签名块"""
    dylibs = [Path(d) for d in dylibs]
    with tempfile.TemporaryDirectory(prefix='s2a_signdylib_') as w:
        w = Path(w); app = w / 'stage/Payload/S2Arm.app'
        shutil.copytree(shell_root, app, symlinks=True, ignore=shutil.ignore_patterns('PlugIns'))
        (app / 'Frameworks').mkdir(exist_ok=True)
        for d in dylibs:
            shutil.copyfile(d, app / 'Frameworks' / d.name)
        stub, out = w / 'stub.ipa', w / 'signed.ipa'
        with zipfile.ZipFile(stub, 'w', zipfile.ZIP_DEFLATED, allowZip64=True) as z:
            for p in sorted((w / 'stage').rglob('*')):
                if p.is_file():
                    z.write(p, p.relative_to(w / 'stage').as_posix())
        sign_install(exe, stub, None, None, log, out, bundle=bundle, register=False)
        with zipfile.ZipFile(out) as z:
            for d in dylibs:
                tmp = d.with_name(d.name + '.signed')
                tmp.write_bytes(z.read(f'Payload/S2Arm.app/Frameworks/{d.name}'))
                os.replace(tmp, d)


def install_signed(exe, app, udid, log):
    """已签好的 App（开发者证书模式，pack/jobs/devcert.py 签）只装不签：plumesign device --install（同版 v2.7.2 源码 commands/device.rs：
    给 .app 目录直接走 installation_proxy 装；--udid 比的是设备编号，同 sign_install）"""
    if not exe: raise Fail('找不到 plumesign（随 S2Arm 附带；源码树用 --plumesign / S2A_PLUMESIGN 指定）', 75, 'impactor_missing')
    try: devs = mux_devices()
    except OSError as e: raise Fail(f'连不上本机苹果设备服务（{e}）：装 iTunes 或「Apple 设备」', 75, 'device_pairing_required')
    dev = device_id(udid, devs)
    if dev is None: raise Fail(f'设备服务里没有 {udid}：插上线并解锁', 75, 'device_not_connected')
    p = subprocess.run([exe, 'device', '--udid', str(dev), '--install', str(app)], capture_output=True, text=True, encoding='utf-8', errors='replace',
                       env=plumesign_env())
    text = p.stdout + p.stderr
    Path(log).open('a', encoding='utf-8').write(text)
    if p.returncode:
        tail = [ln for ln in text.splitlines() if ln.strip() and 'DEBUG' not in ln][-1:] or ['（没有输出）']
        code = impactor.explain(text)
        raise Fail(f'装机失败（{code}）：{tail[0][-300:]}', 1, code)


def main(argv=None):
    for s in (sys.stdout, sys.stderr):
        if hasattr(s, 'reconfigure'): s.reconfigure(encoding='utf-8')
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('ipa'); ap.add_argument('pack'); ap.add_argument('--udid', required=True); ap.add_argument('--name')
    ap.add_argument('--plumesign'); ap.add_argument('--devpush'); ap.add_argument('--full', action='store_true'); ap.add_argument('--log')
    a = ap.parse_args(argv)
    try:
        print(f'🟢 已装好：{run(a)}', flush=True); return 0
    except Fail as e:
        print(f'【红】{e}', flush=True); return e.code


def install_app(a, say=lambda m: print(m, flush=True)):
    """签名 + 覆盖装（同包名不卸载，Documents 保留）→ 装机后的实际包名；失败抛 Fail。
    签好的包里读到的真实到期（signed_expiry）放在 a.signed_expiry（读不到为 None），调用方据此记签名状态（#1772）"""
    exe = plumesign_path(a.plumesign)
    if not exe: raise Fail('找不到 plumesign（Impactor 官方同版命令行，和 Impactor 放在同一目录，或用 --plumesign / S2A_PLUMESIGN 指定）', 75, 'impactor_missing')
    try: devs = mux_devices()
    except OSError as e: raise Fail(f'连不上本机苹果设备服务（{e}）：装 iTunes 或「Apple 设备」', 75, 'device_pairing_required')
    dev = device_id(a.udid, devs)
    if dev is None: raise Fail(f'设备服务里没有 {a.udid}（在列：{", ".join(f"{u}/{c}" for u, c, _ in devs) or "无"}）：插上线并解锁，或打开 Wi-Fi 同步', 75, 'device_pairing_required')
    log = a.log or os.path.join(os.path.dirname(os.path.abspath(a.ipa)), 'plumesign.log')
    with tempfile.TemporaryDirectory() as t:
        out = Path(t, 'signed.ipa'); say(f'签名 + 装机（设备编号 {dev}）…')
        bundle = getattr(a, 'bundle', None)
        try:
            try:
                try:
                    sign_install(exe, a.ipa, dev, a.name, log, out, bundle)
                except Fail as e:
                    # 连不上签名用的 anisette 服务器（#3fc02b，DNS / 连接失败）多是一时的：等 3 秒原样再签一次，两次的输出都留在日志里
                    if e.err != 'signing_server_unreachable': raise
                    first = Path(log).read_text(encoding='utf-8', errors='replace') if Path(log).is_file() else ''
                    say('连不上 Impactor 签名要用的服务器，3 秒后重试一次')
                    time.sleep(RETRY_WAIT)
                    try:
                        sign_install(exe, a.ipa, dev, a.name, log, out, bundle)
                    except Fail as again:
                        # 重试那次 plumesign 常常连不上就直接 panic（omnisette remote_anisette_v3.rs 显式 panic），输出里只剩
                        #   「note: run with RUST_BACKTRACE=1」认不出原因、落回 device_install_failed，玩家看到的就是这句（诊断#3fc02b D-5A5K，0.4.0）。
                        #   第二次认不出就按第一次认出的报：连不上签名服务器
                        if again.err != 'device_install_failed': raise
                        raise e from again
                    finally:
                        Path(log).write_text(first + '\n--- 连不上签名服务器：重试 ---\n' + Path(log).read_text(encoding='utf-8', errors='replace'), encoding='utf-8')
            except Fail:
                first = Path(log).read_text(encoding='utf-8', errors='replace') if Path(log).is_file() else ''
                if not ALREADY_REGISTERED.search(first): raise
                say('这台手机已经登记在这个 Apple ID 的团队里（plumesign 认不出、又去登记被拒），改成只签名、再装到手机')
                try:
                    sign_install(exe, a.ipa, dev, a.name, log, out, bundle, register=False)
                    install_signed(exe, out, a.udid, log)
                finally:
                    Path(log).write_text(first + '\n--- 设备已在团队里：只签名再装 ---\n' + Path(log).read_text(encoding='utf-8', errors='replace'), encoding='utf-8')
        except Fail:
            first = Path(log).read_text(encoding='utf-8', errors='replace') if Path(log).is_file() else ''
            if impactor.explain(first) != 'app_id_taken': raise
            nowidget = Path(t, 'nowidget.ipa')
            if WIDGET_TAKEN.search(first) and strip_plugins(a.ipa, nowidget):
                # 被拒的是小组件扩展的包名（<主包名>.widget）：主包名本身没问题。plumesign 给主程序和扩展同时注册 App ID，
                #   查已有的又分大小写——这个 Apple ID 早先被不带包名签过一次（plumesign 自己接「.大写团队号」），主包名被拒、
                #   扩展却注册成了大写那个，之后小写的扩展包名永远 9401（11-18 Windows 实机复现）。同 Mac（sign_with_apple_id）：
                #   绝不因小组件装不上，这次不带小组件把 S2Arm 装上
                say('小组件的包名被苹果拒绝（9401），这次先不带主屏小组件装上 S2Arm')
                try:
                    sign_install(exe, nowidget, dev, a.name, log, out, bundle)
                finally:
                    Path(log).write_text(first + '\n--- 9401 在小组件包名：不带小组件重签 ---\n' + Path(log).read_text(encoding='utf-8', errors='replace'), encoding='utf-8')
            else:
                # 9401 = 这个包名苹果那边已被占用。本机记录里没有旧包名时（换了电脑 / 重装启动器 / 新游戏库），旧写法是唯一可能的占用者：换它重试一次，
                #   成功后调用方把实际包名记进设备记录，下次直接用（install.WinPhone.bundle_for_signing）。两次的输出都留在日志里
                legacy = legacy_case_bundle(bundle)
                if not legacy: raise
                say(f'包名 {bundle} 被苹果拒绝（9401），改用这个 Apple ID 早先注册的写法 {legacy} 重试')
                try:
                    sign_install(exe, a.ipa, dev, a.name, log, out, legacy)
                finally:
                    Path(log).write_text(first + f'\n--- 9401：改用 {legacy} 重试 ---\n' + Path(log).read_text(encoding='utf-8', errors='replace'), encoding='utf-8')
        bid = bundle_id_of(out) if out.is_file() else None
        a.signed_expiry = signed_expiry(out) if out.is_file() else None
        if out.is_file():
            line = iml_line(out)   # 核对结果一律进装机日志；丢了才交给调用方（作业层记黄字诊断，不当进度显示）
            with open(log, 'a', encoding='utf-8') as f:
                f.write('\n' + line + '\n')
            if line.startswith('【IML 丢了】'):
                say(line)
    if not bid: raise Fail('装好了但读不到签好的 ipa 里的包名，推不了资源包', 1, 'device_install_failed')
    return bid


def run(a, say=lambda m: print(m, flush=True)):
    """→ 装机后的实际包名；失败抛 Fail（oneclick_win --install 同用）"""
    bid = install_app(a, say)
    say(f'推资源包 → {bid}')
    push_pack(a.udid, bid, a.pack, a.devpush, a.full, say)
    return bid


def push_pack(udid, bid, pack, devpush=None, full=False, say=lambda m: print(m, flush=True), docs=None, ctx=None, apply_after=False, progress=None, stage=None, pack_root=None, accept_low_space=False):
    """推资源包到已装 App（Windows 没有 devicectl：s2a-devpush 走本机苹果设备服务 usbmuxd + house_arrest + AFC）→ (文件数, 字节数)；失败抛 Fail。
    容器：docs / ctx = 本款设备侧文档根 / 启动上下文，pack_root = 本款设备侧资源包根（pack/jobs/install.py pick_pack_root；不给按 <docs>/资源包）；apply_after = 推完起一次 App 只解包并等解完（同 Mac --apply）"""
    sys.path.insert(0, str(R / 'tools')); import push_pack as P   # Mac / Windows 共用的推资源包 tools/push_pack.py（11-06 两端合一）
    with tempfile.TemporaryDirectory(dir=os.environ.get('S2A_PUSH_TMP')) as w:
        try:
            dev = P.device(udid, bid, w, udid, devpush, 'devpush', say=say, **({'docs': docs, 'ctx': ctx} if docs else {}), pack=pack_root)
            nb, nt, nbytes = P.run(dev, Path(pack), Path(w), full, say, progress=progress, stage=stage, accept_low_space=accept_low_space)   # progress(已传, 本次待传总量, 批, 总批)：装机作业转成计数（#266 / #308）；stage：准备子阶段
            if apply_after:
                try: P.apply(dev, Path(pack), Path(w), nb, say)
                except P.NeedsPlayer as e:   # 传输是成功的：带上传了多少，装机作业按提醒收尾（#192）
                    need = Fail(str(e), 0, 'manual_unpack_required'); need.transferred = (nt, nbytes); raise need
        except P.Fail as e:   # 推之前探针写不进 = 锁屏 / 断开：单独报 device_locked（#333）
            code = getattr(e, 'code', None)
            raise Fail(str(e) if code in ('device_space_low', 'ios_install_no_space') else f'推资源包失败：{e}', 1, code or 'device_install_failed')
    return nt, nbytes


if __name__ == '__main__':
    sys.exit(main())

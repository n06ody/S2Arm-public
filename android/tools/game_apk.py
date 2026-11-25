#!/usr/bin/env python3
"""Per-game APK (implementation plan §2.3 / §8.2): one Android runtime building one app per game,
with the rewritten modules inside the APK.

  game_apk.py <package dir> [<out dir>] [--version-code N] [--shader-pack <pack dir>]
      APK input from a game package (tools/make_pack.py output). Checks every modules/ file
      against the package's index.tsv (index.tsv against manifest.json), reads the title id,
      name and display version from ctl/control.nacp, and writes <out>/game.properties for
      Gradle (versionCode too, default 1), <out>/res/values/game_name.xml (the launcher label:
      "<game's own NACP title> S2Arm", pack/display_name.py — same function as the iPhone display name),
      and <out>/assets/game/{game.properties,modules/}. When available, launcher PNGs are
      rendered deterministically from the package's icon.jpg; missing, empty or unreadable icons
      warn and leave the APK on its default launcher icon. With --shader-pack <make_pack shader bake
      output root> (android/tools/bake_shaders.py <out>/pack, #1574) it also writes the
      deterministic assets/game/shaders.zip the runtime extracts as the offline shader pack.
      Indexed shaders/s2a-cold-guard-v1/*.bin records are optional debug APK assets.
      Default out: build/android-game/<title id>.
      Build it with  S2ARM_ANDROID_GAME_DIR=<out, relative to the repo> bash android/docker/build.sh.

  game_apk.py --resources [--reuse-from <earlier resource package>] <package dir> <signed apk> <out dir> [--refresh-existing]
      The resource package that app installs (A0, android/contracts): the package
      without modules/, and a game-build-artifact.json whose modules are source "apk-asset"
      under apk.assetRoot game/modules, with the APK's identity: package name, versionCode, the
      APK's SHA-256 and its signing certificate's SHA-256 (v3 signing block, else v2: the
      certificate Android reports for GET_SIGNING_CERTIFICATES). The APK's modules must be
      byte-identical to the package's. Install it into the app like any package (adb import/ or
      the folder picker).
      --refresh-existing emits only manifest/index/artifact and control.nacp for the
      installer's existing-content refresh transaction. It requires the same resource
      content root already active on device; it is not a standalone resource package.
      --reuse-from hard-links a file out of an earlier resource package of this tool instead of
      copying it, when that package's own index lists the same path with the same size and
      SHA-256, the file there is regular, of that size and read-only (a sealed copy of RomFS
      data that nothing writes in place), and the link is allowed (same filesystem, link count
      not full). Every file is still checked against this package's index; a link whose bytes
      differ is dropped and copied. ext4 has no reflink, so without it each release keeps its own
      full copy of the RomFS. Links only ever come from the earlier package: a link into the
      RomFS store would change the store files' ctime, which resource_input_key.py caches on.

Only modules go into the APK; the resource package is deployed separately. The output is
derived from the player's own game: it stays under build/ and never goes into the repository or
a PR."""
import hashlib, io, json, os, re, shutil, stat, struct, sys, warnings, zipfile, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..'))
sys.path.insert(0, HERE)   # filecopy.py / direct_library.py 与本脚本同目录
from filecopy import copy_file   # noqa: E402
import direct_library   # noqa: E402  F5.2：直连描述的文件清单只按这一份规则核对
sys.path.insert(1, os.path.join(REPO, 'pack'))
import display_name   # noqa: E402  桌面标签与 iPhone 同一函数：「<游戏自带 nacp 标题> S2Arm」（11-04 维护者）
GRAPHICS_FEATURE = direct_library.GRAPHICS_FEATURE
COLD_GUARD_PREFIX = 'shaders/s2a-cold-guard-v1/'
REFRESH_PAYLOAD = ('manifest.json', 'index.tsv', 'game-build-artifact.json', 'ctl/control.nacp')


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 22), b''):
            h.update(chunk)
    return h.hexdigest()


def read_index(pack):
    """index.tsv rows, after checking it against manifest.json (pack/pack.py format)."""
    with open(os.path.join(pack, 'manifest.json')) as f:
        manifest = json.load(f)
    if manifest.get('version') != 2:
        sys.exit(f'{pack}: manifest.json is not version 2')
    index_path = os.path.join(pack, 'index.tsv')
    if sha256(index_path) != manifest.get('index_sha256'):
        sys.exit(f'{pack}: index.tsv does not match manifest.json')
    rows = {}
    with open(index_path, encoding='utf-8') as f:
        for line in f:
            path, size, digest = line.rstrip('\n').split('\t')
            rows[path] = (int(size), digest)
    return rows


def nacp_info(path):
    """Title id, name and display version from a legacy or compressed NACP."""
    with open(path, 'rb') as f:
        nacp = f.read()
    if len(nacp) < 0x3070:
        sys.exit(f'{path} is too short for a control.nacp')

    def text_field(data, off, size, *, padded=False):
        field = data[off:off + size]
        value, sep, tail = field.partition(b'\0')
        if padded and sep and any(tail):
            raise ValueError('NACP 名称区不是有效旧格式或完整压缩格式')
        try:
            text = value.decode('utf-8')
        except UnicodeDecodeError as error:
            raise ValueError('NACP 名称不是有效 UTF-8') from error
        if any(ord(c) not in (9, 10, 13) and (ord(c) < 32 or ord(c) in (0xfffe, 0xffff)) for c in text):
            raise ValueError('NACP 名称包含无效控制字符')
        return text.strip(''.join(chr(i) for i in range(33)))

    table, count = nacp, 16
    length = int.from_bytes(nacp[:2], 'little')
    compressed_candidate = 0 < length <= 0x2ffe
    if compressed_candidate:
        try:
            unpacker = zlib.decompressobj(-15)
            unpacked = unpacker.decompress(nacp[2:2 + length], 0x6001)
            if len(unpacked) == 0x6000 and unpacker.eof and not unpacker.unused_data and not unpacker.unconsumed_tail:
                table, count = unpacked, 32
        except zlib.error:
            pass
    # Raw DEFLATE has no magic. On an invalid candidate, only accept a
    # well-formed, zero-padded legacy table; never publish binary as a name.
    legacy_candidate = compressed_candidate and table is nacp
    name = next((n for n in (text_field(table, i * 0x300, 0x200, padded=legacy_candidate)
                              for i in range(count)) if n), '')
    title_id = struct.unpack_from('<Q', nacp, 0x3038)[0]
    if not title_id:
        sys.exit(f'{path} has no application id')
    return f'{title_id:016X}', name, text_field(nacp, 0x3060, 0x10)


def apk_version(version):
    """游戏显示版本 → APK versionName 里放的文字。aapt 把以「?」开头的值当主题属性引用（?attr/…）、找不到就整包出不来：
    pack/ns2_dump.py 合成的控制数据把显示版本写成「?」（表示不知道），也可能是空。取不到真值就写 unknown，不让出包停在 aapt。"""
    text = version.strip()
    return text if text.strip('?') else 'unknown'


def properties_value(text):
    """java.util.Properties escaping for a value (non-ASCII as \\uXXXX, so any encoding reads it)."""
    out = []
    for ch in text:
        if ch in '\\:=#!':
            out.append('\\' + ch)
        elif ' ' <= ch <= '~':
            out.append(ch)
        else:   # UTF-16 code units, a surrogate pair outside the BMP
            units = ch.encode('utf-16-be')
            out += ['\\u' + units[i:i + 2].hex() for i in range(0, len(units), 2)]
    return ''.join(out)


def resource_string(text):
    """An Android string resource value: XML escapes, then aapt's backslash escapes."""
    text = text.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
    text = text.replace('\\', '\\\\').replace("'", "\\'").replace('"', '\\"')
    return '\\' + text if text[:1] in ('@', '?') else text


def render_game_icon(pack):
    """控制分区导出的 icon.jpg → 密度图字节；无可读图片时使用默认图标。"""
    icon = os.path.join(pack, 'icon.jpg')
    try:
        info = os.lstat(icon)
    except FileNotFoundError:
        print('【黄】没有游戏图标：沿用默认图标')
        return None
    except OSError as error:
        print(f'【黄】游戏图标无法读取：{error}；沿用默认图标')
        return None
    if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError('拒绝图标链接或重解析点')
    if not stat.S_ISREG(info.st_mode):
        raise ValueError('游戏图标不是普通文件')
    if info.st_size == 0:
        print('【黄】游戏图标为空：沿用默认图标')
        return None
    try:
        from PIL import Image
    except ImportError:
        print('【黄】缺少 Pillow，无法生成游戏图标：沿用默认图标')
        return None
    try:
        with warnings.catch_warnings():
            warnings.simplefilter('error', Image.DecompressionBombWarning)
            with Image.open(icon) as image:
                image.load()
                image = image.convert('RGB')
                rendered = []
                for density, size in (('mdpi', 48), ('hdpi', 72), ('xhdpi', 96), ('xxhdpi', 144), ('xxxhdpi', 192)):
                    data = io.BytesIO()
                    image.resize((size, size), Image.Resampling.LANCZOS).save(data, format='PNG')
                    rendered.append((density, data.getvalue()))
    except (OSError, ValueError, Image.DecompressionBombError, Image.DecompressionBombWarning) as error:
        print(f'【黄】游戏图标打不开或缩放失败：{error}；沿用默认图标')
        return None
    return rendered


ASSET_ROOT = 'game/modules'


def apk_signer_sha256(apk):
    """SHA-256 of the signing certificate in the APK Signature Scheme v3 block (else v2)."""
    with open(apk, 'rb') as f:
        data = f.read()
    eocd = data.rfind(b'PK\x05\x06', max(0, len(data) - 65557))
    if eocd < 0:
        sys.exit(f'{apk}: no zip end of central directory')
    cd = struct.unpack_from('<I', data, eocd + 16)[0]
    if data[cd - 16:cd] != b'APK Sig Block 42':
        sys.exit(f'{apk}: no APK signing block (sign it first)')
    size = struct.unpack_from('<Q', data, cd - 24)[0]
    at, end, blocks = cd - size - 8 + 8, cd - 24, {}
    while at < end:
        length, ident = struct.unpack_from('<QI', data, at)
        blocks[ident] = data[at + 12:at + 8 + length]
        at += 8 + length

    def prefixed(buf, off):   # one uint32-length-prefixed item: (bytes, next offset)
        n = struct.unpack_from('<I', buf, off)[0]
        return buf[off + 4:off + 4 + n], off + 4 + n

    for ident in (0xf05368c0, 0x7109871a):   # v3, v2
        if ident in blocks:
            signers, _ = prefixed(blocks[ident], 0)
            signer, _ = prefixed(signers, 0)
            signed_data, _ = prefixed(signer, 0)
            _, off = prefixed(signed_data, 0)   # digests
            certs, _ = prefixed(signed_data, off)
            cert, _ = prefixed(certs, 0)
            return hashlib.sha256(cert).hexdigest()
    sys.exit(f'{apk}: no v2 / v3 signature (minSdk 34 builds are signed with them)')


def apk_identity(apk):
    """Read the final manifest itself (axml.py), never infer it from title metadata; no SDK tool needed."""
    import axml
    try:
        with zipfile.ZipFile(apk) as z:
            package, version, _, debuggable = axml.identity(z.read('AndroidManifest.xml'))
    except (OSError, KeyError, zipfile.BadZipFile, axml.AxmlError, struct.error, IndexError) as error:
        raise ValueError(f'Cannot read APK manifest: {error}') from error
    return package, version, debuggable


def validate_apk_package(title, package, debug):
    base = 'org.s2arm.game.t' + title.lower()
    if not re.fullmatch(r'[0-9A-F]{16}', title):
        raise ValueError('Invalid game title identity')
    if package == base:
        return
    suffix = package[len(base):] if package.startswith(base) else ''
    if not debug or suffix != '.w2':
        raise ValueError('APK applicationId must match the title, with only the optional debug-only .w2 suffix')


def write_shader_zip(pack_dir, destination):
    """assets/game/shaders.zip（#1574）：离线包 vulkan/generic/<profileKey>/ 整树的 ZIP，
    运行时 ShaderPackArchive 按同一布局解压。字节可复现：路径排序、时间戳固定、权限位固定；
    软链一律拒绝（与包内其余文件同口径）。"""
    pack_dir = os.path.abspath(pack_dir)
    generic = os.path.join(pack_dir, 'vulkan', 'generic')
    profiles = [name for name in sorted(os.listdir(generic))
                if os.path.isfile(os.path.join(generic, name, 'manifest.json'))] \
        if os.path.isdir(generic) else []
    if not profiles:
        sys.exit(f'{pack_dir}: no vulkan/generic/<profileKey>/manifest.json (run bake_shaders.py first)')
    entries = []
    for directory, _, files in os.walk(os.path.join(pack_dir, 'vulkan')):
        for name in sorted(files):
            full = os.path.join(directory, name)
            if os.path.islink(full):
                sys.exit(f'{full}: shader pack files must not be symlinks')
            rel = os.path.relpath(full, pack_dir).replace(os.sep, '/')
            entries.append((rel, full))
    entries.sort()
    with zipfile.ZipFile(destination, 'w') as archive:
        for rel, full in entries:
            info = zipfile.ZipInfo(rel, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o600 << 16
            with open(full, 'rb') as stream, archive.open(info, 'w') as member:
                shutil.copyfileobj(stream, member)


def cold_guard_assets(rows):
    """APK-only destination names for the optional indexed shader records."""
    for rel in sorted(p for p in rows if p.startswith(COLD_GUARD_PREFIX)):
        name = rel[len(COLD_GUARD_PREFIX):]
        size, _ = rows[rel]
        if not re.fullmatch('[0-9a-f]{64}[.]bin', name) or not 184 < size <= 16 * 1024 * 1024:
            raise ValueError(f'invalid cold guard asset index row: {rel}')
        yield rel, 'assets/s2a-cold-guard-v1/' + name


def previous_package(path, out, pack):
    """(directory, {path: (size, sha256)}) of an earlier resource package to link files from, or None.
    Only an optimisation: whatever makes it unusable is reported and the package is built with plain copies."""
    if not path:
        return None
    path = os.path.abspath(path)
    try:
        if any(os.path.commonpath([path, other]) in (path, other) for other in (out, pack)):
            raise ValueError('it overlaps the output or the input package')
        return path, read_index(path)
    except (OSError, ValueError, SystemExit) as error:
        print(f'not reusing {path}: {error}')
        return None


def link_previous(previous, rel, row, dst):
    """Hard-link dst to rel of the earlier package when, by that package's own index, it is the same
    (size, sha256) as row and the file there is regular, of that size and read-only. False: nothing linked.
    Read-only is the point: the RomFS copies are 0444 (copied from the store, which pack/roots.sh seals), so
    neither package can change the shared bytes without a chmod first; writable files are always copied."""
    root, rows = previous
    if rows.get(rel) != row:
        return False
    source = os.path.join(root, rel)
    try:
        info = os.lstat(source)
        if not stat.S_ISREG(info.st_mode) or info.st_size != row[0] or info.st_mode & 0o222:
            return False
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        os.link(source, dst)
    except OSError:
        return False
    return True


def resources(pack, apk, out, refresh_existing=False, reuse_from=None):
    pack, apk, out = os.path.abspath(pack), os.path.abspath(apk), os.path.abspath(out)
    if any(os.path.commonpath([out, source]) in (out, source) for source in (pack, apk)):
        sys.exit('resource output must not overlap the input package or APK')
    rows = read_index(pack)
    nacp = 'ctl/control.nacp'
    if nacp not in rows or os.path.getsize(os.path.join(pack, nacp)) != rows[nacp][0] or sha256(os.path.join(pack, nacp)) != rows[nacp][1]:
        sys.exit(f'{nacp} does not match index.tsv')
    title_id, name, version = nacp_info(os.path.join(pack, 'ctl', 'control.nacp'))
    with open(os.path.join(pack, 'game-build-artifact.json'), encoding='utf-8') as f:
        artifact = json.load(f)
    if artifact.get('titleId') != title_id:
        sys.exit(f'{pack}: artifact titleId {artifact.get("titleId")} is not {title_id}')
    # 直连包：发出的资源包必须是“描述列出的文件 == index 行 == 磁盘字节”且身份字段由这份
    # index 推导。以前 files[] 无人消费，描述写错也能一路装到设备；在最后一个宿主关口拒掉。
    declared = GRAPHICS_FEATURE in (artifact.get('requiredFeatures') or [])
    has_descriptor = os.path.isfile(os.path.join(pack, direct_library.DESCRIPTOR_REL))
    if declared != has_descriptor:
        sys.exit(f'{pack}: graphics descriptor and requiredFeatures disagree '
                 f'(descriptor={has_descriptor}, required={declared})')
    if declared:
        try:
            direct_library.verify_pack(pack)
        except direct_library.LibraryError as error:
            sys.exit(f'{pack}: {error}')
    modules = sorted(p for p in rows if p.startswith('modules/'))
    guard_assets = list(cold_guard_assets(rows))
    with zipfile.ZipFile(apk) as z:
        props = dict(line.split('=', 1) for line in z.read('assets/game/game.properties').decode('ascii').splitlines() if '=' in line)
        if props.get('titleId') != title_id:
            sys.exit(f'{apk} is the APK of {props.get("titleId")}, not {title_id}')
        for p in modules:   # the code the app runs is the code the package was checked with
            member = 'assets/' + ASSET_ROOT + '/' + p[len('modules/'):]
            if member not in z.namelist() or hashlib.sha256(z.read(member)).hexdigest() != rows[p][1]:
                sys.exit(f'{apk}: {member} is missing or differs from the package')
        for rel, member in guard_assets:
            expected_size, expected_sha = rows[rel]
            if (member not in z.namelist() or z.getinfo(member).file_size != expected_size
                    or hashlib.sha256(z.read(member)).hexdigest() != expected_sha):
                raise ValueError(f'APK cold guard asset missing or differs: {member}')
    package, version_code, debug = apk_identity(apk)
    if guard_assets and not debug:
        raise ValueError('cold guard assets require a debug APK')
    validate_apk_package(title_id, package, debug)
    if version_code != int(props.get('versionCode', '1')):
        raise ValueError('APK manifest versionCode differs from game.properties')
    previous = previous_package(reuse_from, out, pack)   # 在下面 rmtree(out) 之前读：被替换的 out 不能当上一版
    if os.path.lexists(out):
        if not os.path.isfile(os.path.join(out, 'manifest.json')):
            sys.exit(f'{out} exists and is not a package; pick another directory')
        shutil.rmtree(out)
    kept = [p for p in sorted(rows) if not p.startswith('modules/')]
    os.makedirs(out)
    index = ''
    reused = 0
    for p in kept:
        if not refresh_existing or p in REFRESH_PAYLOAD:
            dst = os.path.join(out, p)
            linked = previous is not None and link_previous(previous, p, rows[p], dst)
            if linked and sha256(dst) != rows[p][1]:
                os.remove(dst)   # 上一版的这个文件已不是它 index 里的字节：只丢本包的名字，改为复制，不动上一版
                linked = False
            if not linked:
                copy_file(os.path.join(pack, p), dst)   # CoW：同盘不复制数据块
                if sha256(dst) != rows[p][1]:
                    sys.exit(f'{p} does not match index.tsv')
            if linked:
                reused += 1
        index += f'{p}\t{rows[p][0]}\t{rows[p][1]}\n'
    if previous is not None and not refresh_existing:
        print(f'reused {reused} of {len(kept)} files from {previous[0]} (hard links)')
    with open(os.path.join(out, 'index.tsv'), 'w', newline='\n') as f:
        f.write(index)
    index_sha = hashlib.sha256(index.encode()).hexdigest()
    total = sum(rows[p][0] for p in kept)
    artifact['contentRoot'] = 'sha256-' + index_sha
    artifact['package'] = {'manifest': 'manifest.json', 'indexSha256': index_sha, 'files': len(kept), 'bytes': total}
    for m in artifact['modules']:
        m['source'] = 'apk-asset'
        for key in ('image', 'fixups', 'gates', 'widen'):
            if key in m:
                m[key] = m[key][len('modules/'):] if m[key].startswith('modules/') else m[key]
    artifact['apk'] = {'packageName': package, 'versionCode': version_code,
                       'sha256': sha256(apk), 'signingCertSha256': apk_signer_sha256(apk), 'assetRoot': ASSET_ROOT}
    artifact['toolchain'] = dict(artifact.get('toolchain', {}), packer='android/tools/game_apk.py@1')
    with open(os.path.join(out, 'game-build-artifact.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(artifact, f, ensure_ascii=False, indent=1)
        f.write('\n')
    with open(os.path.join(out, 'manifest.json.tmp'), 'w') as f:   # last: a manifest means a complete pack
        json.dump({'version': 2, 'files': len(kept), 'total': total, 'index_sha256': index_sha}, f, indent=1)
    os.replace(os.path.join(out, 'manifest.json.tmp'), os.path.join(out, 'manifest.json'))
    print(f'{name} {version} [{title_id}]: resource package for {artifact["apk"]["packageName"]} versionCode '
          f'{artifact["apk"]["versionCode"]}, {len(kept)} indexed files, '
          f'{"existing-content refresh only" if refresh_existing else "full resources"} -> {out}')


def main():
    args = sys.argv[1:]
    if args[:1] == ['--resources']:
        refresh = '--refresh-existing' in args
        if refresh:
            args.remove('--refresh-existing')
        reuse = None
        if '--reuse-from' in args:
            i = args.index('--reuse-from')
            reuse = args[i + 1] if i + 1 < len(args) else sys.exit('--reuse-from needs a directory')
            del args[i:i + 2]
        if len(args) != 4:
            sys.exit(__doc__)
        return resources(*args[1:], refresh_existing=refresh, reuse_from=reuse)
    version_code = 1
    if '--version-code' in args:
        i = args.index('--version-code')
        version_code = int(args[i + 1]) if i + 1 < len(args) and args[i + 1].isdigit() else sys.exit('--version-code needs a number')
        del args[i:i + 2]
    shader_pack = None
    if '--shader-pack' in args:
        i = args.index('--shader-pack')
        shader_pack = args[i + 1] if i + 1 < len(args) else sys.exit('--shader-pack needs a directory')
        del args[i:i + 2]
    if len(args) not in (1, 2) or version_code < 1:
        sys.exit(__doc__)
    sys.argv = [sys.argv[0]] + args
    pack = os.path.abspath(sys.argv[1])
    rows = read_index(pack)
    title_id, name, version = nacp_info(os.path.join(pack, 'ctl', 'control.nacp'))
    try:   # 桌面标签：游戏自带标题（简中 → 繁中 → 英文 → 其余）+ S2Arm；取不到报红，不退回程序号
        label = display_name.display_name(os.path.join(pack, 'ctl', 'control.nacp'))
    except display_name.NoTitle as e:
        sys.exit(f'【红】取不到游戏自带名字，桌面标签出不了：{e}')
    modules = sorted(p for p in rows if p.startswith('modules/'))
    if 'modules/main.patched.bin' not in modules:
        sys.exit(f'{pack} has no modules/main.patched.bin (build it with tools/make_pack.py)')
    for p in modules:
        size, digest = rows[p]
        src = os.path.join(pack, p)
        if not os.path.isfile(src) or os.path.getsize(src) != size or sha256(src) != digest:
            sys.exit(f'{p} does not match index.tsv')
    for p in modules:
        if p.endswith('.fixups.tsv'):
            with open(os.path.join(pack, p), encoding='utf-8') as f:
                if '# TARGET\tandroid\n' not in f.read():
                    sys.exit(f'{p} was not rewritten for Android (S2A_TARGET=android)')
    rendered_icon = render_game_icon(pack)
    out = os.path.abspath(sys.argv[2] if len(sys.argv) == 3 else os.path.join(REPO, 'build', 'android-game', title_id))
    if os.path.exists(out):
        if not os.path.isfile(os.path.join(out, 'game.properties')):
            sys.exit(f'{out} exists and is not a game_apk.py output; pick another directory')
        shutil.rmtree(out)
    assets = os.path.join(out, 'assets', 'game')
    os.makedirs(os.path.join(assets, 'modules'))
    for p in modules:
        shutil.copyfile(os.path.join(pack, p), os.path.join(assets, p))
    for rel, member in cold_guard_assets(rows):
        expected_size, expected_sha = rows[rel]
        with open(os.path.join(pack, rel), 'rb') as stream:
            data = stream.read(expected_size + 1)
        if len(data) != expected_size or hashlib.sha256(data).hexdigest() != expected_sha:
            raise ValueError(f'cold guard asset does not match index.tsv: {rel}')
        dest = os.path.join(out, member)
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'wb') as stream:
            stream.write(data)
    if shader_pack is not None:
        write_shader_zip(shader_pack, os.path.join(assets, 'shaders.zip'))
    for density, data in rendered_icon or ():
        directory = os.path.join(out, 'res', f'mipmap-{density}')
        os.makedirs(directory, exist_ok=True)
        with open(os.path.join(directory, 'game_icon.png'), 'wb') as f:
            f.write(data)
    props = (f'titleId={title_id}\nname={properties_value(name or title_id)}\nversion={properties_value(apk_version(version))}\n'
             f'versionCode={version_code}\n'
             f'launcherIcon={"game" if rendered_icon else "default"}\n'
             f'modules={",".join(p[len("modules/"):-len(".fixups.tsv")] for p in modules if p.endswith(".fixups.tsv"))}\n')
    for target in (os.path.join(out, 'game.properties'), os.path.join(assets, 'game.properties')):
        with open(target, 'w', encoding='ascii', newline='\n') as f:
            f.write(props)
    os.makedirs(os.path.join(out, 'res', 'values'))
    with open(os.path.join(out, 'res', 'values', 'game_name.xml'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(f'<?xml version="1.0" encoding="utf-8"?>\n<resources>\n'
                f'    <string name="game_name" translatable="false">{resource_string(label)}</string>\n</resources>\n')
    print(f'{name} {version} [{title_id}]: {len(modules)} module files -> {out}')


if __name__ == '__main__':
    try:
        main()
    except ValueError as error:
        sys.exit('【红】' + str(error))

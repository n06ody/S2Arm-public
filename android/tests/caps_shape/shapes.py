#!/usr/bin/env python3
# 辅助脚本：android/tests/caps_shape/run.sh 用它把设备档案出成假驱动的 C 头、核对真机设备档、核对生成的档案；随 run.sh 在 android/test.sh 里跑
"""整台设备能力形态档（#1749：不读厂商号，只看能力）。

形态 = 一份设备档案（s2a-device-profile-v1，#1749 §4.1）：默认读 android/tests/devprofiles/*.json（环境变量 S2A_DEVPROFILES 可换目录），
档名 = 文件名去掉 .json。现有三档：
  adreno-8xx-qcom = 骁龙 8 至尊版 / Adreno 830
  mali-g710-r32p1 = 天玑 9000 / Mali-G710 MC10
  minimal-1.1     = Vulkan 1.1 保守档（只留建设备必需的特性、不列任何设备扩展；限值 / 格式 / 内存取两台真机逐项更紧）
  lavapipe        = CI 的 Mesa 26.0.8，经 M0 采集；Linux 宿主不提供 Android 系统段
三份怎么来的见 android/tests/devprofiles/make_profiles.py。假驱动只读档案的 api / identity / vulkan 三段；
档案里写 null（来源没有读数）的项，假驱动按「无」回答（与原形态档同一口径）。

子命令：
  header <out.h>                    出各档的 C 头（假驱动 fake_vk.c 按 S2A_CAPS_SHAPE 选档）
  policy-header <out.h>             为 L0 保留原档的未知字段和系统事实
  names                             列档名
  oracle <档名> <设备打出的 caps JSON>  真机档：本构建在该形态下打出的 caps 和 android/tests/vk/profiles/devices/*.json 逐项比
  compare <档名> <生成的档案>          本构建在该形态下现查生成的设备档案和该档的档案文件逐字段比（期望 = 档案文件本身）
  variants <目录> / check-variant <名> <生成的档案>   版本门槛变体档（见 VARIANTS）
"""
import glob, json, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..', '..'))
sys.path.insert(0, os.path.join(REPO, 'tools'))
import devprofile_lint as lint   # noqa: E402

PROFILES = os.environ.get('S2A_DEVPROFILES') or os.path.join(REPO, 'android', 'tests', 'devprofiles')
DEVICES = os.path.join(REPO, 'android', 'tests', 'vk', 'profiles', 'devices')
VK_CORE = os.path.join(REPO, 'graphics', 'nvn', 'third_party', 'vulkan-headers', 'include', 'vulkan', 'vulkan_core.h')
REAL = {'adreno-8xx-qcom': '8e', 'mali-g710-r32p1': '9000'}   # 有应用直连层真机 caps 读数的档
# 实例扩展不是设备能力：各档一样（假驱动给全）
INSTANCE_EXTS = ('VK_KHR_get_physical_device_properties2', 'VK_KHR_surface', 'VK_KHR_android_surface',
                 'VK_KHR_get_surface_capabilities2', 'VK_EXT_surface_maintenance1')
# 假驱动答的扩展特性位（caps_shape.h 的同名字段，di* = 描述符索引支持位）：档案 vulkan.features 的键 → C 字段
EXT_FEATURES = (('shaderDrawParameters', 'shaderDrawParameters'), ('timelineSemaphore', 'timelineSemaphore'),
                ('indexTypeUint8', 'indexTypeUint8'), ('customBorderColors', 'customBorderColors'),
                ('customBorderColorWithoutFormat', 'customBorderColorWithoutFormat'),
                ('vertexAttributeInstanceRateDivisor', 'vertexAttributeInstanceRateDivisor'),
                ('vertexAttributeInstanceRateZeroDivisor', 'vertexAttributeInstanceRateZeroDivisor'),
                ('formatA4B4G4R4', 'formatA4B4G4R4'), ('formatA4R4G4B4', 'formatA4R4G4B4'),
                ('depthClipControl', 'depthClipControl'), ('swapchainMaintenance1', 'swapchainMaintenance1'),
                ('runtimeDescriptorArray', 'diRuntimeArray'), ('descriptorBindingPartiallyBound', 'diPartiallyBound'),
                ('shaderSampledImageArrayNonUniformIndexing', 'diSampledNonUniform'),
                ('shaderStorageImageArrayNonUniformIndexing', 'diStorageNonUniform'),
                ('descriptorBindingVariableDescriptorCount', 'diVariableCount'),
                ('descriptorBindingSampledImageUpdateAfterBind', 'diSampledUpdateAfterBind'))
DYNAMIC_FEATURES = {
    'VK_EXT_extended_dynamic_state': ('extendedDynamicState',),
    'VK_EXT_extended_dynamic_state2': ('extendedDynamicState2', 'extendedDynamicState2LogicOp',
                                      'extendedDynamicState2PatchControlPoints'),
    'VK_EXT_vertex_input_dynamic_state': ('vertexInputDynamicState',),
}
EXT_FEATURES += tuple((field, field) for fields in DYNAMIC_FEATURES.values() for field in fields)
EXT_PROPERTIES = (('subgroupQuadOperationsInAllStages', 'subgroup_quad_all'),
                  ('maxCustomBorderColorSamplers', 'border_max'), ('maxVertexAttribDivisor', 'divisor_max'),
                  ('filterMinmaxSingleComponentFormats', 'minmax_single'), ('filterMinmaxImageComponentMapping', 'minmax_mapping'),
                  ('minImportedHostPointerAlignment', 'host_align'))


def names():
    return sorted(os.path.basename(p)[:-5] for p in glob.glob(os.path.join(PROFILES, '*.json')))


def load(name):
    p = json.load(open(os.path.join(PROFILES, name + '.json'), encoding='utf-8'))
    bad = lint.check(p)
    if bad: sys.exit('【红】形态档 %s 的档案不合格（tools/devprofile_lint.py）：\n  %s' % (name, '\n  '.join(bad)))
    return p


def c_ident(name): return re.sub(r'[^A-Za-z0-9]', '_', name)
def c_str(s): return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
def c_val(v):
    if isinstance(v, list): return '{' + ', '.join(c_val(x) for x in v) + '}'
    if isinstance(v, float): return repr(v) + 'f'
    return str(v) + ('ull' if isinstance(v, int) and v > 0x7fffffff else '')
def z(v, d=0): return d if v is None else v   # null（没有读数）= 按「无」/ 原假驱动的固定值回答


def api_num(s):
    a, b, c = (int(x) for x in s.split('.'))
    return (a << 22) | (b << 12) | c


def header(out):
    fmt_num = {n: int(v) for n, v in re.findall(r'\n\s+(VK_FORMAT_[A-Za-z0-9_]+) = (\d+),', open(VK_CORE, encoding='utf-8').read())}
    lines = ['// 生成文件（android/tests/caps_shape/shapes.py header，读 %s），不进仓库' % os.path.relpath(PROFILES, REPO), '#pragma once']
    shapes = names()
    if not shapes: sys.exit('【红】%s 下没有设备档案' % PROFILES)
    core = None
    for name in shapes:
        p = load(name); v = p['vulkan']; i = p['identity']; pr = v['properties']; f = v['features']; cid = c_ident(name)
        core = core or sorted(lint.names()[1])
        ext = sorted(v['extensions'])
        fmts = sorted((fmt_num[k], x) for k, x in v['formats'].items() if x is not None)
        uuid = bytes.fromhex(i['pipelineCacheUUID']) if i['pipelineCacheUUID'] else bytes(16)
        lines.append('static const char *const cs_ext_%s[] = { %s };' % (cid, ', '.join(c_str(e) for e in ext) or 'NULL'))
        lines.append('static const struct cs_fmt cs_fmt_%s[] = { %s };' % (cid, ', '.join('{%d, %du, %du, %du}' % (n, *x) for n, x in fmts)))
        lines.append('static const struct cs_shape cs_%s = {' % cid)
        lines.append('  .name = %s, .n_ext = %d, .ext = %s, .n_fmt = %d, .fmt = cs_fmt_%s,' % (
            c_str(name), len(ext), 'cs_ext_' + cid if ext else 'NULL', len(fmts), cid))
        lines.append('  .props = { .apiVersion = %du, .driverVersion = %du, .vendorID = %du, .deviceID = %du, .deviceType = %d, .deviceName = %s,' % (
            api_num(p['api']['version']), i['driverVersion'], int(i['vendorId'], 16), int(i['deviceId'], 16), pr['deviceType'], c_str(i['deviceName'])))
        lines.append('    .pipelineCacheUUID = {%s},' % ', '.join(str(b) for b in uuid))
        lines.append('    .limits = { ' + ', '.join('.%s = %s' % (k, c_val(x)) for k, x in sorted(pr['limits'].items())) + ' } },')
        lines.append('  .features = { ' + ', '.join('.%s = %d' % (k, z(f.get(k))) for k in core) + ' },')
        lines.append('  ' + ', '.join('.%s = %d' % (c, z(f.get(k))) for k, c in EXT_FEATURES) + ',')
        lines.append('  .driver_id = %d, .driver_name = %s, .max_alloc = %dull, .max_per_set = %d, .subgroup_size = %d,' % (
            z(i['driverId']), c_str(z(i['driverName'], '')), z(pr.get('maxMemoryAllocationSize')), z(pr.get('maxPerSetDescriptors'), 1024),
            z(pr.get('subgroupSize'))))
        lines.append('  .subgroup_stages = %du, .subgroup_ops = %du,' % (z(pr.get('subgroupSupportedStages')), z(pr.get('subgroupSupportedOperations'))))
        lines.append('  ' + ', '.join('.%s = %s' % (field, c_val(z(pr.get(k)))) for k, field in EXT_PROPERTIES) + ',')
        mem = v['memory']
        lines.append('  .n_heap = %d, .heaps = { %s }, .n_type = %d, .types = { %s },' % (
            len(mem['heaps']), ', '.join('{%dull, %d}' % (h['size'], h['flags']) for h in mem['heaps']),
            len(mem['types']), ', '.join('{%d, %d}' % (t['flags'], t['heap']) for t in mem['types'])))
        lines.append('  .n_queue = %d, .queues = { %s },' % (len(v['queues']), ', '.join(
            '{%d, %d, %d}' % (q['flags'], q['count'], z(q['timestampValidBits'], 64)) for q in v['queues'])))
        lines.append('};')
    lines.append('static const struct cs_shape *const cs_all[] = { %s };' % ', '.join('&cs_' + c_ident(n) for n in shapes))
    lines.append('static const char *cs_feature_name(size_t i) {')
    lines.append('    static const struct { size_t i; const char *n; } t[] = { %s };' % ', '.join(
        '{offsetof(VkPhysicalDeviceFeatures, %s) / sizeof(VkBool32), "%s"}' % (k, k) for k in core))
    lines.append('    for (size_t k = 0; k < sizeof t / sizeof *t; k++) if (t[k].i == i) return t[k].n;')
    lines.append('    return "?";')
    lines.append('}')
    lines.append('static const char *const cs_instance_ext[] = { %s };' % ', '.join(c_str(e) for e in INSTANCE_EXTS))
    open(out, 'w').write('\n'.join(lines) + '\n')


def policy_header(out):
    # 建设备的替身必须对 Vulkan 查询返回 0 或 1。策略 L0 另保留来源里的 null，
    # 并使用档案系统段，而不是把构建宿主的页大小或核数当成设备事实。
    lines = ['static const struct policy_archive policy_archives[] = {']
    for name in names():
        p = load(name)
        s, v = p['system'], p['vulkan']
        features = v['features']
        unknown_formats = [fmt for fmt, flags in v['formats'].items() if flags is None]
        formats = ', '.join([*unknown_formats, 'VK_FORMAT_UNDEFINED'])
        lines.append('    { .name = %s, .ext_unknown = %d, .index_known = %d, .border_known = %d, .packed_known = %d,' % (
            c_str(name), 'extensionsWhy' in v or -1 in v['extensions'].values(), features.get('indexTypeUint8') is not None,
            features.get('customBorderColors') is not None, features.get('formatA4B4G4R4') is not None))
        lines.append('      .page_size = %d, .kgsl = %d, .adpf = %d, .vsync = %d,' % (
            z(s['pageSize'], -1), z(s['kgsl'], -1), z(s['adpf'], -1), z(s['vsyncApi'], -1)))
        lines.append('      .unknown_formats = (const VkFormat[]){%s} },' % formats)
    lines.append('};')
    with open(out, 'w', encoding='utf-8') as output:
        output.write('\n'.join(lines) + '\n')


def caps_format_groups():
    """device_caps.c 每个 formats 键由哪些 VkFormat 决定（读源码里的表，不另抄）。"""
    src = open(os.path.join(REPO, 'android', 'native', 'direct', 'device_caps.c'), encoding='utf-8').read()
    key = {'bc123': 'bc123', 'bc45': 'bc45', 'bc67': 'bc67', 'etc2': 'etc2', 'astc': 'astc', 'five_bit': 'fiveBit', 'r4g4b4a4': 'r4g4b4a4'}
    out = {key[n]: set(re.findall(r'VK_FORMAT_\w+', body)) for n, body in re.findall(r'static const VkFormat k_(\w+)\[\] = \{(.*?)\};', src, re.S)}
    out['scaledVertex'] = set(re.findall(r'VK_FORMAT_\w+', src[src.index('k_scaled_pairs[]'):src.index('static int scaled_vertex_ok')]))
    if set(out) != set(key.values()) | {'scaledVertex'} or not all(out.values()):
        sys.exit('【红】shapes.py：device_caps.c 的格式族表解析不全：%r' % sorted(out))
    return out


def oracle(name, got_path):
    stem = REAL[name]
    want = json.load(open(os.path.join(DEVICES, stem + '.json')))
    got = json.load(open(got_path))
    # 档案里写 null（来源没读数）的格式，假驱动按「能力未知 = 按不支持」回答；device_caps.c 里由这些格式决定的格式族因此不比，点名列出
    null_fmts = {k for k, v in load(name)['vulkan']['formats'].items() if v is None}
    unknown = {('formats', group) for group, fmts in caps_format_groups().items() if fmts & null_fmts}
    bad = []
    for sect in ('limits', 'features', 'formats', 'memory'):
        for k, v in want[sect].items():
            if sect == 'features' and k in ('geometryShader', 'geometryShaderOcclusionQuery'): continue   # capprobe 判定，不是能力形态
            if (sect, k) in unknown: continue
            g = got.get(sect, {}).get(k)
            if (abs(g - v) > 1e-6 if isinstance(v, float) else g != v): bad.append('%s.%s：真机 %r / 本构建 %r' % (sect, k, v, g))
    for k in ('vendorId', 'deviceId', 'deviceName', 'driverName', 'subgroupSize'):
        if got.get(k) != want.get(k): bad.append('%s：真机 %r / 本构建 %r' % (k, want.get(k), got.get(k)))
    if bad:
        print('FAIL: %s 档打出的设备 caps 和真机档 devices/%s.json 不一致（%d 处）：\n  %s' % (name, stem, len(bad), '\n  '.join(bad)))
        return 1
    n = sum(len(want[s]) for s in ('limits', 'features', 'formats', 'memory')) - 2 - len(unknown) + 5
    print('PASS: %s 档打出的设备 caps 与真机档 devices/%s.json 一致（比了 %d 项%s）' % (
        name, stem, n, '；档案未知按不支持、不比：' + '、'.join('%s.%s' % u for u in sorted(unknown)) if unknown else ''))
    return 0


def leaves(v, path=''):
    if isinstance(v, dict):
        for k in v: yield from leaves(v[k], path + '.' + k)
    elif isinstance(v, list) and any(isinstance(x, (dict, list)) for x in v):
        for i, x in enumerate(v): yield from leaves(x, '%s[%d]' % (path, i))
    else:
        yield path, v


def compare(name, got_path):
    """生成的档案 vs 档案文件：api / identity / vulkan 三段逐叶子比；文件里写 null 的只核这项生成档里有、不比值（来源没读数，假驱动按「无」答）。
    model / system 是宿主自己的读数（假驱动管不到），不比；两份都要过 lint（各自的 factsDigest 自洽）。"""
    want = load(name)
    got = json.load(open(got_path, encoding='utf-8'))
    bad = ['生成的档案不合格：' + b for b in lint.check(got)]
    if (got.get('provenance'), got.get('completeness')) != ('probed', 'complete'):
        bad.append('生成的档案元数据应为 probed + complete，实为 %r + %r' % (got.get('provenance'), got.get('completeness')))
    # Historical input may list an extension without its feature fields. The fake
    # driver answers 0 for an unspecified bit; an actual archive stays unchanged.
    if api_num(want['api']['version']) >= api_num('1.1.0'):
        vk = want['vulkan']
        for ext, fields in DYNAMIC_FEATURES.items():
            if ext in vk['extensions']:
                for field in fields:
                    vk['features'].setdefault(field, 0)
    w = dict(leaves({k: want[k] for k in ('api', 'identity', 'vulkan')}))
    g = dict(leaves({k: got.get(k) for k in ('api', 'identity', 'vulkan')}))
    skipped = 0
    for k, v in w.items():
        if k not in g: bad.append('%s：档案 %r / 生成的没有这项' % (k, v)); continue   # null 也算：值不知道，但这项该现查到
        if v is None: skipped += 1; continue
        if g[k] != v: bad.append('%s：档案 %r / 生成 %r' % (k, v, g[k]))
    extra = sorted(k for k in g if k not in w)
    bad += ['%s：生成的多出这项（%r），档案里没有' % (k, g[k]) for k in extra]
    if bad:
        print('FAIL: [%s] 生成的设备档案和 devprofiles/%s.json 不一致（%d 处）：\n  %s' % (name, name, len(bad), '\n  '.join(bad[:40])))
        return 1
    print('PASS: [%s] 生成的设备档案与 devprofiles/%s.json 逐字段一致（比 %d 项，档案标未知跳过 %d 项；生成档 %s 过 lint）'
          % (name, name, len(w) - skipped, skipped, got['factsDigest'][:23]))
    return 0


# 版本门槛变体（dev_facts.c：timeline / 描述符索引「列了扩展或 ≥ 1.2」、有效版本 = min(应用申请的 1.1, 设备)：不列扩展时 1.2 / 1.3 转正的都不用、4444 特性位只在列了扩展时记、低于 1.1 整条扩展查询链不挂）：
#   由 adreno 档派生，去掉这三个已转正扩展再改设备版本；期望只由变体自己的版本号决定
PROMOTED = ('VK_KHR_timeline_semaphore', 'VK_EXT_descriptor_indexing', 'VK_EXT_4444_formats')
VARIANTS = {'v-api10': '1.0.0', 'v-api11-noext': '1.1.0', 'v-api12-noext': '1.2.0', 'v-api13-noext': '1.3.0', 'v-khr-alias': '1.3.0', 'v-eds-bits': '1.3.0'}
# KHR 别名正控：只列 KHR 版的 index_type_uint8 / swapchain_maintenance1（同一个特性结构），照样挂、照样记，假驱动不判红
KHR_ALIAS = {'VK_EXT_index_type_uint8': 'VK_KHR_index_type_uint8', 'VK_EXT_swapchain_maintenance1': 'VK_KHR_swapchain_maintenance1'}
# Synthetic feature mix, not a measurement for the archive from which it is generated.
EDS_VARIANT = {'extendedDynamicState': 1, 'extendedDynamicState2': 1, 'extendedDynamicState2LogicOp': 0,
               'extendedDynamicState2PatchControlPoints': 1, 'vertexInputDynamicState': 1}


def variants(out):
    base = load('adreno-8xx-qcom')
    os.makedirs(out, exist_ok=True)
    for name, ver in VARIANTS.items():
        p = json.loads(json.dumps(base))
        p['api']['version'] = ver
        if name == 'v-khr-alias':
            ex = p['vulkan']['extensions']
            for e, k in KHR_ALIAS.items():
                assert e in ex, '基准档没列 ' + e
                ex[k] = ex.pop(e)
            p['vulkan']['extensions'] = dict(sorted(ex.items()))
        elif name == 'v-eds-bits':
            assert all(ext in p['vulkan']['extensions'] for ext in DYNAMIC_FEATURES)
            p['vulkan']['features'].update(EDS_VARIANT)
        else:
            for e in PROMOTED: p['vulkan']['extensions'].pop(e, None)
        p['factsDigest'] = lint.digest(p)
        open(os.path.join(out, name + '.json'), 'w', encoding='utf-8').write(lint.dumps_pretty(p))


def check_variant(name, got_path):
    got = json.load(open(got_path, encoding='utf-8'))
    minor = int(VARIANTS[name].split('.')[1])
    f, fm, pr = got['vulkan']['features'], got['vulkan']['formats'], got['vulkan']['properties']
    if name == 'v-eds-bits':
        bad = ['%s：入参 %d，档案 %r' % (k, v, f.get(k)) for k, v in EDS_VARIANT.items() if f.get(k) != v]
        bad += ['生成的档案不合格：' + b for b in lint.check(got)]
        if bad:
            print('FAIL: [%s] EDS 能力回读：\n  %s' % (name, '\n  '.join(bad))); return 1
        print('PASS: [%s] EDS 能力回读 5/5，含真假位' % name); return 0
    if name == 'v-khr-alias':
        # 逐值对照：生成档里这两项必须等于基准档（adreno）的输入值；只查键在不在的话，回填成 0 也会过（#1879 复审）
        base = load('adreno-8xx-qcom')['vulkan']['features']
        bad = ['只列 KHR 别名时 %s：输入 %r / 生成 %r' % (k, base.get(k), f.get(k))
               for k in ('indexTypeUint8', 'swapchainMaintenance1') if k not in base or f.get(k) != base[k]]
        bad += ['生成的档案不合格：' + b for b in lint.check(got)]
        if bad:
            print('FAIL: [%s] KHR 别名正控：\n  %s' % (name, '\n  '.join(bad))); return 1
        print('PASS: [%s] 只列 KHR 版 index_type_uint8 / swapchain_maintenance1：照样挂结构、记特性，假驱动放行' % name); return 0
    # 直连实例只申请 1.1（S2A_VK_APP_API_VERSION）：有效版本 = min(1.1, 设备)。变体都不列这三个已转正扩展，
    #   1.2 / 1.3 转正的东西（timeline、描述符索引、A4B4G4R4 格式）一概不能用，不论设备版本多高；4444 特性结构没转正，更不能挂
    want = {'timelineSemaphore 记了': False, 'runtimeDescriptorArray 记了': False,
            'formatA4B4G4R4 记了': False, 'A4B4G4R4 格式查了': False,
            'shaderDrawParameters 记了': minor >= 1, 'maxMemoryAllocationSize 记了': minor >= 1,
            '扩展特性只在 ≥ 1.1 记（swapchainMaintenance1）': minor >= 1,
            '动态状态扩展特性只在 ≥ 1.1 记': minor >= 1}
    have = {'timelineSemaphore 记了': 'timelineSemaphore' in f, 'runtimeDescriptorArray 记了': 'runtimeDescriptorArray' in f,
            'formatA4B4G4R4 记了': 'formatA4B4G4R4' in f, 'A4B4G4R4 格式查了': 'VK_FORMAT_A4B4G4R4_UNORM_PACK16' in fm,
            'shaderDrawParameters 记了': 'shaderDrawParameters' in f, 'maxMemoryAllocationSize 记了': 'maxMemoryAllocationSize' in pr,
            '扩展特性只在 ≥ 1.1 记（swapchainMaintenance1）': 'swapchainMaintenance1' in f,
            '动态状态扩展特性只在 ≥ 1.1 记': all(k in f for k in EDS_VARIANT)}
    bad = ['%s：期望 %s / 实为 %s' % (k, want[k], have[k]) for k in want if want[k] != have[k]]
    bad += ['生成的档案不合格：' + b for b in lint.check(got)]
    if bad:
        print('FAIL: [%s] 版本门槛不对：\n  %s' % (name, '\n  '.join(bad))); return 1
    print('PASS: [%s] 设备 %s、不列三个已转正扩展：%d 项版本门槛都对' % (name, VARIANTS[name], len(want)))
    return 0


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else ''
    if cmd == 'header' and len(sys.argv) == 3: header(sys.argv[2])
    elif cmd == 'policy-header' and len(sys.argv) == 3: policy_header(sys.argv[2])
    elif cmd == 'names': print(' '.join(names()))
    elif cmd == 'oracle' and len(sys.argv) == 4: sys.exit(oracle(sys.argv[2], sys.argv[3]))
    elif cmd == 'compare' and len(sys.argv) == 4: sys.exit(compare(sys.argv[2], sys.argv[3]))
    elif cmd == 'variants' and len(sys.argv) == 3: variants(sys.argv[2])
    elif cmd == 'check-variant' and len(sys.argv) == 4: sys.exit(check_variant(sys.argv[2], sys.argv[3]))
    else: sys.exit(__doc__)

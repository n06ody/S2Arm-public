#!/usr/bin/env python3
"""Direct (NVN -> Vulkan) offline program library: build it from captured device results and
verify it before it is packed, so the installed revision only ever carries programs whose
identity, bytes and file list agree.

    direct_library.py build  --capture DIR --out DIR --descriptor FILE --title-id TITLE_ID
                             (--index PACK/index.tsv | --game-content-root H --module-set-digest H)
                             [--direct-abi N] [--profile-hash H] [--translator-closure S]
    direct_library.py verify --lib DIR [--descriptor FILE] [--index PACK/index.tsv]
    direct_library.py verify --pack DIR

--index is the index.tsv of the pack that will carry this library (the two-pass flow: build the
base pack first, derive the identities from its rows, then rebuild it with the descriptor). An
installed revision's index has no modules/ rows, so it cannot supply moduleSetDigest.

Layout (contract: android/docs/direct-mode-contracts.md §8, fast-generic-v3 §4.1):

    shaders/vulkan/direct/graphics.v1.json                  descriptor (written by this tool)
    shaders/vulkan/direct/library.v1.json                    library index (this tool)
    shaders/vulkan/direct/<directABI>/<profileHash>/<identity>.program
                                                             one program per full S2AI1 v3 identity

<identity> is the 64 lowercase hex of the runtime's S2AI1 v3 content identity (the same value
the device hashes in nvn_vk_draw.c and uses for its private cache file name). The .program
file is the *same S2APC1 v2 container* the runtime's private cache uses
(native/direct/program_cache.c: 88-byte header = magic/version/header/identity/result
SHA-256/payload length, then the s2a-direct-program-v1 bytes). That is deliberate: the future
read-only consumer can call s2a_program_cache_load(<lib>/<abi>/<profileHash>, identity, ...)
unchanged, so an entry that passes this tool's checks is exactly what the runtime already
validates (identity match, payload digest, length, not one byte of a half-written file).

Why the identity comes from the capture and not from the request: the device hashes the
*canonical full input* (input_identity.c: profile/ABI/closure, full source digests over
control || complete guest code, texture facts, graphics facts), while a compile request has
already stripped the 0x30-byte NVN graphics header (nvn_vk_draw.c). A host therefore cannot
recompute the device identity from a request alone. The capture side must export it:

    # suggested dump change (native/nvn/nvn_vk_draw.c, A-line owns that file; NOT applied here).
    # nvk_program_compile_inner already has entry->input_sha (filled in nvk_program_lookup):
    char idhex[65];
    for (int i = 0; i < 32; i++) snprintf(idhex + 2 * i, 3, "%02x", entry->input_sha[i]);
    // name BOTH dumps with it: "%s/%s-%s.compile-request.json" and "%s/%s-%s.program.json"
    // (ident, idhex). Request and result then pair by the identity suffix, and two content
    // variants with the same stage-key group can no longer overwrite one another.

The identity is then copied into capture.v1.json below.

Capture directory (one per device/profile capture):

    capture.v1.json
      {"schema": "s2a-direct-capture-v1", "directABI": 1,
       "translatorClosure": "...", "sourceParserVersion": 1,
       "entries": [{"identity": "<64 hex>", "request": "r1.compile-request.json",
                    "result": "r1.program.json"}]}

The manifest lists every pair explicitly; file names are never guessed. `identity` is the
value exported by the device. This tool cannot recompute it, so what it *does* verify is
everything else around it: request/result schema and identity fields, the result envelope's
contentHash over the exact embedded content bytes, stage set/order, per-stage SPIR-V digest,
support-layout hash, constantBufferUse, and single-profile consistency across entries. A
captured identity that does not describe its payload can only produce a miss or a wrong
program, exactly like a corrupted private-cache entry; closing that last gap needs the
canonical identity preimage (or option (b), entry-carried requests), which is future work.

Verification rules (all failures are hard errors, never a warning):

  * every descriptor `files[]` entry is a relative path under shaders/vulkan/direct/ with
    path/size/full SHA-256; duplicates, traversal, absolute paths and unknown keys are
    rejected;
  * every listed file exists under the direct directory with the recorded size and SHA-256,
    nothing else exists there (only the fixed graphics.v1.json location is exempt);
  * library.v1.json is itself listed in the descriptor and agrees with it field by field;
  * every .program file carries the S2APC1 v2 header for its own identity, its payload digest
    matches the header and the index, and the payload is a valid s2a-direct-program-v1
    envelope (contentHash, content identity, stage set, SPIR-V magic/alignment/digest);
  * when a pack index is supplied, listed files must also be rows of that index with the same
    size/SHA-256, gameContentRoot/moduleSetDigest must be the ones derived from that index,
    and the artifact extension must point at the descriptor with matching bytes/SHA-256.

gameContentRoot keeps the runtime definition (ShaderPackLayout.GameContentRoot over the
installed revision's index). game_apk.py drops modules/ rows and writes the resource index
sorted, so from a make_pack index the same value is "drop modules/ and shaders/, sort, hash".
moduleSetDigest is defined here as the canonical row encoding over modules/ rows; that value
only feeds the direct attempt key today. `verify_pack` (and therefore game_apk.py /
attach_shaders.py / `verify --pack`) recomputes these from the real index instead of trusting
a hand-written value; make_pack.py checks the file list and bytes only, because its index does
not exist yet when the descriptor is read.

No game data, keys or payloads are produced by the tests; captures are the player's own.
"""
import argparse
import base64
import binascii
import functools
import hashlib
import json
import os
import re
import shutil
import struct
import sys

DIRECT_PREFIX = 'shaders/vulkan/direct/'
DESCRIPTOR_REL = DIRECT_PREFIX + 'graphics.v1.json'
DESCRIPTOR_BASENAME = 'graphics.v1.json'
LIBRARY_INDEX_REL = 'library.v1.json'

GRAPHICS_SCHEMA = 's2a-graphics-v1'
CAPTURE_SCHEMA = 's2a-direct-capture-v1'
LIBRARY_SCHEMA = 's2a-direct-library-v1'
REQUEST_SCHEMA = 's2a-direct-compile-v1'
# v2 request: graphics only, program.stages is the five NVN slots [vs, fs, gs, tcs, tes] with null for an
# unbound slot (DirectShaderCompiler.GraphicsRequestSchemaV2 / DirectProgram.Parse); it adds vs+gs+fs.
REQUEST_SCHEMA_V2 = 's2a-direct-compile-v2'
V2_SLOTS = ('vs', 'fs', 'gs', 'tcs', 'tes')
RESULT_SCHEMA = 's2a-direct-program-v1'
CONTENT_CONTRACT = 's2a-direct-program-content-v1'
RUNTIME_COMPILER_ABI = 's2a-direct-compile-v1'
FRONTEND = 'nvn'
BACKEND = 'vulkan-direct'
GRAPHICS_FEATURE = 'org.s2arm.graphics.v1'

# native/direct/program_cache.c: the private-cache container, reused verbatim for the library.
PCACHE_MAGIC = b'S2APC1\n\x00'
PCACHE_VERSION = 2
PCACHE_HEADER = 88
PCACHE_MAX_PAYLOAD = 64 * 1024 * 1024

MAX_DESCRIPTOR = 1 << 20
MAX_REQUEST = 256 << 20
# index.tsv 不设读取上限：它就是这款游戏的文件清单，大小由 RomFS 文件数决定（0100F2C0115B6000 304243 个文件 → 41,884,008 B）。
#   原先默认 32 MiB 上限、要设环境变量才放开：超大型 RomFS 不设变量就永远出不了包，属于自设的拦截；
#   运行时侧本来就不设上限（GamePack.java 的 read() 不带 max）。11-14 去掉，两边同口径
MAX_CAPTURE_MANIFEST = 1 << 20
MAX_LIBRARY_INDEX = 16 << 20

HEX64 = re.compile(r'[0-9a-f]{64}\Z')
CONTENT_ROOT = re.compile(r'sha256-[0-9a-f]{64}\Z')
CLOSURE = re.compile(r'[A-Za-z0-9._+:-]{1,127}\Z')
TITLE_ID = re.compile(r'[0-9A-Fa-f]{16}\Z')
REQUEST_ID = re.compile(r'(?:[0-9a-f]{16})+\Z')
SOURCE_REF = re.compile(r'(?:code|control|c1):sha256:[0-9a-f]{64}\Z')
SPIRV_MAGIC = b'\x03\x02\x23\x07'

# Same stage names as the compiler's serialization; the first product only compiles these.
GRAPHICS_STAGES = ('vs', 'fs')
COMPUTE_STAGES = ('cs',)
# Native geometry (v2 request only): the compiler writes the stages in pipeline order vs, gs, fs.
GEOMETRY_STAGES = ('vs', 'gs', 'fs')

REQUEST_KEYS = frozenset(('request', 'schema', 'session', 'generation', 'requestId',
                          'directABI', 'translatorClosure', 'runtimeCompilerABI',
                          'sourceParserVersion', 'kindCheck', 'debugMode', 'profile', 'program'))
CAPTURE_KEYS = frozenset(('schema', 'directABI', 'translatorClosure', 'sourceParserVersion',
                          'profileHash', 'entries'))


class LibraryError(Exception):
    """A capture, library or descriptor that must not be packed."""


def _fail(message):
    raise LibraryError(message)


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _read_file(path, what, limit, hint=''):
    """limit=None：整份读，不设上限"""
    try:
        with open(path, 'rb') as stream:
            data = stream.read() if limit is None else stream.read(limit + 1)
    except OSError as error:
        _fail(f'{what}: {path}: {error.strerror or error}')
    if limit is not None and len(data) > limit:
        _fail(f'{what}: {path}: larger than {limit} bytes{hint}')
    return data


def _read_json_file(path, what, limit):
    return parse_json(_read_file(path, what, limit), f'{what}: {path}')


def _decode_utf8(raw, what):
    try:
        return raw.decode('utf-8')
    except UnicodeDecodeError as error:
        _fail(f'{what}: not strict UTF-8 ({error})')


def parse_json(raw, what):
    """Strict JSON: UTF-8, no duplicate keys, no non-finite numbers."""
    text = _decode_utf8(raw, what)

    def pairs(items):
        out = {}
        for key, value in items:
            if key in out:
                _fail(f'{what}: duplicate key {key!r}')
            out[key] = value
        return out

    def constant(name):
        _fail(f'{what}: non-finite number {name}')

    try:
        return json.loads(text, object_pairs_hook=pairs, parse_constant=constant)
    except LibraryError:
        raise
    except json.JSONDecodeError as error:
        _fail(f'{what}: invalid JSON ({error})')


def _object(value, what):
    if not isinstance(value, dict):
        _fail(f'{what}: must be an object')
    return value


def _array(value, what):
    if not isinstance(value, list):
        _fail(f'{what}: must be an array')
    return value


def _string(value, what, limit=None, pattern=None, allow_empty=False):
    if not isinstance(value, str):
        _fail(f'{what}: must be a string')
    if not allow_empty and not value:
        _fail(f'{what}: must not be empty')
    if limit is not None and len(value) >= limit:
        _fail(f'{what}: longer than {limit - 1} bytes')
    if pattern is not None and not pattern.fullmatch(value):
        _fail(f'{what}: has the wrong shape')
    return value


def _integer(value, what, minimum=0, maximum=None):
    if not isinstance(value, int) or isinstance(value, bool):
        _fail(f'{what}: must be an integer')
    if value < minimum or (maximum is not None and value > maximum):
        _fail(f'{what}: outside {minimum}..{maximum if maximum is not None else "inf"}')
    return value


def _sha_text(value, what):
    """Accepts bare 64 lowercase hex or the sha256-<hex> spelling; returns bare hex."""
    text = _string(value, what)
    if text.startswith('sha256-'):
        text = text[len('sha256-'):]
    if not HEX64.fullmatch(text):
        _fail(f'{what}: must be 64 lowercase hex (optionally sha256- prefixed)')
    return text


def _content_root(value, what):
    return _string(value, what, pattern=CONTENT_ROOT)


def _relative_path(value, what):
    """A revision-relative path with no traversal, no absolute form, ASCII only."""
    text = _string(value, what, allow_empty=False)
    if not text.isascii() or '\\' in text or '\t' in text or '\n' in text or '\x00' in text:
        _fail(f'{what}: {text!r} is not a plain ASCII relative path')
    if text.startswith('/') or ':' in text.split('/')[0]:
        _fail(f'{what}: {text!r} must be relative')
    parts = text.split('/')
    if any(part in ('', '.', '..') for part in parts):
        _fail(f'{what}: {text!r} is not a normalized path')
    return text


def _direct_relative(value, what):
    path = _relative_path(value, what)
    if not path.startswith(DIRECT_PREFIX):
        _fail(f'{what}: {path!r} is not under {DIRECT_PREFIX}')
    return path[len(DIRECT_PREFIX):]


# ---------------------------------------------------------------------------------------
# Descriptor (graphics.v1.json)
# ---------------------------------------------------------------------------------------

def spirv_requires_storage_feature(data, expected_model=None):
    """Conservative final-module classification; a missing proof is a requirement.

    SPIR-V NonWritable is a variable/type/member decoration, not a reflection
    hint. Every atomic (including atomic load) is a requirement by contract.
    """
    if len(data) < 20 or len(data) % 4:
        raise ValueError('truncated SPIR-V')
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    if words[0] != 0x07230203 or words[3] == 0 or words[3] > 1 << 20:
        raise ValueError('invalid SPIR-V header')
    types, decorations, members, variables, origins, stores = {}, {}, {}, [], {}, []
    def valid_id(value):
        if not 0 < value < words[3]: raise ValueError('SPIR-V id outside bound')
        return value
    pos = 5
    entry = False
    while pos < len(words):
        count, code = words[pos] >> 16, words[pos] & 0xffff
        if not count or pos + count > len(words):
            raise ValueError('invalid SPIR-V instruction length')
        a = words[pos + 1:pos + count]
        if code == 15 and len(a) >= 4 and a[2] == 0x6e69616d and (a[3] & 255) == 0:
            if expected_model is None or a[0] == expected_model:
                entry = True
        if 227 <= code <= 242 or code in (318, 319, 5614, 5615, 6035, 99):
            return True
        if code in (25, 26, 27, 28, 29, 30, 32) and len(a) >= 1:
            valid_id(a[0])
            types[a[0]] = (code, a[1:])
        elif code == 71 and len(a) >= 2:
            valid_id(a[0])
            decorations.setdefault(a[0], set()).add(a[1])
        elif code == 72 and len(a) >= 3:
            valid_id(a[0])
            members.setdefault((a[0], a[1]), set()).add(a[2])
        elif code == 59 and len(a) >= 3:
            valid_id(a[0]); valid_id(a[1])
            variables.append((a[0], a[1], a[2]))
        elif code in (65, 66, 67, 70, 83) and len(a) >= 3:
            valid_id(a[1])
            origins[a[1]] = (a[2],)
        elif code == 169 and len(a) >= 5:
            valid_id(a[1])
            origins[a[1]] = (a[3], a[4])
        elif code in (62, 63, 64) and a:
            stores.append(a[0])
        pos += count

    if not entry:
        raise ValueError('missing matching SPIR-V main entry')

    var_storage = {var: storage for _, var, storage in variables}
    def store_may_write_storage(target, seen):
        if target in seen or len(seen) >= 64: return True
        if target in var_storage: return var_storage[target] not in (1, 3, 4, 5, 6, 7, 9)
        source = origins.get(target)
        if not source: return True  # unknown pointer provenance is not proof of safety
        return any(store_may_write_storage(v, seen | {target}) for v in source)
    if any(store_may_write_storage(target, set()) for target in stores):
        return True

    def unwrap(type_id):
        seen = set()
        while type_id not in seen and len(seen) < 64:
            seen.add(type_id)
            item = types.get(type_id)
            if not item: return None, None
            code, args = item
            if code == 32 and len(args) >= 2: type_id = args[1]
            elif code in (28, 29) and args: type_id = args[0]
            else: return type_id, item
        return None, None

    for pointer, var, storage in variables:
        if storage not in (0, 2, 12):
            continue
        base, item = unwrap(pointer)
        if item is None: return True
        code, args = item
        is_buffer = storage == 12 or (storage == 2 and 3 in decorations.get(base, ()))
        if storage == 2 and not is_buffer:
            if code == 30 and 2 in decorations.get(base, ()): continue  # UBO
            return True
        if storage == 0:
            if code in (26, 27): continue  # sampler or sampled image
            if code != 25 or len(args) < 7: return True
            if args[5] == 1: continue  # sampled image, not storage image
            if args[5] != 2: return True
        if 24 in decorations.get(var, ()) or 24 in decorations.get(base, ()):
            continue
        if is_buffer and code == 30 and 0 < len(args) <= 64 and all(24 in members.get((base, i), ()) for i in range(len(args))):
            continue
        return True
    return False


def parse_storage_requirements(value):
    if value is None:
        return {'schema': 1, 'writeStages': 0, 'coverage': 'unknown'}
    if not isinstance(value, dict) or set(value) != {'schema', 'writeStages', 'coverage'}:
        raise ValueError('invalid shaderStorageRequirements')
    if type(value['schema']) is not int or value['schema'] != 1:
        raise ValueError('unsupported shaderStorageRequirements schema')
    if type(value['writeStages']) is not int or not 0 <= value['writeStages'] <= 63:
        raise ValueError('invalid writeStages')
    if value['coverage'] not in ('unknown', 'complete'):
        raise ValueError('invalid coverage')
    return dict(value)


def storage_stages_for_result(result):
    stage_bits = {'vs': 1, 'fs': 2, 'gs': 4, 'tcs': 8, 'tes': 16, 'cs': 32}
    stage_models = {'vs': 0, 'tcs': 1, 'tes': 2, 'gs': 3, 'fs': 4, 'cs': 5}
    required = 0
    for stage in result['content']['stages']:
        name = stage['stage']
        if name not in stage_bits:
            raise ValueError('unknown stage')
        try:
            data = base64.b64decode(stage['spirv'], validate=True)
            needs = spirv_requires_storage_feature(data, stage_models[name])
        except (ValueError, TypeError):
            # An unclassifiable stage is not proof that a device may run it.
            needs = True
        if needs: required |= stage_bits[name]
    return required

def load_descriptor(path):
    """Parse and check a graphics.v1.json. Field rules mirror make_pack.load_graphics and the
    native consumer (native/direct/graphics.c); files[] is checked here because no consumer
    used to look inside it."""
    raw = _read_file(path, 'graphics descriptor', MAX_DESCRIPTOR)
    doc = _object(parse_json(raw, f'graphics descriptor {path}'), f'graphics descriptor {path}')
    try:
        storage = parse_storage_requirements(doc.get('shaderStorageRequirements'))
    except ValueError as error:
        _fail(f'{path}: {error}')
    if _string(doc.get('schema'), f'{path}: schema') != GRAPHICS_SCHEMA:
        _fail(f'{path}: schema is not {GRAPHICS_SCHEMA}')
    title_id = _string(doc.get('titleId'), f'{path}: titleId', pattern=TITLE_ID).upper()
    game_content_root = _content_root(doc.get('gameContentRoot'), f'{path}: gameContentRoot')
    module_set_digest = _content_root(doc.get('moduleSetDigest'), f'{path}: moduleSetDigest')
    if _string(doc.get('frontend'), f'{path}: frontend') != FRONTEND:
        _fail(f'{path}: frontend is not {FRONTEND}')
    if _string(doc.get('backend'), f'{path}: backend') != BACKEND:
        _fail(f'{path}: backend is not {BACKEND}')
    direct_abi = _integer(doc.get('directABI'), f'{path}: directABI', 0)
    closure = _string(doc.get('translatorClosure'), f'{path}: translatorClosure', pattern=CLOSURE)
    source_parser = _integer(doc.get('sourceParserVersion'), f'{path}: sourceParserVersion', 1)
    compiler_abi = _string(doc.get('runtimeCompilerABI'), f'{path}: runtimeCompilerABI')
    if compiler_abi != RUNTIME_COMPILER_ABI:
        _fail(f'{path}: runtimeCompilerABI is not {RUNTIME_COMPILER_ABI}')
    profiles = _array(doc.get('profiles'), f'{path}: profiles')
    if len(profiles) > 8:
        _fail(f'{path}: profiles must hold 0..8 entries')
    profile_hashes = []
    for index, entry in enumerate(profiles):
        entry = _object(entry, f'{path}: profiles[{index}]')
        profile_hashes.append(_content_root(entry.get('profileHash'), f'{path}: profiles[{index}].profileHash'))

    files = []
    seen = set()
    for index, entry in enumerate(_array(doc.get('files'), f'{path}: files')):
        where = f'{path}: files[{index}]'
        entry = _object(entry, where)
        unknown = set(entry) - {'path', 'size', 'sha256'}
        if unknown:
            _fail(f'{where}: unknown key(s) {sorted(unknown)}')
        rel = _direct_relative(entry.get('path'), f'{where}.path')
        if entry['path'] == DESCRIPTOR_REL:
            _fail(f'{where}: the descriptor cannot list itself')
        size = entry.get('size')
        if size is not None:
            size = _integer(size, f'{where}.size')
        digest = _sha_text(entry.get('sha256'), f'{where}.sha256')
        if entry['path'] in seen:
            _fail(f'{where}: duplicate path {entry["path"]!r}')
        seen.add(entry['path'])
        files.append({'path': entry['path'], 'rel': rel, 'size': size, 'sha256': digest})

    if len(set(profile_hashes)) != len(profile_hashes):
        _fail(f'{path}: duplicate profiles')
    if bool(files) != bool(profile_hashes):
        _fail(f'{path}: empty files and empty profiles must coincide')
    file_profiles = set()
    for entry in files:
        rel = entry['rel']
        if rel == LIBRARY_INDEX_REL:
            continue  # shared library index has no device profile of its own
        parts = rel.split('/')
        if len(parts) < 3 or parts[0] != str(direct_abi):
            _fail(f'{path}: file is outside directABI/profileHash')
        profile = _content_root(parts[1], f'{path}: file profile')
        file_profiles.add(profile)
    if file_profiles != set(profile_hashes):
        _fail(f'{path}: files and profiles must refer to exactly the same profiles')

    return {
        'path': os.path.abspath(path),
        'raw': raw,
        'bytes': len(raw),
        'sha256': sha256_bytes(raw),
        'doc': doc,
        'files': files,
        'title_id': title_id,
        'game_content_root': game_content_root,
        'module_set_digest': module_set_digest,
        'direct_abi': direct_abi,
        'translator_closure': closure,
        'source_parser_version': source_parser,
        'runtime_compiler_abi': compiler_abi,
        'profile_hashes': profile_hashes,
        'storage_requirements': storage,
    }


def _walk_relative(directory):
    """All regular files below directory, relative with '/' separators (sorted). Links are
    refused at every level: os.walk() would silently skip a linked directory while a listed-file
    read would follow it, so a link could hide files from the unlisted-file check."""
    found = []
    if not os.path.isdir(directory):
        return found
    for base, directories, names in os.walk(directory, followlinks=False):
        directories.sort()
        for name in directories:
            if os.path.islink(os.path.join(base, name)):
                _fail(f'{directory}: {os.path.join(base, name)} is a linked directory')
        for name in sorted(names):
            full = os.path.join(base, name)
            if os.path.islink(full) or not os.path.isfile(full):
                _fail(f'{directory}: {full} is a link or not a regular file')
            found.append(os.path.relpath(full, directory).replace(os.sep, '/'))
    return found


def _reject_linked_path(base, relative, what):
    """Refuse a symlink anywhere on the way to relative: a linked shaders/vulkan/direct could
    point at a complete library outside the pack and every byte hash would still match."""
    current = base
    for part in relative.strip('/').split('/'):
        if not part:
            continue
        current = os.path.join(current, part)
        if os.path.islink(current):
            _fail(f'{what}: {current} is a symbolic link')


def verify_descriptor_files(direct_dir, files, label):
    """Every listed file must exist under direct_dir with the recorded size/SHA-256, and no
    file may exist there that the descriptor does not list (the fixed descriptor location is
    not library content and is exempt)."""
    listed = {}
    root = os.path.realpath(direct_dir)
    for entry in files:
        listed[entry['rel']] = entry
        literal = os.path.join(direct_dir, entry['rel'])
        if os.path.islink(literal):
            _fail(f'{label}: {entry["path"]} is a link; a library entry must be a regular file')
        full = os.path.realpath(literal)
        if os.path.commonpath((root, full)) != root:
            _fail(f'{label}: {entry["path"]} resolves outside {direct_dir}')
        data = _read_file(full, f'{label}: listed file', PCACHE_MAX_PAYLOAD + PCACHE_HEADER)
        digest = sha256_bytes(data)
        if entry['size'] is not None and len(data) != entry['size']:
            _fail(f'{label}: {entry["path"]}: {len(data)} bytes on disk, descriptor says {entry["size"]}')
        if digest != entry['sha256']:
            _fail(f'{label}: {entry["path"]}: sha256 {digest} does not match {entry["sha256"]}')
    allowed = set(listed) | {DESCRIPTOR_BASENAME}
    extra = [rel for rel in _walk_relative(direct_dir) if rel not in allowed]
    if extra:
        shown = ', '.join(extra[:8]) + (' ...' if len(extra) > 8 else '')
        _fail(f'{label}: unlisted file(s) under {direct_dir}: {shown}')
    return len(listed)


# ---------------------------------------------------------------------------------------
# Pack index / content identities
# ---------------------------------------------------------------------------------------

def _index_bytes(path):
    """index.tsv 的原始字节：三处读取点（read_index / describe_pack / verify_pack）只走这一个入口；不设上限（见文件头 index.tsv 说明）。"""
    return _read_file(path, 'index.tsv', None)


def read_index(path):
    """index.tsv rows in file order: [(path, size, sha256-hex)]."""
    raw = _index_bytes(path)
    text = _decode_utf8(raw, f'index.tsv {path}')
    rows = []
    seen = set()
    for number, line in enumerate(text.splitlines(), 1):
        parts = line.split('\t')
        if len(parts) != 3:
            _fail(f'{path}:{number}: index row is not path<TAB>bytes<TAB>sha256')
        path_text, size_text, digest = parts
        row_path = _relative_path(path_text, f'{path}:{number}')
        if row_path in seen:
            _fail(f'{path}:{number}: duplicate index row {row_path!r}')
        seen.add(row_path)
        if not size_text.isdigit():
            _fail(f'{path}:{number}: size is not a non-negative integer')
        if not HEX64.fullmatch(digest):
            _fail(f'{path}:{number}: sha256 is not 64 lowercase hex')
        rows.append((row_path, int(size_text), digest))
    return rows


def rows_map(rows):
    return {path: (size, digest) for path, size, digest in rows}


def index_content_root(rows):
    """The installed revision's GameContentRoot (ShaderPackLayout.GameContentRoot).

    The runtime hashes the *resource package* index: game_apk.py drops every modules/ row (the
    modules travel in the APK) and writes the remaining rows sorted. So from a make_pack index
    the same value is "drop modules/ and shaders/, sort, hash the canonical lines" — a pack
    built before game_apk.py can then claim the identity the installed revision will have."""
    kept = sorted(row for row in rows if not row[0].startswith(('shaders/', 'modules/')))
    text = ''.join(f'{path}\t{size}\t{digest}\n' for path, size, digest in kept)
    return 'sha256-' + sha256_bytes(text.encode('utf-8'))


def index_module_digest(rows):
    """Canonical digest of modules/ rows. Producer definition (the contract only requires a
    sha256-<64> identity); computed from the index so it cannot be hand-written wrong.
    Returns None when the index has no modules rows: an installed revision carries them in the
    APK, so it cannot make up the value that bound the pack."""
    kept = sorted(row for row in rows if row[0].startswith('modules/'))
    if not kept:
        return None
    text = ''.join(f'{path}\t{size}\t{digest}\n' for path, size, digest in kept)
    return 'sha256-' + sha256_bytes(text.encode('utf-8'))


# ---------------------------------------------------------------------------------------
# Request / result validation
# ---------------------------------------------------------------------------------------

def _decode_base64(value, what, limit):
    text = _string(value, what, allow_empty=True)
    try:
        data = base64.b64decode(text, validate=True)
    except (binascii.Error, ValueError) as error:
        _fail(f'{what}: invalid base64 ({error})')
    if len(data) > limit:
        _fail(f'{what}: larger than {limit} bytes')
    return data


def _top_level_value_span(text, key, what):
    """Raw text of one top-level value, so the envelope hash is checked over the exact bytes
    that were written (re-serializing in Python would change number formatting)."""
    decoder = json.JSONDecoder()

    def skip_ws(index):
        while index < len(text) and text[index] in ' \t\r\n':
            index += 1
        return index

    def skip_string(index):
        index += 1
        while index < len(text):
            char = text[index]
            if char == '\\':
                index += 2
                continue
            if char == '"':
                return index + 1
            index += 1
        _fail(f'{what}: unterminated string')

    index = skip_ws(0)
    if index >= len(text) or text[index] != '{':
        _fail(f'{what}: root is not an object')
    index = skip_ws(index + 1)
    while index < len(text) and text[index] != '}':
        if text[index] != '"':
            _fail(f'{what}: malformed object key')
        end = skip_string(index)
        name = json.loads(text[index:end])
        index = skip_ws(end)
        if index >= len(text) or text[index] != ':':
            _fail(f'{what}: missing ":"')
        index = skip_ws(index + 1)
        try:
            _, end = decoder.raw_decode(text, index)
        except json.JSONDecodeError as error:
            _fail(f'{what}: invalid value ({error})')
        if name == key:
            return text[index:end]
        index = skip_ws(end)
        if index < len(text) and text[index] == ',':
            index = skip_ws(index + 1)
        elif index >= len(text) or text[index] != '}':
            _fail(f'{what}: malformed object')
    _fail(f'{what}: no {key!r} field')


def validate_request(raw, what):
    doc = _object(parse_json(raw, what), what)
    unknown = set(doc) - REQUEST_KEYS
    if unknown:
        _fail(f'{what}: unknown key(s) {sorted(unknown)}')
    for key in ('schema', 'session', 'generation', 'requestId', 'directABI', 'translatorClosure',
                'runtimeCompilerABI', 'sourceParserVersion', 'profile', 'program'):
        if key not in doc:
            _fail(f'{what}: missing {key!r}')
    schema = _string(doc['schema'], f'{what}.schema')
    if schema not in (REQUEST_SCHEMA, REQUEST_SCHEMA_V2):
        _fail(f'{what}.schema is not {REQUEST_SCHEMA} or {REQUEST_SCHEMA_V2}')
    v2 = schema == REQUEST_SCHEMA_V2
    # 可选调试开关：native 过了调试门且 debug.s2a.kind_check=0 时才带 false；缺省表示校验开着。
    if 'kindCheck' in doc and not isinstance(doc['kindCheck'], bool):
        _fail(f'{what}.kindCheck is not a boolean')
    # 可选调试开关：debug.s2a.xlate_debug=1 时才带 true；缺省表示翻译器走正常（非 DebugMode）口径。
    if 'debugMode' in doc and not isinstance(doc['debugMode'], bool):
        _fail(f'{what}.debugMode is not a boolean')
    _integer(doc['session'], f'{what}.session', 1, 2 ** 64 - 1)
    _integer(doc['generation'], f'{what}.generation', 1, 2 ** 64 - 1)
    # The device producer builds requestId from the stage keys (16 hex per stage); other legal
    # producers may use any <=127-byte string, so only the device shape is decoded here.
    request_id = _string(doc['requestId'], f'{what}.requestId', 128)
    direct_abi = _integer(doc['directABI'], f'{what}.directABI', 1)
    closure = _string(doc['translatorClosure'], f'{what}.translatorClosure', pattern=CLOSURE)
    compiler_abi = _string(doc['runtimeCompilerABI'], f'{what}.runtimeCompilerABI')
    if compiler_abi != RUNTIME_COMPILER_ABI:
        _fail(f'{what}.runtimeCompilerABI is not {RUNTIME_COMPILER_ABI}')
    source_parser = _integer(doc['sourceParserVersion'], f'{what}.sourceParserVersion', 1)

    profile = _object(doc['profile'], f'{what}.profile')
    profile_hash = _content_root(profile.get('profileHash'), f'{what}.profile.profileHash')
    if _integer(profile.get('directABI'), f'{what}.profile.directABI', 1) != direct_abi:
        _fail(f'{what}: profile.directABI does not match directABI')
    if _string(profile.get('translatorClosure'), f'{what}.profile.translatorClosure', pattern=CLOSURE) != closure:
        _fail(f'{what}: profile.translatorClosure does not match translatorClosure')
    if _string(profile.get('runtimeCompilerABI'), f'{what}.profile.runtimeCompilerABI') != compiler_abi:
        _fail(f'{what}: profile.runtimeCompilerABI does not match runtimeCompilerABI')
    support = _object(profile.get('supportLayout'), f'{what}.profile.supportLayout')
    support_hash = _content_root(support.get('hash'), f'{what}.profile.supportLayout.hash')

    program = _object(doc['program'], f'{what}.program')
    kind = _string(program.get('kind'), f'{what}.program.kind')
    if kind not in ('graphics', 'compute'):
        _fail(f'{what}.program.kind is not graphics/compute')
    stages = _array(program.get('stages'), f'{what}.program.stages')
    if not stages:
        _fail(f'{what}.program.stages is empty')
    if v2 and (kind != 'graphics' or len(stages) != len(V2_SLOTS)):
        _fail(f'{what}.program.stages: a v2 request is graphics with five NVN slots')
    names = []
    constant_buffer_use = {}
    sources = {}
    for index, stage in enumerate(stages):
        where = f'{what}.program.stages[{index}]'
        if v2 and stage is None:
            continue
        stage = _object(stage, where)
        name = _string(stage.get('stage'), f'{where}.stage')
        if name not in GRAPHICS_STAGES + COMPUTE_STAGES + (('gs',) if v2 else ()):
            _fail(f'{where}.stage {name!r} is not one of vs/fs/cs' + ('/gs' if v2 else ''))
        if v2 and name != V2_SLOTS[index]:
            _fail(f'{where}.stage {name!r} does not match its NVN slot {V2_SLOTS[index]!r}')
        if name in names:
            _fail(f'{what}: duplicate stage {name!r}')
        names.append(name)
        code = _decode_base64(stage.get('code'), f'{where}.code', 16 << 20)
        if not code:
            _fail(f'{where}.code is empty')
        control = _decode_base64(stage.get('control'), f'{where}.control', 64 << 10)
        if not control:
            _fail(f'{where}.control is empty')
        c1 = b''
        if stage.get('c1') is not None:
            c1 = _decode_base64(stage.get('c1'), f'{where}.c1', 64 << 10)
        # The compiler stamps codeSource/controlSource/c1Source with these exact digests
        # (DirectProgram.cs), so they bind a result to the request bytes it was built from.
        sources[name] = {'code': sha256_bytes(code), 'control': sha256_bytes(control),
                         'c1': sha256_bytes(c1)}
        # 契约已扩到完整 32 位（编译器 DirectProgram.Parse 同上限）：>=5 号 cbuf 不再丢信息。
        constant_buffer_use[name] = _integer(stage.get('constantBufferUse', 0),
                                             f'{where}.constantBufferUse', 0, 0xFFFFFFFF)
    expected = GRAPHICS_STAGES if kind == 'graphics' else COMPUTE_STAGES
    if set(names) != set(expected) and not (v2 and set(names) == set(GEOMETRY_STAGES)):
        _fail(f'{what}: program.stages {sorted(names)} is not {list(expected)}'
              + (f' or {list(GEOMETRY_STAGES)}' if v2 else '') + f' for kind={kind}')
    if REQUEST_ID.fullmatch(request_id) and len(request_id) != 16 * len(names):
        _fail(f'{what}: requestId is not {len(names)} concatenated 8-byte stage keys')

    doc['_requestId'] = request_id
    doc['_profileHash'] = profile_hash
    doc['_directABI'] = direct_abi
    doc['_translatorClosure'] = closure
    doc['_sourceParserVersion'] = source_parser
    doc['_supportHash'] = support_hash
    doc['_kind'] = kind
    doc['_stages'] = names
    doc['_constantBufferUse'] = constant_buffer_use
    doc['_sources'] = sources
    return doc


def validate_result(raw, what, request=None, descriptor=None, expected_profile=None):
    """Check a s2a-direct-program-v1 envelope and its content. `request` (build) additionally
    binds it to the request it came from; `descriptor` (verify) pins the identity half."""
    text = _decode_utf8(raw, what)
    doc = _object(parse_json(raw, what), what)
    unknown = set(doc) - {'schema', 'transport', 'contentHash', 'content'}
    if unknown:
        _fail(f'{what}: unknown key(s) {sorted(unknown)}')
    for key in ('schema', 'transport', 'contentHash', 'content'):
        if key not in doc:
            _fail(f'{what}: missing {key!r}')
    schema = _string(doc['schema'], f'{what}.schema')
    if schema not in (RESULT_SCHEMA, 's2a-direct-program-v2'):
        _fail(f'{what}.schema is unknown')
    transport = _object(doc['transport'], f'{what}.transport')
    request_id = _string(transport.get('requestId'), f'{what}.transport.requestId', 128)
    if request is not None and request_id != request['_requestId']:
        _fail(f'{what}: transport.requestId {request_id!r} is not the request requestId {request["_requestId"]!r}')
    content_hash = _string(doc['contentHash'], f'{what}.contentHash')
    if not content_hash.startswith('sha256-') or not HEX64.fullmatch(content_hash[7:]):
        _fail(f'{what}.contentHash is not sha256-<64 lowercase hex>')
    content_raw = _top_level_value_span(text, 'content', what).encode('utf-8')
    if 'sha256-' + sha256_bytes(content_raw) != content_hash:
        _fail(f'{what}.contentHash does not match the embedded content bytes')

    content = _object(doc['content'], f'{what}.content')
    contract = CONTENT_CONTRACT if schema == RESULT_SCHEMA else 's2a-direct-program-content-v2'
    if _string(content.get('contract'), f'{what}.content.contract') != contract:
        _fail(f'{what}.content.contract does not match schema')
    identity = _object(content.get('identity'), f'{what}.content.identity')
    content_abi = _integer(identity.get('directABI'), f'{what}.content.identity.directABI', 1)
    content_closure = _string(identity.get('translatorClosure'),
                              f'{what}.content.identity.translatorClosure', pattern=CLOSURE)
    content_compiler = _string(identity.get('runtimeCompilerABI'),
                               f'{what}.content.identity.runtimeCompilerABI')
    content_profile = _content_root(identity.get('profileHash'),
                                    f'{what}.content.identity.profileHash')
    if content_compiler != RUNTIME_COMPILER_ABI:
        _fail(f'{what}.content.identity.runtimeCompilerABI is not {RUNTIME_COMPILER_ABI}')

    kind = _string(content.get('kind'), f'{what}.content.kind')
    if kind not in ('graphics', 'compute'):
        _fail(f'{what}.content.kind is not graphics/compute')
    stages = _array(content.get('stages'), f'{what}.content.stages')
    expected = GRAPHICS_STAGES if kind == 'graphics' else COMPUTE_STAGES
    names = []
    constant_buffer_use = {}
    source_refs = {}
    for index, stage in enumerate(stages):
        where = f'{what}.content.stages[{index}]'
        stage = _object(stage, where)
        name = _string(stage.get('stage'), f'{where}.stage')
        names.append(name)
        spirv = _decode_base64(stage.get('spirv'), f'{where}.spirv', PCACHE_MAX_PAYLOAD)
        if len(spirv) < 20 or len(spirv) % 4 or not spirv.startswith(SPIRV_MAGIC):
            _fail(f'{where}.spirv is not word-aligned SPIR-V (magic/length)')
        if _sha_text(stage.get('spirvSha256'), f'{where}.spirvSha256') != sha256_bytes(spirv):
            _fail(f'{where}.spirvSha256 does not match the SPIR-V bytes')
        source_refs[name] = {
            'code': _string(stage.get('codeSource'), f'{where}.codeSource', pattern=SOURCE_REF),
            'control': _string(stage.get('controlSource'), f'{where}.controlSource', pattern=SOURCE_REF),
            'c1': _string(stage.get('c1Source'), f'{where}.c1Source', pattern=SOURCE_REF),
        }
        constant_buffer_use[name] = _integer(stage.get('constantBufferUse', 0),
                                             f'{where}.constantBufferUse', 0, 0xFFFFFFFF)
    if tuple(names) != expected and not (kind == 'graphics' and tuple(names) == GEOMETRY_STAGES):
        _fail(f'{what}: content.stages is {names}, expected {list(expected)}'
              + (f' or {list(GEOMETRY_STAGES)}' if kind == 'graphics' else '') + f' for kind={kind}')
    support = _object(content.get('supportLayout'), f'{what}.content.supportLayout')
    support_hash = _content_root(support.get('hash'), f'{what}.content.supportLayout.hash')
    bindings = _array(content.get('bindings'), f'{what}.content.bindings')
    for index, binding in enumerate(bindings):
        where = f'{what}.content.bindings[{index}]'
        binding = _object(binding, where)
        if 'samp_slot' in binding:
            if schema == RESULT_SCHEMA or binding.get('kind') != 'texture':
                _fail(f'{where}: separate sampler source requires v2 texture')
            _integer(binding['samp_slot'], f'{where}.samp_slot', 0, 127)
            texword = _integer(binding.get('slot'), f'{where}.slot', 90, 345)
            if texword % 2 or binding.get('count') != 1:
                _fail(f'{where}: invalid separate texture slot or array')
        if 'constSampler' in binding:
            # #1594：类型 3 句柄——采样器池下标（0..4095，0 是真下标）是着色器里的立即数。贴图字的来源二选一：
            #   * c2 分离贴图表：槽 90..345 偶数；
            #   * 游戏 UBO（片 3）：平铺的 uboSlot 0..15 + uboOffset（4 的倍数、0..65535），没有 slot。
            #   与宿主解析（nvn_vk_program_result.c）同一份合同：v2、单个贴图；与 samp_slot / uboHandle / sampler 对象互斥
            #   （c2 形态还与 uboSlot 互斥）；计算阶段宿主没接，计算结果也不收。
            from_ubo = 'uboSlot' in binding
            if (schema == RESULT_SCHEMA or kind == 'compute' or binding.get('kind') != 'texture'
                    or 'samp_slot' in binding or 'uboHandle' in binding or 'sampler' in binding
                    or (from_ubo and 'slot' in binding)):
                _fail(f'{where}: constant sampler source requires a v2 graphics texture with one texture-word source '
                      '(slot, or uboSlot without slot) and no samp_slot / uboHandle / sampler')
            _integer(binding['constSampler'], f'{where}.constSampler', 0, 4095)
            if from_ubo:
                _integer(binding['uboSlot'], f'{where}.uboSlot', 0, 15)
                offset = _integer(binding.get('uboOffset'), f'{where}.uboOffset', 0, 0xFFFF)
                if offset % 4 or binding.get('count') != 1:
                    _fail(f'{where}: invalid constant sampler uboOffset or array')
            else:
                texword = _integer(binding.get('slot'), f'{where}.slot', 90, 345)
                if texword % 2 or binding.get('count') != 1:
                    _fail(f'{where}: invalid constant sampler texture slot or array')
    _array(content.get('dependencies'), f'{what}.content.dependencies')

    if descriptor is not None:
        if ((content_abi, content_closure) != (descriptor['direct_abi'], descriptor['translator_closure'])
                or content_profile not in descriptor['profile_hashes']
                or (expected_profile is not None and content_profile != expected_profile)):
            _fail(f'{what}: content identity does not match the graphics descriptor')
    if request is not None:
        if (content_abi, content_closure, content_compiler, content_profile) != (
                request['_directABI'], request['_translatorClosure'], RUNTIME_COMPILER_ABI,
                request['_profileHash']):
            _fail(f'{what}: content identity does not match the request profile')
        if kind != request['_kind']:
            _fail(f'{what}: content.kind {kind} does not match request kind {request["_kind"]}')
        if set(names) != set(request['_stages']):
            _fail(f'{what}: content.stages {names} are not the request stages {request["_stages"]}')
        if support_hash != request['_supportHash']:
            _fail(f'{what}: supportLayout.hash does not match the request profile')
        for name in request['_stages']:
            if constant_buffer_use.get(name) != request['_constantBufferUse'][name]:
                _fail(f'{what}: stage {name} constantBufferUse does not match the request')
            for kind in ('code', 'control', 'c1'):
                want = request['_sources'][name][kind]
                if source_refs[name][kind] != f'{kind}:sha256:{want}':
                    _fail(f'{what}: stage {name} {kind}Source does not match the captured '
                          'request bytes (result belongs to a different request?)')
    return doc


# ---------------------------------------------------------------------------------------
# S2APC1 v2 container (native/direct/program_cache.c format)
# ---------------------------------------------------------------------------------------

def encode_container(identity_hex, payload):
    if not HEX64.fullmatch(identity_hex):
        _fail(f'identity {identity_hex!r} is not 64 lowercase hex')
    if not payload or len(payload) > PCACHE_MAX_PAYLOAD:
        _fail(f'payload length {len(payload)} is outside 1..{PCACHE_MAX_PAYLOAD}')
    header = (PCACHE_MAGIC + struct.pack('<II', PCACHE_VERSION, PCACHE_HEADER)
              + bytes.fromhex(identity_hex) + bytes.fromhex(sha256_bytes(payload))
              + struct.pack('<Q', len(payload)))
    assert len(header) == PCACHE_HEADER
    return header + payload


def decode_container(raw, identity_hex, what):
    if len(raw) < PCACHE_HEADER:
        _fail(f'{what}: shorter than the {PCACHE_HEADER}-byte S2APC1 header')
    if raw[:8] != PCACHE_MAGIC:
        _fail(f'{what}: not a S2APC1 container (magic)')
    version, header = struct.unpack_from('<II', raw, 8)
    if version != PCACHE_VERSION or header != PCACHE_HEADER:
        _fail(f'{what}: container version/header {version}/{header} is not {PCACHE_VERSION}/{PCACHE_HEADER}')
    if raw[16:48] != bytes.fromhex(identity_hex):
        _fail(f'{what}: container identity does not match the file name {identity_hex}')
    length = struct.unpack_from('<Q', raw, 80)[0]
    if not 1 <= length <= PCACHE_MAX_PAYLOAD:
        _fail(f'{what}: payload length {length} is out of range')
    if len(raw) != PCACHE_HEADER + length:
        _fail(f'{what}: {len(raw)} bytes on disk, header declares {PCACHE_HEADER + length}')
    payload = raw[PCACHE_HEADER:]
    digest = sha256_bytes(payload)
    if digest != raw[48:80].hex():
        _fail(f'{what}: payload SHA-256 does not match the header (corrupt bytes)')
    return payload, digest


# ---------------------------------------------------------------------------------------
# Capture manifest and program library build
# ---------------------------------------------------------------------------------------

def _capture_file(capture_dir, name, what):
    path = _relative_path(name, what)
    literal = os.path.join(capture_dir, path)
    root = os.path.realpath(capture_dir)
    full = os.path.realpath(literal)
    if os.path.commonpath((root, full)) != root:
        _fail(f'{what}: {name!r} escapes the capture directory')
    if os.path.islink(literal):
        _fail(f'{what}: {name!r} must be a regular file in the capture directory (no links)')
    if not os.path.isfile(full):
        _fail(f'{what}: {name!r} does not exist')
    return full


def load_capture(capture_dir):
    capture_dir = os.path.abspath(capture_dir)
    if not os.path.isdir(capture_dir):
        _fail(f'capture directory does not exist: {capture_dir}')
    doc = _object(_read_json_file(os.path.join(capture_dir, 'capture.v1.json'),
                                  'capture manifest', MAX_CAPTURE_MANIFEST), 'capture manifest')
    unknown = set(doc) - CAPTURE_KEYS
    if unknown:
        _fail(f'capture manifest: unknown key(s) {sorted(unknown)}')
    if _string(doc.get('schema'), 'capture manifest.schema') != CAPTURE_SCHEMA:
        _fail(f'capture manifest.schema is not {CAPTURE_SCHEMA}')
    direct_abi = _integer(doc.get('directABI'), 'capture manifest.directABI', 1)
    closure = _string(doc.get('translatorClosure'), 'capture manifest.translatorClosure', pattern=CLOSURE)
    source_parser = _integer(doc.get('sourceParserVersion'), 'capture manifest.sourceParserVersion', 1)
    profile_hash = doc.get('profileHash')
    if profile_hash is not None:
        profile_hash = _content_root(profile_hash, 'capture manifest.profileHash')
    entries = _array(doc.get('entries'), 'capture manifest.entries')
    if not entries:
        _fail('capture manifest.entries is empty')
    seen_identity, seen_request, seen_result = set(), set(), set()
    loaded = []
    for index, entry in enumerate(entries):
        where = f'capture manifest.entries[{index}]'
        entry = _object(entry, where)
        unknown = set(entry) - {'identity', 'request', 'result'}
        if unknown:
            _fail(f'{where}: unknown key(s) {sorted(unknown)}')
        identity = _string(entry.get('identity'), f'{where}.identity', pattern=HEX64)
        if identity in seen_identity:
            _fail(f'{where}: duplicate identity {identity}')
        seen_identity.add(identity)
        request_name = _string(entry.get('request'), f'{where}.request')
        result_name = _string(entry.get('result'), f'{where}.result')
        if request_name in seen_request:
            _fail(f'{where}: request {request_name!r} is captured more than once')
        if result_name in seen_result:
            _fail(f'{where}: result {result_name!r} is claimed by more than one identity')
        seen_request.add(request_name)
        seen_result.add(result_name)
        loaded.append({
            'identity': identity,
            'request_path': _capture_file(capture_dir, request_name, f'{where}.request'),
            'result_path': _capture_file(capture_dir, result_name, f'{where}.result'),
        })
    return {
        'dir': capture_dir,
        'direct_abi': direct_abi,
        'translator_closure': closure,
        'source_parser_version': source_parser,
        'profile_hash': profile_hash,
        'entries': loaded,
    }


def _make_parent(path):
    parent = os.path.dirname(path)
    if parent and not os.path.isdir(parent):
        try:
            os.makedirs(parent)
        except OSError as error:
            _fail(f'cannot create {parent}: {error.strerror or error}')


def _write_new_file(path, data, what):
    _make_parent(path)
    try:
        with open(path, 'xb') as stream:
            stream.write(data)
    except FileExistsError:
        _fail(f'{what}: {path} already exists')
    except OSError as error:
        _fail(f'{what}: cannot write {path}: {error.strerror or error}')


def _dump_json(doc):
    return (json.dumps(doc, ensure_ascii=False, indent=1) + '\n').encode('utf-8')


def load_compiler_identity(path):
    """Read 'cli identity' JSON (s2a-direct-identity-v1). The producer never hand-writes the
    closure: it must be the value the compiler build actually embeds."""
    doc = _object(_read_json_file(path, 'compiler identity', 1 << 20), 'compiler identity')
    if _string(doc.get('schema'), 'compiler identity.schema') != 's2a-direct-identity-v1':
        _fail('compiler identity.schema is not s2a-direct-identity-v1')
    return {
        'closure': _string(doc.get('translatorClosure'), 'compiler identity.translatorClosure',
                           pattern=CLOSURE),
        'sources_digest': doc.get('sourcesDigest'),
        'module_version_id': doc.get('moduleVersionId'),
        'rule': doc.get('rule'),
    }


def build(capture_dir, out_dir, descriptor_path, title_id,
          rows=None, game_content_root=None, module_set_digest=None,
          direct_abi=None, profile_hash=None, translator_closure=None, identity=None):
    """Create <out_dir> (must not exist) from a capture, then the descriptor (must not exist).
    Returns a summary dict. Nothing is written in place: an existing library is never edited.

    identity (path to 'cli identity' JSON) pins the capture to the compiler build that produced
    it: a capture compiled by another closure is rejected instead of being packed."""
    capture = load_capture(capture_dir)
    if identity is not None:
        compiler = load_compiler_identity(identity)
        if capture['translator_closure'] != compiler['closure']:
            _fail(f"capture closure {capture['translator_closure']} is not this compiler "
                  f"build's closure ({compiler['closure']})")
    out_dir = os.path.abspath(out_dir)
    descriptor_path = os.path.abspath(descriptor_path)
    if os.path.lexists(out_dir):
        _fail(f'{out_dir} already exists; a library is immutable, build another directory and '
              'publish a new revision instead')
    if os.path.lexists(descriptor_path):
        _fail(f'{descriptor_path} already exists; refusing to overwrite a descriptor')
    if os.path.commonpath((out_dir, descriptor_path)) == out_dir:
        _fail('the descriptor must live outside the library root (make_pack takes it via '
              '--direct-graphics; it is packed at ' + DESCRIPTOR_REL + ')')
    title_id = _string(title_id, 'title id', pattern=TITLE_ID).upper()
    if direct_abi is not None and _integer(direct_abi, 'direct abi', 1) != capture['direct_abi']:
        _fail('--direct-abi does not match the captured requests')
    if profile_hash is not None:
        profile_hash = _content_root(profile_hash, 'profile hash')
    if translator_closure is not None:
        translator_closure = _string(translator_closure, 'translator closure', pattern=CLOSURE)

    validated = []
    identities = set()
    for entry in capture['entries']:
        identity = entry['identity']
        if identity in identities:
            _fail(f'duplicate identity {identity}')
        identities.add(identity)
        request_raw = _read_file(entry['request_path'], 'capture request', MAX_REQUEST)
        result_raw = _read_file(entry['result_path'], 'capture result', PCACHE_MAX_PAYLOAD)
        request_what = f'capture request {entry["request_path"]}'
        request = validate_request(request_raw, request_what)
        result_what = f'capture result {entry["result_path"]}'
        result = validate_result(result_raw, result_what, request=request)
        validated.append({
            'identity': identity,
            'request_sha256': sha256_bytes(request_raw),
            'result_sha256': sha256_bytes(result_raw),
            'payload': result_raw,
            'write_stages': storage_stages_for_result(result),
            'direct_abi': request['_directABI'],
            'translator_closure': request['_translatorClosure'],
            'source_parser_version': request['_sourceParserVersion'],
            'profile_hash': request['_profileHash'],
        })

    # One library = one device profile: a descriptor lists profiles, but an entry cannot be
    # verified against several, and the directory is per profileHash.
    abis = {item['direct_abi'] for item in validated}
    closures = {item['translator_closure'] for item in validated}
    parsers = {item['source_parser_version'] for item in validated}
    profiles = {item['profile_hash'] for item in validated}
    if len(abis) != 1 or len(closures) != 1 or len(parsers) != 1 or len(profiles) != 1:
        _fail(f'capture mixes identities (abi={sorted(abis)} closure={sorted(closures)} '
              f'parser={sorted(parsers)} profile={sorted(profiles)}); build one library per profile')
    abi, closure, parser, profile = abis.pop(), closures.pop(), parsers.pop(), profiles.pop()
    if (abi, closure, parser) != (capture['direct_abi'], capture['translator_closure'],
                                  capture['source_parser_version']):
        _fail('capture manifest identity does not match the captured requests')
    if capture['profile_hash'] is not None and capture['profile_hash'] != profile:
        _fail('capture manifest.profileHash does not match the captured requests')
    if profile_hash is not None and profile_hash != profile:
        _fail('--profile-hash does not match the captured requests')
    if translator_closure is not None and translator_closure != closure:
        _fail('--translator-closure does not match the captured requests')

    if rows is not None:
        rows = list(rows)
        derived_root = index_content_root(rows)
        derived_modules = index_module_digest(rows)
    else:
        derived_root = derived_modules = None
    if game_content_root is None:
        if derived_root is None:
            _fail('give --index (the base pack index.tsv) or --game-content-root')
        game_content_root = derived_root
    else:
        game_content_root = _content_root(game_content_root, '--game-content-root')
        if derived_root is not None and game_content_root != derived_root:
            _fail(f'--game-content-root {game_content_root} is not the one derived from --index '
                  f'({derived_root})')
    if module_set_digest is None:
        if derived_modules is None:
            _fail('--index has no modules/ rows (an installed revision keeps them in the APK); '
                  'pass the pack index this library will ship in, or --module-set-digest')
        module_set_digest = derived_modules
    else:
        module_set_digest = _content_root(module_set_digest, '--module-set-digest')
        if derived_modules is not None and module_set_digest != derived_modules:
            _fail(f'--module-set-digest {module_set_digest} is not the one derived from --index '
                  f'({derived_modules})')

    validated.sort(key=lambda item: item['identity'])
    entry_dir = f'{abi}/{profile}'
    index_entries = []
    staging = out_dir + '.tmp-' + str(os.getpid())
    if os.path.lexists(staging):
        _fail(f'staging directory already exists: {staging}')
    wrote_descriptor = False
    try:
        try:
            os.makedirs(os.path.join(staging, entry_dir))
        except OSError as error:
            _fail(f'cannot create {staging}: {error.strerror or error}')
        for item in validated:
            name = f'{item["identity"]}.program'
            container = encode_container(item['identity'], item['payload'])
            full = os.path.join(staging, entry_dir, name)
            with open(full, 'wb') as stream:
                stream.write(container)
            index_entries.append({
                'identity': item['identity'],
                'path': f'{entry_dir}/{name}',
                'size': len(container),
                'sha256': sha256_bytes(container),
                'resultSha256': item['result_sha256'],
                'requestSha256': item['request_sha256'],
            })
        index_doc = {
            'schema': LIBRARY_SCHEMA,
            'titleId': title_id,
            'gameContentRoot': game_content_root,
            'moduleSetDigest': module_set_digest,
            'directABI': abi,
            'translatorClosure': closure,
            'sourceParserVersion': parser,
            'runtimeCompilerABI': RUNTIME_COMPILER_ABI,
            'profileHash': profile,
            'entries': index_entries,
        }
        index_bytes = _dump_json(index_doc)
        with open(os.path.join(staging, LIBRARY_INDEX_REL), 'wb') as stream:
            stream.write(index_bytes)

        files = [{'path': DIRECT_PREFIX + entry['path'], 'size': entry['size'],
                  'sha256': entry['sha256']} for entry in index_entries]
        files.append({'path': DIRECT_PREFIX + LIBRARY_INDEX_REL,
                      'size': len(index_bytes), 'sha256': sha256_bytes(index_bytes)})
        descriptor_doc = {
            'schema': GRAPHICS_SCHEMA,
            'titleId': title_id,
            'gameContentRoot': game_content_root,
            'moduleSetDigest': module_set_digest,
            'frontend': FRONTEND,
            'backend': BACKEND,
            'directABI': abi,
            'translatorClosure': closure,
            'sourceParserVersion': parser,
            'runtimeCompilerABI': RUNTIME_COMPILER_ABI,
            'profiles': [{'profileHash': profile}],
            'files': files,
            'shaderStorageRequirements': {'schema': 1,
                                          'writeStages': functools.reduce(lambda a, x: a | x['write_stages'], validated, 0),
                                          'coverage': 'unknown'},
        }
        descriptor_bytes = _dump_json(descriptor_doc)

        # Validate what will be published before publishing: the descriptor we just built is
        # parsed back and the staging tree is checked with the same rules `verify` uses.
        _write_new_file(descriptor_path, descriptor_bytes, 'descriptor')
        wrote_descriptor = True
        descriptor = load_descriptor(descriptor_path)
        verify_library(staging, descriptor)
        try:
            os.rename(staging, out_dir)
        except OSError as error:
            _fail(f'cannot publish {out_dir}: {error.strerror or error}')
        wrote_descriptor = False
    except BaseException:
        if os.path.isdir(staging):
            shutil.rmtree(staging, ignore_errors=True)
        # Only remove a descriptor this call created; a pre-existing one was refused above.
        if wrote_descriptor:
            try:
                os.unlink(descriptor_path)
            except OSError:
                pass
        raise
    return {
        'library': out_dir,
        'descriptor': descriptor_path,
        'entries': len(index_entries),
        'bytes': sum(entry['size'] for entry in index_entries) + len(index_bytes),
        'directABI': abi,
        'profileHash': profile,
        'translatorClosure': closure,
        'gameContentRoot': game_content_root,
        'moduleSetDigest': module_set_digest,
        'titleId': title_id,
    }


def describe_pack(pack_root, descriptor_path, identity):
    """Derive a descriptor from an existing, verified offline library without writing the pack.

    identity is the path to 'cli identity' JSON from the compiler build that goes into the APK:
    the descriptor's closure is that build's closure, never a hand-written constant.

    The empty-library case requires a schema decision and fails closed for now.
    """
    closure = load_compiler_identity(identity)['closure']
    pack_root = os.path.realpath(pack_root)
    descriptor_path = os.path.abspath(descriptor_path)
    if os.path.commonpath((pack_root, os.path.realpath(descriptor_path))) == pack_root:
        _fail('descriptor output must be outside the read-only pack')
    manifest = _object(_read_json_file(os.path.join(pack_root, 'manifest.json'), 'pack manifest', MAX_DESCRIPTOR), 'pack manifest')
    if manifest.get('version') != 2: _fail('pack manifest version is not 2')
    index_path = os.path.join(pack_root, 'index.tsv')
    raw = _index_bytes(index_path)
    if sha256_bytes(raw) != manifest.get('index_sha256'): _fail('pack index hash mismatch')
    rows = read_index(index_path); row_map = rows_map(rows)
    if manifest.get('files') != len(rows) or manifest.get('total') != sum(size for _, size, _ in rows):
        _fail('pack manifest file count or byte total mismatch')
    for rel, size, digest in rows:
        _reject_linked_path(pack_root, rel, 'pack indexed file')
        path = os.path.join(pack_root, rel)
        try:
            h = hashlib.sha256(); length = 0
            with open(path, 'rb') as stream:
                for block in iter(lambda: stream.read(4 << 20), b''):
                    h.update(block); length += len(block)
        except OSError as error:
            _fail(f'pack indexed file {rel}: {error.strerror or error}')
        if length != size or h.hexdigest() != digest:
            _fail(f'pack indexed file {rel}: bytes do not match index')
    root, modules = index_content_root(rows), index_module_digest(rows)
    if modules is None: _fail('pack has no modules/ rows')
    artifact = _object(_read_json_file(os.path.join(pack_root, 'game-build-artifact.json'), 'pack artifact', MAX_DESCRIPTOR), 'pack artifact')
    if artifact.get('version') != 1 or artifact.get('target') != 'android':
        _fail('pack artifact is not Android version 1')
    title = _string(artifact.get('titleId'), 'pack titleId', pattern=TITLE_ID).upper()
    if artifact.get('contentRoot') != 'sha256-' + sha256_bytes(raw): _fail('pack artifact index identity mismatch')
    package = _object(artifact.get('package'), 'pack artifact package')
    if package.get('indexSha256') != sha256_bytes(raw): _fail('pack artifact package index mismatch')
    if 'ctl/control.nacp' not in row_map: _fail('pack control.nacp is not indexed')
    nacp = _read_file(os.path.join(pack_root, 'ctl/control.nacp'), 'pack control', 1 << 20)
    if len(nacp) < 0x3040 or f'{struct.unpack_from("<Q", nacp, 0x3038)[0]:016X}' != title:
        _fail('pack title does not match control.nacp')
    _reject_linked_path(pack_root, DIRECT_PREFIX, 'pack direct library')
    library = os.path.join(pack_root, DIRECT_PREFIX)
    index_file = os.path.join(library, LIBRARY_INDEX_REL)
    if not os.path.isfile(index_file):
        if _walk_relative(library): _fail('direct library files exist without a library index')
        doc = {'schema': GRAPHICS_SCHEMA, 'titleId': title, 'gameContentRoot': root,
               'moduleSetDigest': modules, 'frontend': FRONTEND, 'backend': BACKEND,
               'directABI': 1, 'translatorClosure': closure, 'sourceParserVersion': 1,
               'runtimeCompilerABI': RUNTIME_COMPILER_ABI, 'profiles': [], 'files': [],
               'shaderStorageRequirements': {'schema': 1, 'writeStages': 0, 'coverage': 'unknown'}}
        wrote = False
        try:
            _write_new_file(descriptor_path, _dump_json(doc), 'descriptor'); wrote = True
            load_descriptor(descriptor_path)
            return {'descriptor': descriptor_path, 'files': 0, 'gameContentRoot': root, 'moduleSetDigest': modules}
        except BaseException:
            if wrote: os.unlink(descriptor_path)
            raise

    index_bytes = _read_file(index_file, 'library index', MAX_LIBRARY_INDEX)
    index = _object(parse_json(index_bytes, 'library index'), 'library index')
    if index.get('translatorClosure') != closure:
        _fail('offline library translator closure differs from this compiler')
    if (index.get('titleId') != title or index.get('gameContentRoot') != root or
        index.get('moduleSetDigest') != modules): _fail('library identity does not match pack')
    files = []
    write_stages = 0
    for item in _array(index.get('entries'), 'library entries'):
        item = _object(item, 'library entry')
        rel = _relative_path(item.get('path'), 'library entry path')
        ident = _string(item.get('identity'), 'library entry identity', pattern=HEX64)
        container = _read_file(os.path.join(library, rel), 'library entry', PCACHE_MAX_PAYLOAD + PCACHE_HEADER)
        payload, _ = decode_container(container, ident, 'library entry')
        write_stages |= storage_stages_for_result(validate_result(payload, 'library entry'))
        files.append({'path': DIRECT_PREFIX + rel, 'size': _integer(item.get('size'), 'library entry size'),
                      'sha256': _sha_text(item.get('sha256'), 'library entry sha256')})
    files.append({'path': DIRECT_PREFIX + LIBRARY_INDEX_REL, 'size': len(index_bytes), 'sha256': sha256_bytes(index_bytes)})
    for entry in files:
        if row_map.get(entry['path']) != (entry['size'], entry['sha256']):
            _fail(f'library file is not bound by pack index: {entry["path"]}')
    doc = {'schema': GRAPHICS_SCHEMA, 'titleId': title, 'gameContentRoot': root,
           'moduleSetDigest': modules, 'frontend': FRONTEND, 'backend': BACKEND,
           'directABI': index.get('directABI'), 'translatorClosure': index.get('translatorClosure'),
           'sourceParserVersion': index.get('sourceParserVersion'),
           'runtimeCompilerABI': index.get('runtimeCompilerABI'),
           'profiles': [{'profileHash': index.get('profileHash')}], 'files': files,
           'shaderStorageRequirements': {'schema': 1, 'writeStages': write_stages, 'coverage': 'unknown'}}
    wrote = False
    try:
        _write_new_file(descriptor_path, _dump_json(doc), 'descriptor'); wrote = True
        descriptor = load_descriptor(descriptor_path)
        verify_library(library, descriptor)
        return {'descriptor': descriptor_path, 'files': len(files), 'gameContentRoot': root, 'moduleSetDigest': modules}
    except BaseException:
        if wrote: os.unlink(descriptor_path)
        raise


# ---------------------------------------------------------------------------------------
# Verification
# ---------------------------------------------------------------------------------------

def _identity_from_name(name, what):
    if not name.endswith('.program'):
        _fail(f'{what}: {name!r} does not end in .program')
    identity = name[:-len('.program')]
    if not HEX64.fullmatch(identity):
        _fail(f'{what}: {identity!r} is not a 64-hex identity')
    return identity


def _check_index_entry_fields(index, number, entry):
    where = f'library index entry {number}'
    entry = _object(entry, where)
    unknown = set(entry) - {'identity', 'path', 'size', 'sha256', 'resultSha256', 'requestSha256'}
    if unknown:
        _fail(f'{where}: unknown key(s) {sorted(unknown)}')
    identity = _string(entry.get('identity'), f'{where}.identity', pattern=HEX64)
    # Index paths are relative to the library root (which itself is DIRECT_PREFIX in the pack).
    path = _relative_path(entry.get('path'), f'{where}.path')
    size = _integer(entry.get('size'), f'{where}.size', 1, PCACHE_MAX_PAYLOAD + PCACHE_HEADER)
    digest = _sha_text(entry.get('sha256'), f'{where}.sha256')
    result_sha = _sha_text(entry.get('resultSha256'), f'{where}.resultSha256')
    request_sha = _sha_text(entry.get('requestSha256'), f'{where}.requestSha256')
    expected = f'{index["directABI"]}/{index["profileHash"]}/{identity}.program'
    if path != expected:
        _fail(f'{where}.path {path!r} is not {expected!r}')
    _identity_from_name(f'{identity}.program', where)
    return {'identity': identity, 'path': path, 'size': size, 'sha256': digest,
            'resultSha256': result_sha, 'requestSha256': request_sha}


def verify_library(library, descriptor):
    """Check a library directory (the content that will live at shaders/vulkan/direct/) against
    its descriptor: container headers, payload digests, envelope content, index agreement and
    the exact file list."""
    library = os.path.abspath(library)
    if not os.path.isdir(library):
        _fail(f'library directory does not exist: {library}')
    verify_descriptor_files(library, descriptor['files'], f'library {library}')
    inside = os.path.join(library, DESCRIPTOR_BASENAME)
    if os.path.isfile(inside):
        with open(inside, 'rb') as stream:
            if stream.read(len(descriptor['raw']) + 1) != descriptor['raw']:
                _fail(f'{inside}: a descriptor is installed there but its bytes are not the '
                      'descriptor being verified (stale copy?)')

    listed = {entry['rel'] for entry in descriptor['files']}
    index_rel = LIBRARY_INDEX_REL
    if index_rel not in listed:
        _fail(f'{descriptor["path"]}: files[] does not list {LIBRARY_INDEX_REL}; the library '
              'index must be covered by the descriptor file list')
    index_path = os.path.join(library, index_rel)
    index = _object(_read_json_file(index_path, 'library index', MAX_LIBRARY_INDEX),
                    f'library index {index_path}')
    unknown = set(index) - {'schema', 'titleId', 'gameContentRoot', 'moduleSetDigest', 'directABI',
                            'translatorClosure', 'sourceParserVersion', 'runtimeCompilerABI',
                            'profileHash', 'entries'}
    if unknown:
        _fail(f'{index_path}: unknown key(s) {sorted(unknown)}')
    if _string(index.get('schema'), f'{index_path}.schema') != LIBRARY_SCHEMA:
        _fail(f'{index_path}.schema is not {LIBRARY_SCHEMA}')
    for key, want in (('titleId', descriptor['title_id']),
                      ('gameContentRoot', descriptor['game_content_root']),
                      ('moduleSetDigest', descriptor['module_set_digest']),
                      ('translatorClosure', descriptor['translator_closure']),
                      ('runtimeCompilerABI', descriptor['runtime_compiler_abi']),
                      ('profileHash', descriptor['profile_hashes'][0])):
        if _string(index.get(key), f'{index_path}.{key}') != want:
            _fail(f'{index_path}.{key} does not match the descriptor')
    if _integer(index.get('directABI'), f'{index_path}.directABI', 1) != descriptor['direct_abi']:
        _fail(f'{index_path}.directABI does not match the descriptor')
    if _integer(index.get('sourceParserVersion'), f'{index_path}.sourceParserVersion', 1) != \
            descriptor['source_parser_version']:
        _fail(f'{index_path}.sourceParserVersion does not match the descriptor')
    entries = _array(index.get('entries'), f'{index_path}.entries')
    if not entries:
        _fail(f'{index_path}.entries is empty')

    program_files = sorted(rel for rel in listed if rel.endswith('.program'))
    seen = set()
    checked = []
    for number, entry in enumerate(entries):
        checked_entry = _check_index_entry_fields(index, number, entry)
        if checked_entry['path'] in seen:
            _fail(f'{index_path}: duplicate entry path {checked_entry["path"]!r}')
        seen.add(checked_entry['path'])
        full = os.path.join(library, checked_entry['path'])
        raw = _read_file(full, f'library entry {checked_entry["path"]}', PCACHE_MAX_PAYLOAD + PCACHE_HEADER)
        payload, digest = decode_container(raw, checked_entry['identity'], f'library entry {checked_entry["path"]}')
        if digest != checked_entry['resultSha256']:
            _fail(f'{checked_entry["path"]}: payload SHA-256 does not match resultSha256 in the index')
        if sha256_bytes(raw) != checked_entry['sha256']:
            _fail(f'{checked_entry["path"]}: file SHA-256 does not match the index')
        if len(raw) != checked_entry['size']:
            _fail(f'{checked_entry["path"]}: file size does not match the index')
        result = validate_result(payload, f'library entry {checked_entry["path"]}', descriptor=descriptor)
        if 'shaderStorageRequirements' in descriptor['doc']:
            actual = storage_stages_for_result(result)
            if actual & ~descriptor['storage_requirements']['writeStages']:
                _fail(f'{checked_entry["path"]}: shaderStorageRequirements omits scanned write stages')
        checked.append(checked_entry)
    if sorted(seen) != program_files:
        _fail(f'{index_path}: entries differ from the descriptor file list '
              f'(index {sorted(seen)}, descriptor {program_files})')
    return {
        'library': library,
        'entries': len(checked),
        'bytes': sum(entry['size'] for entry in checked),
        'profileHash': descriptor['profile_hashes'][0],
        'directABI': descriptor['direct_abi'],
    }


def _artifact_graphics(pack, descriptor_path):
    artifact_path = os.path.join(pack, 'game-build-artifact.json')
    if not os.path.isfile(artifact_path):
        _fail(f'{pack}: no game-build-artifact.json; a direct pack must declare {GRAPHICS_FEATURE}')
    artifact = _object(_read_json_file(artifact_path, 'build artifact', MAX_DESCRIPTOR),
                       f'build artifact {artifact_path}')
    features = _array(artifact.get('requiredFeatures'), f'{artifact_path}.requiredFeatures')
    if GRAPHICS_FEATURE not in features:
        _fail(f'{artifact_path}: requiredFeatures does not contain {GRAPHICS_FEATURE}')
    extensions = _object(artifact.get('extensions'), f'{artifact_path}.extensions')
    extension = _object(extensions.get(GRAPHICS_FEATURE), f'{artifact_path}.extensions.{GRAPHICS_FEATURE}')
    if set(extension) != {'manifest', 'sha256', 'bytes'}:
        _fail(f'{artifact_path}: extension must contain exactly manifest, sha256, bytes')
    if _string(extension.get('manifest'), f'{artifact_path}.extensions.manifest') != DESCRIPTOR_REL:
        _fail(f'{artifact_path}: extension.manifest is not {DESCRIPTOR_REL}')
    want_sha = _sha_text(extension.get('sha256'), f'{artifact_path}.extensions.sha256')
    want_bytes = _integer(extension.get('bytes'), f'{artifact_path}.extensions.bytes', 1, MAX_DESCRIPTOR)
    data = _read_file(descriptor_path, 'graphics descriptor', MAX_DESCRIPTOR)
    if len(data) != want_bytes:
        _fail(f'{artifact_path}: extension.bytes {want_bytes} != descriptor length {len(data)}')
    if sha256_bytes(data) != want_sha:
        _fail(f'{artifact_path}: extension.sha256 does not match the packed descriptor')
    return artifact


def verify_pack(pack_root):
    """Check a packed revision (manifest/index/artifact + shaders/vulkan/direct/). This is the
    last host gate before deployment: every listed file must be indexed with the same bytes,
    nothing may be unlisted, and identity fields must derive from the real index."""
    pack_root = os.path.abspath(pack_root)
    if not os.path.isdir(pack_root):
        _fail(f'pack does not exist: {pack_root}')
    manifest_path = os.path.join(pack_root, 'manifest.json')
    if not os.path.isfile(manifest_path):
        _fail(f'{pack_root}: no manifest.json (an incomplete pack must not be packed)')
    manifest = _object(_read_json_file(manifest_path, 'pack manifest', MAX_DESCRIPTOR),
                       f'pack manifest {manifest_path}')
    if manifest.get('version') != 2:
        _fail(f'{manifest_path}: manifest version is not 2')
    index_path = os.path.join(pack_root, 'index.tsv')
    index_raw = _index_bytes(index_path)
    if sha256_bytes(index_raw) != manifest.get('index_sha256'):
        _fail(f'{index_path}: does not match manifest.index_sha256')
    rows = read_index(index_path)
    row_map = rows_map(rows)
    if DESCRIPTOR_REL not in row_map:
        _fail(f'{index_path}: {DESCRIPTOR_REL} is not an index row; the fixed descriptor '
              'location must be a packed file')

    _reject_linked_path(pack_root, DIRECT_PREFIX, f'pack {pack_root}')
    descriptor_path = os.path.join(pack_root, DESCRIPTOR_REL)
    descriptor = load_descriptor(descriptor_path)
    descriptor_size, descriptor_digest = row_map[DESCRIPTOR_REL]
    if descriptor_size != descriptor['bytes'] or descriptor_digest != descriptor['sha256']:
        _fail(f'{index_path}: {DESCRIPTOR_REL} row ({descriptor_size} B, {descriptor_digest}) '
              'does not match the packed descriptor bytes')
    artifact = _artifact_graphics(pack_root, descriptor_path)
    if _string(artifact.get('titleId'), f'{pack_root}: artifact titleId').upper() != descriptor['title_id']:
        _fail(f'{pack_root}: graphics descriptor titleId {descriptor["title_id"]} is not the '
              'artifact titleId')

    missing = [entry['path'] for entry in descriptor['files'] if entry['path'] not in row_map]
    if missing:
        _fail(f'{pack_root}: descriptor lists file(s) that index.tsv does not contain: {missing[:8]}')
    direct_dir = os.path.join(pack_root, DIRECT_PREFIX)
    for entry in descriptor['files']:
        size, digest = row_map[entry['path']]
        if entry['size'] is not None and entry['size'] != size:
            _fail(f'{pack_root}: {entry["path"]}: descriptor size {entry["size"]} != index {size}')
        if entry['sha256'] != digest:
            _fail(f'{pack_root}: {entry["path"]}: descriptor SHA-256 != index SHA-256')
    # The indexed direct tree must be exactly the descriptor's list plus the descriptor itself;
    # an extra indexed path without a file would otherwise be invisible to a disk walk.
    indexed_direct = {path for path in row_map if path.startswith(DIRECT_PREFIX)}
    expected_direct = {DESCRIPTOR_REL} | {entry['path'] for entry in descriptor['files']}
    if indexed_direct != expected_direct:
        extra = sorted(indexed_direct - expected_direct)
        absent = sorted(expected_direct - indexed_direct)
        _fail(f'{pack_root}: index rows under {DIRECT_PREFIX} do not match the descriptor '
              f'(extra={extra[:8]}, missing={absent[:8]})')
    verify_descriptor_files(direct_dir, descriptor['files'], f'pack {pack_root}')
    for entry in descriptor['files']:
        size = row_map[entry['path']][0]
        disk = os.path.getsize(os.path.join(direct_dir, entry['rel']))
        if disk != size:
            _fail(f'{pack_root}: {entry["path"]}: index says {size} bytes, on disk {disk}')
    verify_program_files(direct_dir, descriptor['files'], descriptor)

    derived_root = index_content_root(rows)
    if derived_root != descriptor['game_content_root']:
        _fail(f'{pack_root}: gameContentRoot {descriptor["game_content_root"]} is not the one '
              f'derived from index.tsv ({derived_root})')
    # moduleSetDigest binds the modules/ rows. A pack has them; the installed revision does not
    # (they travel in the APK), so there it can only be reported, not recomputed.
    derived_modules = index_module_digest(rows) if any(path.startswith('modules/') for path, _, _ in rows) else None
    if derived_modules is not None and derived_modules != descriptor['module_set_digest']:
        _fail(f'{pack_root}: moduleSetDigest {descriptor["module_set_digest"]} is not the one '
              f'derived from index.tsv ({derived_modules})')

    library = None
    if DIRECT_PREFIX + LIBRARY_INDEX_REL in row_map:
        library = verify_library(os.path.join(pack_root, DIRECT_PREFIX), descriptor)
    return {'pack': pack_root, 'files': len(descriptor['files']),
            'gameContentRoot': derived_root, 'moduleSetDigest': derived_modules,
            'library': library}


def verify_program_files(direct_dir, files, descriptor):
    """Every listed .program must sit at <directABI>/<profileHash>/<identity>.program and be a
    S2APC1 container for that identity whose payload is a valid s2a-direct-program-v1 envelope
    bound to this descriptor. This runs even when no library index is present, so deleting the
    index cannot turn content checks off, and renaming a container to another extension cannot
    hide it from parsing."""
    allowed_dirs = {f'{descriptor["direct_abi"]}/{profile}'
                    for profile in descriptor['profile_hashes']}
    checked = 0
    for entry in files:
        rel = entry['rel']
        if rel == LIBRARY_INDEX_REL:
            continue
        if not rel.endswith('.program'):
            _fail(f'{descriptor["path"]}: {entry["path"]!r} is not a .program or the library '
                  'index; a direct tree carries no other file type')
        name = rel.rsplit('/', 1)[-1]
        identity = _identity_from_name(name, entry['path'])
        parent = rel.rsplit('/', 1)[0] if '/' in rel else ''
        if parent not in allowed_dirs:
            _fail(f'{descriptor["path"]}: {entry["path"]!r} is not under '
                  f'<directABI>/<profileHash>/ ({sorted(allowed_dirs)}); the runtime derives that '
                  'path from its own ABI/profile')
        raw = _read_file(os.path.join(direct_dir, rel),
                         f'program {entry["path"]}', PCACHE_MAX_PAYLOAD + PCACHE_HEADER)
        payload, _ = decode_container(raw, identity, f'program {entry["path"]}')
        validate_result(payload, f'program {entry["path"]}', descriptor=descriptor,
                        expected_profile=parent.split('/', 1)[1])
        checked += 1
    return checked


def verify_pack_files(pack_root, descriptor, rows):
    """Check used by make_pack right after it writes the index: every descriptor file must be an
    index row with the same size/SHA-256 and the same bytes on disk, nothing may exist under the
    direct directory that the descriptor does not list, and every listed program must actually be
    a valid program container (not just a matching hash)."""
    pack_root = os.path.abspath(pack_root)
    files = descriptor['files']
    row_map = rows_map(rows) if not isinstance(rows, dict) else rows
    if DESCRIPTOR_REL not in row_map:
        _fail(f'{pack_root}: {DESCRIPTOR_REL} is not in the pack index')
    direct_dir = os.path.join(pack_root, DIRECT_PREFIX)
    _reject_linked_path(pack_root, DIRECT_PREFIX, f'pack {pack_root}')
    for entry in files:
        if entry['path'] not in row_map:
            _fail(f'{pack_root}: descriptor lists {entry["path"]!r} but the pack index does not contain it')
        size, digest = row_map[entry['path']]
        if entry['size'] is not None and entry['size'] != size:
            _fail(f'{pack_root}: {entry["path"]}: descriptor size {entry["size"]} != index {size}')
        if entry['sha256'] != digest:
            _fail(f'{pack_root}: {entry["path"]}: descriptor SHA-256 != index SHA-256')
    # File existence/hash first (it reports a missing file as a verification error, not OSError),
    # then the indexed length, which must hold even when the descriptor omits `size`.
    verify_descriptor_files(direct_dir, files, f'pack {pack_root}')
    for entry in files:
        size = row_map[entry['path']][0]
        disk = os.path.getsize(os.path.join(direct_dir, entry['rel']))
        if disk != size:
            _fail(f'{pack_root}: {entry["path"]}: index says {size} bytes, on disk {disk}')
    verify_program_files(direct_dir, files, descriptor)
    if DIRECT_PREFIX + LIBRARY_INDEX_REL in row_map:
        verify_library(direct_dir, descriptor)


# ---------------------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)

    build_parser = commands.add_parser('build', help='create an immutable library + descriptor from a capture')
    build_parser.add_argument('--capture', required=True, help='directory with capture.v1.json')
    build_parser.add_argument('--out', required=True, help='new library root (packed at ' + DIRECT_PREFIX + ')')
    build_parser.add_argument('--descriptor', required=True, help='new graphics.v1.json (outside --out)')
    build_parser.add_argument('--title-id', required=True, help='16-hex application id')
    build_parser.add_argument('--index', help='index.tsv of the pack that will carry the library; '
                                              'derives gameContentRoot and moduleSetDigest')
    build_parser.add_argument('--game-content-root', help='sha256-<64> override (checked against --index)')
    build_parser.add_argument('--module-set-digest', help='sha256-<64> override (checked against --index)')
    build_parser.add_argument('--direct-abi', type=int, help='must match the captured requests')
    build_parser.add_argument('--profile-hash', help='must match the captured requests')
    build_parser.add_argument('--translator-closure', help='must match the captured requests')
    build_parser.add_argument('--identity', help='compiler identity JSON (cli identity); the captured '
                                                'closure must be this build')

    describe_parser = commands.add_parser('describe', help='derive a descriptor from a read-only pack')
    describe_parser.add_argument('--pack', required=True)
    describe_parser.add_argument('--descriptor', required=True)
    describe_parser.add_argument('--identity', required=True,
                                 help="compiler identity JSON ('cli identity' of the build packed into the APK)")

    verify_parser = commands.add_parser('verify', help='check a library or a packed revision')
    verify_target = verify_parser.add_mutually_exclusive_group(required=True)
    verify_target.add_argument('--lib', help='library root (content of ' + DIRECT_PREFIX + ')')
    verify_target.add_argument('--pack', help='packed revision root')
    verify_parser.add_argument('--descriptor', help='descriptor path; defaults to <lib>/graphics.v1.json')
    verify_parser.add_argument('--index', help='index.tsv of the pack that ships this library: rows and identity fields')

    args = parser.parse_args(argv)
    try:
        if args.command == 'build':
            rows = read_index(args.index) if args.index else None
            result = build(args.capture, args.out, args.descriptor, args.title_id,
                           rows=rows,
                           game_content_root=args.game_content_root,
                           module_set_digest=args.module_set_digest,
                           direct_abi=args.direct_abi,
                           profile_hash=args.profile_hash,
                           translator_closure=args.translator_closure,
                           identity=args.identity)
            print(f"built {result['entries']} program(s), {result['bytes']} bytes -> {result['library']}")
            print(f"descriptor: {result['descriptor']} "
                  f"({result['profileHash']}, gameContentRoot {result['gameContentRoot'][:19]}...)")
            return 0
        if args.command == 'describe':
            result = describe_pack(args.pack, args.descriptor, args.identity)
            print(f"descriptor: {result['descriptor']} ({result['files']} offline files)")
            return 0
        if args.pack:
            result = verify_pack(args.pack)
            print(f"direct pack ok: {result['pack']}: {result['files']} direct file(s), "
                  f"directABI {result['library']['directABI'] if result['library'] else '?'}, "
                  f"gameContentRoot {result['gameContentRoot']}")
            if result['library']:
                print(f"  library: {result['library']['entries']} program(s), "
                      f"{result['library']['bytes']} bytes, {result['library']['profileHash']}")
            return 0
        library = os.path.abspath(args.lib)
        descriptor_path = args.descriptor or os.path.join(library, DESCRIPTOR_BASENAME)
        descriptor = load_descriptor(descriptor_path)
        result = verify_library(library, descriptor)
        if args.index:
            rows = read_index(args.index)
            if index_content_root(rows) != descriptor['game_content_root']:
                _fail(f'{args.index}: gameContentRoot is not the one the descriptor claims')
            if index_module_digest(rows) != descriptor['module_set_digest']:
                _fail(f'{args.index}: moduleSetDigest is not the one the descriptor claims')
            row_map = rows_map(rows)
            missing = [entry['path'] for entry in descriptor['files'] if entry['path'] not in row_map]
            if missing:
                _fail(f'{args.index}: descriptor lists file(s) that are not in the index: {missing[:8]}')
            for entry in descriptor['files']:
                size, digest = row_map[entry['path']]
                if (entry['size'] is not None and entry['size'] != size) or entry['sha256'] != digest:
                    _fail(f'{args.index}: {entry["path"]} does not match the index row')
        print(f"direct library ok: {result['library']}: {result['entries']} program(s), "
              f"{result['bytes']} bytes, directABI {result['directABI']}, {result['profileHash']}")
        return 0
    except LibraryError as error:
        print(f'direct library: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())

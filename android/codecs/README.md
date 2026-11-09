# Android codecs

`build.sh --prepare-sources [local-directory]` explicitly prepares pinned source
archives. Without a directory this preparation command may download; with a
directory it only imports locally supplied tarballs and never falls back to the
network. Normal `build.sh` and `--avc-only` builds are offline and fail when a
source archive is missing or fails its pinned identity check. They build arm64-v8a/API29 static
archives with NDK 29.0.14206865. Its stdout contains archive paths only. The
NativeAOT static build links all three into `libs2arm_android.so`.

libavc is AOSP `android-16.0.0_r4`, commit
`d380d04a78c6c08702e36aca86d0fd4baee390f9`. Its canonical content SHA is pinned in `build.sh`.
Gitiles archives have no outer directory. A verified local source archive can be
provided with `S2A_AVC_ARCHIVE`; this never changes the expected content hash. To prepare
only libavc, run `bash android/codecs/build.sh --avc-only` with
`ANDROID_HOME`, `S2A_BUILD_CACHE` and CMake 3.31.6 configured. First prepare all three sources separately (or pass a verified local libavc
archive for `--avc-only`). Gates and tests never download.

The default codec and Q19 cache is `$HOME/.cache/s2arm-build`, outside the
checkout. Set `S2A_BUILD_CACHE` to override it; if neither it nor `HOME` is
available, the builder fails rather than using the checkout. The Docker builder
explicitly uses `/cache` for its persistent volume.

The source cache is content addressed at
`$S2A_BUILD_CACHE/codecs/sources/<sha256>.tar.gz` and verified on every build.
Artifact manifests verify the library, headers and notices before reuse; corrupt
or incomplete cache entries fail explicitly. Each codec has an independent
recipe key containing its source SHA, exact NDK/compiler identity, ABI/API and
recipe. libavc also includes CMake identity/options, the shared `cache.sh` and
`build.sh` helper layer, and shim C/header bytes, so helper-side flag/toolchain
changes cannot silently reuse an old archive. Opus/VPX keys do not include the
libavc recipe or the dispatcher. `--cache-keys` prints the keys without building;
`--check-sources` verifies all prepared source archives without a compiler or
network access. A build with a missing shim source fails instead of publishing a
bare `libavcdec.a`. LICENSE, NOTICE and provenance are copied to APK assets.

## H.264 contract and validation

`AvcH264Decoder` lives under `android/cs/video/` and is registered by
`AndroidVideo.Register()` before `Exports.Initialize` (#1069); Apple keeps
VideoToolbox/libvpx via its own registration. Unregistered factories use a
decoder that reports `【红】` and returns false. There is no FFmpeg or
MediaCodec fallback. NativeAOT imports bind directly to `s2arm_android`; the C
provenance function retains the marker used by the APK license audit even
after ELF stripping.

The C ABI is version 1 (config 16, diagnostic 56, plane 32, view 120 bytes on
64-bit). All calls for a handle belong to the same Nvdec owner sequence. A
successful public Decode consumes one complete progressive picture, outputs one
I420 image with the current token, and lends bounded planes only until the next
Decode/Dispose. The caller finishes SurfaceWriter synchronously. Failure poisons
that context, clears its old view and never writes the guest destination.

The first implementation accepts 8-bit progressive 4:2:0 I/P/B, CABAC/CAVLC,
multiple slices and reference pictures, POC 0/2 and IDR sequence/size changes.
It rejects field/MBAFF, POC 1, partitioned/extension NALs, non-IDR SPS changes,
known high bit depth/chroma conflicts, malformed data and any libavc error
(including concealment). An in-band PPS declaring FMO/ASO
(`num_slice_groups_minus1 > 0`) is rejected by the shim's PPS parser. Nvdec's
H.264 `PictureInfo` has no FMO/ASO fields (`PictureInfo.Convert` always supplies
`NumSliceGroupsMinus1=0` and `FmoAsoEnable=false`), so a production stream whose
FMO PPS is not in-band cannot be pre-flighted; that is a documented limitation,
not a claimed production rejection. SPS scaling matrices in an original in-band
SPS are explicitly unsupported; matrices reconstructed in the PPS are validated.
Without original SPS, Nvdec metadata does not carry profile/bit depth: support
is limited to its 8-bit contract, not universal High10 detection. Dimensions are
bounded to 8192 per axis, AU to 64 MiB, parameter sets to 512 bytes. These bounds
are validation limits, not a claim that all such dimensions fit device memory.
The reconstruction work buffer covers the worst-case scaling-list syntax; a
reconstructed SPS or PPS over the native 512-byte limit is a `【红】` failure and
never an exception escaping `Decode`.

Identical reconstructed SPS/PPS are cached rather than resubmitted. PPS-only
updates prefix only PPS to the current AU, preserving DPB. SPS changes require
an IDR and a new decoder instance. In particular, resending the fixed Level31
SPS after libavc has corrected its level at coded 1920x1088 returns RES_CHANGED
and would reset DPB; the explicit resend mutation must fail the vector suite.

Default Android gate runs `tests/build_test.py` and `tests/managed_test.py`
without network access. The latter compiles real Nvdec/H264Decoder/SurfaceWriter,
PictureInfo.Convert and the reconstruction source; a bounded fixture guest-memory
manager supplies only the memory boundary. It tests routes, actual red logger
output through `tools/redline.py`, missing library/entry, no-backend no-write,
and cache cleanup after a write exception. An isolated baseline FFmpeg decoder
probe records the old avutil failure. It does not install FFmpeg into the APK.

Explicit second layer (pre-download the fixed archive with build.sh; no test
fetches source):

```sh
python3 android/codecs/tests/run.py --archive <pinned.tar.gz> --out <new-output>
python3 android/codecs/tests/run.py --archive <pinned.tar.gz> --out <new-output> --sanitize
python3 android/codecs/tests/run.py --archive <pinned.tar.gz> --out <new-output> --sanitize-shim
python3 android/codecs/tests/managed_test.py --library <output/libs2arm_android.so> --out <new-output>
```

The real-library suite requires ARM64, CMake and Clang. Run it on Linux ARM64
for ARMv8 implementation coverage; macOS ARM64 is additional host evidence.
`--sanitize` instruments the pinned library and shim with ASan/UBSan and executes
a native driver (no sanitizer injection into Python/.NET). Native fault injection
covers output flags/token/consumption/bounds and every callback/wrapper allocation.
A 16x16 slice-only IDR AU plus a 47-byte production-reconstructed PPS is run
under `--sanitize-shim` to cover the PPS-only prefix reallocation. An in-band PPS
declaring FMO and an AU whose second slice changes `slice_type` while every other
identity field is equal must be rejected by the input preflight (UNSUPPORTED /
CONTRACT). Display-order, swapped-UV and repeated-SPS mutations must be rejected.

Vectors are original CC0 mathematical patterns. `generate_vectors.py` explicitly
runs FFmpeg/libx264, captures exact tool versions/parameters/source SHA, and maps
reference display-order frames to Annex B AU decode indices using packet offsets.
The encoder's reference usage percentages show actual multiple-reference use and
B-reference pictures, beyond header declarations. They do not claim 16 distinct
references are simultaneously used or long-term-reference coverage.
`generate_reconstructed.py` independently decodes streams with production-emitted
headers and a PPS-only chroma-QP update at decode index 4; every original,
reconstructed and PPS-updated frame has separate Y/U/V and combined SHA values.
Changing initial QP would also change CABAC initialization, so that is not used
as a valid slice-preserving PPS update.

Generation is explicit, never part of gates. To regenerate header oracles, build
`tests/managed/Avc.Tests.csproj`, run its DLL with `<vector-dir> <header-output>`,
then `generate_reconstructed.py --headers <header-output>` using the recorded
reference tool version. Normal tests need neither encoder nor reference decoder.

Host evidence does not establish Bionic runtime/device performance, game playback,
iOS regression, memory/PSS peaks, long-term refs, or full H.264 conformance.
Device acceptance is separate and must use a leased device.

Sanitizer results must be read strictly: `--sanitize` instruments the upstream with recoverable UBSan so all findings
remain visible, while the shim and driver use `-fno-sanitize-recover=all`. An
exact pinned source/file/line/type list in `tests/upstream-ubsan.json` records
known upstream findings; any other diagnostic fails even on exit zero. Full
upstream UBSan is explicitly reported as not clean. The pinned
upstream has host UBSan findings (null-pointer offset and signed shifts) that
must be reported, not suppressed. `--sanitize-shim` is the separately labelled
scope: upstream instrumented with ASan, shim/driver with strict ASan+UBSan. It is
not a replacement for or a green verdict on full-upstream UBSan.

The `b-1080-crop` fixture has a 1920x1088 coded surface and a 1920x1080 original
cropped display. Its original display oracle and reconstructed coded-size oracle
are separate. Passing original cropped SPS as the requested coded-size native
contract is a negative test; the production Nvdec path uses its real reconstructed
uncropped SPS and compares all 1088 rows against the independent reference.

To seed an offline build, place `opus-1.6.1.tar.gz`, `libvpx-1.17.0.tar.gz` and
`libavc-d380d04a78c6c08702e36aca86d0fd4baee390f9.tar.gz` in one local directory,
then run the explicit preparation command against the cache used by the build.
Sources may also be imported into the Docker cache volume using the fixed build
image with that directory mounted read-only and `--network none`. Formal build
source preparation must finish before starting Gradle/NativeAOT.

Gitiles may emit different gzip/tar timestamps for the same libavc commit.
The libavc source verifier pins the complete sorted set of paths, file bytes,
entry types and permission bits, and rejects duplicate paths, links, traversal
and unexpected entries. It ignores archive timestamps and gzip headers. The
legacy archive-SHA cache is still accepted after the same content verification;
the build verifies its own private copy before extraction. Opus and VPX retain
their existing archive-byte pins.

The prior script included the entire dispatcher SHA in both Opus and VPX keys.
Adding libavc therefore invalidated unrelated artifacts, and its temporary-only
fetch path retried the network during the formal build. This is covered by
independent-key, missing-source, corrupt-source and forbidden-network fixtures.

## VP9 / AMediaCodec contract (#44)

`s2a_mc.c` is the AMediaCodec boundary for hardware VP9 decode, reached through
`McVp9Backend` (`android/cs/video/`), an `IVp9NativeBackend` implementation
registered by `AndroidVideo` ahead of the libvpx backend and gated by
`debug.s2a.video_hw` (`v[0]=='1'`; read inside `s2a_mc_probe`/`s2a_mc_create`,
matching the `debug.s2a.buffer_geometry` convention). It carries no pinned
third-party source — pure NDK `libmediandk`, compiled directly into
`libs2arm_android.so` (`s2a_android_host_sources`), so `build.sh` source
preparation does not cover it.

The decode contract differs from `s2a_avc`: output is written into caller-owned
Y/U/V planes (`s2a_mc_plane`: data/capacity/stride) passed to `s2a_mc_decode`,
not a borrowed native view. `McSurface` allocates the planes unmanaged and the
returned `Vp9NativePicture` borrows them, satisfying the backend contract that
the picture stays valid until the next call. The shim deinterleaves the codec's
actual output format reported by `AMediaCodec_getOutputFormat`
(`color-format`/`stride`/`slice-height`/`crop-*`): YUV420 planar copies rows,
semiplanar splits interleaved UV into separate U/V planes because
`Vp9NativePicture` exposes three independent planes. Byte order differs by
format: NV12 (0x15) is U-first while QCOM semiplanar 0x7FA30C00 is
`OMX_QCOM_COLOR_FormatYVU420SemiPlanar` — V-first (NV21), so the deinterleave
swaps U/V destinations for it. FLEXIBLE (0x7F420888) is only accepted as
semiplanar when the codec actually reports `stride` and `slice-height`;
missing keys or unknown color formats fail as unsupported rather than guessing
a layout. Reported color/stride/slice-height ride in `s2a_mc_diag` on failures
for per-device comparison.

`AMediaFormat_set*` returns `void` (NDK 29): only `getInt32` signals presence.
`AMEDIACODEC_INFO_*` dequeue results are normal flow control and retried with a
bounded budget (input 20×10 ms, output 10×10 ms per call). Hardware decoders
have pipeline latency: an accepted input with no output yet returns `0` with
`diag.output==0` — recoverable. `s2a_mc_wait` drains queued output without a
new AU; `McVp9Backend` feeds one job, then drains with a bounded total budget
until the job's own picture arrives (VP9 has no reordering — the k-th produced
frame answers the k-th shown AU), so one backend call still corresponds to the
current job's picture. Only two bounds mark a dead codec: input-slot
starvation and a run of more than `S2A_MC_MAX_EMPTY` (32) consecutive
no-output calls — the count resets on any produced frame, so sparse hidden
frames cannot accumulate into a false positive. `diag.output_token` records
the current call's input token, not the produced frame's. Frame-size changes
surface as `S2A_MC_RECONFIG` before input queueing — `McVp9Decoder` destroys
and recreates the codec at the new dimensions, so RECONFIG is only observable
through the raw C ABI.

The repacked frame's compressed header is a single `0x00` stand-in byte
(`Vp9NativeHeader.Write` ends with it): `tx_mode`, `reference_mode`,
per-job probability deltas and the `use_prev_frame_mvs` bit were all
injected out-of-band into libvpx by the hook — AMediaCodec has no such
channel and decodes the placeholder literally (tx_mode=ONLY_4X4,
reference_mode=SINGLE, no updates). Under the `IVp9NativeBackend`
contract the backend therefore reports honestly what the placeholder
cannot represent, and `Vp9NativePath` hands that job to the managed
decoder until the next key frame: `show_frame=0` jobs (intra-only hidden
frames — the hook surfaces the previous picture, hardware cannot),
inter jobs whose `TxMode`/`ReferenceMode` exceed the placeholder
semantics (intra frames are spec-forced to ONLY_4X4+SINGLE and need no
check), `use_prev_frame_mvs` jobs (the placeholder's first bit is the
flag and it is always 0 — the guest MV buffer also cannot hold this
frame's real MV field, so a managed takeover would only find a stale
value; `NeedsKeyFrame` marks the key frame as the point the dependency
edge safely closes), supplied entropy-probability tables that differ
from the spec defaults the codec's context always carries, and
non-key-frame size changes (a recreated codec has no reference state).
`mvsOut` and `countsOut` are metadata hardware cannot produce, so the
backend writes nothing (the guest buffers keep their upstream content;
the path's counts stay all-zero, which is the faithful value for a
`refresh_frame_context=0` frame) — nothing fabricated. Every 300 jobs
and at Dispose the backend logs a one-line outcome distribution
(`mc-vp9 outcome:` decoded/hidden/header/prevMv/probs/size/starved/error)
for real-device coverage measurement. After any decode-stage failure
the shim marks the context failed; the backend maps that to `Error`
once, and sustained starvation escalates to the same result so the
native path disables itself instead of spinning.

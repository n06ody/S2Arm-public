# Android host bring-up

## Supported devices / 支持的设备

| Tier / 档 | Devices / 设备 |
| --- | --- |
| Guaranteed / 保证 | Snapdragon 8 Gen 2 and newer (Adreno 740+); Dimensity 9000 and newer (Mali-G710 / Immortalis). Stock vendor driver, 64-bit (`arm64-v8a`), Vulkan 1.1+, Android 14+.<br>骁龙 8 Gen 2 及以上（Adreno 740 起）；天玑 9000 及以上（Mali-G710 / Immortalis）。原厂驱动、64 位（`arm64-v8a`）、Vulkan 1.1+、Android 14+。 |
| Best effort / 尽力 | Adreno 6xx–7xx, Dimensity 8000 series, Turnip (Mesa driver for Adreno), Samsung Xclipse.<br>Adreno 6xx–7xx、天玑 8000 系、Turnip（Adreno 的 Mesa 开源驱动）、三星 Xclipse。 |
| Separate track / 单独跑道 | Kirin with Maleoon GPU (HarmonyOS): served by the HarmonyOS port (`ohos/` and its integration branch), not by this APK.<br>麒麟 Maleoon GPU（鸿蒙）：走鸿蒙移植（`ohos/` 及其集成分支），不走本 APK。 |
| Not supported / 不支持 | GPUs that only expose Vulkan 1.0; 32-bit devices or systems.<br>只支持 Vulkan 1.0 的 GPU；32 位设备或系统。 |

- Android 14 is the policy floor and the package floor: minSdk 34 (Android 14).
  Android 14 是支持范围与包的下限：最低 Android 14（minSdk 34）。
- Dimensity (Mali / Immortalis) devices are verified by the community on real hardware.
  天玑（Mali / Immortalis）机型由社区真机验证。
- A GPU that only supports Vulkan 1.0 is rejected when the Vulkan device is created (`android/native/direct/vk_device.c`), and the player sees a readable reason on the failed start. Nothing is checked before the start.
  只支持 Vulkan 1.0 的 GPU 在建 Vulkan 设备时具名拒绝（`android/native/direct/vk_device.c`），玩家在启动失败时看到看得懂的原因；启动前不做检查。

This directory follows the multi-platform plan.
It builds an ARM64 diagnostic APK with Activity / SurfaceView lifecycle, JNI,
memory and rewrite probes, a Vulkan test presenter, and a gamepad snapshot using
the existing `s2a_vpad_read(int32_t[4])` ABI. Optional NativeAOT service builds
and the AAudio backend are described below. The integration branch also supplies
a per-game runtime; device compatibility and complete game behavior require
validation for each final build and device.

This README records the implemented bring-up and its verification scope.

Android community patches follow the same policy as Apple: only internal packs
built with `S2A_COMMUNITY_PATCHES=1` carry `modules/cpatch.tsv`; without
`S2A_CPATCH_ON` each patch uses the table's default, `S2A_CPATCH_ON=<ids>` enables
only those ids and `none` disables all. The Android app has no settings page for
this yet; set `S2A_CPATCH_ON` through the debug environment channel. This
source-level policy has not been verified on a device.

Shared shader-cache location, reset, write and periodic counter diagnostics are
off by default on every platform. Set `S2A_SHADER_CACHE_DIAG=1` in the host
process environment before starting it to enable them; failure reports remain
enabled.

The preview reports real native checks: runtime page size, a shared-memory alias
using Android shared backing and Linux `mremap(old_size=0)`, and a temporary
64 GiB / 1 GiB aligned reservation wholly below the 39-bit guest-address limit,
and the Bionic pthread key-table probe used by the Android rewrite target
(`native/tsd.c`: table offset, second-key re-check, direct slot write read back
by `pthread_getspecific`, independent slot on another thread).
The reservation and probe keys are released immediately. The Vulkan test
presenter draws clear colours and orientation markers; it is not connected to
Ryujinx game rendering. No game-specific
addresses, assets, firmware, keys, storage permissions, or network permission
are included.

## Local wireless (LDN) settings

The release game library settings panel includes **Local wireless (LDN)**.
Automatic is the default and writes no environment variables, preserving the
existing LAN behavior. Manual host uses `S2A_LDN_SERVER=127.0.0.1`,
`S2A_LDN_RELAY=1`, and `S2A_LDN_LISTEN=0.0.0.0`. Manual client uses the entered
reachable IPv4 as `S2A_LDN_SERVER` and `S2A_LDN_RELAY=0`. Manual mode performs
no automatic discovery or relay election. The port is fixed at TCP 30456.
Saving a client requires four dotted ASCII decimal octets (0–255, no leading
zeroes, hostname, IPv6 or port suffix). Empty/invalid input stays in the dialog
with a specific field error and does not update preferences. The environment
policy retains the empty-client `0.0.0.0` fallback only for legacy/Intent input,
so it cannot silently re-enable discovery.

**Test connection** optionally connects to the entered IPv4 on TCP 30456 on a
background thread with a 3-second timeout, then closes without sending payload.
This checks the actual relay port rather than ICMP ping. A successful connection
only proves that a TCP listener accepted it, not LDN protocol/game compatibility.
Failure is advisory: saving remains available when the relay/VPN is not up yet.
The result names its target, is cleared after edits, and cannot update a closed
dialog or a destroyed Activity.

All players must use the same relay; keep the relay app in the foreground.
Use reachable public or LAN addresses or an externally configured VPN; the app includes
no VPN SDK. Hosting listens on all network interfaces, exposing TCP 30456 to
reachable peers (including other interfaces); use trusted networks only.
Ensure TCP 30456 can reach the relay. Fully exit the app and reopen
after changing settings. Returning through the app icon or resuming a running
game retains its existing process settings.

Preferences persist per app package. `GameActivity.launch` copies them into a
cold launch Intent so the separate game process receives the library's current
snapshot without relying on cross-process SharedPreferences cache refresh.
The production startup applies it through Android `Os.setenv` before Host
creation, without `DebugEnvironment` or debug build flags. A nonempty existing
`S2A_LDN_SERVER` preserves all externally supplied LDN environment settings as a
unit. Direct cold starts without a snapshot read persisted settings.

Once-per-process application matches the supported game lifecycle:
`MainActivity.stopGame` admits native stop, `bridge.cpp` transfers to shared
`s2a_host_shutdown`, and `runtime/src/lifecycle.c` terminates with `_exit` (the
Android watchdog also exits on timeout). The library waits for the old game PID
to disappear before switching games. Activity recreation/resume is not a new
configuration window: shared `Host.LdnServer` is cached statically. The Java
10-second task-finish fallback alone does not prove process death; device
lifecycle validation remains required.

`python3 android/tests/ldn_settings_test.py` covers policy mapping, external
precedence, empty-client fallback, strict IPv4 validation, offline injected TCP
success/failure and port/timeout contracts, release wiring and negative controls.
`python3 android/tests/ldn_dialog_test.py` compiles the real settings dialog with
Android stubs and an offline connector: invalid input stays open, failed tests
allow saving, late callbacks respect dialog/Activity lifetime, and an ignored
address-error negative control fails. Java policy tests compile the real policy
with `javac`; Android wiring checks inspect the source. These do not replace APK
or device validation. Device
acceptance still needs a Release APK, a full restart, host/client environment
receipts, and two peers on a reachable relay.

## FPS overlay

In the game library, **Show FPS (successful frame submissions)** controls the
game overlay for this app package. It is off by default and persists across
launches. The existing `s2arm.fps=on|off` Intent override writes the same
`display` / `fps` preference; returning to the library refreshes its checkbox.
The library does not load the native runtime or start an overlay timer.

The overlay refreshes once per second. NVN direct sessions read the atomic
successful-present counter through `s2a_android_nvn_present_count`; Vulkan
guest-direct sessions and the legacy counter retain their existing sources.
Missing windows, acquire timeouts and failed presents do not add NVN samples.
This is a submission rate, not the screen refresh rate or GPU completion rate.
The first sample and a paused/stopping session show `FPS —`; a valid sample
with no new submissions shows `FPS 0.0`.

Host coverage: `bash android/core-runtime/fps-tests/test.sh` and
`python3 android/tests/nvn_present_count_test.py`. Diagnostic UI coverage uses
`-Ps2arm.testRunner=org.s2arm.android.FpsOverlayInstrumentation`; add the existing
debug-only `-Ps2arm.testAppIdSuffix=.w2` for an isolated install when needed.
The runner restores only the original FPS preference. Diagnostic samples are
unavailable and do not certify real NVN game frame rates.

The separate `android_fps_table` CTest exercises the 60 FPS patch table, not
the overlay sampler. Its enabled scenario explicitly sets `S2A_FPS60=1`;
leaving the variable unset preserves the game's native frame-rate policy.

The protected device runner requires a POSIX host (`fcntl`, `fork`, and
process-exit monitoring); native Windows Python cannot provide this lock.
Its `getprop` preflight accepts multiline property values, including boot
history, while still rejecting malformed, duplicate, or incomplete records.
This does not bypass debug-property admission or installation-state protection.

## Which build path to use

Docker is no longer the default way to build Android packages. The backend is
chosen by the host and neither direction falls back to the other (#1131, #1299):

| Host | Game package / APK build | Offline APK gate |
|---|---|---|
| macOS, Apple Silicon | host-native: `android/host/build.sh` (no Docker) | `android/host/gate-apk.sh` (`sandbox-exec`, network denied) |
| Linux x64 / arm64 | Docker image: `android/docker/build.sh` | `android/docker/gate-apk.sh` |
| macOS Intel, other systems | reported red (not verified) | — |

To build a playable game package, use the one-click entry
`bash pack/oneclick/oneclick.sh <source dir> --target android --keys <prod.keys> --out <private dir>`;
it picks the backend above (see `pack/targets/android/README.md`). On macOS the
native path needs .NET 10 SDK, JDK 17, Android SDK platform 36, build-tools
36.0.0, NDK 29.0.14206865 and CMake 3.31.6; run `bash android/host/prepare.sh`
once online to fetch them and write the receipt, then builds and the APK gate run offline.

On macOS, Docker (or Colima / OrbStack) is only needed for Linux-only work:
the aarch64 Linux runtime CTest (`android/linux_ctest.sh`, for tests
that need Linux POSIX / ELF and do not build on Darwin) and Linux NativeAOT
builds of the runtime library (the APK only accepts `linux-arm64` / `linux-x64`
AOT hosts). The sections below keep the Docker recipe for Linux / WSL hosts.

## Build with Docker (Linux / WSL hosts)

Pinned tools: JDK 17, Gradle 9.8.1, Android Gradle Plugin 9.4.1, SDK / target 36,
build-tools 36.0.0, NDK 29.0.14206865, CMake 3.31.6. The current package declares
minSdk 34 (Android 14, see "Supported devices" above). Only `arm64-v8a` is packaged. Native code queries the actual host page
size, and the library / APK are built with 16 KB alignment.

From the repository root:

```sh
docker build -t s2arm-android-build:1 android/docker
bash android/docker/build.sh
```

This manual `docker build` does not add the offline gate recipe label. To share
the labeled `linux/amd64` image with the APK gate, run `docker/prepare.sh`
instead of rebuilding the same tag manually. `docker/build.sh` only checks that
the image tag exists; it does not validate the recipe label.

The image build was verified on 2026-10-22 (about 6 minutes in WSL). Before
Gradle, the build runs `cs/build.sh` (.NET SDK and NativeAOT libraries) and
`openssl/build.sh` (OpenSSL 3.5.9 for .NET's crypto); in the single-library
build `cs/build.sh` also runs `codecs/build.sh` (libopus and libvpx). All of them
download pinned, checksum-verified sources on first use and then reuse the cache
volume, so the first build needs network access. `openssl/build.sh` needs the full Perl that
this image installs. The `dm-android-build:1` image from `xbl-android` has the
same Android toolchain but no Perl modules, so it can no longer build this
project. To keep an existing debug signing key, reuse its cache volume:

```sh
S2ARM_ANDROID_CACHE=dm-android-build-cache bash android/docker/build.sh
```

The default command runs `assembleDebug`, `lintDebug`, APK ABI / alignment checks,
and signature verification. It does not invoke adb, install, launch, enumerate
devices, mount USB, or use host networking. Output:
`android/app/build/outputs/apk/debug/app-debug.apk`.

Dependencies may be downloaded during the first build. Subsequent compilation
can explicitly use `docker/build.sh --offline :app:assembleDebug`. Gradle caches
and the debug key persist in the `s2arm-android-build-cache` Docker volume;
`S2ARM_ANDROID_CACHE` selects another volume. Retain that cache to retain the
debug signing identity. The source mount is this checkout, and containers run as
the host UID. No files in the reference checkout are modified.

## Prepare the offline APK gate (Linux hosts)

On macOS the `apk` gate step skips the Docker image and runs
`android/host/gate-apk.sh` instead (prepare with
`android/host/prepare.sh`); this section applies to Linux hosts.

The Linux `apk` gate uses an x86-64 Docker build image and a separate persistent
dependency volume. On Apple Silicon it runs under x86-64 emulation. Before the
first gate run, install Docker and the GitHub CLI (`gh`), sign in to `gh`, and
run this explicit **networked** preparation from the repository root:

On Apple Silicon, enable Rosetta for the `linux/amd64` container. In one Colima
QEMU configuration the .NET SDK failed during MSBuild target-framework
assessment; behavior can depend on the emulator and version. Check your host:

- OrbStack: Rosetta is normally enabled; check **Settings → System → Use Rosetta
  to run Intel code** ([OrbStack settings](https://docs.orbstack.dev/settings)).
- Colima: use an Apple Virtualization (`vz`) VM with Rosetta, for example
  `colima start --vm-type vz --vz-rosetta` (macOS 13+ and Apple Silicon). VM
  type changes may require creating a new VM; preserve needed data before doing
  so ([Colima configuration](https://github.com/abiosoft/colima/blob/main/embedded/defaults/colima.yaml)).
- Docker Desktop: under **Settings → General**, use the Apple Virtualization
  framework and enable **Use Rosetta for x86_64/amd64 emulation on Apple
  Silicon** ([Docker settings](https://docs.docker.com/desktop/settings-and-maintenance/settings/)).
- Linux arm64 has no Apple Rosetta: `linux/amd64` needs a working binfmt/QEMU
  setup, which can be slower or incompatible. Use a native amd64 builder if
  the probe fails ([Docker multi-platform builds](https://docs.docker.com/build/building/multi-platform/)).

After the image is available, preparation bootstraps the pinned .NET SDK in
`/cache` and tests both its startup and probe project target-framework
assessment in an amd64 container before downloading the remaining build inputs
or starting the long build. Bootstrap failures retain their own diagnostics;
a failed .NET probe stops with a Rosetta/runtime hint. A successful short probe
is not a guarantee that every later build step will pass.

```sh
bash android/docker/prepare.sh
GATE_ONLY=apk bash android/gate.sh
```

Preparation builds `s2arm-android-build:1` when absent or when its Dockerfile
fingerprint differs, using the Dockerfile's pinned base-image digest. It creates
`s2arm-android-build-cache` if needed and populates it with .NET/NuGet, Gradle, codec and
OpenSSL dependencies. GitHub source archives are fetched on the host through
`gh`; the existing build scripts verify their pinned hashes. Other pinned
sources are downloaded by the build container. Preparation builds and checks
the diagnostic APK once, prints wall time, image bytes and cache KiB, and may
be repeated: it retains and reuses the image and volume. It uses a temporary
copy of tracked sources and removes the copy on exit. No game files are needed.

The latest cold measurement (2026-10-28, commit `eb66111f9`, Apple M2 Max,
OrbStack, x86-64 emulation): an initially absent image and empty volume with
`--no-cache` image layers took 1197 seconds; the image's reported logical size
was 4,031,284,086 bytes and the cache volume used 2,689,736 KiB. The base image and host caches already
existed, so this is not a completely empty Docker installation; actual time
and physical disk use vary with the machine and its existing layers. An older
`4fe0b7a4f` preparation script measured 970 seconds; neither number is a cold
measurement of the current head.

The gate itself always uses `--network none`: a missing Docker binary, image,
cache volume, or completion receipt reports the APK check as not run, prints
`待宿主证\t安卓宿主\tapk` with tab separators, and exits successfully, with
preparation instructions. A successful build prints `已证宿主项\t安卓宿主\tapk`
with tab separators; host proof is tied to the tested commit. An existing image
with the wrong recipe label, a mismatched or corrupt receipt, unreadable dependency inputs,
Docker access errors, and build failures are errors, not skips. Preparation
removes the old receipt before updating the cache and atomically writes a new one only after success;
a manually warmed legacy volume must therefore run preparation once. The
receipt includes the image ID, Dockerfile fingerprint and a conservative hash
of the probe/AOT project-reference closure, ancestor build configuration,
Android Gradle and codec preparation inputs. Re-run preparation when these
inputs change. For an isolated
setup, set both `S2ARM_ANDROID_IMAGE` and `S2ARM_ANDROID_CACHE` consistently
for preparation and the gate. `S2ARM_ANDROID_PREPARE_NO_CACHE=1` builds a
missing or outdated image without Docker layer cache; it does not delete an
existing volume. Preserve the cache if its debug signing identity is needed.

The Android hook is routed for the existing Android, rewriter and native
coverage plus the shared native-import files `services/S2A.Services/Native.cs`,
`InputPump.cs`, `ControllerFeedback.cs`, `services/aot/`, `services/build.mk`,
`runtime/include/s2a_native.h`, `compat/tree/src/Ryujinx.Graphics.Nvdec/` (decoder seam),
and `graphics/vulkan/wsi/AndroidWindow.cs`. Other services and Ryujinx changes do not
trigger this hook solely because they are in those trees. This explicit file
list is owned by the native team and must be updated if the import surface moves.

## Build on the host without Docker (macOS Apple Silicon; Linux by hand)

The same steps run directly when the SDK, JDK 17 and .NET 10 are installed on the
host:

```sh
export ANDROID_HOME=$HOME/Android/Sdk
bash android/host/build.sh
```

`host/build.sh` resolves the SDK from `ANDROID_HOME`, `sdk.dir` in the ignored
`local.properties`, or `~/Android/Sdk`, and fails with the exact `sdkmanager`
command when a pinned package is missing. JDK 17 is located by
`tools/jdk17.sh` (`S2A_JAVAC`, `JAVA_HOME`, `java_home -v 17`, Homebrew, the
distro `/usr/lib/jvm/*17*` layout); .NET 10 by `S2A_ANDROID_DOTNET` or `dotnet`
on `PATH`. Nothing is installed automatically: a missing JDK, .NET SDK or SDK
package is reported red instead of being skipped.

`S2A_BUILD_CACHE` (default `$HOME/.cache/s2arm-build`) holds the OpenSSL source build
and, when `S2A_ANDROID_DOTNET` is unset, the pinned .NET SDK; NuGet packages
default to `~/.nuget/packages`. Gradle keeps its normal host caches, so the debug
signing identity is the one in `~/.android`. Passing Gradle tasks (for example
`:app:assembleDebug --offline`) runs only those tasks, exactly like
`docker/build.sh` with arguments.

## Android Studio on Windows

Open `android` as the project and select JDK 17 or later compatible with
the pinned Gradle / AGP versions. Install SDK 36, NDK 29.0.14206865, and
CMake 3.31.6 in the Windows SDK Manager. The Windows SDK path belongs in the
ignored `local.properties`; do not point a Linux build at Windows NDK binaries.
The checked-in Gradle wrapper pins the distribution and its SHA-256. Use
**Build > Make Project** to build without deploying to the connected device.
Android Studio and Docker have separate default debug signing keys.

## Device-free tests

On Linux with CMake 3.18+, a C/C++ compiler, JDK 17, pinned offline rewriter
sources (`S2A_REWRITER_DEPS`) and Python 3.12+ with Capstone 5.0.7 / LZ4 4.4.5.
Both scripts choose `S2A_PY`, then the compatible explicit `PY`, then
`$HOME/.s2arm/venv/bin/python3`, matching the shared rewriter gate's fixed venv.
An explicitly empty or invalid choice fails rather than falling back to PATH:

```sh
bash android/test.sh
python3 android/verify-apk.py android/app/build/outputs/apk/debug/app-debug.apk
```

拆线程新套件的默认门禁/单跑登记见 `tests/gate_not_default.tsv`。

The device-free gate also runs on macOS with the same rewriter dependencies:

```sh
S2A_JAVAC=/path/to/jdk17/bin/javac \
S2A_REWRITER_DEPS=/path/to/prepared/rewriter-deps \
S2A_PY=/path/to/pinned-venv/bin/python3 \
bash android/gate.sh
```

Both `test.sh` and `gate.sh` always execute the real C/Python package matrix,
including full package byte/SHA comparisons, rejection before old-package
replacement, and Java consumers. `S2A_TEST_NO_NATIVE=1` skips only the Linux
runtime CTest step (`native/memory.c` needs `mremap`); it does not skip the
portable rewriter build or matrix. The matrix step in `test.sh` clears
`S2A_REWRITER_C` / `S2A_REWRITER_BUNDLED` only in its subshell and uses its own
fresh cache, forcing a build from this commit's source. Production selection
and direct developer use of an explicit C CLI remain unchanged. Before JDK or
test execution, `gate.sh` checks the selected Python version, reads package
versions from `tools/rewriter-requirements.txt`, and actually exercises
`lz4.block` compression/decompression and Capstone AArch64 decoding. Missing,
broken or wrong-version Python dependencies produce one red diagnostic and exit
before the matrix. All Python commands in `test.sh` and the real backend
subprocesses use this same interpreter (`PY` and `S2A_PY` are exported together).
The offline `S2A_REWRITER_DEPS` tree contains C sources; it does not install or
supply Python extension modules. Missing CMake, a compiler or pinned C sources
still fails through the existing resolver. The gate never installs or downloads
dependencies; prepare them explicitly before running it (see
`tools/rewriter-requirements.txt` and `runtime/rewriter/README.md`). These host
checks do not require the Android
SDK's packaging CMake 3.31.6 or constitute an APK/device run.

Native tests exercise an actual 64 GiB reservation / page commit, invalid
allocation inputs, coherent aliases of shared backing, concurrent input
snapshots, reset, and the Bionic key-table search on synthetic `bionic_tls`
blocks (key-flag masking, seq check, ambiguity, scan limit). The on-device
probe itself needs Bionic and runs only in the APK. The plain Java test covers axis direction, dead zone,
clamping, key / hat independence, and reset. They require no game files,
network, emulator, or physical device.

Initial verification on WSL: `assembleDebug` / `lintDebug` succeeded using
`dm-android-build:1`; both CTest cases and the plain Java test passed; APK
signature, ARM64 ABI, ELF segment alignment, and ZIP payload alignment passed.
The remaining lint warnings
concern newer available tool / SDK versions and the deliberate ARM64-only ABI.
The standalone Dockerfile is adapted from the reference recipe; this build used
the already installed image rather than rebuilding that image.

Device check (2026-10-21, Redmi K50 Ultra, Android 15, 4 KB pages): the
preview reported a shared-memory alias, the 39-bit / 64 GiB window, a Bionic key
table at offset 0x0 with independent per-thread slots, and an Adreno 730
Vulkan 1.1 surface at 1220 × 2712. Going to the home screen and back detached
and recreated the surface in the same process without a crash. The first
device run found that `bionic_tls` lives in a mapping shorter than 16 KB, so
the key-table scan is clamped to the readable span from `/proc/self/maps`.
No controller, rotation, 16 KB-page device, or long-running behavior has been
checked. On 2026-10-22 the same phone also passed the rewrite self-test (6 rewrite
kinds x 2 guest threads, key table 0x0) and the signal red-zone probe.

## On-device rewrite self-test

`tools/device_selftest.sh` checks that `S2A_TARGET=android` rewriter output really
executes on bionic. It rewrites a synthetic module (`tools/selftest_nso.py`: one
`mrs tpidr_el0`, `msr tpidr_el0`, `mrs tpidrro_el0`, x18 write, x18 read and `svc`
each) with the current `runtime/rewriter`. It then imports the module into the debug
app's private `files/selftest` with `run-as` and restarts the app. The preview maps the
module like the shared loader does: anonymous memory, key page from the measured
bionic slot offsets, then text and patch areas sealed R|X. It stamps every key
on the thread first, then calls each rewritten function on two threads. A failed
key-table probe or a module without `# TARGET android` is reported as a failure; there
is no fallback.

```sh
SERIAL=<device serial> PY=<python with capstone + lz4> \
  bash android/tools/device_selftest.sh
```

The script names the device explicitly, never clears logcat and touches only
`org.s2arm.android`. On a shared phone, coordinate access before running it.
`native/module.c` mapping is also host-tested (`tests/module_test.c`; pass
`<dir> <name>` to map real rewriter output on the host).

## Vulkan presentation (render thread)

`native/presenter.cpp` owns the Vulkan device, swapchain and frame loop on a
render thread. Each frame is a time-varying clear colour plus two orientation
markers: a red square at the user's top-left and a wider green bar at the user's
top-right. They exercise acquire / present, swapchain recreation, pre-rotation
and frame pacing before the shared Vulkan renderer (PR #1's backend) takes over
the frames. `--es s2arm.panel off` hides the diagnostic text so screenshots show
the surface. Lifecycle
rules, after xbl-android's experience:

- **Window lease (implementation plan §7.3, `runtime/wsi_lease.{h,c}`).** The
  shell offers, updates and revokes the window; the render thread takes the
  latest state and acknowledges each release.
  - Identity is `{session, generation}`. `surfaceCreated` offers a new
    generation. `surfaceChanged` and inset changes publish a new revision of it:
    size, `Surface.ROTATION_*`, insets and refresh rate. `surfaceDestroyed`
    revokes it.
  - Window pointers are never compared, because Android may reuse a freed
    window's address. A revoked generation cannot be revived by a late update.
  - Plain C and pthreads with retain / release ops, so the C# Vulkan backend can
    bind the same calls. `tests/wsi_lease_test.c` covers it, with a 300-window
    concurrent stress run and a ThreadSanitizer-clean run.
- **The main thread never waits for the GPU.** Offer, update and `onResume` /
  `onPause` (foreground) only hand over state. While backgrounded or without a
  surface, the render thread sleeps in the lease. A new generation rebuilds the
  surface; a new revision only marks the swapchain for rebuild.
- **Bounded waits, required by Android.** `surfaceDestroyed` returns only once
  the render thread has destroyed the swapchain and surface and released the
  window. Its acquire waits at most 50 ms, so it notices quickly. Teardown
  (`onDestroy`) likewise waits for the render thread to finish. Each wait has a
  1 s deadline. Driver calls cannot be interrupted, so a missed deadline means
  the thread may still be on the old surface or device; the process then aborts
  rather than return (implementation plan §4.2 / §7.3). The tombstone holds
  every thread's stack and the reason as its abort message.
  `--ei s2arm.release_stall_ms <ms>` delays the render thread's release, and
  `--ei s2arm.stop_stall_ms <ms>` delays its teardown. Then
  `--ez s2arm.finish true` finishes the activity (on Android 12+, BACK only
  backgrounds it). Together they run both outcomes of each deadline on a device.
- **Swapchain.** FIFO, 3+ images, two frames in flight, render-finished semaphores
  per swapchain image. A resize hand-over or `SUBOPTIMAL` rebuilds only if the
  extent or transform really changed; `OUT_OF_DATE` always rebuilds.
- **Pre-rotation** follows Android's Vulkan pre-rotation guide:
  `preTransform = currentTransform`, and the swapchain is built in the display's
  natural orientation. Android reports `currentExtent` already rotated, so width
  and height are swapped for 90 / 270 degrees. The app draws its content rotated;
  `Presenter::to_image` maps a rectangle the user sees into the pre-rotated image.
  The shared renderer must apply the same transform to its output.
- The manifest takes orientation / size changes in place (`configChanges`), so
  rotation reaches `surfaceChanged`, not activity recreation.
  `am start -n org.s2arm.android/.MainActivity --es s2arm.orientation landscape`
  (or `reverse-landscape`, `portrait`) rotates this activity only; no system
  setting changes.
- The render thread logs frame pacing and swapchain details every 2 s
  (`logcat -s S2Arm`).

Measured 2026-10-22:

| Device | Result |
|---|---|
| Redmi K50 Ultra, Android 15, Adreno 730 | 1220×2712, 4 images, about 8.2 ms per frame (120 Hz), 1 swapchain build at launch. HOME: the surface was released in `surfaceDestroyed` and the thread slept. Resume: one rebuild, pacing back to 8.2 ms. Landscape (ROTATE_90) and reverse landscape (ROTATE_270): identity 1220×2712 swapchain, and in screenshots the red marker is at the user's top-left and the green bar at the top-right. Part of a marker sits under the side navigation bar. System rotation settings were unchanged afterwards |
| OnePlus 13, Android 17, Adreno 830 | 1080×2376, 4 images, about 16.6 ms per frame (60 Hz) once settled. Landscape (ROTATE_90) markers were correct in a screenshot; BACK (activity teardown) was clean |
| Redmi K50 Ultra, deadline paths (`s2arm.release_stall_ms`, HOME) | 500 ms stall: "Surface detached in 502 ms", process kept running. 1500 ms stall: FATAL "surfaceDestroyed: render thread still on the surface after 1000 ms", then SIGABRT; the tombstone's abort message carries that line. The render thread logged "no surface" about 500 ms after the abort began, which is the stale-surface access the deadline exists to stop. |
| Redmi K50 Ultra, teardown deadline (`s2arm.stop_stall_ms`, then `s2arm.finish`) | 500 ms stall: the surface was released in 2 ms, teardown waited out the stall within its deadline, and the process kept running. 1500 ms stall: at 1000 ms, FATAL "Vulkan presenter did not stop within 1000 ms (render thread stuck)", then SIGABRT; the tombstone's abort message carries that line |
| Redmi K50 Ultra, window lease (§7.3) | Generation 1 offered at 1220×2712. Rotating the activity to landscape, reverse landscape and back produced revisions of generation 1. Each rebuilt only the swapchain, with transforms 0x2, 0x8 and 0x1, and no new surface. HOME revoked generation 1 in 6 ms; relaunching in the same process offered generation 2. The release and teardown deadlines behave as above: 500 ms stalls pass (502 ms), 1500 ms stalls abort at 1000 ms. System rotation settings were unchanged afterwards |

The first version built rotated swapchains in the rotated extent (2712×1220 with
ROTATE_90). A clear colour could not show that; the markers would have. Android 17
also has one more platform signal handler without `SA_ONSTACK` (45) than Android 15.

## Physical gamepad mapping

The face-button layout is the same `padFace` choice as iOS and Mac (`runtime/src/pad.c`):
`auto` (default) follows the printed letters, `switch` puts Switch A on the right button,
`xbox` puts it on the bottom button. Android key codes are positional on every controller
(AOSP key layouts send `BUTTON_A` for a Nintendo pad's bottom "B"), so `auto` is the Switch
layout on Nintendo pads (vendor `057e`, or a name with Nintendo / Switch / Joy-Con) and the
Xbox layout on everything else (PlayStation: × confirms). Pick it in the in-game four-finger
menu ("Controller button layout", shown while a controller is attached) or with an intent:

| Android button (position) | `switch` | `xbox` |
|---|---|---|
| A (bottom) | Switch B (`0x0002`) | Switch A (`0x0001`) |
| B (right) | Switch A (`0x0001`) | Switch B (`0x0002`) |
| X (left) | Switch Y (`0x0008`) | Switch X (`0x0004`) |
| Y (top) | Switch X (`0x0004`) | Switch Y (`0x0008`) |

```sh
adb shell am start -n org.s2arm.android/.MainActivity --es s2arm.gamemap xbox --es s2arm.touchlog on
adb shell am start -n org.s2arm.android/.MainActivity --es s2arm.gamemap auto
```

The older values still work: `position` = `switch`, `label` = `xbox` (a saved
`gamepadMapping` from before carries over). For a per-game APK, use its application ID with
the same `org.s2arm.android.MainActivity` class. The option applies to physical face buttons
only. Sticks, the D-pad, shoulder buttons and the on-screen pad keep their existing mappings.

A new intent with this extra changes the current Activity's layout immediately and saves
it in app-private preferences (`padFace`). An intent without it keeps the current layout; a
later launch restores the saved one. Unknown values are rejected with an error message before
they are saved; the previous layout remains active. The option is also a running-session
control when a development runtime is loaded. Focus loss clears input but keeps the layout.

`GamepadStateTest` covers all three layouts on Nintendo and other pads, the older values,
kind detection, release after switching while held, reset, and independent hat/trigger input.
These are host tests of the production state implementation; physical-controller validation
is still pending.

The diagnostic APK also has an SDK-only instrumentation runner (no third-party test dependency):

```sh
# Build both APKs with the normal pinned Android toolchain, nativeMode=diagnostic / services=0.
# Install app-debug.apk and app-debug-androidTest.apk, then run:
adb shell am instrument -w org.s2arm.android.test/org.s2arm.android.GamepadMappingInstrumentation
```

A successful run reports `passed=true`. It checks the real Activity, preferences, the default
(`auto`, which software events without a device read as the Xbox layout), `switch`, the older
`position` / `label` values, held-button switching/release, invalid values and Activity recreation,
then restores the original preferences. It uses software KeyEvents; cold process restart and
physical input are separate checks. A controller whose Android key layout reports a different X/Y
code must still be checked against its printed labels; no controller-model override is applied.

## Controller rumble and six-axis

Player 1's controller is the one that last sent input (else the first attached gamepad).
`PadFeedback.java` gives native (`native/phone_feedback.c`) its motors and sensors:

- Rumble: a controller with a motor rumbles instead of the phone (four-finger menu
  "Controller rumble", on by default). Two motors (API 31 `InputDevice.getVibratorManager`,
  id 0 left / strong, id 1 right / weak) get the left and right HD Rumble sides; one motor
  (or Android 10/11 `InputDevice.getVibrator`) gets the stronger side. Same 1 s lease as the
  phone motor.
- Six-axis (API 31 `InputDevice.getSensorManager`, only while the game wants six-axis): a
  controller with samples wins over the phone (`runtime/src/motion_pick.c`); switching drops
  one frame, and the phone sensors rest after the controller has supplied samples for 2 s.
  Android keeps each driver's axes, so the conversion is per vendor: PlayStation (DS4,
  DualSense: x right, y face normal, z toward the player) and Nintendo (Pro, Joy-Con: x top,
  y left, z face normal). Other vendors' sensors are not used (the phone gyro stays on).

`phone_feedback_test` checks the axis conversion by pose (plus SDL's Nintendo-to-PlayStation
shuffle as a cross-check), the per-side motor levels and the source routing.

## On-screen gamepad

`TouchPad` (plain Java, unit-tested in `tests/TouchPadTest.java`) and
`TouchPadView` draw a Switch-style pad over the surface:
- **Controls:** two sticks, the D-pad, and all 16 Npad buttons (A/B/X/Y, L/R,
  ZL/ZR, +/−, and stick presses LS/RS).
- **Sticks:** a finger that lands on a stick keeps it until lifted; dead zone 0.12,
  clamped to the unit circle, up positive.
- **Buttons:** other fingers press whatever they are over, so they can slide
  between face buttons.
- **D-pad:** 8 sectors with a centre dead zone, so diagonals press two directions.
- **Layout:** fills the area inside the system bars and cutouts in landscape; in
  portrait it sits in a landscape-shaped box at the bottom. Geometry is an F01
  layout (`TouchLayout`, the iOS `ios/layouts/layout.schema.json` format)
  projected into that box: the built-in one is `TouchLayout.androidDefault`, and
  the four direction keys form one D-pad disc. Unlike iOS, Android sticks are
  fixed at the centre of their activation region.
- **Touch menu (no adb):** hold four fingers for 1.5 s. *Edit button positions*
  dims the game and lets every key, the D-pad and both sticks be dragged; *Save*
  writes the player's layout for this game to `files/touch-layouts/<title id>.json`
  (the diagnostic app uses `app.json`), *Default* deletes it. *Touch passthrough*
  sends touches that miss the controls to the game, and is remembered per game;
  it is listed only in builds with `-Ps2arm.experimentalTouch=true` (the native
  transport is off by default).

The Activity merges the pad with a physical controller before publishing through
`s2a_vpad_read`: buttons are OR'ed, and each stick comes from whichever is
deflected more. In `auto` mode the pad hides while a physical gamepad is
connected. Focus loss, pause and device changes release it.

Debug extras: `--es s2arm.touchpad on|off|auto`, and `--es s2arm.touchlog on`,
which logs the layout and every change of the published input.
`--ez s2arm.experimental_touch true|false` still switches passthrough for one run
without changing the remembered choice.

Checked on the Redmi K50 Ultra (landscape, 2026-10-22) with injected touches
(`input motionevent`, sent only while this app was in front):
- tap A gave `0x0001` then `0x0000`;
- sliding A to B gave `0x0001`, then `0x0002`, then release;
- left stick full right gave (32767, −65), and full up (11, 32767);
- D-pad up-right gave `0x6000`;
- a press 0.75 / 0.25 of the radius from the stick centre gave (24608, 8130).

Multi-touch is covered by the unit test, because `input` injects one pointer.

Reverse landscape (2026-10-23, Redmi): with `s2arm.touchlog on`, taps injected at
the logged control centres gave the same input in landscape (ROTATION_90) and
reverse landscape (ROTATION_270): A 0x0001, B 0x0002, D-pad up 0x2000, and the
left stick reaching +32767. The layout follows the cutout and navigation-bar
insets.

## Game packages (import)

A game on S2Arm is the rewritten code plus a data directory. The data directory is
资源包 v2, the format of `pack/pack.py`: `manifest.json`, then `index.tsv` (path, bytes,
SHA-256), then the files themselves, read by the service layer through
`S2A_GAME_DATA`. iOS compiles the code into the app; Android cannot. So an Android
game package is that same data directory plus `modules/`:
- every module rewritten with `S2A_TARGET=android` (`.patched.bin`, `.fixups.tsv`,
  `.gates.tsv`, `.widen.tsv`);
- `main.npdm`;
- the widescreen tables.

All of it is listed in one index.

- **Building a package.** `tools/make_pack.py <out> --root <product root>` takes the
  entries `pack/ipa.sh` packs and rewrites the `exefs` modules for Android.
  Run the shared refresh system-data stage first; it prepares `systemdata/` in
  the product root from the player's data or shared synthetic data. This tool
  consumes that prepared directory and fails if the shared synthetic-capable
  IDs are missing. Every included file is covered by `index.tsv`, its SHA-256,
  and the artifact's `systemData` root.
  `--synthetic` builds a test package with no game data. `pack/pack.py` clones
  files with macOS `clonefile`, so this tool writes the same format by copying.
  It also writes `game-build-artifact.json` (A0 v1, `contracts/`) beside
  `manifest.json`. The artifact names `index.tsv`'s hash, which is also the
  content root, so it stays outside the index. `pack/verify.py`, which compares an
  iOS package with its sources, therefore reports it as an extra file. Android
  packages are checked by `GamePack` and, once installed, by `BootContract`.
  Modules must be `REWRITE_ABI android-sp-safe-v1` output.
  `SOURCE_DATE_EPOCH` makes the artifact reproducible.
  The host adapter reuses `pack/rewriter.py`: the C rewriter is the only
  rewriter (the Python one was removed 10-31). The C resolver
  uses pinned offline dependencies (see `runtime/rewriter/README.md`), or
  `S2A_REWRITER_C=<executable>`. It never downloads dependencies or silently
  falls back after a C failure. Python remains necessary for package metadata
  and the shared address-requirement collector; the Python backend needs the
  pinned Capstone/LZ4 modules. Android defaults are `spin=0`, `sdk_addr_cut=1`;
  explicit `S2A_REWRITE_SPIN` / `S2A_SDK_ADDR_CUT` settings are preserved.
  Incomplete output, wrong target/ABI, rewrite gaps and nonzero `LOAD_BIAS`
  are rejected before replacing an old package. The v2 address table is bound
  to the original NSO/NPDM and indexed beside both `exefs/main.npdm` and
  `modules/main.npdm`; changing it changes `contentRoot`.
  This is a development/test adapter for the unified Android target: the
  computer-side product entry remains `pack/refresh.sh` (target wiring is T03),
  and this change does not add a separate Job/Event/Artifact lifecycle or JNI.
  Run the real backend matrix directly with
  `python3 android/tests/make_pack_rewriter_test.py -v` after supplying
  the pinned offline C dependencies and Python modules. Both `test.sh` and
  `gate.sh` run this matrix unconditionally; `S2A_TEST_NO_NATIVE=1` only skips
  the Linux runtime CTest stage. A missing matrix dependency is a failed gate,
  not a skipped test.
- **Verifying** (`GamePack`, unit-tested in `tests/GamePackTest.java`):
  - follows `pack/verify.py`: manifest version 2, index hash and counts, every
    file's size and SHA-256, no files outside the index;
  - safe relative paths, no duplicates;
  - the files the Android runtime needs (`exefs/main.npdm`, `ctl/control.nacp`,
    `romfs/`, and each module's three files);
  - `# TARGET android` in every fixups file;
  - reads the title (presence group) ID, name and version from `control.nacp`.
- **Installing** (`GameInstall`, plan §6.5, A0 contracts in `contracts/`). Games
  live in the app's internal files directory, `files/games/<title id>/`: private,
  removed on uninstall, no storage permission.
  - **Explicit private or external input, one transaction.** Start
    `LibraryActivity` with string intent extra
    `org.s2arm.android.IMPORT_STAGING=<absolute path>` pointing to exactly one
    direct child of the app's internal or external `files/import/` directory.
    The full path distinguishes equal names in the two roots. Relative paths,
    `..`, symlinks and paths outside these roots are refused; no fallback guesses
    a queue. Resume without a target, repeated resume after the one-shot extra
    is consumed, and an already-consumed target never scan other packages.
    The player's **Import game package…** still opens the system folder picker.

    A debuggable APK may receive authorized developer input through `run-as`
    into its private `files/import/`; release builds do not gain debug access.
    No exported component or storage permission is added. Keep the app stopped
    while staging a new directory; use regular files and commit `manifest.json`
    last. Do not write `games/current.json` yourself. Allow space for the input,
    installed copy and rollback data. Validation, installation and cleanup of
    the selected input share one GameStorage lease. Failed or incomplete inputs
    remain for retry; only successful activation permits source cleanup. A
    cleanup failure after activation is reported separately, not as a failed
    installation. Existing APK identity/signature/hash admission is unchanged.
    In either import path:
    1. every file is copied in with its size and SHA-256 checked;
    2. the copy goes to `revisions/<content root>.staging/` (content root =
       `sha256-<index.tsv hash>`);
    3. `BootContract.validateRevision` checks it against
       `game-build-artifact.json`;
    4. it is renamed into place;
    5. `current.json` is switched by a flushed temp file and a rename, recording
       `previousContentRoot`.

    A managed revision syncs its file data and directories before activation.
    Recovery keeps the old revision when the pointer was not switched, or
    retains the complete new revision after activation. Staging folders and
    interrupted replacements are recovered under the game lock.
  - **Versions share storage.** Managed private revisions store checked,
    read-only files in `objects/<aa>/<sha256>` and expose them through relative
    file links. A delta reuses an unchanged object when the previous index and
    its verified file stamp agree. A missing or stale stamp requires a hash
    check. The previous revision continues to reference its objects for
    rollback. Generic offline shader packs require regular files and use hard
    links or checked copies. A delta from a legacy plain revision uses checked
    copies until that revision is migrated.
  - Game files are only read. Rewritten modules are copied into anonymous memory
    by `native/module.c`, never mapped executable from storage.
- **Load check:** the panel lists installed games, and maps each module through
  `module.c` (`TARGET android`, R|X). Nothing executes.

Game files, keys and firmware stay out of the repository. Packages are built from
the player's own dumps through the packaging pipeline.

Checked on the Redmi K50 Ultra (2026-10-22) with synthetic packages pushed by adb:
- **First import (#17):** "S2Arm synthetic 0.1 [0100000000ABCDEF]" was imported,
  its module mapped R|X, and a copy with one changed byte was refused.
- **A0 transaction, v1 then v2.** v2 changes one romfs file. Each became active
  with the previous version recorded, and `BootContract` reported ReadyToPlay
  from `files/games/<id>/revisions/<root>/`.
- **v3 refused.** A package damaged after packing was rejected with
  "romfs/readme.txt does not match index.tsv", leaving v2 active.
- **On-device layout:** revision files are 84-byte relative symbolic links
  into `objects/`. Two versions took 155 KiB of objects plus 70 KiB of
  folders and links, against about 312 KiB as two full copies.

The folder-picker path shares the same checks but needs a person to pick the
folder; the test drove no other app's UI.

## Per-game APK (`pack/android`)

The implementation plan's output form (§2.3, §8.2): one Android runtime builds one
app per game, as on iOS. The rewritten modules ship inside that game's APK, and
the resource package (romfs etc.) is deployed separately. The import above stays
the diagnostic app's development path.

```sh
python3 android/tools/make_pack.py build/pack/<game> --root <product root>
python3 android/tools/game_apk.py build/pack/<game> --version-code 1   # -> build/android-game/<title id>
S2ARM_ANDROID_CS_SERVICES=static S2ARM_ANDROID_GAME_DIR=build/android-game/<title id> \
  bash android/docker/build.sh
python3 android/tools/game_apk.py --resources build/pack/<game> \
  android/app/build/outputs/apk/debug/app-debug.apk build/pack/<game>-res   # what that app installs
```

出包链路（`pack/targets/android/package.py`）给 `--resources` 加 `--reuse-from <上一个已发布 release 的 resources/>`：上一包自己的 index 里与新包同路径、同大小、同 SHA-256 的**只读**文件改成硬链接，不再各写一份（ext4 没有 reflink，以前每个 release 的 `resources/` 都是一整份 RomFS）。链接后仍逐个核新包的 index；上一包不可用（读不出、与输出或输入重叠）、跨盘、链接被拒、文件可写或字节已变的，一律退回复制。所以同一台机器上相邻 release 的只读文件共享同一个 inode：别原地改 release 里的文件（先改权限再写会同时改到两个包），删哪个 release 都不影响另一个。

构建后的设备部署统一用 `python3 android/tools/deploy_game.py --pack <完整源包> --apk <最终签名APK> --serial <设备> --mode direct`。

要在正式包恢复已有进度，先关闭游戏，在游戏库选中目标游戏，打开设置面板的「导入存档备份」，用系统文件选择器选择 Mac 或 iPhone 导出的 `.s2asave`。核对检查结果再确认恢复，现有存档会先备份。

QA 可在设备租约内给同一部署命令加 `--save-archive <备份.s2asave> --no-start`。应用完成恢复后，部署回执的 `saveImport` 包含标题号、恢复的文件数和字节数、以及相对于应用私有文件目录的备份位置。无旧存档时位置为空。确认回执后在游戏内选择继续，并验证预期的检查点场景。
脚本冻结 APK、生成配套资源绑定、安装、导入并核对设备 APK 哈希，避免重编后反复出现 `artifact.apk.sha256` 不匹配。
同资源版本仅更新 APK 时加 `--refresh-existing`，通过正常安装事务刷新绑定，无须重传 RomFS。
已绑定的资源目录可改用 `--resources <目录>`；默认导入后启动并等待接受，`--no-start` 只导入。导入仅指定本次暂存目录。
完整用法、适用边界和回执见 统一方案 §4.2.1。

Provide player-exported archives to the shared refresh system-data stage before
running `make_pack.py`; the latter reads the prepared product root.

For example, the observed Scarlet 4.0.0 build `421c5411b487eb4d049dd065fec95477`
needs `0100000000000823/` for its character-name check. The table records that
specific build's actual `OpenDataStorageByDataId` request, not the unrelated
startup `MountSystemData` warning. No firmware/archive data is distributed by
this tool. See [系统数据缺了怎么办](../docs/贡献者开发测试套件.md#22-系统数据缺了怎么办).

- `tools/game_apk.py` checks every `modules/` file against the package's
  `index.tsv` (and `index.tsv` against `manifest.json`), and requires
  `# TARGET android`. It reads the title ID, name and display version from
  `control.nacp`. It writes `game.properties`, the launcher label as a string
  resource, and `assets/game/modules/`.
- Gradle (`S2ARM_ANDROID_GAME_DIR` or `-Ps2arm.gameDir`, relative to the
  repository) gives the app the game's identity:
  - application ID `org.s2arm.game.t<title id>` (stable per game, so updates
    keep the save data and the signing identity);
  - the game's name as its label;
  - landscape either way up (`sensorLandscape`, plan §7.4). Phones honour it;
    from Android 16, large screens ignore fixed orientations, and the presenter
    handles any orientation. On the Redmi it opened at 2712 × 1220, rotation 90°,
    with system rotation settings untouched;
  - `versionName` `<game version> (S2Arm <runtime version>)`; `versionCode` from
    `--version-code` (default 1), raised for each APK update.
  
  Without the setting it builds the diagnostic app `org.s2arm.android` as before.
- Module images are stored uncompressed (`noCompress 'bin'`). At startup they
  are read in place from the signed APK (`AAsset_getBuffer`). `s2a_module_map_mem`
  then copies them into anonymous memory and seals them R|X, with nothing
  written to storage first. The APK signature covers the assets, so there is no
  separate copy to verify. Until the shared loader runs on Android, the panel
  only checks that each module maps.
- **The app's resource package** (`game_apk.py --resources`): the package without
  `modules/`, plus an A0 `game-build-artifact.json` whose modules are
  `source: "apk-asset"` under `apk.assetRoot` `game/modules`. The artifact carries
  the APK's identity:
  - package name and versionCode;
  - the APK's SHA-256;
  - the signing certificate's SHA-256. It is read from the APK's v3 signing block
    (else v2), the certificate Android reports for `GET_SIGNING_CERTIFICATES`, and
    matches `apksigner --print-certs`.

  The tool refuses an APK whose modules differ from the package's. The app
  installs this package like any other (adb `import/` or the folder picker,
  through `GameInstall`). An APK update with unchanged resources (new versionCode,
  same files) refreshes only the artifact.
- **At launch**, apk-asset modules run only from the APK the artifact names:
  - its package name, versionCode (`getLongVersionCode()`), and the SHA-256 of
    `SigningInfo.getApkContentsSigners()` must match the artifact. The APK bytes
    are not re-hashed; Android verified the signature at install.
  - Then exactly the artifact's module paths are mapped from the assets
    (`checkAssetModulePaths`), and `BootContract` reports ReadyToPlay.

The output derives from the player's own game. It stays under `build/` and
never goes into the repository or a PR.

Checked on the Redmi K50 Ultra (2026-10-22, synthetic package):
- the per-game build installed as its own app, `org.s2arm.game.t0100000000abcdef`,
  next to `org.s2arm.android`;
- it reported "Game in this APK: 0100000000ABCDEF" and "modules main map R|X
  (TARGET android, 80 KiB); images read in place from the APK: 1/1";
- it was uninstalled afterwards;
- the diagnostic app built from the same tree runs as before and reports no game.

Checked on the Redmi K50 Ultra (2026-10-23, synthetic package, versionCode 3):
- the resource package installed into the per-game app, and `BootContract`
  reported "ReadyToPlay … 1 module(s) from apk-asset", with the module read in
  place from the APK (1/1);
- the same files with an artifact for versionCode 99 refreshed the artifact
  ("build artifact updated (same files)"), and the app then refused it: "the
  artifact is for versionCode 99, this app is 3";
- the app was uninstalled afterwards.

### Distribution constraints (2026-10-28 research)

Per-game APKs are built by the player and sideloaded; they are never published
through Google Play. Google's developer verification changes what sideloading
means on certified devices:

- It took effect on 2026-09-30 in Brazil, Indonesia, Singapore and Thailand and
  extends to certified devices worldwide in 2027 (most devices sold outside
  mainland China; devices without GMS are mostly not affected).
- Apps from unregistered developers then install only by **adb**, through
  **limited distribution** (no identity verification, at most 20 devices), or
  through the user-enabled **advanced flow** (a 24-hour wait on first use is
  reported in forums, not yet confirmed from an official source).
- Updates go through the same check: if the user turns the advanced-flow switch
  off, updates of unregistered apps fail (re-enabling within 10 minutes is
  reported to skip the wait). Player documentation must say this before the
  first update.
- The Google Play 16 KB page-size mandate (2027-02-01) does not apply because
  S2Arm does not use Play; 16 KB alignment is still required for 16 KB devices
  (see #880).

## Boot configuration (BootSpec, shell side)

BootSpec v1 (`contracts/boot-spec.v1.schema.json`, plan §4.1) is the one document
the runtime freezes before the guest starts. `BootSpec` / `BootSpecSource` fill in
the part only Android knows, following the injection rules agreed in #18:
- **Paths** come from `Context` (`getFilesDir()`, `getCacheDir()`), so
  multi-user `/data/user/<n>` and adoptable storage are right. The cache entry is
  only the root: the backend derives its shader and pipeline domains itself.
- **Language** is the first system locale's BCP 47 tag; an unusable tag falls
  back to `en`, with a diagnostic. **Region** is written only when that locale
  has a two-letter one; otherwise the shared layer defaults it.
- **Time zone** is an IANA name: ICU's canonical ID first. A custom
  `GMT±hh:00` becomes `Etc/GMT∓h` (POSIX sign). A non-whole-hour or
  unrecognised ID becomes `UTC`, with a diagnostic.
- **Screen**, for now, is the default display's full size in landscape, with
  its density and refresh rate. The real freeze at session start (§8.2) will
  use the prepared surface instead.
- **Mode** is `game` for a per-game app and `diagnostic` for this one. Console
  mode is `handheld` and network `offline` (the defaults).

Nothing consumes it yet. For each ReadyToPlay game the app logs a summary and
writes a preview to `cache/boot-spec-<title id>.json`. `tests/BootSpecTest.java`
covers the rules, and `test.sh` checks its output against the schema.

Checked on the Redmi K50 Ultra (2026-10-23, a synthetic game staged with
`tools/stage_a0.py`): "BootSpec 0100000000ABCDEF: language en-US, region US,
time zone Pacific/Auckland, screen 2712x1220 480 dpi 120 Hz, mode diagnostic".
The preview pulled from the device passes the schema check.

## Development runtime library (debuggable builds only)

For a C/C++ or C# change the inner loop does not need a new APK: a debuggable
package can run a replacement `libs2arm_android.so` pushed into its private
files, **for one launch that names it**. The APK and its SHA-256 stay as they
are; the replacement is bound to that exact installed APK.

- **Switch**: every launch passes `--es s2arm.dev_runtime <sha256 of the library>`.
  Without the extra the APK's library runs, whatever the private directory holds
  (one Info line says unused replacements are present). A cold home-screen launch never
  uses a replacement. Tapping the icon while a development session is still running
  only brings that task to the front (no new intent), so that session keeps running.
- **Layout**: `files/dev-runtime/<sha256>/libs2arm_android.so` and
  `files/dev-runtime/<sha256>/dev-runtime.json` (both 0444, the directory 0555):
  `libSha256`, `baseApkSha256`, `sourceCommit` (40 hex), `dirty` (boolean),
  `diffSha256`, `ilcConfig` (`Release`/`Debug`), `aotHost` (`linux-arm64`/`linux-x64`).
  `tools/dev_runtime.py manifest` writes it (not by hand). Before writing, it checks the
  library the way the app does, and checks the library carries this tree's JNI ABI.
- **Checks before anything native runs** (`RuntimeLibrary.select`, plain JDK,
  `tests/RuntimeLibraryTest.java`):
  - the package is debuggable (`FLAG_DEBUGGABLE`) and the extra is a lowercase SHA-256;
  - below the canonical files directory nothing is a symbolic link (`files/dev-runtime`
    itself, `<sha256>/`, the library, the manifest), and no write bit is set on the
    directory, the library or the manifest;
  - `baseApkSha256` equals the installed base APK (hashed once per process, shared with
    the artifact binding): a Java change means reinstalling the APK;
  - the manifest carries the build identity;
  - the library is copied (opened without following links) into a new read-only file under
    `code_cache/s2a-dev-runtime/<sha16>-<pid>-<random>.so`, hashing exactly the bytes written;
    that copy's SHA-256 must equal the extra, and the copy is what `System.load` gets, so
    replacing the pushed file after the check changes nothing. At startup, a copy is removed
    only when procfs proves that its owning PID is gone; the library and game processes may
    both be alive. If procfs is unavailable or its status cannot be checked, copies are kept.
    Re-entering `onCreate` with the same selection reuses the verified library;
  - every ELF LOAD segment is 16 KB aligned (`verify-apk.py`'s rule) and `DT_SONAME` is
    `libs2arm_android.so`, so a lookup by name (the diagnostic self-test,
    `cs/aot/AndroidSelfTest.cs`) finds this library and never loads the APK's copy too.
  Any failure refuses the start: the red "Startup failed: runtime library rejected: …"
  panel, logcat `E S2Arm Runtime library load failed`. The APK's library is **not** loaded
  instead, and nothing native runs (no surface callbacks, touch pad hidden, lifecycle
  entries guarded; `tests/runtime_library_activity_test.py`).
- **One load entry**: `NativeBridge.load` (first in `MainActivity.onCreate`, before
  `GameLibrary` exists) uses `System.load(<copy>)` for a replacement and
  `System.loadLibrary` otherwise; nothing else loads the library (`TouchInputBridge`
  binds to it). Afterwards:
  - `NativeBridge.loadedPath()` (`dladdr`) must name the selected file: the copy, or an
    entry of the installed base / split APK or its library directory, compared also by
    canonical path;
  - If loading or verification fails, the process refuses another load attempt and must be
    restarted: a failed verification can leave the first native library mapped.
  - `NativeBridge.jniAbi()` must equal `RuntimeLibrary.jniAbi(NativeBridge, TouchInputBridge)`.
    That is a SHA-256 over every native declaration. `native/jni_abi.py` computes it from the
    Java sources as a build-time output, regenerated whenever either source changes, and
    the library carries it. Any modifier order, varargs and C-style arrays match the
    reflection form; anything the parser cannot match is a build error. `verify-apk.py`
    fails an APK whose library does not carry the value of its tree's sources. A library
    built against other native declarations is refused before any other native call; this
    holds for the APK's own library too.
- **Bound to the process**: a process keeps the library it loaded first. A later launch of
  the same Activity (singleTop `onNewIntent`) that asks for another library, or for none
  while a replacement runs, is refused: `E Runtime library mismatch: … cold start needed`,
  red panel, and the session ends.
  - A launch is an intent that names `s2arm.dev_runtime`, or carries any other `s2arm.*`
    extra the running Activity does not handle (cold-start extras such as
    `s2arm.console_mode`), or carries none at all (a plain `am start -n`).
  - A control intent carries only the extras the running Activity handles
    (`MainActivity.CONTROL_EXTRAS`: `s2arm.finish`, `s2arm.fps`, `s2arm.orientation`,
    `s2arm.panel`, `s2arm.touchpad`, `s2arm.touchlog`, `s2arm.experimental_touch`,
    `s2arm.release_stall_ms`, `s2arm.stop_stall_ms`, `s2arm.request_stop`). It steers the
    running session whatever library it runs.
- **Own handle**: the APK's library keeps its soname lookup
  (`dlopen("libs2arm_android.so", RTLD_NOLOAD)`) for the providers' `native_owner` and the
  diagnostic RTLD_GLOBAL promotion. A replacement, loaded from another file, takes the handle
  by its `dladdr` path (`RTLD_NOLOAD`), so a second copy is never loaded.
- **Labelled everywhere**:
  - logcat (W for a replacement) and the status panel carry `runtime-library: apk`, or
    `runtime-library: dev-override sha256=… base-apk=… source-commit=… dirty=… diff=… ilc=… aot-host=…; 开发局，不作性能读数`;
  - BootSpec `diagnostics.runtimeLibrary` carries the same as a closed record,
    `{"kind": "apk"}` or `{"kind": "dev-override", "sha256", "baseApkSha256", …}`;
  - `<state dir>/runtime-library.json` (`{"pid", "sessionId", "runtimeLibrary"}`) sits
    next to the session's host log and metrics. `metrics.py export` checks it against the
    capture's PID and refuses a dev-override capture as no reading (`--allow-dev-run` exports
    the raw evidence marked `NOT-A-READING.txt`). A missing record is refused only when the
    package's BootSpec already has `diagnostics.runtimeLibrary`. APKs built before this
    record existed are formal (`apk-legacy`).
  - `tools/frame_gate.py` and `tools/score_cells.py` call such a run directory invalid.
    Not checked yet: other reading entries (`tools/ab.sh`, `health_table`, `alt_report`,
    `beta_intake`) and a bare `adb pull` of `frames.tsv`. Take readings only from
    `metrics.py export`.

  A replacement's run is a development run: no performance readings (frame rate, energy,
  first-encounter), no formal receipt; confirm on a formal APK. Formal APKs are debuggable
  too (`assembleDebug`), so the extra, not `FLAG_DEBUGGABLE`, is what tells the two apart.
- **Threat model**: the Activity is exported, so any app can start it with the extra. The
  extra can only select a library already in the private files that passes every check. It
  cannot bring in new code; at most it causes a development run or a red refusal. Only
  this app's uid (and `run-as`) can write the private files; the checks guard against
  pushing mistakes and misuse, not against that uid itself.
- **Cost**: the checks run on the main thread in `onCreate`: hashing the base APK plus copying
  and hashing the library. The time is logged
  (`I Development runtime checks took N ms`); without the extra nothing extra runs.
- **Pushing**: `tools/dev_runtime.py push --serial S --package P --console-mode M --lib-dir <dir>`
  (run it under your device lease). The steps it takes:
  - check the manifest and the installed base APK;
  - stop the game with `--activity-single-top --ez s2arm.finish true` (not `force-stop`, which
    skips the shader-cache export) and wait for the process to exit;
  - push the two files to `/data/local/tmp`;
  - `run-as` copy them into `files/dev-runtime/<sha256>/`, `chmod 0444` both and `0555` the
    directory, then check the SHA-256 on the device;
  - start with `--es s2arm.dev_runtime <sha256> --es s2arm.console_mode <mode>`, and wait for
    logcat's `runtime-library: dev-override` line.

  A pushed directory is 0555, so it can only be removed after
  `run-as <package> chmod -R u+w files/dev-runtime/<sha256>`, then `rm -rf`
  (`dev_runtime.py push --clean` removes all but the pushed one this way). Uninstalling
  removes everything; `install -r` keeps the replacements (unused without the extra).
- **Not covered**:
  - Native→Java callbacks (methods native code calls by name) are not part of the JNI ABI hash.
  - A shader-translation change can be hidden by the host shader cache from an earlier run;
    clear or disable the cache for those.

## C# service layer: NativeAOT on Android (feasibility)

`cs/build.sh` (run by `docker/build.sh` before Gradle) installs the repository's
pinned .NET SDK 10.0.403 into the cache volume, checking its SHA-512 against
Microsoft's release metadata. It then publishes NativeAOT shared libraries for
`linux-bionic-arm64`, linked with the NDK's clang. The settings are
`PublishAotUsingRuntimePack` and `DisableUnsupportedError`; the runtime pack
10.0.14 is on nuget.org.

- `cs/probe`: always packaged. Exercises the runtime and reports which OpenSSL
  libraries the app can load.
- `cs/aot`: `S2A.Services`, Ryujinx HLE and the Vulkan backend, mirroring
  `services/aot` without the Apple renderer. A process holds one NativeAOT
  runtime, so each mode packages only one .NET library.
  - `S2ARM_ANDROID_CS_SERVICES=1`: its own `libs2a_cs.so`; the preview starts
    that runtime instead of the probe's.
  - `S2ARM_ANDROID_CS_SERVICES=static`: the **single-library build**, as on iOS.
    A NativeAOT static library plus the runtime pieces the SDK lists (written to
    `build/android-cs/static/link.txt`) link into `libs2arm_android.so`, with
    `s2a_native` bound at link time (`DirectPInvoke`). Until the native core runs
    on Android, `native/s2a_native_stub.c` provides the 20 imported functions,
    with signatures from `runtime/include/s2a_native.h`.
  - The runtime archive carries its own LLVM libunwind internals, which collide
    with the NDK's libunwind for C++ exceptions. `cs/build.sh` renames the runtime's
    89 unwinder symbols (`llvm-objcopy --redefine-syms`), so each side keeps its
    own unwinder.
  - **Opus and VP8 decoding** (implementation plan §6.2 / §6.3). The service layer
    binds `s2a_opus` (Horizon's `HardwareOpusDecoder`) and `s2a_vpx` (Nvdec's
    `VpxVp8Decoder`), C shims in `services/opus` and `services/vpx` that Apple links
    statically. `codecs/build.sh` builds the same libraries for the NDK (API 34),
    with the version, source SHA-256 and configure options read from
    `services/*/build.sh`. It also builds each shim into one archive with its
    library. `cs/build.sh` appends both archives to `link.txt`, and the AOT project
    binds them with `DirectPInvoke`. Without them, a game's first Opus packet or
    VP8 frame would fail to load its library. FFmpeg (H.264) is not built yet.
    How to link it (LGPL) is still open.
  - `cs/aot/AndroidCodecs.cs` (`s2a_android_codec_selftest`) checks both through
    the shims. It encodes 0.5 s of stereo tones and decodes them single-stream and
    multistream, checking packet sizes, reset and SNR after the encoder's lookahead.
    It also decodes two embedded 64×48 VP8 frames (a key frame, then an inter frame
    with the picture shifted). The pixels must match, hash for hash, what the
    plain-C reference decoder gave on the host, and a damaged frame must be refused.

Measured on the Redmi K50 Ultra (Android 15), 2026-10-22:

| Check | Result |
|---|---|
| Probe runtime | .NET 10.0.14 `linux-bionic-arm64`, 8 CPUs, loaded and run in 39 ms |
| Probe checks | Compacting GC, exceptions with stack traces, null dereference via ART's sigchain, threads, thread pool and timers all pass |
| Service library | 0 errors, 147 s cold build, 36.6 MB |
| Service library linking | Needs only `libc` / `libm` / `libdl` / `libz` / `liblog`, 16 KB aligned; exports `S2A_Init` |
| Service runtime | Loads, starts and runs a service-layer method in 15 ms; RSS +14 MiB |
| .NET crypto | `RandomNumberGenerator`, SHA-256 and AES go through .NET's OpenSSL shim. Without a bundled OpenSSL the shim calls `abort()` (process killed, not an exception). With `openssl/build.sh`'s OpenSSL 3.5.9, all three return the known answers |
| Main-program symbol lookup | `NativeLibrary.GetMainProgramHandle()` does not see exports of app libraries, even after `RTLD_GLOBAL` |
| `SIGRTMIN` | With `s2a_android_runtime_signals_prepare` first, .NET's GC hijack signal inherits `SA_ONSTACK` |
| Single library (APK size) | 39.8 MB APK; one `libs2arm_android.so` of 36.9 MB |
| Single library (linking) | Depends only on public NDK libraries; exports only the host's own 63 symbols |
| Single library (startup) | Loads in 2–3 ms; runtime start plus first call 11 ms (+14 MiB) |
| Single library (calls) | C# → C calls through `DirectPInvoke` return values and pass strings |
| Single library (unwinding) | .NET exceptions with stack traces, null dereference and compacting GC (runtime's renamed unwinder), plus a C++ exception (NDK unwinder), all work |
| Audio (AAudio backend) | The service layer's `AudioFactory` returns `AAudioHardwareDeviceDriver`. A 0.4 s tone through Ryujinx's session API played 19,200/19,200 frames in 479 ms, set the update event and had 0 xruns. Shared mode, 192-frame burst, 384-frame device buffer |
| Audio feeder wakeups | 3 while playing (start, queue, buffer finished), 0 in 300 ms paused. A fixed 4 ms poll would be about 120 and 75 |
| Codecs (`s2a_opus`, `s2a_vpx`) | Opus (libopus 1.6.1): 25 packets / 6460 bytes, the same as the host encoder. Single-stream and multistream both decode with a 312-sample delay (the encoder's 6.5 ms lookahead) and an SNR of 40.6 / 44.6 dB (host: 40.7 / 43.9). VP8 (libvpx 1.15.0, NEON): key and inter frames match the reference decoder exactly, and a damaged frame is refused |

What integration needs, in the shared code:

1. **Crypto.** Ryujinx HLE uses .NET crypto for `spl` random numbers, NRO
   SHA-256 checks (`IRoInterface`), accounts and captures. Without OpenSSL, each
   call aborts the process. `openssl/build.sh` now bundles an NDK-built OpenSSL
   3.5.9 (about 6 MB), which .NET's shim loads through its Android `libssl.so`
   path; PR #1 needed the same on HarmonyOS. The alternatives remain to route
   those uses through managed code, or to use .NET's `android` target framework,
   whose crypto calls Android's Java providers.
2. **Native binding.** `[DllImport("s2a_native")]` relies on `DirectPInvoke` and
   static linking on Apple. The single-library build above does the same on
   Android (one `.so`, `DirectPInvoke`, checked on the device). It needs the
   native core linked in place of the stubs.
   `TryGetExport(GetMainProgramHandle(), …)` lookups (`InputPump.cs`
   `s2a_vpad_read`, `Native.cs` deterministic mode / guest tick) must get a host
   handle or function pointers from the native layer instead.
3. **Signals.** Call `s2a_android_runtime_signals_prepare` before the runtime
   starts. When the services use `Console`, .NET also installs SIGINT, SIGQUIT and
   SIGCONT without `SA_ONSTACK`; unlike bionic's reserved signals these can be
   blocked, and `s2a_android_guest_sigmask` blocks them on guest threads.
4. **Renderer and audio.** Silk.NET must get `libvulkan.so` explicitly, as PR #1
   does. Audio: `cs/audio` is a Ryujinx `IHardwareDeviceDriver` over AAudio, which
   the composition installs as `Exports.AudioFactory`. AAudio's real-time
   callback is C (`native/aaudio_sink.c`) and only reads a lock-free ring
   (`native/pcm_ring.c`). The C# session feeds that ring and keeps the Apple
   session's accounting, so a GC pause delays bookkeeping, not playback. Its feeder
   thread sleeps until the callback reports that the head buffer finished, so it
   wakes once per played game buffer, like Apple's per-buffer hand-back, and not
   at all while paused or with nothing queued. Power has not been measured.

## Third-party dependencies

Packaged in the APK, in the same form as the root README's
「致谢与第三方依赖」 table:

| Dependency | Version | License | Use |
|---|---|---|---|
| [OpenSSL](https://www.openssl.org) | 3.5.9 (`openssl-3.5.9.tar.gz`, sha256 `603f5602e2eef00d77fbd429d34dcd5822bb301757a1bc9cdb24c670f1eb859a`; the GitHub release and openssl.org agree) | Apache-2.0 | `libssl.so` / `libcrypto.so` for .NET's crypto on Android, which aborts the process without a loadable libssl. Built by `openssl/build.sh` with the NDK (`android-arm64`, API 34) from the pinned source, not stored in the repository |
| [.NET runtime](https://github.com/dotnet/runtime) (NativeAOT runtime pack `linux-bionic-arm64`) | 10.0.14 (SDK 10.0.403, `compat/tree/global.json`) | MIT (third-party components such as LLVM libunwind under the notices in that package) | The C# service layer's runtime, linked from nuget.org's runtime pack by `cs/build.sh`, either into its own `.so` or into `libs2arm_android.so` (single-library build) |
| [libopus](https://opus-codec.org) | 1.6.1 (pin and options from `services/opus/build.sh`) | BSD-3-Clause (with patent license notices) | Horizon's hardware Opus decoder (hwopus), linked into `libs2arm_android.so` by the single-library build. Built by `codecs/build.sh` with the NDK, not stored in the repository |
| [libvpx](https://chromium.googlesource.com/webm/libvpx) | 1.17.0 (pin and options from `services/vpx/build.sh`; VP8 and VP9 decoders, with S2Arm's NVDEC hook patch `services/vpx/patches/`) | BSD-3-Clause plus patent grant (`PATENTS`) | Nvdec's VP8 video, and Nvdec's VP9 video by default (NVDEC jobs through the hook; the managed decoder takes any job libvpx cannot represent exactly; `S2A_NVDEC_VP9=managed` in debuggable builds turns it off), linked into `libs2arm_android.so` by the single-library build. Built by `codecs/build.sh` with the NDK, not stored in the repository |

`verify-apk.py` requires each component's license files in `assets/licenses/`
when its marker string appears in a packaged library. Matching by content rather
than by file name also covers what the single-library build links into
`libs2arm_android.so`.

The Docker toolchain recipe adapted from `xbl-android` keeps its MIT terms in
[REFERENCE-LICENSE](REFERENCE-LICENSE).

## Integration boundaries and reference patches

Sources consulted:

- [S2Arm PR #1](https://github.com/jhaizhou-ops/S2Arm/pull/1), head
  `73b0cee62ea0ea8da91888c3a51d0cfd76742dc7`: dynamic low address windows,
  shared backing for Linux aliases, Vulkan platform surface boundary, and the
  review requirement to keep platform identity in rewrite-cache keys.
- Local `xbl-android`, commit `43f9abbe18ac28cb04095ee3330de15b551dbf3e`:
  `dm/android/docker/Dockerfile` supplies the pinned toolchain recipe (adapted
  here; MIT terms in [REFERENCE-LICENSE](REFERENCE-LICENSE)); `dm/host/loader.c`,
  `dm/tools/patch_nso.py`, `dm/android/probe/dm_probe.c`, and
  `dm/android/lifecycle_dispatch.h` document Bionic TLS and lifecycle pitfalls.

Android's Bionic key layout is different from the PR's musl layout:
`tpidr_el0 -> TCB[-1] -> bionic_tls`, with signed `pthread_key_t`'s validity bit
masked off and `{sequence, data}` entries at a measured table offset plus
`16 * index + 8`. The rewriter's `S2A_TARGET=android` emits
`mrs tpidr_el0; ldur [#-8]` for the slot base and writes `# TARGET android` to
`fixups.tsv` (the Apple loader refuses it); `pack/stage_rewrite.sh` keys the
rewrite cache by target. The xbl-android reference measured a table offset of 0
on Android 15 and 17, but key numbers vary per build, so a loader must probe at
start and stamp each key on every guest thread with `pthread_setspecific`
before rewritten code touches the slot directly. No Android loader exists yet.

Rewritten blocks stash scratch registers below `sp` (down to `sp - 112`), relying
on the 128-byte red zone of Apple's arm64 ABI. Linux / AAPCS64 has no red zone.
On Android and HarmonyOS, every signal handler that can run on a guest thread must
therefore use `SA_ONSTACK`, and each guest thread needs its own `sigaltstack`.
Otherwise a signal frame can silently overwrite guest registers. The core
(`runtime/src/crash.c`, `det.c`, the 256 KiB per-thread stack from `thread.c`) and
xbl-android already work this way; `native/guest_signal.c` is the Android-side
equivalent used by the self-test's guest threads.

Measured on the Redmi K50 Ultra (Android 15) by `s2a_android_redzone_check`:
- A signal handled without `SA_ONSTACK` destroyed `sp - 8` and `sp - 16`, where
  the kernel puts its frame record. Most blocks stash their two scratch registers
  there (`stp S, T, [sp, #-16]`).
- With `SA_ONSTACK` and the guest stack, all 14 words survived.
- A write-protect `SIGSEGV` forwarded by ART's sigchain also kept all 14 words.

A plain app process also has handlers *without* `SA_ONSTACK`:
- 7 (`SIGBUS`), which the core's crash handler replaces;
- bionic's reserved RT signals 34 (libcore I/O interrupt), 36 (heapprofd /
  traced_perf) and 38 (ART heap dumps).

Bionic does not let an app block those three. Its `sigprocmask64`, which
`pthread_sigmask` goes through, runs `filter_reserved_signals`. That forces every
reserved RT signal `__SIGRTMIN + 1..8` to stay unblocked (checked in AOSP
`libc/bionic/sigprocmask.cpp` and `libc/platform/bionic/reserved_signals.h`,
android15-release). Those reserved signals are 33 libbacktrace, 34 libcore,
35 debuggerd, 36 profilers, 37 coverage, 38 ART heap dumps, 39 fdtrack, 40
`android_run_on_all_threads`; main adds 41 for MTE.

Guest-thread entry still calls `s2a_android_guest_sigmask`. It asks to block
every non-fault signal whose handler lacks `SA_ONSTACK`, reads the mask back and
reports what libc left unblocked. On Android that is exactly 34, 36 and 38, so
the call cannot protect guest threads from them. It never asks for fault
signals, or for the signals whose sender waits for every thread (+1, +3, +8,
+9). Bionic enforces that already; the list matters for other libcs such as
musl on HarmonyOS.

Platform code therefore cannot close this gap on Android. A profiler, a heap dump,
libcore or `android_run_on_all_threads` can deliver a non-`SA_ONSTACK` signal to a
guest thread in the middle of a rewritten block.

The complete fix belongs in the rewriter: for Linux targets, stash with a
pre-index writeback (`stp S, T, [sp, #-16]!` ... `ldp S, T, [sp], #16`), so
nothing ever lives below `sp`. Instructions in a block that address `sp` would then
need their offsets adjusted. That is a core change for the native line.

Until then, another option is to re-register those three handlers with
`SA_ONSTACK` added. They would then run on bionic's 16 KiB per-thread alternate
stacks in every thread. That changes platform handlers process-wide, so it is not
done here.

Next integration work:

1. Consume the shared `platform/plat.h` / `launchers/linux` work after the
   agreed directory migration. Keep Android-only code here; avoid Darwin shim
   headers and new platform conditionals throughout `runtime/src`.
2. Build the Android loader on the probe: key page from real key numbers,
   per-thread key stamping, anonymous mapping + `mprotect` RX for rewritten
   images (targetSdk >= 29 forbids executable mappings of app-data files).
3. Connect the shared Vulkan backend from PR #1 to the render thread's surface
   lifecycle (`native/presenter.cpp`: hand-over, bounded release, pre-rotation);
   the presenter's clear colour is the placeholder for the renderer's frames.
4. Compose the Ryujinx services / GPU layer for Android, replace Apple audio and
   memory diagnostics, and attach the existing Npad input hook. The snapshot
   export is ready; the C# runtime is not linked in this APK.
5. Point the runtime at an installed package once the native core runs on Android:
   `games/<title id>/modules/` for the loader, the package as `S2A_GAME_DATA`.
   The on-screen gamepad and import are in place. The phone is shared:
   coordinate access before using it.
6. Delete `native/s2a_native_stub.c` once the native core runs on Android, and
   link the core in its place for the single-library build.

Outside this directory, only the rewriter target, the Apple loader's `TARGET`
refusal, and the rewrite cache key changed; Apple rewrite output is
byte-identical (`tests/rewrite_target_test.sh`). Passing a compile / capability probe does not establish
game compatibility.

## Android APK 环境 doctor（#107）

`bash android/doctor.sh` 只读检查当前源码要求的工具与缓存；
`--format json` 输出机器报告，`--plan` 仅打印准备步骤、不下载安装。
复用目录、离线限制及 Apple Silicon/Linux x64 支持矩阵见 [doctor 说明](doctor/README.md)。
环境、编译、签名、安装与游戏验收分开报告；本入口不调用 Gradle、构建或部署。

### Experimental in-process preparation (T05)

The opt-in [standalone JNI rewriter](rewriter/README.md) exposes `NativeRewriter`
for private frozen NSO/table inputs and isolated staging outputs. It uses the shared
C API with explicit Android options and serializes entries in its loaded library.
`S2ARM_ANDROID_REWRITER=1` (or `-Ps2arm.rewriter=1`) enables its Gradle/CMake target
and generated Capstone/LZ4 license assets; it is disabled by default. It does not
change the game-launch bridge or the installer's activation authority.

The Java-only gate compiles this facade. The actual host-JNI suite is run directly
with `python3 android/tests/native_rewriter_test.py` and by the platform
gate through `test.sh`, using JDK17, CMake and pinned offline C dependencies.
The gate also executes the native ZIP admission tests in explicit format-fixture
mode; supplying real NDK libraries remains a separate packaging check. Host
JVM/NDK and native ZIP checks have separate evidence from APK/App UID/device checks.

## Game/static screenshot JPEG provider

The Android build entry passes `S2AAndroidHost=true` as a global MSBuild property. The SDK can remove RuntimeIdentifier when building project references, so a local RID guess alone is insufficient. The platform build excludes Skia-specific code/assets; default hosts keep the original Skia JPEG implementation and keyboard renderer. The official APK library contract is unchanged.

`HostConfiguration.ScreenshotEncoder` carries packed, premultiplied RGBA8888, top row first, width/height and JPEG quality. The actual Android composition supplies `AndroidScreenshotEncoder`; native JNI invokes `CaptureEncoder` using Android Bitmap's JPEG codec. The JPEG allocation has one native owner and is released even if managed copying fails. Class lookup is performed on the Java registration thread, with no Activity retained; only threads attached by the encoder are detached. Invalid input, absent provider, Java exceptions, empty or incomplete output never return success.

This does not implement Android software keyboard UI: its existing Unsupported boundary remains. Host capture regressions and reproduction instructions are in [tests/capture](tests/capture/README.md). The formal game/static build, private APK static checks, host JVM JPEG tests and actual Android Bitmap/device execution are separate evidence; shared feasibility mode and device gameplay are not accepted by these host checks.

### Debug-only cold-process environment (`s2arm.debug_env`)

Debug APKs accept a **String** `KEY=VALUE` or **String[]** of those assignments in
`s2arm.debug_env`. This is a general diagnostic transport, not an alternative LAN
or service startup: the shared services read the ordinary process environment.
For an already built per-game debug APK, append to its normal cold launch:

```sh
adb shell am start -n '<package>/org.s2arm.android.MainActivity' \
  --es s2arm.debug_env 'S2A_LDN_SERVER=lan'
# Multiple entries use one string-array extra, not repeated --es with the same key:
adb shell am start -n '<package>/org.s2arm.android.MainActivity' \
  --esa s2arm.debug_env 'S2A_LDN_SERVER=lan,S2A_TEST_NOTE=a=b'
```

Use a fresh process after the normal shutdown flow; a running Activity's new
intent is rejected for this extra, and native host creation permanently closes
the process configuration window (including Activity recreation). The batch is
applied after the existing fixed cold-start switches and before `create` /
`createGame` / HostApi registration; thus an explicit debug environment value
wins over a fixed switch's default. It is not saved across process launches.

Contract: split each entry at the **first** `=`; the rest is the value, including
additional `=` or an empty value. Names must match `S2A_[A-Za-z0-9_]+`; bare
`S2A_`, duplicate names, missing `=`, null entries, wrong extra types, NUL and
unpaired UTF-16 surrogates are rejected. Java encodes values as UTF-8 (not JNI
modified UTF-8). Maximum 32 entries, 8192 UTF-8 bytes per entry and 65536 bytes
in the entire batch. No trimming, newline splitting or shell evaluation occurs.
For `--esa`, commas are the adb array delimiter; values containing commas require
adb's escaping or an actual String[] Intent. A single `--es` preserves commas.

The complete batch is validated before any environment write. A system `setenv`
failure is **not** claimed to be atomic: it permanently poisons this process's
host-start admission, so a partially applied batch cannot boot services or be
retried in the same process. Errors/logs from this transport omit names/values.
Do not pass credentials through diagnostic command lines. This does not grant
permissions or configure system properties, credentials or security settings.

Release Java does not parse or call this entry; native builds outside CMake
`Debug` return `ENOTSUP` without inspecting data or changing the environment.
Both build-time gates are required; an Intent cannot opt a release build in.
`python3 android/tests/debug_environment_test.py` runs real host JVM/JNI
parser, release, one-shot, race, failure and load-order contracts plus source
wiring checks. Its Android property adapters do not simulate an App/game/NSD
startup. Full APK/device startup and LAN timing remain separate validation.

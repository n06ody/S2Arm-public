#!/usr/bin/env python3
"""Actual first-surface methods + NativeLease/budget; controlled ABI/Vulkan, no GPU or Android.
Also compile the real native stop/admission methods to check startup-only cancellation ordering.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]


def method(source, marker):
    assert source.count(marker) == 1, marker
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    withdraw_native = sys.argv[1:] == ['--withdraw-native']
    if sys.argv[1:] and not withdraw_native:
        raise SystemExit('usage: startup_surface_test.py [--withdraw-native]')
    surface = HERE.parent/'AndroidVulkanSurface.cs'
    runtime = REPO/'graphics/vulkan/runtime/AndroidVulkanRuntime.cs'
    native = REPO/'android/native/host_android.c'
    code = surface.read_text()
    methods = '\n'.join(method(code, marker) for marker in ['private SurfaceKHR CreateInitialSurface(', 'internal SurfaceKHR CreateSurface('])
    constructor = method(code, 'public AndroidVulkanSurface(').replace('public AndroidVulkanSurface(', 'public Boundary(', 1)
    assert 'StartupWait.Wait(_ready, _lifecycle, 30_000)' in runtime.read_text(), 'real GPU Start must use the tested foreground budget'
    window_code = (HERE.parent/'AndroidWindow.cs').read_text()
    renderer_code = (REPO/'compat/tree/src/Ryujinx.Graphics.Vulkan/VulkanRenderer.cs').read_text()
    window_ctor = method(window_code, 'public AndroidWindow(')
    renderer_dispose = method(renderer_code, 'public unsafe void Dispose()')
    assert '_renderer.S2ADeviceCleanup = ReleaseDeferredPresentResources;' in window_ctor, 'actual window must register deferred resources with device owner'
    assert renderer_dispose.index('_window.Dispose();') < renderer_dispose.index('S2ADeviceCleanup?.Invoke();') < renderer_dispose.index('Api.DestroyDevice(_device, null);'), 'device owner must release deferred resources after window retirement and before device destruction'
    with tempfile.TemporaryDirectory(prefix='s2a-startup-surface-') as directory:
        out = Path(directory)
        (out/'Program.cs').write_text((HERE/'StartupSurfaceFixture.cs.in').read_text().replace('PRODUCTION_METHODS', methods).replace('PRODUCTION_CONSTRUCTOR', constructor))
        for source in [HERE.parent/'NativeLease.cs', HERE.parent/'RetirementLedger.cs', runtime.parent/'StartupWait.cs']:
            shutil.copy2(source, out/source.name)
        (out/'Proof.csproj').write_text('<Project Sdk="Microsoft.NET.Sdk"><PropertyGroup><OutputType>Exe</OutputType><TargetFramework>net10.0</TargetFramework><AllowUnsafeBlocks>true</AllowUnsafeBlocks><Nullable>annotations</Nullable><TreatWarningsAsErrors>true</TreatWarningsAsErrors></PropertyGroup></Project>')
        (out/'NuGet.Config').write_text('<configuration><packageSources><clear/></packageSources></configuration>')
        dotnet = os.environ.get('DOTNET', shutil.which('dotnet'))
        assert dotnet, '.NET 10 SDK required'
        subprocess.run([dotnet, 'build', str(out/'Proof.csproj'), '--configfile', str(out/'NuGet.Config'), '-v:q'], check=True)
        print('STARTUP_MANAGED_COMPILED', flush=True)
        subprocess.run([dotnet, str(out/'bin/Debug/net10.0/Proof.dll')], check=True, timeout=20)
        native_code = native.read_text()
        methods = '\n'.join(method(native_code, marker) for marker in [
            'static void copy_utf8(', 'static void request_stop(', 'int s2a_android_host_try_enter_guest('])
        (out/'stop.c').write_text((HERE/'StartupStopFixture.c.in').read_text().replace('PRODUCTION_METHODS', methods))
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', str(out/'stop.c'), '-o', str(out/'stop')], check=True)
        print('STARTUP_STOP_COMPILED', flush=True)
        subprocess.run([str(out/'stop')], check=True, timeout=20)
        bridge = (native.parent/'bridge.cpp').read_text()
        getter = 'jlong ' + method(bridge, 'Java_org_s2arm_android_NativeBridge_gamePresentCount(')
        if withdraw_native:
            original = 'if (uint64_t native = s2a_android_nvn_present_count())'
            assert getter.count(original) == 1, 'native present counter mutation anchor'
            getter = getter.replace(original, 'if (uint64_t native = 0)')
        (out/'present.cpp').write_text((HERE/'StartupPresentFixture.cpp.in').read_text().replace('PRODUCTION_METHODS', getter))
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                        '-I'+str(native.parent), '-I'+str(REPO/'runtime/include'), str(out/'present.cpp'), '-o', str(out/'present')], check=True)
        print('STARTUP_PRESENT_COMPILED', flush=True)
        subprocess.run([str(out/'present')], check=True, timeout=20)
        print('Startup surface source SHA256=' + hashlib.sha256(code.encode()).hexdigest())


if __name__ == '__main__':
    main()

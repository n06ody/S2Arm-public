#!/usr/bin/env python3
"""Compile production Nvdec sources and exercise offline failure boundaries."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import sys

here=Path(__file__).resolve().parent
root=here.parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--library',type=Path)
parser.add_argument('--out',type=Path)
args=parser.parse_args()
with tempfile.TemporaryDirectory(prefix='s2a-avc-managed-') as temporary:
    out=args.out or Path(temporary)
    out.mkdir(exist_ok=True,parents=True)
    config=out/'NuGet.Config'; config.write_text('<configuration><packageSources><clear /></packageSources></configuration>')
    command=['dotnet','build',str(here/'managed/Avc.Tests.csproj'),'--configfile',str(config),'-p:NuGetAudit=false','-p:ArtifactsPath='+str(out/'artifacts'),'-v:q']
    build=subprocess.run(command,capture_output=True,text=True)
    (out/'build.log').write_text(build.stdout+build.stderr)
    if build.returncode: sys.exit(build.stdout+build.stderr)
    binary=next((out/'artifacts/bin/Avc.Tests').rglob('Avc.Tests.dll'))
    env=dict(os.environ); env.pop('S2A_AVC_LIBRARY',None); env.pop('S2A_MC_LIBRARY',None)
    contracts=subprocess.run(['dotnet',str(binary)],env=env,capture_output=True,text=True)
    log=contracts.stdout+contracts.stderr
    (out/'contracts.log').write_text(log)
    assert contracts.returncode==0 and 'SENTINEL' in log and '【红】' in log, log
    # Consume the actual logger output with the current production redline parser.
    sys.path.insert(0,str(root/'tools'))
    import redline
    assert redline.red_lines(log), 'current parser failed to detect Nvdec red'
    # The baseline non-Apple implementation selected this exact FFmpeg decoder.
    old=subprocess.run(['dotnet',str(binary),'baseline-ffmpeg'],env=env,capture_output=True,text=True)
    (out/'baseline-ffmpeg.log').write_text(old.stdout+old.stderr)
    assert old.returncode!=0 and 'avutil' in old.stdout+old.stderr, old.stdout+old.stderr
    import platform
    missing=out/('missing.dylib' if platform.system()=='Darwin' else 'missing.so')
    source=out/'missing.c';source.write_text('void fixture_missing_entry(void) {}\n')
    subprocess.run(['cc','-fPIC','-shared',str(source),'-o',str(missing)],check=True)
    mc=out/('mc.dylib' if platform.system()=='Darwin' else 'mc.so')
    subprocess.run(['cc','-fPIC','-shared',str(here/'managed/mc_fixture.c'),'-o',str(mc)],check=True)
    for mode in ('ok','latefail','pending','starve','noprobe','createfail','decodefail'):
        mc_env=dict(env,S2A_MC_LIBRARY=str(mc),S2A_MC_FAKE_MODE=mode)
        run=subprocess.run(['dotnet',str(binary),'vp9'],env=mc_env,capture_output=True,text=True)
        (out/f'vp9-{mode}.log').write_text(run.stdout+run.stderr)
        assert run.returncode==0 and 'PASS vp9 selection boundary mode='+mode in run.stdout, run.stdout+run.stderr
    missing_env=dict(env,S2A_AVC_LIBRARY=str(missing))
    entry=subprocess.run(['dotnet',str(binary)],env=missing_env,capture_output=True,text=True)
    (out/'missing-entry.log').write_text(entry.stdout+entry.stderr)
    assert entry.returncode==0 and 'EntryPointNotFoundException' in entry.stdout+entry.stderr
    if args.library:
        env['S2A_AVC_LIBRARY']=str(args.library.resolve())
        vectors=subprocess.run(['dotnet',str(binary),str(here/'vectors')],env=env,capture_output=True,text=True)
        (out/'vectors.log').write_text(vectors.stdout+vectors.stderr)
        assert vectors.returncode==0, vectors.stdout+vectors.stderr
        print(vectors.stdout,end='')
    print('PASS managed production sources: backend routes, no-backend/loader red+false, sentinel, guest untouched, parser')

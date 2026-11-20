#!/usr/bin/env python3
"""Offline production lifetime regression; no device or GPU."""
import argparse
import json
import os
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path, required=True)
parser.add_argument('--dotnet', default='dotnet')
args = parser.parse_args()
here = Path(__file__).resolve().parent
root = here.parents[2]
out = args.out.resolve()
if out == root or root in out.parents:
    parser.error("output must be outside checkout")
out.mkdir(parents=True, exist_ok=False)
config = out / 'NuGet.Config'
config.write_text('<configuration><packageSources><clear /></packageSources></configuration>')
rows = []

def run(name, command, red=False, contains=None, env=None):
    p = subprocess.run(command, cwd=root, capture_output=True, text=True, timeout=120, env=env)
    text = p.stdout + p.stderr
    (out / (name + '.log')).write_text(text)
    good = (p.returncode != 0 if red else p.returncode == 0) and (not contains or contains in text)
    rows.append({'name': name, 'exit': p.returncode, 'passed': good, 'command': command})
    (out / 'results.json').write_text(json.dumps(rows, indent=2))
    if not good:
        raise RuntimeError(name + ': ' + text[-3000:])

run('build', [args.dotnet, 'build', str(here / 'AudioLifetime.csproj'), '--configfile', str(config),
              '-p:ArtifactsPath=' + str(out / 'artifacts'), '-v:q'])
binary = next((out / 'artifacts/bin').rglob('AudioLifetime.dll'))
run('partial-start', [args.dotnet, str(binary), 'partial-start'], contains='PASS partial audio start cleanup and retry',
    env={**os.environ, 'S2A_AUDIO_INLINE': '1'})
run('partial-start-split', [args.dotnet, str(binary), 'partial-start'], contains='PASS partial audio start cleanup and retry',
    env={**os.environ, 'S2A_AUDIO_INLINE': '0'})
run('old-order', [args.dotnet, str(binary), 'old-order'], True, 'AudioProcessor.Signal')
run('fixed', [args.dotnet, str(binary)], contains='PASS ')
print('PASS AudioLifetime old-order red, fixed green; host fixture only')

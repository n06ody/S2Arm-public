#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Compile a private, exact captured request with only the shared-memory profile limit changed.
Run the matching diagnostic-head compiler build. Never infer missing shader facts.
"""
import argparse
import base64
import copy
import hashlib
import json
from pathlib import Path
import subprocess

p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--request',type=Path,required=True)
p.add_argument('--compiler',type=Path,required=True)
p.add_argument('--out',type=Path,required=True)
a=p.parse_args();a.out.mkdir(parents=True,exist_ok=False)
request=json.loads(a.request.read_bytes())
shared=request['program']['computeState']['sharedMemoryBytes']
results={}
for tag in ('spill','original'):
    r=copy.deepcopy(request)
    if tag=='original':
        profile=r['profile'];profile['capabilities']['maximumComputeSharedMemorySize']=65536
        # Composer hashes the ordered, compact JSON body with profileHash omitted.
        body={k:v for k,v in profile.items() if k!='profileHash'}
        profile['profileHash']='sha256-'+hashlib.sha256(json.dumps(body,separators=(',',':'),ensure_ascii=True).encode()).hexdigest()
    rp=a.out/(tag+'.request.json');rp.write_text(json.dumps(r,separators=(',',':')))
    dest=a.out/tag
    subprocess.run(['dotnet',str(a.compiler),'compile','--request',str(rp),'--output',str(dest)],check=True)
    doc=json.loads((dest/'program.json').read_bytes())['content']
    if len(doc['stages'])!=1 or doc['stages'][0]['stage']!='cs':
        raise ValueError('not a single compute stage')
    code=base64.b64decode(doc['stages'][0]['spirv'],validate=True)
    if hashlib.sha256(code).hexdigest()!=doc['stages'][0]['spirvSha256']:
        raise ValueError('SPIR-V checksum mismatch')
    (a.out/(tag+'.spv')).write_bytes(code)
    results[tag]=doc
spill=[b for b in results['spill']['bindings'] if b['kind']=='spill']
if len(spill)!=1 or [b for b in results['spill']['bindings'] if b['kind']!='spill']!=results['original']['bindings']:
    raise ValueError('interface changed beyond shared-memory spill')
info={'shared':shared,'spill':spill[0],'request_sha256':hashlib.sha256(a.request.read_bytes()).hexdigest(),
      'only_profile_change':'capabilities.maximumComputeSharedMemorySize=65536 and derived profileHash',
      'source_identity':a.request.name.removeprefix('csf-').removesuffix('.request.json')}
(a.out/'pair.json').write_text(json.dumps(info,indent=2)+'\n')
print(json.dumps(info))

#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 S2Arm contributors
"""Host tests of the production compute program parser, not GPU validation."""
import base64
import copy
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
NATIVE = ROOT / 'android/native'
SPIRV = struct.pack('<5I', 0x07230203, 0x10000, 0, 1, 0)

def fixture():
    return {'schema': 's2a-direct-program-v1', 'content': {
        'contract': 's2a-direct-program-content-v1', 'kind': 'compute', 'hasUnalignedStorageBuffer': False, 'c0Reads': [], 'supportReads': [],
        'supportScaleRanges': [{'name':'render_scale','offset':192,'stride':16,'count':73,'elementBytes':4},
                               {'name':'render_scale_y','offset':1440,'stride':16,'count':73,'elementBytes':4}],
        'stages': [{'stage': 'cs', 'spirv': base64.b64encode(SPIRV).decode(), 'attributeReadMask': 0}],
        'bindings': [{'stage': 'program', 'kind': 'support', 'set': 0, 'binding': 0, 'count': 1},
                     {'stage': 'cs', 'kind': 'storage', 'set': 1, 'binding': 0, 'rw': True, 'cbSlot': 0, 'cbOffset': 0x380, 'nvnSlot': 7, 'count': 1}],
        'supportLayout': {'requiredSize': 2608}}}

HARNESS = r'''
#include "nvn/nvn_vk_program_result.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc,char **argv) {
    if(argc!=2)return 2;
    FILE*f=fopen(argv[1],"rb");if(!f)return 2;
    fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);unsigned char*b=malloc(n);
    if(!b)return 2;
    if(fread(b,1,n,f)!=(size_t)n)return 2;fclose(f);
    struct nvn_vk_compute_program p={0};char why[256]={0};
    int rc=nvn_vk_program_parse_compute(b,n,&p,why,sizeof why);
    if(rc) {puts(why);nvn_vk_program_free_compute(&p);free(b);return 1;}
    printf("%d %zu %u %d",p.stage.present,p.stage.spirv_bytes,p.support_bytes,p.binding_count);
    for(int i=0;i<p.binding_count;i++)printf(" | %d,%d,%d,%d,%d,%d,%d",p.bindings[i].set,p.bindings[i].binding,p.bindings[i].kind,p.bindings[i].slot,p.bindings[i].stage_idx,p.bindings[i].ubo_slot,p.bindings[i].tex_slot);
    for(int i=0;i<p.binding_count;i++)if(p.bindings[i].kind==1)printf(" / %u,%u,%d,%u,%u",p.storage[i].cb_slot,p.storage[i].cb_offset,p.storage[i].mapping,p.storage[i].index,p.storage[i].offset);
    puts("");nvn_vk_program_free_compute(&p);nvn_vk_program_free_compute(&p);free(b);return 0;
}
'''

class ComputeProgram(unittest.TestCase):
    def test_real_parser_and_mutations(self):
        cases = [('valid', fixture(), 0)]
        def bad(label, change):
            obj=fixture();change(obj['content']);cases.append((label,obj,1))
        for value in [None, 0, 1, 'false', [], {}]:
            bad('invalid_alignment_'+str(value),lambda c,v=value:c.update(hasUnalignedStorageBuffer=v))
        bad('missing_alignment',lambda c:c.pop('hasUnalignedStorageBuffer'))
        obj=fixture();obj['content']['hasUnalignedStorageBuffer']=True;cases.append(('true_alignment',obj,0))
        # 合并后语义：c0Reads=null = 编译器分析不出读集（unknown），运行时按完整驱动布局填，接受；
        # 空数组仍是“已证明 0 条读”。非数组非 null 与缺失仍拒绝。
        obj=fixture();obj['content']['c0Reads']=None;cases.append(('c0_reads_null',obj,0))
        for value in [{}, 1, 'unknown', [[0x610, 4]], [[0x208, 8]], [[0x21c, 4]], [[0x210, 16]], [[0x200, 0]], [[0xffffffff, 4]]]:
            bad('invalid_c0_'+str(value),lambda c,v=value:c.update(c0Reads=v))
        bad('missing_c0',lambda c:c.pop('c0Reads'))
        for value in [None, 0, 1, 'true']:
            bad('invalid_rw_'+str(value),lambda c,v=value:c['bindings'][1].update(rw=v))
        bad('missing_rw',lambda c:c['bindings'][1].pop('rw'))
        obj=fixture();obj['content']['c0Reads']=[[0x200,12],[0x210,12],[0x400,12]];obj['content']['bindings'][0].update(stage='cs',kind='uniform',slot=0);cases.append(('system_c0',obj,0))
        bad('missing_support_reads',lambda c:c.pop('supportReads'))
        # supportReads=null 同理：unknown 接受（count=0、support_reads_unknown=1）；非数组非 null 仍拒。
        obj=fixture();obj['content']['supportReads']=None;cases.append(('support_reads_null',obj,0))
        for value in [{}, [[0,4]], [[1392,4]], [[1408,4]], [[196,4]], [[192,16]], [[1344,8]], [[2596,4]], [[192,0]]]:
            bad('bad_support_reads_'+str(value),lambda c,v=value:c.update(supportReads=v))
        obj=fixture();obj['content']['supportReads']=[[192,4],[1344,4],[1440,4],[2592,4]];cases.append(('valid_scale_reads',obj,0))
        bad('missing_scales',lambda c:(c.update(supportReads=[[192,4]]),c.pop('supportScaleRanges')))
        for key,value in [('stride',0),('stride',2),('count',0),('count',0xffffffff),('elementBytes',16),('offset',0xffffffff),('name','flags')]:
            bad('bad_scale_'+key,lambda c,k=key,v=value:(c.update(supportReads=[[192,4]]),c['supportScaleRanges'][0].update({k:v})))
        bad('duplicate_scale',lambda c:(c.update(supportReads=[[192,4]]),c['supportScaleRanges'][1].update(name='render_scale')))
        bad('overlap_scales',lambda c:(c.update(supportReads=[[192,4]]),c['supportScaleRanges'][1].update(offset=192)))
        obj=fixture();obj['content']['supportScaleRanges']=None;cases.append(('no_support_no_type_proof',obj,0))
        bad('missing_support_binding',lambda c:(c.update(supportReads=[[192,4]]),c['bindings'].pop(0)))
        # NVN cbuf 语义：0 = c0/support、1 = c1 常量段、2 = 纹理句柄表（未实现）、3..18 = 用户 uniform。
        obj=fixture();obj['content']['bindings'].append({'stage':'cs','kind':'uniform','set':0,'binding':3,'slot':1,'count':1});cases.append(('uniform_c1',obj,0))
        obj=fixture();obj['content']['bindings'].append({'stage':'cs','kind':'uniform','set':0,'binding':3,'slot':2,'count':1});cases.append(('uniform_c2_texture_bank',obj,1))
        obj=fixture();obj['content']['bindings'].append({'stage':'cs','kind':'uniform','set':0,'binding':3,'slot':19,'count':1});cases.append(('uniform_slot19',obj,1))
        bad('graphics_kind',lambda c:c.update(kind='graphics'))
        bad('graphics_stage',lambda c:c['stages'][0].update(stage='fs'))
        bad('two_stages',lambda c:c['stages'].append(copy.deepcopy(c['stages'][0])))
        bad('zero_stages',lambda c:c.update(stages=[]))
        # texture 是 compute 的一等资源：合法 handleIndex（8+2*slot）接受；奇数/越界/缺 slot/夹带
        # storage 地址字段拒绝。存储图像仍未接，按 set/binding/handleIndex 报红。
        def texgood(label):
            obj=fixture();obj['content']['bindings'][1]={'stage':'cs','kind':'texture','set':2,'binding':3,'slot':8,'count':1};cases.append((label,obj,0))
        def texbad(label,change):
            obj=fixture();obj['content']['bindings'][1]={'stage':'cs','kind':'texture','set':2,'binding':3,'slot':8,'count':1};change(obj['content']);cases.append((label,obj,1))
        texgood('texture_binding')
        texbad('texture_odd_handle',lambda c:c['bindings'][1].update(slot=9))
        texbad('texture_handle_out_of_range',lambda c:c['bindings'][1].update(slot=72))
        texbad('texture_missing_slot',lambda c:c['bindings'][1].pop('slot'))
        texbad('texture_with_storage_fields',lambda c:c['bindings'][1].update(nvnSlot=7))
        obj=fixture();obj['content']['bindings'][1]={'stage':'cs','kind':'image','set':3,'binding':0,'slot':72,'count':1};cases.append(('unsupported_image',obj,1))
        bad('unknown_kind',lambda c:c['bindings'][1].update(kind='future'))
        bad('wrong_binding_stage',lambda c:c['bindings'][1].update(stage='vs'))
        bad('duplicate_binding',lambda c:c['bindings'][1].update(set=0))
        # FD_MAX_BINDINGS 按 producer 合同推导：1 + 3×(18 uniform + 16 storage + 64 texture + 16 image)
        # = 343（图形程序固定 vs+fs+gs 三阶段，再加 1 条全程序共享 support；compute 只 cs 一阶段
        # 1+18+16=35，被同一上限覆盖）。17 条是 0100F2C0115B6000 实测的下限证据，不是上限；65/343 必须接受，
        # 344 越过合同才准确失败——与 nvn_vk_program_binding_cap_test.c 的 cap=343 断言同源。
        obj=fixture();obj['content']['bindings']=[{'stage':'program','kind':'support','set':0,'binding':i,'count':1} for i in range(17)];cases.append(('seventeen_bindings',obj,0))
        obj=fixture();obj['content']['bindings']=[{'stage':'program','kind':'support','set':0,'binding':i,'count':1} for i in range(65)];cases.append(('sixty_five_bindings',obj,0))
        obj=fixture();obj['content']['bindings']=[{'stage':'program','kind':'support','set':0,'binding':i,'count':1} for i in range(343)];cases.append(('contract_max_bindings',obj,0))
        obj=fixture();obj['content']['bindings']=[{'stage':'program','kind':'support','set':0,'binding':i,'count':1} for i in range(344)];cases.append(('over_contract_bindings',obj,1))
        bad('missing_mapping',lambda c:c['bindings'][1].pop('nvnSlot'))
        bad('wrong_guest_slot',lambda c:c['bindings'][1].update(nvnSlot=16))
        bad('legacy_translator_slot',lambda c:c['bindings'][1].update(slot=7))
        bad('negative_slot',lambda c:c['bindings'][1].update(slot=-1))
        bad('overflow_slot',lambda c:c['bindings'][1].update(slot=4294967296))
        bad('float_slot',lambda c:c['bindings'][1].update(slot=7.0))
        bad('descriptor_array',lambda c:c['bindings'][1].update(count=2))
        bad('set_limit',lambda c:c['bindings'][1].update(set=4))
        bad('small_support',lambda c:(c.update(supportReads=[[192,4]]),c['supportLayout'].update(requiredSize=16)))
        bad('vertex_input',lambda c:c['stages'][0].update(attributeReadMask=1))
        bad('bad_spirv',lambda c:c['stages'][0].update(spirv=base64.b64encode(b'\0'*20).decode()))
        for label, fields in [('nvn0',dict(cbSlot=0,cbOffset=0x310,nvnSlot=0)),
            ('nvn1',dict(cbSlot=0,cbOffset=0x320,nvnSlot=1)),
            ('uboPtr1',dict(cbSlot=0,cbOffset=0x220,uboPtr=1)),
            ('sbufUbo',dict(cbSlot=3,cbOffset=0x38,uboSlot=0,uboOffset=0x38))]:
            obj=fixture();obj['content']['bindings'][1].pop('nvnSlot');obj['content']['bindings'][1].update(fields);cases.append((label,obj,0))
        bad('mapping_mismatch',lambda c:c['bindings'][1].update(nvnSlot=0))
        bad('unaligned_source',lambda c:c['bindings'][1].update(cbOffset=0x381))
        bad('two_mappings',lambda c:c['bindings'][1].update(uboPtr=7))
        bad('missing_source',lambda c:c['bindings'][1].pop('cbSlot'))
        bad('wrong_source_bank',lambda c:c['bindings'][1].update(cbSlot=1))
        bad('orphan_ubo_offset',lambda c:c['bindings'][1].update(uboOffset=0))
        good=json.dumps(fixture(),separators=(',',':'))
        cases += [('duplicate_content_key',good.replace('"kind":"compute"','"kind":"compute","kind":"compute"'),1),
                  ('duplicate_slot',good.replace('"nvnSlot":7','"nvnSlot":7,"nvnSlot":7'),1),
                  ('trailing_bytes',good+'{}',1),('malformed_json',good.replace('"count":1','"count":01'),1),
                  ('large_spirv', {**fixture(), 'content': {**fixture()['content'], 'stages':[{'stage':'cs','spirv':base64.b64encode(SPIRV+b'\0'*2048).decode()}]}},0)]
        with tempfile.TemporaryDirectory(prefix='w2-program-') as directory:
            td=Path(directory);h=td/'h.c';h.write_text(HARNESS)
            source=(NATIVE/'nvn/nvn_vk_program_result.c').read_text()
            variants={'positive':source,
                'negative_accept_image':source.replace('if (dst->kind == 3) {', 'if (0) {'),
                'negative_texture_range':source.replace('dst->slot < 8 || dst->slot >= 72 || (dst->slot & 1)', '0'),
                'negative_texture_storage_fields':source.replace('if (dst->kind != 1) {', 'if (0) {'),
                'negative_support_padding':source.replace('r->size <= scale->element_bytes - delta % scale->stride','r->size <= scale->stride - delta % scale->stride'),
                'negative_support_required':source.replace('CS_FAIL("compute support reads missing");','reads=(struct s2a_jv){"[]",2};'),
                'negative_c0_boundary':source.replace('r->size > 12 - (r->offset - begin)', '0'),
                'negative_c0_required':source.replace('CS_FAIL("compute c0 reads are missing");', 'reads=(struct s2a_jv){"[]",2};'),
                'negative_rw_required':source.replace('if (s2a_jv_obj_find(b, "rw", &v)) return -1;', 'if (s2a_jv_obj_find(b, "rw", &v)) v=(struct s2a_jv){"true",4};'),
                'negative_alignment_required':source.replace('CS_FAIL("compute storage alignment fact missing");','alignment=(struct s2a_jv){"false",5};'),
                'negative_stage_check':source.replace('!compute_string_is(st, "stage", "cs")','0'),
                'negative_unknown_kind':source.replace('else CS_FAIL("unknown compute binding kind");','else dst->kind = 1;'),
                'negative_c1_slot':source.replace('if (dst->slot != 0 && dst->slot != 1 && (dst->slot < 3 || dst->slot > 18))','if ((dst->slot != 0 && dst->slot < 3) || dst->slot > 18)'),
                'negative_duplicate_binding':source.replace('CS_FAIL("duplicate compute binding");','(void)0;'),
                'negative_source_mapping':source.replace('nvn == (s->cb_offset - 0x310) / 16u', 'nvn < 16')}
            for label,code in variants.items():
                if label!='positive':self.assertNotEqual(code,source)
                c=td/(label+'.c');c.write_text(code);exe=td/label
                subprocess.run([os.environ.get('CC','cc'),'-std=c11','-I'+str(NATIVE),'-I'+str(NATIVE/'nvn'),str(h),str(c),str(NATIVE/'direct/jv.c'),str(NATIVE/'direct/json.c'),'-lm','-o',str(exe)],check=True)
                mismatches=[]
                for name,obj,expected in cases:
                    f=td/'input.json';f.write_text(obj if isinstance(obj,str) else json.dumps(obj,separators=(',',':')))
                    run=subprocess.run([str(exe),str(f)],text=True,capture_output=True)
                    if run.returncode!=expected:mismatches.append(name)
                    if label=='positive' and name=='valid':
                        self.assertEqual(run.stdout.strip(),'1 20 2608 2 | 0,0,0,-1,-1,-1,-1 | 1,0,1,-1,5,-1,-1 / 0,896,1,7,0')
                    if label=='positive' and name=='uniform_slot19':
                        self.assertIn('compute uniform slot 19 outside NVN cbuf range (set=0 binding=3)',run.stdout)
                    if label=='positive' and name=='uniform_c2_texture_bank':
                        self.assertIn('compute c2 texture-handle bank used as a uniform buffer is not implemented: set=0 binding=3',run.stdout)
                    if label=='positive' and name=='over_contract_bindings':
                        # 344 条必须准确报“超出上限”，不是静默截断到 343（与 cap 测试同一判据）。
                        self.assertIn('compute binding count exceeds limit',run.stdout)
                    if label=='positive' and name in ('nvn0','nvn1','uboPtr1','sbufUbo'):
                        self.assertEqual(run.stdout.strip().split(' / ')[1], {'nvn0':'0,784,1,0,0','nvn1':'0,800,1,1,0','uboPtr1':'0,544,2,1,0','sbufUbo':'3,56,3,0,56'}[name])
                if label=='positive':self.assertEqual(mismatches,[])
                else:self.assertTrue(mismatches,'mutation unexpectedly passed')
                print(label,len(cases),'cases', 'mismatches',mismatches)

if __name__=='__main__':unittest.main()

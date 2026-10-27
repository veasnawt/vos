#!/usr/bin/env python3
"""Exact USB.1 ELF/map accounting using retained, comparable build outputs.

No rebuild, relink or linker changes. The input map must belong to its ELF.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def sections(path):
    b=path.read_bytes()
    if b[:6]!=b'\x7fELF\x02\x01':raise ValueError('expected ELF64 little endian')
    off=struct.unpack_from('<Q',b,40)[0];size,count,names=struct.unpack_from('<HHH',b,58)
    rows=[struct.unpack_from('<IIQQQQIIQQ',b,off+i*size) for i in range(count)]
    strings=b[rows[names][4]:rows[names][4]+rows[names][5]]
    return b,{strings[r[0]:].split(b'\0',1)[0].decode():dict(type=r[1],flags=r[2],address=r[3],offset=r[4],size=r[5],alignment=r[8]) for r in rows if r[0]}


def input_sections(map_text,name):
    start=map_text.index('\n'+name+' ')
    match=re.search(r'\n\.[A-Za-z]',map_text[start+1:])
    block=map_text[start:start+1+match.start()] if match else map_text[start:]
    rows=[];pending=''
    for line in block.splitlines():
        if re.match(r'^ \.[^\s]+$',line):pending=line;continue
        combined=(pending+' '+line.strip()) if pending else line
        pending=''
        m=re.match(r'^ (\.[^\s]+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)$',combined)
        if m and int(m[3],16):rows.append(dict(section=m[1],address=int(m[2],16),size=int(m[3],16),owner=m[4]))
    if not rows:raise ValueError('map input sections not found '+name)
    return block,rows


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline',type=Path,required=True);p.add_argument('--migration',type=Path,required=True)
    p.add_argument('--out',type=Path,required=True);a=p.parse_args();out=a.out.resolve()
    if out.exists() or not out.is_relative_to(ROOT/'build'):p.error('fresh output below build/ required')
    out.mkdir(parents=True);result={'passed':False,'images':{}}
    try:
        for label,path in [('C',a.baseline.resolve()),('Rust',a.migration.resolve())]:
            raw,sec=sections(path);map_path=path.parent/'kernel.map';text=map_path.read_text()
            image={'path':str(path),'sha256':hashlib.sha256(raw).hexdigest(),'ELF_file_bytes':len(raw),'map_sha256':hashlib.sha256(map_path.read_bytes()).hexdigest(),'sections':sec,'loads':[],'inputs':{}}
            phoff=struct.unpack_from('<Q',raw,32)[0];ps,pn=struct.unpack_from('<HH',raw,54)
            for i in range(pn):
                typ,flags,off,va,pa,fs,ms,al=struct.unpack_from('<IIQQQQQQ',raw,phoff+i*ps)
                if typ==1:image['loads'].append(dict(flags=flags,offset=off,virtual=va,physical=pa,file_bytes=fs,memory_bytes=ms,alignment=al))
            loads=image['loads'];image['PT_LOAD_file_bytes']=sum(x['file_bytes'] for x in loads);image['PT_LOAD_memory_bytes']=sum(x['memory_bytes'] for x in loads)
            image['physical_reserved_bytes']=max(x['physical']+x['memory_bytes'] for x in loads)-min(x['physical'] for x in loads)
            for name in ['.text','.rodata']:
                block,rows=input_sections(text,name);(out/(label+'-'+name[1:]+'-map.txt')).write_text(block)
                used=max(x['address']+x['size'] for x in rows)-sec[name]['address'];total=sum(x['size'] for x in rows)
                if used>sec[name]['size']:raise ValueError('map/ELF range mismatch')
                image['inputs'][name]={'sections':rows,'allocated_input_bytes':total,'used_extent_with_internal_alignment':used,'internal_alignment_bytes':used-total,'tail_padding_bytes':sec[name]['size']-used}
            for name,args in [('sections',['readelf','-SW']),('loads',['readelf','-lW']),('symbols',['nm','-S','--size-sort'])]:
                (out/f'{label}-{name}.txt').write_text(subprocess.check_output([*args,str(path)],text=True))
            assert len(loads)==5 and all(x['flags']!=7 for x in loads)
            subprocess.run(['python3','-I','-B','scripts/check-kernel-layout.py',str(path)],cwd=ROOT,check=True,stdout=subprocess.PIPE)
            result['images'][label]=image
        c=result['images']['C'];r=result['images']['Rust']
        result['deltas']={key:r[key]-c[key] for key in ['ELF_file_bytes','PT_LOAD_file_bytes','PT_LOAD_memory_bytes','physical_reserved_bytes']}
        result['section_deltas']={name:r['sections'].get(name,{}).get('size',0)-c['sections'].get(name,{}).get('size',0) for name in sorted(c['sections'].keys()|r['sections'].keys())}
        result['text_padding_reconciliation']={key:r['inputs']['.text'][key]-c['inputs']['.text'][key] for key in ['allocated_input_bytes','used_extent_with_internal_alignment','internal_alignment_bytes','tail_padding_bytes']}
        tables=[x for x in r['inputs']['.rodata']['sections'] if x['section'] in ['.rodata.xhci_probe','.rodata..Lswitch.table.xhci_decode_caps']]
        raw,sec=sections(a.migration.resolve());s=sec['.rodata']
        contents=[raw[s['offset']+t['address']-s['address']:s['offset']+t['address']-s['address']+t['size']] for t in tables]
        regions=[]
        for content in contents:
            targets=[x[0] for x in struct.iter_unpack('<Q',content)]
            regions.append(sorted({name for name in ['.text','.rodata'] if all(sec[name]['address']<=v<sec[name]['address']+sec[name]['size'] for v in targets)}))
        result['generated_switch_tables']={'bytes_each':[len(x) for x in contents],'byte_identical':len(contents)==2 and contents[0]==contents[1],
                                          'target_regions':regions,'physical_page_saving_from_removing_either_alone':0,
                                          'policy':'different purposes: code dispatch versus string lookup; no blind deduplication'}
        result['passed']=True
    except BaseException as error:result['error']=str(error);raise
    finally:(out/'result.json').write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='images'},indent=2))


if __name__=='__main__':main()

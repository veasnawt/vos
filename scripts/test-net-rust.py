#!/usr/bin/env python3
"""R3-N1 Rust ARP/IPv4/UDP: actual archive, frozen accepted C oracle (879c511), whole-arena differential,
isolated mutants, frames against the accepted C, panic freedom and symbol ownership.
Fresh confined outputs only. No cleanup or historical evidence changes.
"""
import argparse,hashlib,json,os,re,subprocess,csv,io,statistics
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
COVERAGE='PASS 635285 network comparisons and 3000 random operation sequences'
PUBLIC=['net_checksum','net_ipv4_transport_checksum','net_ethernet_type','net_ipv4_payload','net_ipv4_begin','net_arp_init','net_neighbor_get','net_arp_query',
        'net_ethernet_input','net_ipv4_init','net_ipv4_echo_request','net_ipv4_input','net_udp_init','net_udp_send','net_udp_input','net_udp_receive','net_udp_close']
ENTRY={n:('vos_rust_net_transport_checksum' if n=='net_ipv4_transport_checksum' else 'vos_rust_'+n) for n in PUBLIC}
# Frame exceptions awaiting source-specific owner approval: {public name: Rust bytes allowed}. C 0 B leaf.
FRAME_EXCEPTIONS={'net_ipv4_transport_checksum':16}
MODULES=('checksum','context','ipv4','arp','udp')
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def need(x,s):
 if not x:raise RuntimeError(s)
MUTATIONS=[ # (name, core file, exact source text, replacement, intended failing test)
 ('ck-fold','checksum.rs','    while sum >> 16 != 0 {\n        sum = (sum & 0xffff) + (sum >> 16);\n    }\n    sum as u16','    sum as u16','checksum_end_around_carry'),
 ('ck-odd-high','checksum.rs','sum = sum.wrapping_add((*last as u32) << 8);','sum = sum.wrapping_add(*last as u32);','checksum_odd_byte_padded_high'),
 ('ck-invert','checksum.rs','    !folded_sum(bytes)','    folded_sum(bytes)','checksum_rfc1071_vector'),
 ('ck-word-order','checksum.rs','(*hi as u32) * 256 + *lo as u32','(*lo as u32) * 256 + *hi as u32','checksum_rfc1071_vector'),
 ('tr-proto','checksum.rs','.wrapping_add(protocol as u32)','.wrapping_add(protocol as u32 & 0)','transport_pseudoheader'),
 ('tr-length','checksum.rs','.wrapping_add(bytes.len() as u32)','.wrapping_add(bytes.len() as u32 + 1)','transport_pseudoheader'),
 ('tr-fold4','checksum.rs','    ((s & 0xffff) + (s >> 16)) as u16','    (s & 0xffff) as u16','transport_address_carry'),
 ('unicast-zero','checksum.rs','ip[0] != 0 && ip[0] != 127','ip[0] != 127','ip_unicast_bounds'),
 ('unicast-loopback','checksum.rs','ip[0] != 0 && ip[0] != 127 && ip[0] < 224','ip[0] != 0 && ip[0] < 224','ip_unicast_bounds'),
 ('unicast-class-d','checksum.rs','&& ip[0] < 224','&& ip[0] <= 224','ip_unicast_bounds'),
 ('mac-multicast','checksum.rs','mac[0] & 1 == 0 && ','','mac_unicast_rules'),
 ('mac-zero','checksum.rs','mac[0] & 1 == 0 && mac[..6] != [0u8; 6]','mac[0] & 1 == 0','mac_unicast_rules'),
 ('out-capacity','checksum.rs','&& capacity >= needed','&& capacity + 1 >= needed','output_admission'),
 ('out-overlap','checksum.rs','        && !(out < length.wrapping_add(LEN) && length < out.wrapping_add(needed))','','output_admission'),
 ('out-wrap','checksum.rs','        && out.wrapping_add(needed) > out\n','','output_admission'),
 ('eth-dest','ipv4.rs','if p[..6] != a.mac && p[..6] != BROADCAST {','if p[..6] != a.mac && p[..6] == BROADCAST {','ethernet_destinations'),
 ('eth-src-self','ipv4.rs','if !mac_unicast(&p[6..12]) || p[6..12] == a.mac {','if !mac_unicast(&p[6..12]) {','ethernet_sources'),
 ('eth-8023','ipv4.rs','if kind <= 1500 {','if kind < 1500 {','ethernet_type_classes'),
 ('eth-gap','ipv4.rs','if kind < 0x0600 {','if kind < 0x05ff {','ethernet_type_classes'),
 ('pl-version','ipv4.rs','if ip[0] >> 4 != 4 ||','if ip[0] >> 4 == 0 ||','payload_version_and_ihl'),
 ('pl-ihl-min','ipv4.rs','header < 20 || header > available','header < 16 || header > available','payload_version_and_ihl'),
 ('pl-total-min','ipv4.rs','if total < header ||','if total + 1 < header ||','payload_total_length'),
 ('pl-total-max','ipv4.rs','|| total > available ||','|| total > available + 1 ||','payload_total_length'),
 ('pl-ttl','ipv4.rs','|| ip[8] == 0 ||','|| ip[8] == 255 ||','payload_ttl_zero'),
 ('pl-checksum','ipv4.rs','checksum(head) != 0','checksum(head) == 1','payload_header_checksum'),
 ('pl-reserved','ipv4.rs','if fragments & 0x8000 != 0 {','if fragments & 0x8000 == 1 {','payload_fragment_rules'),
 ('pl-more-fragments','ipv4.rs','fragments & 0x3fff != 0','fragments & 0x1fff != 0','payload_fragment_rules'),
 ('pl-options','ipv4.rs','if header != 20 ||','if header > 24 ||','payload_version_and_ihl'),
 ('pl-protocol','ipv4.rs','|| ip[9] != protocol {','|| ip[9] != protocol && protocol == 0 {','payload_protocol_and_addresses'),
 ('pl-destination','ipv4.rs','if ip[16..20] != c.arp.ip {','if ip[16..20] == [0u8; 4] {','payload_protocol_and_addresses'),
 ('pl-source-self','ipv4.rs','|| ip[12..16] == c.arp.ip {','{','payload_protocol_and_addresses'),
 ('pl-broadcast','ipv4.rs','    if p[..6] != c.arp.mac {\n        return Err(DROP); // No broadcast echo replies.','    if p[..6] == [0u8; 6] {\n        return Err(DROP); // No broadcast echo replies.','payload_protocol_and_addresses'),
 ('begin-ttl','ipv4.rs','    h[8] = 64;','    h[8] = 63;','begin_layout'),
 ('begin-df','ipv4.rs','put16(&mut h[6..8], 0x4000);','put16(&mut h[6..8], 0);','begin_layout'),
 ('begin-padding','ipv4.rs','    if wire < 60 {\n        wire = 60;\n    }\n    // Every caller','    if wire < 34 {\n        wire = 60;\n    }\n    // Every caller','begin_minimum_frame'),
 ('begin-identification','ipv4.rs','put16(&mut h[4..6], c.identification);','put16(&mut h[4..6], c.identification.wrapping_add(1));','begin_layout'),
 ('arp-ttl','arp.rs','now.wrapping_sub(n.seen) >= ARP_TTL','now.wrapping_sub(n.seen) > ARP_TTL','neighbor_expiry_boundary'),
 ('query-ttl','arp.rs','now.wrapping_sub(a.pending_since) >= ARP_QUERY_TTL {\n        a.pending = 0;','now.wrapping_sub(a.pending_since) > ARP_QUERY_TTL {\n        a.pending = 0;','query_cached_busy_request'),
 ('learn-oldest','arp.rs','if age >= oldest {','if age > oldest {','learn_slot_selection'),
 ('learn-existing','arp.rs','        if n.valid != 0 && n.ip == *ip {\n            slot = i;\n            found = true;','        if n.valid != 0 && n.ip == [0u8; 4] {\n            slot = i;\n            found = true;','learn_slot_selection'),
 ('learn-free-slot','arp.rs','            if n.valid == 0 {\n                slot = i;\n                break;\n            }\n','','learn_slot_selection'),
 ('learn-clears-pending','arp.rs','    if a.pending != 0 && a.pending_ip == *ip {\n        a.pending = 0;\n    }\n','','input_reply_rules'),
 ('query-busy','arp.rs','    if a.pending != 0 {\n        return BUSY;\n    }\n','','query_cached_busy_request'),
 ('query-cached','arp.rs','    if neighbor_get(a, ip, now).is_some() {\n        return CACHED;\n    }\n','','query_cached_busy_request'),
 ('probe-learn','arp.rs','    if !probe {\n        learn(a, &spa, &sha, now);\n    }','    learn(a, &spa, &sha, now);','input_probe_replies_without_learning'),
 ('reply-solicited','arp.rs','|| spa != a.pending_ip {','{','input_reply_rules'),
 ('reply-destination','arp.rs','if p[..6] != a.mac || tha != a.mac ||','if tha != a.mac ||','input_reply_rules'),
 ('sender-hardware','arp.rs','if sha[..] != p[6..12] ||','if false ||','input_field_validation'),
 ('arp-operation','arp.rs','(op != 1 && op != 2)','(op != 1 && op != 3)','input_field_validation'),
 ('arp-hardware-type','arp.rs','if be16(&p[14..16]) != 1 ||','if be16(&p[14..16]) == 0 ||','input_field_validation'),
 ('arp-short','arp.rs','if p.len() < 42 {','if p.len() < 41 {','input_field_validation'),
 ('echo-busy','arp.rs','if c.pending != 0 && now.wrapping_sub(c.since) < PING_TTL {','if c.pending != 0 && now.wrapping_sub(c.since) <= PING_TTL {','echo_request_busy_and_neighbor'),
 ('echo-capture','arp.rs','    if let (Some(dst), Some(src)) = (c.data.get_mut(..n), out.get(42..42 + n)) {\n        dst.copy_from_slice(src);\n    }\n','','echo_request_layout_and_state'),
 ('echo-identification','arp.rs','    c.identification = c.identification.wrapping_add(1);\n    ECHO_REQUEST','    ECHO_REQUEST','echo_request_layout_and_state'),
 ('match-sequence','arp.rs','|| be16(&icmp[6..8]) != c.sequence ','','echo_reply_match_rules'),
 ('match-data','arp.rs','\n            || (data_length != 0 && c.data.get(..data_length) != icmp.get(8..))','','echo_reply_match_rules'),
 ('match-ttl','arp.rs','if c.pending == 0 || now.wrapping_sub(c.since) >= PING_TTL ||','if c.pending == 0 || now.wrapping_sub(c.since) > PING_TTL ||','echo_reply_match_rules'),
 ('icmp-code','arp.rs','if icmp_length < 8 || icmp[1] != 0 ||','if icmp_length < 8 ||','icmp_validation'),
 ('icmp-type','arp.rs','if icmp[0] != 0 && icmp[0] != 8 {','if icmp[0] > 8 {','icmp_validation'),
 ('echo-reply-type','arp.rs','*produced = build(&mut out[..], c, &mac, &peer, 0,','*produced = build(&mut out[..], c, &mac, &peer, 8,','echo_request_is_answered'),
 ('udp-zero-checksum','udp.rs','if sum != 0 { sum } else { 65535 }','sum','udp_send_zero_checksum_is_ffff'),
 ('udp-source-port','udp.rs','put16(&mut udp[0..2], c.port);','put16(&mut udp[0..2], port);','udp_send_layout'),
 ('udp-length','udp.rs','put16(&mut udp[4..6], (8 + n) as u16);','put16(&mut udp[4..6], n as u16);','udp_send_layout'),
 ('udp-identification','udp.rs','    c.ip.identification = c.ip.identification.wrapping_add(1);\n    UDP_SENT','    UDP_SENT','udp_send_layout'),
 ('udp-peer-self','udp.rs','|| *peer == c.ip.arp.ip','','udp_send_rejections'),
 ('udp-input-max','udp.rs','if size - 8 > UDP_DATA_MAX {','if size - 8 > UDP_DATA_MAX + 1 {','udp_input_validation'),
 ('udp-queue-full','udp.rs','if c.count == UDP_QUEUE {','if c.count == UDP_QUEUE + 1 {','udp_input_queues_fifo'),
 ('udp-slot','udp.rs','let slot = (c.head.wrapping_add(c.count) % UDP_QUEUE) as usize;','let slot = (c.count % UDP_QUEUE) as usize;','udp_head_wraps'),
 ('udp-checksum-present','udp.rs','if present && transport_checksum(','if transport_checksum(','udp_input_validation'),
 ('udp-size-mismatch','udp.rs','if size < 8 || size != view.length ||','if size < 8 || size > view.length ||','udp_input_validation'),
 ('udp-port-drop','udp.rs','    if be16(&udp[2..4]) != c.port {\n        return UdpIn::Code(DROP);\n    }\n','','udp_input_validation'),
 ('udp-zero-destination','udp.rs','|| be16(&udp[2..4]) == 0 {','{','udp_input_validation'),
 ('udp-delegate','udp.rs','Err(UNSUPPORTED) => return UdpIn::Ipv4Input,','Err(UNSUPPORTED) => return UdpIn::Code(UNSUPPORTED),','udp_input_delegates_other_protocols'),
 ('receive-clear','udp.rs','    c.queue[slot] = Datagram::ZERO;\n','    let _ = slot;\n','udp_input_queues_fifo'),
 ('receive-advance','udp.rs','c.head = c.head.wrapping_add(1) % UDP_QUEUE;','c.head = c.head.wrapping_add(1);','udp_head_wraps'),
 ('receive-empty','udp.rs','    if c.count == 0 {\n        return Err(UDP_EMPTY);\n    }\n','','udp_input_queues_fifo'),
 ('close-zeroes','udp.rs','    *c = Udp::ZERO;\n}','    c.count = 0;\n}','udp_closed_endpoint'),
 ('udp-init-port','udp.rs','    if port == 0 {\n        return EINVAL;\n    }\n    let mut ipctx','    if port == 1 {\n        return EINVAL;\n    }\n    let mut ipctx','udp_init_rules'),
]
def frame(asm,name):
 m=re.search(r'^[0-9a-f]+ <'+re.escape(name)+r'>:\n(.*?)(?=\n\n|\Z)',asm,re.M|re.S);need(m,'function '+name)
 return 8*len(re.findall(r'\tpush\s',m[1]))+sum(int(x,16) for x in re.findall(r'\tsub\s+\$0x([0-9a-f]+),%rsp',m[1]))
def main():
 ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--archive',type=Path,required=True);ap.add_argument('--rustc',required=True);ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 out=a.out.resolve();archive=a.archive.resolve();need(not out.exists() and out.is_relative_to(ROOT/'build') and archive.is_file(),'fresh confined output');out.mkdir(parents=True)
 report={'status':'RUNNING','archive_sha256':sha(archive),'commands':[],'seed':'52334e3172757374'}
 def run(cmd,name,expect=0,timeout=1500):
  cmd=list(map(str,cmd));row={'argv':cmd,'log':name};report['commands'].append(row)
  with (out/name).open('x') as f:p=subprocess.run(cmd,cwd=ROOT,stdout=f,stderr=subprocess.STDOUT,timeout=timeout)
  row['exit']=p.returncode;need(p.returncode==expect,'unexpected exit '+name);return (out/name).read_text()
 try:
  ref=json.loads((ROOT/'tests/reference/net-r3n1.json').read_text())
  for n,v in ref['sources_sha256'].items():need(sha(ROOT/'tests/reference'/n)==v,'frozen accepted source '+n)
  for n,v in ref['headers_sha256'].items():need(sha(ROOT/'tests/reference/net-headers'/n)==v,'frozen header '+n)
  cc=['gcc','-std=gnu11','-Wall','-Wextra','-Werror','-Iarch/x86_64','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-fno-pie','-no-pie']
  defs=['-D'+n+'='+n+'_reference' for n in PUBLIC]
  refcc=[*cc[:5],'-Itests/reference/net-headers',*cc[5:]]
  kernel=['-ffreestanding','-fno-builtin','-fno-stack-protector','-fno-pic','-mno-red-zone','-mgeneral-regs-only','-mcmodel=kernel']
  objs={}
  for mode,flags in [('sanitizer',['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']),('release',['-O2','-fno-lto'])]:
   scalar=flags+(kernel if mode=='release' else [])
   objs[mode]=[]
   for f in ('arp','ipv4','udp'):
    run(refcc+scalar+defs+['-c','tests/reference/net-%s.c'%f,'-o',out/('%s-reference-%s.o'%(mode,f))],'%s-reference-%s-build.txt'%(mode,f))
    run(cc+scalar+['-c','arch/x86_64/net_%s.c'%f,'-o',out/('%s-driver-%s.o'%(mode,f))],'%s-driver-%s-build.txt'%(mode,f))
    objs[mode]+=[out/('%s-reference-%s.o'%(mode,f)),out/('%s-driver-%s.o'%(mode,f))]
   run(cc+flags+['tests/host/net-differential.c',*objs[mode],archive,'-o',out/(mode+'-differential')],mode+'-build.txt')
   report[mode]=run([out/(mode+'-differential')],mode+'.txt');need(COVERAGE in report[mode],'exact differential coverage '+mode)
  report['cpu']=min(os.sched_getaffinity(0));raw=run(['taskset','-c',str(report['cpu']),out/'release-differential','--bench'],'performance.csv')
  records=list(csv.DictReader(io.StringIO(raw)));labels=['checksum_payload','udp_input_queue','icmp_echo_answer','arp_request_reply','udp_send','udp_input_reject'];report['performance']={}
  for n,label in enumerate(labels):
   rows=[x for x in records if int(x['workload'])==n];c=statistics.median(float(x['C_ns']) for x in rows);r=statistics.median(float(x['Rust_ns']) for x in rows)
   report['performance'][label]={'C_ns':c,'Rust_ns':r,'percent':100*(r/c-1)}
  report['regressions_over_5_percent']=sorted(k for k,v in report['performance'].items() if v['percent']>5)
  sources={m:(ROOT/'kernel/rust/net'/(m+'.rs')).read_text() for m in MODULES}
  units=(ROOT/'tests/rust/net.rs').read_text().replace('extern crate net_core;\n','').replace('net_core::','self::net::')
  for name,f,old,new,target in MUTATIONS:need(sources[f[:-3]].count(old)==1,'unique named mutation '+name);need('fn '+target+'(' in units,'mutation target exists '+target)
  need(len(MUTATIONS)>=40,'at least 40 named mutation controls')
  harness=out/'shared-tests.rs';chunks=[];rows=[]
  for name,f,old,new,target in [('baseline',None,None,None,None),*MUTATIONS]:
   ident=name.replace('-','_');d=out/ident;d.mkdir();mods=''
   for m in MODULES:
    text=sources[m] if f!=m+'.rs' else sources[m].replace(old,new);(d/(m+'.rs')).write_text(text);mods+='#[path='+json.dumps(str(d/(m+'.rs')))+'] pub mod '+m+';\n'
   chunks.append('pub mod '+ident+' {\npub mod net {\n'+mods+'}\n'+units+'\n}\n');rows.append({'module':ident,'target':target,'file':f,'policy_sha256':sha(d/(f or 'checksum.rs'))})
  harness.write_text(''.join(chunks));common=[a.rustc,'--edition=2024','-Dwarnings','-Copt-level=2','-Cdebuginfo=0','-Coverflow-checks=yes','-Ccodegen-units=1']
  run(common+['--test','-Cstrip=symbols',harness,'-o',out/'shared-tests'],'shared-build.txt',timeout=2400);need((out/'shared-tests').stat().st_size<=8000000,'shared test-runtime budget 8 MB')
  report['core_tests']=run([out/'shared-tests','--test-threads=1','baseline::'],'baseline-tests.txt');need('51 passed' in report['core_tests'],'51 positive core tests');report['mutations']={}
  for row in rows[1:]:
   target=row['module']+'::'+row['target'];t=run([out/'shared-tests','--test-threads=1','--exact',target],row['module']+'.txt',101)
   need(target+' ... FAILED' in t,'intended runtime assertion '+target);report['mutations'][row['module']]={'assertion':target,'file':row['file'],'detected':True,'policy_sha256':row['policy_sha256']}
  report['shared_harness']={'binary_bytes':(out/'shared-tests').stat().st_size,'binary_sha256':sha(out/'shared-tests'),'variants':len(rows),
   'method':'Distinct namespace per variant, identical positive fixture; intended assertions run individually, isolated process per mutant.'}
  root=out/'checked-root.rs';root.write_text('#![no_std]\n#[path='+json.dumps(str(ROOT/'arch/x86_64/rust/net.rs'))+'] mod net;\nunsafe extern "C" {fn abort()->!;}\n#[panic_handler] fn checked_panic(_: &core::panic::PanicInfo)->! {unsafe{abort()}}\n')
  checked=out/'libchecked.a';run(common+['--crate-type=staticlib','-Cpanic=abort',root,'-o',checked],'checked-rust-build.txt')
  run(cc+['-O1','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','tests/host/net-differential.c',*objs['sanitizer'],checked,'-o',out/'checked-differential'],'checked-build.txt')
  report['checked_Rust']=run([out/'checked-differential'],'checked.txt');need(COVERAGE in report['checked_Rust'],'checked complete coverage')
  asm=run(['objdump','-dr','--no-show-raw-insn',archive],'production-disassembly.txt')
  cref='\n'.join(run(['objdump','-d','--no-show-raw-insn',out/('release-reference-%s.o'%f)],'reference-%s-disassembly.txt'%f) for f in ('arp','ipv4','udp'))
  frames={n:{'C_reference':frame(cref,n+'_reference'),'Rust':frame(asm,ENTRY[n])} for n in PUBLIC}
  report['frames']=frames;over={n:v for n,v in frames.items() if v['Rust']>v['C_reference']}
  report['frame_exceptions_pending_owner_approval']={n:v for n,v in over.items()}
  need(all(n in FRAME_EXCEPTIONS and v['Rust']<=FRAME_EXCEPTIONS[n] for n,v in over.items()),'Rust frames not larger than accepted C except the disclosed list: %s'%over)
  funcs=dict(re.findall(r'^[0-9a-f]+ <([^>]+)>:\n(.*?)(?=\n\n|\Z)',asm,re.M|re.S))
  ours={n:b for n,b in funcs.items() if n.startswith('vos_rust_net_') or '10vos_kernel3net' in n}
  panics=sorted(n for n,b in ours.items() if re.search(r'R_X86_64_\w+\s+\S*(?:panic|unwrap_failed|expect_failed|bounds_check|slice_index|len_mismatch)',b))
  report['panic_free']={'functions':len(ours),'with_panic_paths':panics};need(len(ours)>=17 and not panics,'R3-N1 Rust network code has no panic path')
  symbols=run(['nm','--defined-only',archive],'archive-owners.txt')
  for n in PUBLIC:need(sum(x.endswith(' T '+ENTRY[n]) for x in symbols.splitlines())==1,'one Rust owner '+ENTRY[n])
  csyms=''.join(run(['nm',out/('release-driver-%s.o'%f)],'C-symbols-%s.txt'%f) for f in ('arp','ipv4','udp'))
  for n in PUBLIC:need(' T '+n in csyms and ' U '+ENTRY[n] in csyms,'C keeps the public ABI and forwards '+n)
  driver_src=''.join((ROOT/'arch/x86_64'/('net_%s.c'%f)).read_text() for f in ('arp','ipv4','udp'))
  need(not re.search(r'\bmem(cpy|cmp|set)\b|\bfor\s*\(|\bwhile\s*\(',driver_src),'no C protocol logic remains in net_arp.c/net_ipv4.c/net_udp.c')
  report['source_sha256']={n:sha(ROOT/n) for n in ['kernel/rust/net/checksum.rs','kernel/rust/net/context.rs','kernel/rust/net/ipv4.rs','kernel/rust/net/arp.rs','kernel/rust/net/udp.rs',
   'arch/x86_64/rust/net.rs','arch/x86_64/rust/kernel_probe.rs','arch/x86_64/net_arp.c','arch/x86_64/net_ipv4.c','arch/x86_64/net_udp.c','arch/x86_64/rust_kernel.h',
   'tests/host/net-differential.c','tests/rust/net.rs','tests/reference/net-arp.c','tests/reference/net-ipv4.c','tests/reference/net-udp.c','tests/reference/net-r3n1.json','scripts/test-net-rust.py']}
  report['status']='PASS'
 except Exception as e:report.update(status='FAIL',error=str(e));raise
 finally:
  with (out/'result.json').open('x') as f:json.dump(report,f,indent=2)
 print(json.dumps({k:v for k,v in report.items() if k not in ('commands','core_tests')},indent=2)[:6000])
if __name__=='__main__':main()

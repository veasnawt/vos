"""Gate B: cumulative 256 MiB admission, native session continuity, no cleanup."""
import os,sys,json,time,re,subprocess,hashlib,math
from pathlib import Path
sys.path.insert(0,'/home/veasnawt/src/myos/scripts')
from storage_stress_host import Supervisor,allocated,host_free,require,MIB,GIB
LIMIT=256*MIB
# Versioned allowances. 384 MiB (storage extension v1, user-approved 2026-10-22) is cumulative: it requires
# state.json 'carried', exactly the other Gate B evidence roots and E: archives, whose live allocation counts.
LIMITS={LIMIT:'N3.6 Gate B original allowance',384*MIB:'N3.6 Gate B storage extension v1 (cumulative)',
        448*MIB:'N3.6 Gate B storage extension v2 (cumulative)'}
# An extended allowance needs storage-authorization.json in the root: this exact version, its limit, an earlier
# allowance as previous_limit_bytes, and the record's sha256 in state.json (storage extension v2, 2026-10-22).
VERSIONS={384*MIB:'N3.6 Gate B storage extension v1',448*MIB:'N3.6 Gate B storage extension v2'}
EVIDENCE=Path('/home/veasnawt/vos-evidence');GATEB='n36-stage3-gateb-*'
# Gate C: its own cumulative family; Gate B roots never count towards it and Gate C roots never towards Gate B.
# Every Gate C allowance needs its versioned authorization record. v2 (user-approved 2026-10-22) extends v1: its
# previous_limit_bytes must be v1's limit, and the carry must hold every other Gate C root and E: archive.
GATEC='n36-stage3-gatec-*';GATEC_VERSIONS={256*MIB:'N3.6 Gate C allowance v1',400*MIB:'N3.6 Gate C allowance v2'}
GATEC_PREVIOUS={256*MIB:0,400*MIB:256*MIB}
# Gate D (user-approved 2026-10-22): an independent family with its own versioned cumulative allowance. It carries
# only earlier Gate D roots and E: archives; no other gate's evidence counts towards it, nor it towards them.
GATED='n36-stage3-gated-*';GATED_VERSIONS={320*MIB:'N3.6 Gate D allowance v1'};GATED_PREVIOUS={320*MIB:0}
# N3 final campaign (user-approved 2026-10-23): an independent family with its own versioned cumulative allowance. It
# carries only earlier final-campaign roots and E: archives; no gate's evidence counts towards it, nor it towards them.
FINAL='n36-final-*';FINAL_VERSIONS={2560*MIB:'N3 final campaign allowance v1',3339*MIB:'N3 final campaign allowance v2',3360*MIB:'N3 final campaign allowance v3'};FINAL_PREVIOUS={2560*MIB:0,3339*MIB:2560*MIB,3360*MIB:3339*MIB}
FAMILIES={GATEC:(GATEC_VERSIONS,GATEC_PREVIOUS),GATED:(GATED_VERSIONS,GATED_PREVIOUS),
          FINAL:(FINAL_VERSIONS,FINAL_PREVIOUS)}   # Gate B: LIMITS/VERSIONS
PROC=Path('/proc');REPO=Path('/home/veasnawt/src/myos')
# Session handoff, append-only beside state.json (which never changes): the newest record names the
# controlling process. Boot, original deadline, journal start, storage and execution window stay Gate B-wide.
ADMISSIONS='session-admissions.jsonl'
FIELDS=('seq','utc','agent','boot_id','session_pid','session_start_ticks','session_exe','started_monotonic',
        'started_real','supersedes','reason','state_sha256','prev_sha256')
AGENTS={'claude-code':'/home/veasnawt/.claude/remote/ccd-cli/'}
LAYERS=('/usr/bin/bash','/usr/bin/dash','/usr/lib/cargo/bin/coreutils/timeout')
MAX_LAYERS=2
def identity(pid):
 """Live process (ppid, start ticks, executable), or None."""
 try:s=(PROC/str(pid)/'stat').read_text()
 except FileNotFoundError:return None
 f=s[s.rindex(')')+2:].split()
 try:exe=os.readlink(PROC/str(pid)/'exe')
 except (FileNotFoundError,PermissionError):exe=''
 return dict(ppid=int(f[1]),ticks=f[19],exe=exe)
def controller(pid):
 """First ancestor from pid that is not an allowlisted shell layer, through at most MAX_LAYERS layers."""
 for _ in range(MAX_LAYERS+1):
  me=identity(pid);require(me is not None,'STOP ancestry unreadable')
  if me['exe'] not in LAYERS:return pid,me
  pid=me['ppid']
 raise RuntimeError('STOP ancestry beyond verified shell layers')
class Guard(Supervisor):
 def __init__(self,root):
  self.root=Path(root).resolve(strict=True);self.state=json.loads((self.root/'state.json').read_text())
  self.family=next((f for f in FAMILIES if self.root.name.startswith(f[:-1])),GATEB)
  self.gatec=self.family==GATEC
  versions,previous=FAMILIES.get(self.family,(None,None))
  require(self.state['limit_bytes'] in (versions or LIMITS),'wrong Gate B allowance')
  self.limit=self.state['limit_bytes']
  require((self.limit==LIMIT and not versions) or type(self.state.get('carried')) is list,'extended allowance without cumulative carry')
  if versions or self.limit!=LIMIT:
   a=self.root/'storage-authorization.json'
   require(a.is_file() and hashlib.sha256(a.read_bytes()).hexdigest()==self.state.get('storage_authorization_sha256'),
           'extended allowance without its authorization record')
   auth=json.loads(a.read_text())
   require(type(auth) is dict,'invalid storage authorization version')
   prev=auth.get('previous_limit_bytes')
   if versions:
    require(auth.get('version')==versions[self.limit] and auth.get('limit_bytes')==self.limit and
            type(prev) is int and prev==previous[self.limit],'invalid storage authorization version')
   else:
    require(auth.get('version')==VERSIONS[self.limit] and auth.get('limit_bytes')==self.limit and
            type(prev) is int and prev in LIMITS and prev<self.limit,'invalid storage authorization version')
  self.archive=Path(self.state['evidence_copy'])
 def admissions(self):
  """Every admission record, validated as a whole; any defect fails closed."""
  p=self.root/ADMISSIONS
  if not p.exists():return []
  s=self.state;data=p.read_bytes();state=hashlib.sha256((self.root/'state.json').read_bytes()).hexdigest()
  require(data.endswith(b'\n'),'STOP incomplete admission record')
  recs=[];offset=0;prev=dict(seq=0,session_pid=s['session_pid'],session_start_ticks=s['session_start_ticks'],
                              started_monotonic=s['started_monotonic'],started_real=s['started_real'])
  for line in data.split(b'\n')[:-1]:
   try:r=json.loads(line)
   except ValueError:r=None
   require(type(r) is dict and set(r)==set(FIELDS),'STOP malformed admission record')
   require(type(r['seq']) is int and r['seq']==prev['seq']+1,'STOP out-of-order admission sequence')
   require(r['boot_id']==s['boot_id'] and r['state_sha256']==state,'STOP admission boot/state mismatch')
   require(r['prev_sha256']==hashlib.sha256(data[:offset]).hexdigest(),'STOP prior admission records changed')
   require(r['supersedes']=='%d:%d:%s'%(prev['seq'],prev['session_pid'],prev['session_start_ticks']),
           'STOP admission does not supersede its predecessor')
   require(type(r['agent']) is str and r['agent'] in AGENTS and type(r['session_exe']) is str and r['session_exe'].startswith(AGENTS[r['agent']]),
           'STOP admitted controller is not the named agent')
   require(type(r['session_pid']) is int and r['session_pid']>1 and type(r['session_start_ticks']) is str and
           r['session_start_ticks'].isdigit() and type(r['reason']) is str and r['reason'].strip() and
           type(r['utc']) is str,'STOP malformed admission identity')
   require(all(type(r[k]) in (int,float) and math.isfinite(r[k]) and r[k]>prev[k] for k in ('started_monotonic','started_real')),
           'STOP out-of-order admission time')
   require(all((x['session_pid'],x['session_start_ticks'])!=(r['session_pid'],r['session_start_ticks'])
               for x in recs+[prev]),'STOP duplicate admitted session')
   recs.append(r);prev=r;offset+=len(line)+1
  require(recs,'STOP empty admission record')
  return recs
 def carried(self):
  """Live allocation of every other Gate B root and E: archive; the set must equal state.json 'carried'."""
  if 'carried' not in self.state:return 0
  found={str(p) for p in EVIDENCE.glob(self.family) if p.resolve()!=self.root}
  found|={str(p) for p in self.archive.parent.glob(self.family+'.tar.gz') if p!=self.archive}
  require(found==set(self.state['carried']),'STOP Gate B evidence set differs from the admitted carry')
  return sum(allocated(Path(p)) for p in sorted(found))
 def check_session(self):
  s=self.state;recs=self.admissions()
  if not recs:
   require(os.getppid()==s['session_pid'] and (PROC/str(s['session_pid'])/'stat').read_text().split()[21]==s['session_start_ticks'],'STOP session lost')
   return s['session_pid'],0
  r=recs[-1];pid,me=controller(os.getppid())
  require(pid==r['session_pid'] and me['ticks']==r['session_start_ticks'] and me['exe']==r['session_exe'],'STOP session lost')
  return pid,r['seq']
 def guard(self,extra=0):
  s=self.state
  require((PROC/'sys/kernel/random/boot_id').read_text().strip()==s['boot_id'],'STOP boot ID changed')
  session=self.check_session()
  return self.bounds(extra,session)
 def bounds(self,extra,session):
  ROOT=self.root
  s=self.state
  require(time.monotonic()-s['started_monotonic']<10000,'STOP bounded execution session')
  j=subprocess.run(['journalctl','--boot='+s['boot_id'].replace('-',''),'--since=@'+str(s['started_real']),'--no-pager','-o','json'],capture_output=True,text=True,timeout=10)
  require(j.returncode==0 or(j.returncode==1 and not j.stdout and not j.stderr),'STOP journal unavailable')
  for line in j.stdout.splitlines():
   msg=str(json.loads(line).get('MESSAGE',''))
   require(not re.search(r'powering down|system will power off|shutdown.target|systemctl poweroff|EXT4-fs error|I/O error|kernel panic|BUG:|unmounting filesystem 49ca',msg,re.I),'STOP shutdown/filesystem warning '+msg)
  require(type(extra) is int and extra>=0,'invalid admission')
  baseline=json.loads((ROOT/'baseline-source.json').read_text())
  repo=REPO
  ns=set(filter(None,subprocess.check_output(['git','ls-files','--cached','--others','--exclude-standard','-z'],cwd=repo).decode().split('\0')))
  source=0
  for n in ns:
   p=repo/n;t=p.lstat();old=baseline.get(n)
   if old is None or t.st_mode!=old['mode'] or hashlib.sha256(p.read_bytes()).hexdigest()!=old['sha256']:source+=t.st_blocks*512
  carried=self.carried()
  used=allocated(ROOT)+allocated(self.archive)+source+carried
  charged=used+2*MIB
  require(charged+extra<=self.limit,'STOP Gate B cumulative allocation')
  require(allocated(ROOT/'.tmp')<=64*MIB,'STOP temporary allowance')
  remaining=self.limit-charged;host=host_free()
  require(host['C']>=10*GIB+remaining+256*MIB,'STOP C reserve')
  require(host['E']>=extra+256*MIB,'STOP E reserve')
  v=os.statvfs(ROOT);linux=v.f_bavail*v.f_frsize
  require(linux>=GIB+remaining,'STOP Linux reserve')
  execution=ROOT/'execution-window.json'
  if execution.exists():require(time.monotonic()-json.loads(execution.read_text())['started_monotonic']<5400,'STOP 90-minute execution window')
  row=dict(utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),allocated=used,charged=charged,remaining=remaining,extra=extra,C_free=host['C'],E_free=host['E'],linux_free=linux,boot_id=s['boot_id'],session_pid=session[0],admission=session[1],carried=carried,limit=self.limit)
  with (ROOT/'storage-samples.jsonl').open('a') as f:f.write(json.dumps(row)+'\n')
  return row
 def admit(self,agent,reason):
  """Append one admission for this command's own controlling process; no caller-supplied identity."""
  s=self.state;recs=self.admissions()
  require(type(agent) is str and agent in AGENTS and type(reason) is str and 0<len(reason.strip())<=1024,'invalid admission request')
  require((PROC/'sys/kernel/random/boot_id').read_text().strip()==s['boot_id'],'STOP boot ID changed')
  prior=[dict(seq=0,session_pid=s['session_pid'],session_start_ticks=s['session_start_ticks'])]+recs
  for r in prior:
   who=identity(r['session_pid'])
   require(who is None or who['ticks']!=r['session_start_ticks'],'STOP previous controlling session still alive')
  pid,me=controller(os.getppid())
  require(me['exe'].startswith(AGENTS[agent]) and me['ticks'].isdigit(),'STOP controlling process is not the named agent')
  path=self.root/ADMISSIONS;before=path.read_bytes() if path.exists() else b''
  self.bounds(4096,(pid,'pending-%d'%(len(recs)+1)))
  last=prior[-1]
  row=dict(seq=len(recs)+1,utc=time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),agent=agent,boot_id=s['boot_id'],
           session_pid=pid,session_start_ticks=me['ticks'],session_exe=me['exe'],started_monotonic=time.monotonic(),
           started_real=time.time(),supersedes='%d:%d:%s'%(last['seq'],last['session_pid'],last['session_start_ticks']),
           reason=reason.strip(),state_sha256=hashlib.sha256((self.root/'state.json').read_bytes()).hexdigest(),
           prev_sha256=hashlib.sha256(before).hexdigest())
  fd=os.open(path,os.O_WRONLY|os.O_APPEND|os.O_CREAT,0o644)
  try:
   require(os.fstat(fd).st_size==len(before),'STOP concurrent admission')
   os.write(fd,(json.dumps(row)+'\n').encode());os.fsync(fd)
  finally:os.close(fd)
  require(self.admissions()[-1]==row,'STOP admission not recorded exactly')
  self.guard()
  return row
if __name__=='__main__':
 import argparse
 a=argparse.ArgumentParser(description=__doc__);a.add_argument('evidence',type=Path)
 c=a.add_subparsers(dest='command',required=True)
 x=c.add_parser('admit');x.add_argument('--agent',required=True,choices=sorted(AGENTS));x.add_argument('--reason',required=True)
 c.add_parser('check')
 args=a.parse_args();g=Guard(args.evidence)
 print(json.dumps(g.admit(args.agent,args.reason) if args.command=='admit' else g.guard(),indent=1))

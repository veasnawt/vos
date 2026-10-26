"""Portable synthetic controller checks; no private worktrees or host-volume requirements."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

REPO=Path(__file__).resolve().parents[2]
PACKAGE=REPO/"tools/vos_control/v1"
spec=importlib.util.spec_from_file_location("_tested_bootstrap",PACKAGE/"bootstrap.py")
bootstrap=importlib.util.module_from_spec(spec);spec.loader.exec_module(bootstrap)
MANIFEST=(PACKAGE/"module-manifest.json").read_bytes()
PIN=hashlib.sha256(MANIFEST).hexdigest()
api=bootstrap.load_verified(PACKAGE.resolve(),MANIFEST,PIN)
contracts=sys.modules[api.__name__+".contracts"]
transport=sys.modules[api.__name__+".transport"]
authority_module=sys.modules[api.__name__+".authority"]
jsonbytes=lambda x:json.dumps(x,sort_keys=True).encode()
hashbytes=lambda x:hashlib.sha256(x).hexdigest()
NATIVE=sys.platform.startswith("linux") and hasattr(os,"WNOWAIT")
supplied=os.environ.get("VOS_CONTROL_TEST_ROOT")
ROOT=Path(supplied).resolve() if supplied else Path(tempfile.mkdtemp(prefix="vos-control-tests-")).resolve()
ROOT.mkdir(parents=True,exist_ok=True)
if list(ROOT.iterdir()):raise RuntimeError("test root must be admitted and empty")
(ROOT/"fixture-owner").write_text("owned synthetic fixtures only\n")
HIGH={}
def meter():
    for p in [ROOT,*ROOT.rglob("*")]:
        n=str(p.relative_to(ROOT))
        s=p.lstat();blocks=getattr(s,"st_blocks",None)
        size=blocks*512 if blocks is not None else max(4096,((s.st_size+4095)//4096)*4096)
        HIGH[n]=max(HIGH.get(n,0),size)
    # This ledger deliberately retains former temporary/removed path charges.
    HIGH["usage.next"]=max(HIGH.get("usage.next",0),4096)
    HIGH["usage.json"]=max(HIGH.get("usage.json",0),4096)
    while True:
        data=json.dumps(dict(highwater=HIGH,charged=sum(HIGH.values())),sort_keys=True).encode()
        size=max(4096,((len(data)+4095)//4096)*4096)
        if HIGH["usage.json"]>=size and HIGH["usage.next"]>=size:break
        HIGH["usage.json"]=max(HIGH["usage.json"],size);HIGH["usage.next"]=max(HIGH["usage.next"],size)
    if supplied and sum(HIGH.values())>512*1024:raise RuntimeError("synthetic fixture ceiling")
    with (ROOT/"usage.next").open("wb") as stream:
        stream.write(data);stream.flush();os.fsync(stream.fileno())
    os.replace(ROOT/"usage.next",ROOT/"usage.json")
def setUpModule():meter()
def tearDownModule():meter()

class Foundation(unittest.TestCase):
    def setUp(self):
        self.live=ROOT/"live";self.live.mkdir()
        self.root=self.live/"workspace";self.root.mkdir()
        (self.root/"src").mkdir();(self.root/"src/allowed.txt").write_text("source\n")
        (self.root/"history").mkdir();(self.root/"history/immutable.txt").write_text("history\n")
        (self.root/".git").mkdir();(self.root/".git/HEAD").write_text("protected\n")
        self.doc=dict(schema=1,task_id="portable-test",baseline="1"*40,allowed_sources=["src/allowed.txt"],
                      protected=["history"],scratch="scratch",evidence="evidence",
                      limits=dict(timeout_seconds=1,max_output_bytes=128,max_snapshot_bytes=1048576,
                                  max_files=128,poll_ms=10,cleanup_seconds=1))
        self.cfg=api.Config.parse(jsonbytes(self.doc))
        self.exe=str(Path(sys.executable).resolve());self.exehash=hashbytes(Path(self.exe).read_bytes())
        self.obs=api.Observation.parse(dict(executor="test-executor",baseline="1"*40,
                        aggregate_charge=10,reservation_obligation=50,task_charge=4,physical_free=dict(disk=100)))
    def tearDown(self):
        meter()
        # Only the fixture root created above is eligible; never a caller's workspace.
        self.assertEqual(self.live.parent,ROOT)
        self.assertTrue((ROOT/"fixture-owner").is_file())
        self.assertFalse(self.live.is_symlink())
        shutil.rmtree(self.live)
        meter()
    def grant(self,program="print('foundation')"):
        argv=[self.exe,"-I","-c",program]
        workspace,identity=authority_module.workspace_identity(self.root)
        self.receipt=dict(schema=1,task_id=self.cfg.task_id,baseline=self.cfg.baseline,
             config_sha256=self.cfg.sha256,manifest_sha256=PIN,executor="test-executor",
             workspace=workspace,workspace_id=identity,issued=time.time()-1,expires=time.time()+60,
             commands=dict(demo=dict(argv=argv,executable_sha256=self.exehash)),
             resources=dict(aggregate_ceiling=100,task_limit=20,minimum_margin=10,physical_required=dict(disk=20)))
        raw=jsonbytes(self.receipt)
        return api.authorize(raw,hashbytes(raw),self.cfg,PIN,self.root,"test-executor",time.time())
    def request(self,a,operation="check",sequence=1,nonce="a"*32):
        return dict(schema=1,task_id=self.cfg.task_id,authorization_sha256=a.receipt_sha256,
                    sequence=sequence,request_id=nonce,operation=operation,
                    payload={} if operation=="check" else dict(command="demo"))

    def test_manifest_origins_and_closed_namespace(self):
        self.assertEqual(api.VERSION,1)
        for name in bootstrap.MODULES:
            module=sys.modules[api.__name__+("." + name[:-3] if name!="__init__.py" else "")]
            self.assertEqual(Path(module.__spec__.origin),PACKAGE/name)
            self.assertEqual(module.__verified_sha256__,json.loads(MANIFEST)["modules"][name])
        with self.assertRaises(ModuleNotFoundError):__import__(api.__name__+".unverified")

    def test_trust_rejects_tampered_module_before_execution(self):
        clone=self.live/"package";shutil.copytree(PACKAGE,clone)
        marker=self.live/"executed"
        with (clone/"contracts.py").open("a") as out:out.write("\nfrom pathlib import Path\nPath("+repr(str(marker))+").touch()\n")
        with self.assertRaises(ImportError):bootstrap.load_verified(clone,MANIFEST,PIN)
        self.assertFalse(marker.exists())
        with self.assertRaises(ImportError):bootstrap.load_verified(PACKAGE,MANIFEST,"0"*64)

    def test_manifest_rejects_unknown_modules(self):
        clone=self.live/"package";shutil.copytree(PACKAGE,clone)
        (clone/"extra.py").write_text("raise RuntimeError('must not import')\n")
        with self.assertRaises(ImportError):bootstrap.load_verified(clone,MANIFEST,PIN)

    def test_verified_bytes_survive_disk_swap(self):
        clone=self.live/"package";shutil.copytree(PACKAGE,clone)
        with (clone/"contracts.py").open("a") as f:f.write("\n# distinct test image\n")
        manifest=jsonbytes(dict(schema=1,modules={n:hashbytes((clone/n).read_bytes()) for n in bootstrap.MODULES}))
        marker=self.live/"swapped-executed";original=bootstrap.importlib.import_module;swapped=[False]
        def swap(name,*args,**kwargs):
            if not swapped[0]:
                swapped[0]=True
                (clone/"contracts.py").write_text("from pathlib import Path\nPath("+repr(str(marker))+").touch()\n")
            return original(name,*args,**kwargs)
        with patch.object(bootstrap.importlib,"import_module",side_effect=swap):
            verified=bootstrap.load_verified(clone,manifest,hashbytes(manifest))
        self.assertEqual(verified.VERSION,1);self.assertFalse(marker.exists())

    def test_bootstrap_isolation_prevents_stdlib_shadowing(self):
        clone=self.live/"package";shutil.copytree(PACKAGE,clone)
        marker=self.live/"shadow-executed"
        (clone/"hashlib.py").write_text("from pathlib import Path\nPath("+repr(str(marker))+").touch()\n")
        for flags in (["-B"],["-I","-B"]):
            p=subprocess.run([self.exe,*flags,str(clone/"bootstrap.py"),"--manifest",str(clone/"module-manifest.json"),"--manifest-pin",PIN],
                             capture_output=True,text=True,timeout=10)
            self.assertNotEqual(p.returncode,0);self.assertFalse(marker.exists())

    def test_strict_data_only_configuration(self):
        for key,value in (("schema",True),("allowed_sources",["../escape"]),
                          ("scratch","history/nested"),("evidence","scratch/nested"),
                          ("allowed_sources",[".git/HEAD"]),("allowed_sources",["src//x"]),
                          ("allowed_sources",["evidence/output"])):
            doc=dict(self.doc);doc[key]=value
            with self.subTest(key=key,value=value),self.assertRaises(api.ControlError):
                api.Config.parse(jsonbytes(doc))
        doc=dict(self.doc);doc["python"]="import os"
        with self.assertRaises(api.ControlError):api.Config.parse(jsonbytes(doc))
        with self.assertRaises(api.ControlError):api.Config.parse(b'{"schema":1,"schema":1}')
        with self.assertRaises(api.ControlError):api.Config.parse(b'{"schema":NaN}')
        with self.assertRaises(api.ControlError):api.Config.parse(b'{')

    def test_separate_authorization_binding(self):
        self.grant()
        for key,value in (("task_id","other"),("baseline","2"*40),("config_sha256","0"*64),
                          ("manifest_sha256","0"*64),("executor","foreign"),
                          ("workspace_id","other"),("expires",time.time()-2)):
            x=dict(self.receipt);x[key]=value;raw=jsonbytes(x)
            with self.subTest(key=key),self.assertRaises(api.ControlError):
                api.authorize(raw,hashbytes(raw),self.cfg,PIN,self.root,"test-executor",time.time())
        raw=jsonbytes(self.receipt)
        with self.assertRaises(api.ControlError):
            api.authorize(raw,"0"*64,self.cfg,PIN,self.root,"test-executor",time.time())

    def test_accounting_contract_and_reserve_rejections(self):
        a=self.grant();a.check(self.root,self.obs,time.time())
        base=dict(executor="test-executor",baseline="1"*40,aggregate_charge=10,
                  reservation_obligation=50,task_charge=4,physical_free=dict(disk=100))
        for key,value in (("aggregate_charge",101),("task_charge",21),("reservation_obligation",91),
                          ("executor","foreign"),("baseline","2"*40),("physical_free",dict(disk=0))):
            x=dict(base);x[key]=value
            with self.subTest(key=key),self.assertRaises(api.ControlError):
                a.check(self.root,api.Observation.parse(x),time.time())
        base["task_charge"]=True
        with self.assertRaises(api.ControlError):api.Observation.parse(base)

    def test_source_scope_protection_and_aliases(self):
        scope=api.Scope(self.root,self.cfg);before=scope.capture()
        (self.root/"src/allowed.txt").write_text("approved\n")
        scope.check(before,scope.capture())
        (self.root/"history/immutable.txt").write_text("changed\n")
        with self.assertRaises(api.ControlError):scope.check(before,scope.capture())
        if os.name!="posix":return
        (self.root/"src/allowed.txt").unlink()
        os.symlink(self.root/"history/immutable.txt",self.root/"src/allowed.txt")
        with self.assertRaises(api.ControlError):api.Scope(self.root,self.cfg)

    @unittest.skipUnless(os.name=="posix","POSIX hardlink fixture")
    def test_hardlink_and_scratch_evidence_boundaries(self):
        (self.root/"src/allowed.txt").unlink()
        os.link(self.root/"history/immutable.txt",self.root/"src/allowed.txt")
        with self.assertRaises(api.ControlError):api.Scope(self.root,self.cfg)
        doc=dict(self.doc);doc["scratch"]="evidence"
        with self.assertRaises(api.ControlError):api.Config.parse(jsonbytes(doc))

    def test_malformed_unauthorized_request(self):
        a=self.grant();base=self.request(a)
        for key,value in (("operation","write"),("sequence",True),("sequence",0),
                          ("authorization_sha256","0"*64),("task_id","other"),("request_id","bad"),
                          ("payload",{"unexpected":1})):
            x=dict(base);x[key]=value
            with self.subTest(key=key),self.assertRaises(api.ControlError):transport.validate(x,a)
        x=self.request(a,"run");x["payload"]={"command":"ungranted"}
        with self.assertRaises(api.ControlError):transport.validate(x,a)

    @unittest.skipUnless(NATIVE,"Linux native atomic primitive")
    def test_atomic_publication_and_no_overwrite(self):
        a=self.grant();request=self.request(a);directory=self.live/"requests";directory.mkdir()
        original=transport.commit_no_replace
        def measured(*args):
            meter()
            try:return original(*args)
            finally:meter()
        with patch.object(transport,"commit_no_replace",side_effect=measured):
            p=api.publish(directory,request,a);before=p.read_bytes()
            self.assertEqual(api.read_request(p,a),request)
            with self.assertRaises(FileExistsError):api.publish(directory,request,a)
        self.assertEqual(p.read_bytes(),before)
        partial=directory/"incomplete.partial";partial.write_bytes(b'{"schema":')
        with self.assertRaises(api.ControlError):api.read_request(partial,a)
        invalid=directory/"invalid.json";invalid.write_bytes(b'{')
        with self.assertRaises(api.ControlError):api.read_request(invalid,a)

    @unittest.skipUnless(NATIVE,"Linux native atomic primitive")
    def test_post_publish_sync_failure_is_indeterminate(self):
        a=self.grant();directory=self.live/"requests";directory.mkdir();request=self.request(a)
        original=transport.commit_no_replace
        def measured(*args):
            meter()
            try:return original(*args)
            finally:meter()
        with patch.object(transport,"commit_no_replace",side_effect=measured),patch.object(transport,"sync_directory",side_effect=OSError("injected sync")):
            with self.assertRaises(transport.PublicationUncertain):api.publish(directory,request,a)
        self.assertTrue((directory/transport.filename(request)).exists())

    @unittest.skipUnless(NATIVE,"Linux durable claims")
    def test_durable_replay_and_sequence_rejection(self):
        a=self.grant();request=self.request(a);store=api.ClaimStore(self.root,a);store.claim(request)
        for x in (request,self.request(a,sequence=2),self.request(a,nonce="b"*32)):
            with self.assertRaises(api.ControlError):api.ClaimStore(self.root,a).claim(x)

    @unittest.skipUnless(NATIVE,"Linux durable claims")
    def test_partial_claim_burns_identity(self):
        a=self.grant();request=self.request(a);store=api.ClaimStore(self.root,a)
        with patch.object(transport,"sync_directory",side_effect=OSError("injected")):
            with self.assertRaises(api.ControlError):store.claim(request)
        with self.assertRaises(api.ControlError):store.claim(request)

    @unittest.skipUnless(NATIVE,"Linux native supervision")
    def test_authorized_execution_and_reclamation(self):
        a=self.grant("import os;print(os.getpid())");request=self.request(a,"run")
        result=api.execute(self.root,a,request,lambda:self.obs)
        self.assertEqual(result["returncode"],0)
        with self.assertRaises(ProcessLookupError):os.kill(int(result["stdout"]),0)
        with self.assertRaises(api.ControlError):api.execute(self.root,a,request,lambda:self.obs)
        self.assertEqual((self.root/"history/immutable.txt").read_text(),"history\n")

    @unittest.skipUnless(NATIVE,"Linux native supervision")
    def test_timeout(self):
        a=self.grant("import time;time.sleep(3)")
        with self.assertRaisesRegex(api.ControlError,"timeout"):
            api.execute(self.root,a,self.request(a,"run"),lambda:self.obs)

    @unittest.skipUnless(NATIVE,"Linux native supervision")
    def test_cancellation(self):
        a=self.grant("import time;time.sleep(3)")
        with self.assertRaisesRegex(api.ControlError,"cancelled"):
            api.execute(self.root,a,self.request(a,"run"),lambda:self.obs,cancel=lambda:True)

    @unittest.skipUnless(NATIVE,"Linux native supervision")
    def test_output_limit(self):
        a=self.grant("print('x'*10000)")
        with self.assertRaisesRegex(api.ControlError,"output limit"):
            api.execute(self.root,a,self.request(a,"run"),lambda:self.obs)

    @unittest.skipUnless(NATIVE,"Linux native supervision")
    def test_guard_failure_stops_workload(self):
        a=self.grant("import time;time.sleep(3)");calls=[0]
        def guard():
            calls[0]+=1
            if calls[0]>1:raise api.ControlError("guard unavailable")
            return self.obs
        with self.assertRaisesRegex(api.ControlError,"guard unavailable"):
            api.execute(self.root,a,self.request(a,"run"),guard)

    @unittest.skipUnless(NATIVE,"Linux native supervision")
    def test_executable_identity_rejection(self):
        a=self.grant();x=dict(self.receipt);x["commands"]=dict(demo=dict(argv=[self.exe,"-c","pass"],executable_sha256="0"*64))
        raw=jsonbytes(x);a=api.authorize(raw,hashbytes(raw),self.cfg,PIN,self.root,"test-executor",time.time())
        with self.assertRaisesRegex(api.ControlError,"executable identity"):
            api.execute(self.root,a,self.request(a,"run"),lambda:self.obs)

    @unittest.skipUnless(NATIVE,"Linux native supervision")
    def test_unscoped_command_write_is_reported(self):
        a=self.grant("from pathlib import Path;Path('unapproved').write_text('bad')")
        with self.assertRaisesRegex(api.ControlError,"unscoped"):
            api.execute(self.root,a,self.request(a,"run"),lambda:self.obs)

if __name__=="__main__":unittest.main()


#!/usr/bin/env python3
"""Trusted entry point: verify pinned complete module bytes BEFORE package imports."""
import sys
if __name__=="__main__" and not sys.flags.isolated:
    # Before importing anything else, use only the preloaded frozen OS module.
    _os=sys.modules.get("os")
    if _os is None or getattr(getattr(_os,"__spec__",None),"origin",None)!="frozen":
        raise SystemExit("launch trusted bootstrap with python3 -I -B")
    _os.execv(sys.executable,[sys.executable,"-I","-B",__file__,*sys.argv[1:]])
import argparse
import hashlib
import importlib
import importlib.abc
import importlib.util
import json
from pathlib import Path
import stat
import sys

MODULES=("__init__.py","authority.py","bootstrap.py","contracts.py","scope.py","supervision.py","transport.py")

def load_verified(root,manifest_bytes,manifest_pin):
    if type(manifest_bytes) is not bytes or len(manifest_bytes)>16384:raise ImportError("manifest bound")
    root=Path(root)
    if not root.is_absolute() or root!=root.resolve() or root.is_symlink():
        raise ImportError("canonical trusted package root required")
    if hashlib.sha256(manifest_bytes).hexdigest()!=manifest_pin:
        raise ImportError("manifest pin mismatch")
    def unique(pairs):
        x={}
        for k,v in pairs:
            if k in x:raise ImportError("duplicate manifest key")
            x[k]=v
        return x
    manifest=json.loads(manifest_bytes,object_pairs_hook=unique)
    if (type(manifest) is not dict or set(manifest)!={"schema","modules"} or
        type(manifest["schema"]) is not int or manifest["schema"]!=1 or
        type(manifest["modules"]) is not dict or set(manifest["modules"])!=set(MODULES)):
        raise ImportError("exact versioned module manifest required")
    if set(p.relative_to(root).as_posix() for p in root.rglob("*.py"))!=set(MODULES):
        raise ImportError("unmanifested Python module")
    image={}
    for name,expected in manifest["modules"].items():
        p=root/name;st=p.lstat()
        if not stat.S_ISREG(st.st_mode) or p.is_symlink() or st.st_size>131072:
            raise ImportError("regular bounded module required")
        data=p.read_bytes()
        if hashlib.sha256(data).hexdigest()!=expected:raise ImportError("module identity mismatch: "+name)
        image[name]=data
    namespace="_vos_control_v1_"+manifest_pin
    if any(n==namespace or n.startswith(namespace+".") for n in sys.modules):
        raise ImportError("namespace already loaded")
    class VerifiedLoader(importlib.abc.MetaPathFinder,importlib.abc.Loader):
        def find_spec(self,fullname,path=None,target=None):
            if fullname!=namespace and not fullname.startswith(namespace+"."):return None
            name="__init__.py" if fullname==namespace else fullname[len(namespace)+1:]+".py"
            if name not in image:raise ImportError("unverified submodule")
            return importlib.util.spec_from_loader(fullname,self,origin=str(root/name),is_package=fullname==namespace)
        def create_module(self,spec):return None
        def exec_module(self,module):
            name="__init__.py" if module.__name__==namespace else module.__name__[len(namespace)+1:]+".py"
            module.__file__=str(root/name)
            if name=="__init__.py":module.__path__=[]
            module.__verified_sha256__=hashlib.sha256(image[name]).hexdigest()
            exec(compile(image[name],str(root/name),"exec"),module.__dict__)
    finder=VerifiedLoader();sys.meta_path.insert(0,finder)
    try:
        package=importlib.import_module(namespace)
        for name in MODULES:
            if name!="__init__.py":importlib.import_module(namespace+"."+name[:-3])
        return package
    except BaseException:
        for n in list(sys.modules):
            if n==namespace or n.startswith(namespace+"."):del sys.modules[n]
        raise
    finally:sys.meta_path.remove(finder)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest",type=Path,required=True)
    parser.add_argument("--manifest-pin",required=True,help="digest from independently trusted review/authorization")
    args=parser.parse_args()
    root=Path(__file__).resolve().parent
    package=load_verified(root,args.manifest.read_bytes(),args.manifest_pin)
    print(json.dumps(dict(version=package.VERSION,verified_modules=len(MODULES),execution_performed=False)))
if __name__=="__main__":main()

"""Closed atomic requests and durable claim-before-execution replay rejection."""
import ctypes
import errno
import json
import os
from pathlib import Path
import stat
import sys
import uuid
from .contracts import *
from .scope import path

class PublicationUncertain(ControlError):
    """Destination may exist; stop and reconcile, never retry blindly."""

def sync_directory(directory):
    fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:os.fsync(fd)
    finally:os.close(fd)

def commit_no_replace(directory, temporary, destination):
    require(sys.platform.startswith("linux"),"native atomic publication unsupported on this platform")
    fd=os.open(directory,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:
        libc=ctypes.CDLL(None,use_errno=True)
        fn=libc.renameat2
        fn.argtypes=[ctypes.c_int,ctypes.c_char_p,ctypes.c_int,ctypes.c_char_p,ctypes.c_uint]
        fn.restype=ctypes.c_int
        if fn(fd,os.fsencode(temporary),fd,os.fsencode(destination),1):
            e=ctypes.get_errno()
            raise OSError(e,os.strerror(e))
    finally:os.close(fd)

def validate(request, authority):
    object_keys(request,("schema","task_id","authorization_sha256","sequence","request_id","operation","payload"))
    integer(request["schema"],1,1);integer(request["sequence"],1,2**31-1)
    hexhash(request["request_id"],32)
    require(request["task_id"]==authority.config.task_id and
            request["authorization_sha256"]==authority.receipt_sha256,"request authority")
    require(request["operation"] in ("check","run"),"unsupported operation")
    if request["operation"]=="check":object_keys(request["payload"],())
    else:
        object_keys(request["payload"],("command",))
        require(type(request["payload"]["command"]) is str and request["payload"]["command"] in authority.commands,"ungranted command")
    return request

def filename(request):
    return "%08d-%s.json"%(request["sequence"],request["request_id"])

def publish(directory,request,authority):
    validate(request,authority)
    require(sys.platform.startswith("linux"),"native atomic publication unsupported on this platform")
    d=Path(directory);require(d.is_dir() and not d.is_symlink(),"request directory")
    data=json.dumps(request,separators=(",",":")).encode()
    require(len(data)<=65536,"request byte bound")
    temporary="."+uuid.uuid4().hex+".partial";destination=filename(request)
    fd=os.open(d,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try:
        out=os.open(temporary,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600,dir_fd=fd)
        with os.fdopen(out,"wb") as f:f.write(data);f.flush();os.fsync(f.fileno())
        commit_no_replace(d,temporary,destination)
        try:sync_directory(d)
        except OSError as exc:raise PublicationUncertain("request published; directory sync failed") from exc
    finally:os.close(fd)
    return d/destination

def read_request(file,authority):
    p=Path(file)
    require(p.name.endswith(".json"),"only published JSON requests are actionable")
    fd=os.open(p,os.O_RDONLY|os.O_NOFOLLOW)
    with os.fdopen(fd,"rb") as f:
        s=os.fstat(f.fileno())
        require(stat.S_ISREG(s.st_mode) and s.st_nlink==1 and s.st_size<=65536,"regular bounded request")
        request=validate(decode(f.read(65537)),authority)
    require(p.name==filename(request),"request filename identity")
    return request

def exclusive(file,data):
    fd=os.open(file,os.O_WRONLY|os.O_CREAT|os.O_EXCL|os.O_NOFOLLOW,0o600)
    with os.fdopen(fd,"wb") as f:f.write(data);f.flush();os.fsync(f.fileno())

class ClaimStore:
    def __init__(self,root,authority):
        require(sys.platform.startswith("linux"),"native durable claims unsupported")
        self.authority=authority
        self.directory=path(root,authority.config.evidence)
        self.directory.mkdir(parents=True,exist_ok=True)
        owner=self.directory/"owner.json"
        data=json.dumps(dict(task=authority.config.task_id,authorization=authority.receipt_sha256),sort_keys=True).encode()
        if not owner.exists():exclusive(owner,data);sync_directory(self.directory)
        require(not owner.is_symlink() and owner.read_bytes()==data,"evidence ownership")
    def claim(self,request):
        validate(request,self.authority)
        # A partially persisted claim burns the identity. No rollback or automatic retry.
        data=json.dumps(request,sort_keys=True).encode()
        prefix=self.authority.receipt_sha256[:16]+"-"
        try:
            exclusive(self.directory/(prefix+"id-"+request["request_id"]+".claim"),data)
            exclusive(self.directory/(prefix+"seq-"+str(request["sequence"])+".claim"),data)
            sync_directory(self.directory)
        except OSError as exc:raise ControlError("replayed or indeterminate claim; stop and reconcile") from exc

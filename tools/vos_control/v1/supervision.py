"""Bounded Linux subprocess supervision, not an OS sandbox or an isolation provider."""
import json
import os
from pathlib import Path
import selectors
import signal
import stat
import subprocess
import sys
import time
from .contracts import require, ControlError, digest
from .authority import Authority
from .scope import Scope
from .transport import ClaimStore, validate, exclusive, sync_directory

def execute(root,authority,request,guard,cancel=lambda:False,wall=time.time,monotonic=time.monotonic):
    require(type(authority) is Authority,"separately verified authority required")
    require(sys.platform.startswith("linux") and hasattr(os,"WNOWAIT"),"native execution unsupported")
    require(callable(guard) and callable(cancel),"explicit guard/cancellation providers required")
    authority.check(root,guard(),wall())
    validate(request,authority)
    config=authority.config;scope=Scope(root,config);before=scope.capture()
    store=ClaimStore(root,authority);store.claim(request)
    proc=None;selector=selectors.DefaultSelector();output=[bytearray(),bytearray()];overflow=False;error=None
    def drain(timeout):
        nonlocal overflow
        for key,_ in selector.select(timeout):
            chunk=os.read(key.fileobj.fileno(),4096)
            if not chunk:selector.unregister(key.fileobj);continue
            room=max(0,config.limits["max_output_bytes"]-sum(map(len,output)))
            output[key.data].extend(chunk[:room])
            if len(chunk)>room:overflow=True
    try:
        if request["operation"]=="run":
            argv,expected=authority.commands[request["payload"]["command"]]
            exe=Path(argv[0]);st=exe.lstat()
            require(stat.S_ISREG(st.st_mode) and not exe.is_symlink() and
                    st.st_size<=134217728 and digest(exe.read_bytes())==expected,"executable identity")
            env={"PATH":os.defpath,"LANG":"C","PYTHONDONTWRITEBYTECODE":"1"}
            proc=subprocess.Popen(argv,cwd=root,env=env,stdin=subprocess.DEVNULL,
                                  stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
            for n,stream in enumerate((proc.stdout,proc.stderr)):
                os.set_blocking(stream.fileno(),False);selector.register(stream,selectors.EVENT_READ,n)
            deadline=monotonic()+config.limits["timeout_seconds"]
            while True:
                authority.check(root,guard(),wall())
                require(not cancel(),"cancelled")
                drain(config.limits["poll_ms"]/1000)
                require(not overflow,"output limit exceeded")
                if os.waitid(os.P_PID,proc.pid,os.WEXITED|os.WNOHANG|os.WNOWAIT):break
                require(monotonic()<deadline,"execution timeout")
    except BaseException as exc:
        error=str(exc)
    finally:
        if proc:
            try:os.killpg(proc.pid,signal.SIGTERM)
            except ProcessLookupError:pass
            end=monotonic()+config.limits["cleanup_seconds"]
            while selector.get_map() and monotonic()<end:drain(.02)
            try:os.killpg(proc.pid,signal.SIGKILL)
            except ProcessLookupError:pass
            proc.wait(timeout=config.limits["cleanup_seconds"])
            selector.close();proc.stdout.close();proc.stderr.close()
        else:selector.close()
        if overflow:error=error or "output limit exceeded"
        try:
            authority.check(root,guard(),wall())
            scope.check(before,scope.capture())
        except BaseException as exc:error=(error+"; " if error else "")+str(exc)
    result=dict(passed=error is None,returncode=proc.returncode if proc else None,
                stdout=bytes(output[0]).decode("utf-8",errors="replace"),
                stderr=bytes(output[1]).decode("utf-8",errors="replace"),error=error)
    if proc and proc.returncode!=0 and error is None:
        error="command failed with status "+str(proc.returncode);result.update(passed=False,error=error)
    exclusive(store.directory/("result-"+request["request_id"]+".json"),
              json.dumps(result,separators=(",",":")).encode())
    sync_directory(store.directory)
    if error:raise ControlError(error)
    return result

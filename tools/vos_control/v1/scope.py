"""Workspace change detection and nonoverlapping, symlink-safe path boundaries."""
import os
from pathlib import Path
import stat
from .contracts import require, relative, within, digest

def path(root, name):
    relative(name)
    current = Path(root)
    require(current.is_dir() and not current.is_symlink(), "workspace directory")
    for part in name.split("/"):
        current = current / part
        require(not current.is_symlink(), "symlink path component")
    return current

def roots(root, config):
    for name in (*config.allowed_sources, *config.protected, config.scratch, config.evidence):
        p = path(root,name)
        if name in config.allowed_sources and p.exists():
            st=p.stat()
            require(stat.S_ISREG(st.st_mode) and st.st_nlink == 1, "source must be unaliased regular file")
    return path(root,config.scratch), path(root,config.evidence)

class Scope:
    def __init__(self, root, config):
        self.root=Path(root);self.config=config
        roots(self.root,config)

    def capture(self):
        rows={};total=0
        for directory,dirs,files in os.walk(self.root,followlinks=False):
            for name in list(dirs)+files:
                p=Path(directory)/name;n=p.relative_to(self.root).as_posix()
                if any(within(n,x) for x in (self.config.scratch,self.config.evidence)):
                    if name in dirs:dirs.remove(name)
                    continue
                st=p.lstat()
                if stat.S_ISLNK(st.st_mode):
                    rows[n]=("link",os.readlink(p))
                    if name in dirs:dirs.remove(name)
                elif stat.S_ISDIR(st.st_mode):
                    if not any(x.startswith(n+"/") for x in (self.config.scratch,self.config.evidence)):
                        rows[n]=("dir",st.st_mode)
                else:
                    require(stat.S_ISREG(st.st_mode), "special source file")
                    total+=st.st_size
                    require(total<=self.config.limits["max_snapshot_bytes"],"snapshot byte bound")
                    rows[n]=("file",st.st_mode,digest(p.read_bytes()))
                require(len(rows)<=self.config.limits["max_files"],"snapshot file bound")
        return rows

    def check(self,before,after):
        for n in set(before)|set(after):
            if before.get(n)==after.get(n):continue
            require(not any(within(n,p) for p in self.config.protected),"protected path changed: "+n)
            if n not in before and after[n][0]=="dir":
                require(any(x.startswith(n+"/") for x in self.config.allowed_sources),"unscoped directory: "+n)
            else:
                require(n in self.config.allowed_sources and
                        (n not in after or after[n][0]=="file"),"unscoped source change: "+n)
        roots(self.root,self.config)

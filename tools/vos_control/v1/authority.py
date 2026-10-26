"""Pinned, separate owner authorization. A configuration is never an execution grant."""
from dataclasses import dataclass
from pathlib import Path
import math
from types import MappingProxyType
from .contracts import *

def workspace_identity(root):
    p = Path(root)
    require(p.is_absolute() and not p.is_symlink() and p.is_dir(), "workspace root")
    require(p == p.resolve(), "canonical workspace")
    st = p.stat()
    return str(p), str(st.st_dev) + ":" + str(st.st_ino)

@dataclass(frozen=True)
class Authority:
    config: Config
    receipt_sha256: str
    manifest_sha256: str
    executor: str
    workspace: str
    workspace_id: str
    issued: float
    expires: float
    commands: object
    resources: object

    def check(self, root, observation, now):
        require(type(observation) is Observation, "trusted observation provider required")
        require(type(now) in (int, float) and math.isfinite(now) and self.issued <= now < self.expires, "expired authorization")
        require(workspace_identity(root) == (self.workspace, self.workspace_id), "workspace identity changed")
        require(observation.executor == self.executor and observation.baseline == self.config.baseline, "executor/baseline changed")
        r = self.resources
        require(observation.aggregate_charge <= r["aggregate_ceiling"] and observation.task_charge <= r["task_limit"], "charged capacity")
        require(r["aggregate_ceiling"] - observation.reservation_obligation >= r["minimum_margin"], "reservation capacity")
        require(set(observation.physical_free) == set(r["physical_required"]) and
                all(observation.physical_free[n] >= v for n,v in r["physical_required"].items()), "physical reserves")

def authorize(data, receipt_pin, config, manifest_pin, root, executor, now):
    hexhash(receipt_pin); hexhash(manifest_pin)
    require(digest(data) == receipt_pin, "authorization pin mismatch")
    x = decode(data)
    object_keys(x, ("schema","task_id","baseline","config_sha256","manifest_sha256","executor",
                    "workspace","workspace_id","issued","expires","commands","resources"))
    integer(x["schema"],1,1)
    require(x["task_id"] == config.task_id and x["baseline"] == config.baseline and
            x["config_sha256"] == config.sha256 and x["manifest_sha256"] == manifest_pin, "authorization binding")
    require(x["executor"] == executor and identifier(executor), "executor authority")
    require(workspace_identity(root) == (x["workspace"],x["workspace_id"]), "workspace authority")
    for n in ("issued","expires"):
        require(type(x[n]) in (int,float) and math.isfinite(x[n]), "lease time")
    require(x["issued"] <= now < x["expires"] and x["issued"] < x["expires"], "lease window")
    require(type(x["commands"]) is dict and len(x["commands"]) <= 16, "command grants")
    commands = {}
    for name, grant in x["commands"].items():
        identifier(name); object_keys(grant, ("argv","executable_sha256"))
        argv = grant["argv"]
        require(type(argv) is list and 0 < len(argv) <= 64 and
                all(type(v) is str and "\0" not in v and len(v) <= 8192 for v in argv), "argv bounds")
        require(sum(map(len,argv)) <= 65536 and Path(argv[0]).is_absolute() and
                str(Path(argv[0]).resolve()) == argv[0], "absolute executable")
        hexhash(grant["executable_sha256"])
        commands[name] = (tuple(argv), grant["executable_sha256"])
    r = x["resources"]
    object_keys(r, ("aggregate_ceiling","task_limit","minimum_margin","physical_required"))
    for n in ("aggregate_ceiling","task_limit","minimum_margin"): integer(r[n])
    require(type(r["physical_required"]) is dict and r["physical_required"], "reserve profile")
    for n,v in r["physical_required"].items():identifier(n);integer(v)
    frozen = dict(r);frozen["physical_required"] = MappingProxyType(dict(r["physical_required"]))
    return Authority(config,receipt_pin,manifest_pin,executor,x["workspace"],x["workspace_id"],
                     x["issued"],x["expires"],MappingProxyType(commands),MappingProxyType(frozen))

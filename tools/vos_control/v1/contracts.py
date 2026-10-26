"""Strict data contracts. No dynamic imports, commands or accounting policy in config."""
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import PurePosixPath
import re
from types import MappingProxyType

class ControlError(RuntimeError):
    pass

def require(ok, message):
    if not ok:
        raise ControlError(message)

def digest(data):
    return hashlib.sha256(data).hexdigest()

def object_keys(value, keys):
    require(type(value) is dict and set(value) == set(keys), "unexpected object fields")

def integer(value, minimum=0, maximum=2**63-1):
    require(type(value) is int and minimum <= value <= maximum, "integer bound")
    return value

def hexhash(value, length=64):
    require(type(value) is str and re.fullmatch("[0-9a-f]{%d}" % length, value), "digest format")
    return value

def identifier(value):
    require(type(value) is str and re.fullmatch("[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}", value), "identifier")
    return value

def relative(value):
    require(type(value) is str and value and "\\" not in value and
            re.fullmatch(r"[a-zA-Z0-9_.-]+(?:/[a-zA-Z0-9_.-]+)*", value), "relative path")
    p = PurePosixPath(value)
    require(not p.is_absolute() and all(x not in (".", "..") for x in p.parts) and str(p) == value, "normalized path")
    return value

def within(path, root):
    return path == root or path.startswith(root + "/")

def decode(data):
    require(type(data) is bytes and len(data) <= 65536, "JSON input bound")
    def unique(pairs):
        out = {}
        for k, v in pairs:
            require(k not in out, "duplicate JSON key")
            out[k] = v
        return out
    try:
        return json.loads(data, object_pairs_hook=unique,
                          parse_constant=lambda _: (_ for _ in ()).throw(ControlError("nonfinite JSON")))
    except (ValueError, UnicodeError) as exc:
        raise ControlError("invalid JSON") from exc

@dataclass(frozen=True)
class Config:
    task_id: str
    baseline: str
    allowed_sources: tuple
    protected: tuple
    scratch: str
    evidence: str
    limits: object
    sha256: str

    @classmethod
    def parse(cls, data):
        x = decode(data)
        object_keys(x, ("schema", "task_id", "baseline", "allowed_sources", "protected", "scratch", "evidence", "limits"))
        integer(x["schema"], 1, 1)
        identifier(x["task_id"]); hexhash(x["baseline"], 40)
        for k in ("allowed_sources", "protected"):
            require(type(x[k]) is list and len(x[k]) <= 256, "path list bound")
            require(len(set(relative(v) for v in x[k])) == len(x[k]), "duplicate path")
        protected = tuple(dict.fromkeys((".git", *x["protected"])))
        scratch, evidence = relative(x["scratch"]), relative(x["evidence"])
        require(not within(scratch, evidence) and not within(evidence, scratch), "scratch/evidence overlap")
        require(not any(within(scratch, p) or within(p, scratch) or within(evidence, p) or within(p, evidence)
                        for p in protected), "protected root overlap")
        for n in x["allowed_sources"]:
            require(not any(within(n, p) or within(p, n) for p in (*protected, scratch, evidence)), "source boundary")
        bounds = dict(timeout_seconds=(1,300), max_output_bytes=(1,65536), max_snapshot_bytes=(1,16777216),
                      max_files=(1,10000), poll_ms=(10,500), cleanup_seconds=(1,5))
        object_keys(x["limits"], bounds)
        for n, (low, high) in bounds.items():
            integer(x["limits"][n], low, high)
        return cls(x["task_id"], x["baseline"], tuple(x["allowed_sources"]), protected,
                   scratch, evidence, MappingProxyType(dict(x["limits"])), digest(data))

@dataclass(frozen=True)
class Observation:
    """Already-computed accounting inputs; this does not compute or retire reservations."""
    executor: str
    baseline: str
    aggregate_charge: int
    reservation_obligation: int
    task_charge: int
    physical_free: object

    @classmethod
    def parse(cls, x):
        object_keys(x, cls.__dataclass_fields__)
        identifier(x["executor"]); hexhash(x["baseline"], 40)
        for n in ("aggregate_charge", "reservation_obligation", "task_charge"):
            integer(x[n])
        require(type(x["physical_free"]) is dict and x["physical_free"], "physical measurements")
        for n, v in x["physical_free"].items():
            identifier(n); integer(v)
        return cls(x["executor"], x["baseline"], x["aggregate_charge"], x["reservation_obligation"],
                   x["task_charge"], MappingProxyType(dict(x["physical_free"])))

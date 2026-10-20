#!/usr/bin/env python3
"""Check independently defined ABI constants and static wire offsets."""
import pathlib
import re


def defines(path):
    return dict(re.findall(r"^#define\s+(\w+)\s+(\d+)[uU]?\b", pathlib.Path(path).read_text(), re.M))


kernel = defines("arch/x86_64/syscall.h")
user = defines("user/include/vos/file.h")
native = defines("arch/x86_64/file.h")
checks = 0
for name in ["FILE_OPEN_AT", "FILE_READ_AT", "FILE_STAT", "DIRECTORY_NEXT", "FILE_INITIAL"]:
    assert kernel["SYS_" + name] == user["VOS_SYS_" + name], name
    checks += 1
for name in ["ENOENT", "ENOTDIR", "EISDIR", "ENAMETOOLONG", "ELOOP", "EIO", "EUNAVAIL", "ESTALE", "ETIMEDOUT", "EINTR", "EUNSUPP"]:
    assert kernel["VOS_" + name] == user["VOS_" + name] == native["FILE_" + name], name
    checks += 1
for name in ["READ", "STAT", "ENUMERATE", "TRAVERSE"]:
    assert native["FILE_" + name] == user["VOS_FILE_" + name], name
    checks += 1
print(f"test-file-abi: {checks} independent constant comparisons passed")

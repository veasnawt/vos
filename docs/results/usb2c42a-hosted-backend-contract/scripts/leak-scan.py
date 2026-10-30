#!/usr/bin/env python3
"""C4.1 secret audit: reproduces the Gitleaks 8.30.1 default generic-api-key rule (keyword near an assignment
operator, a 10+ character value, Shannon entropy >= 3.5, no hex exclusion) plus common credential shapes over
every file this change adds or modifies. Read-only. Usage: audit-secrets.py BASE_REV"""
import json, math, re, subprocess, sys, collections
GENERIC = re.compile(r"""(?i)[\w.-]{0,50}?(?:access|auth|(?-i:[Aa]pi|API)|credential|creds|key|passw(?:or)?d|secret|token)(?:[ \t\w.-]{0,20})[\s'"]{0,3}(?:=|>|:{1,3}=|\|\||:|=>|\?=|,)[\s'"=]{0,5}([\w.=-]{10,150}|[a-z0-9][a-z0-9+/]{11,}={0,3})(?:[\s'"]|\\[nr]|$)""")
SHAPES = {'private-key': re.compile(r'-----BEGIN [A-Z ]*PRIVATE KEY-----'), 'aws-access-key-id': re.compile(r'\bAKIA[0-9A-Z]{16}\b'),
          'github-token': re.compile(r'\bgh[pousr]_[A-Za-z0-9]{36,}\b'), 'slack-token': re.compile(r'\bxox[baprs]-[A-Za-z0-9-]{10,}\b'),
          'jwt': re.compile(r'\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b')}
def entropy(s):
    c = collections.Counter(s); n = len(s)
    return -sum(v / n * math.log2(v / n) for v in c.values())
base = sys.argv[1]
files = subprocess.run(['git', 'diff', '--name-only', '--diff-filter=AM', base], capture_output=True, text=True, check=True).stdout.split()
files += subprocess.run(['git', 'ls-files', '--others', '--exclude-standard'], capture_output=True, text=True, check=True).stdout.split()
findings, scanned = [], 0
for f in sorted(set(files)):
    try:
        data = open(f, 'rb').read()
    except OSError:
        continue
    if b'\0' in data[:4096]:
        continue
    scanned += 1
    for i, line in enumerate(data.decode(errors='replace').splitlines(), 1):
        for m in GENERIC.finditer(line):
            if entropy(m.group(1)) >= 3.5:
                findings.append({'file': f, 'line': i, 'rule': 'generic-api-key', 'value_prefix': m.group(1)[:6] + '...'})
        for name, rx in SHAPES.items():
            if rx.search(line):
                findings.append({'file': f, 'line': i, 'rule': name})
print(json.dumps({'base': base, 'files_scanned': scanned, 'findings': findings}, indent=1))
sys.exit(1 if findings else 0)

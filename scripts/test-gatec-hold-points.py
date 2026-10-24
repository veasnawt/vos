#!/usr/bin/env python3
"""Host test: Gate C's four publication hold points cannot let a cancelled operation publish; no VOS execution.

In the Gate C image each hold must follow its operation's private result and immediately precede the file
layer's final cancellation check, with no publication (token, staging copy, user copy, cookie) before that
check; the check must fail a killed owner with -FILE_EINTR first. Every other image must compile no hold,
and the hooks must not move a line of file.c: three trail the line completing the operation, and read_at's
is a single directive line (file_hold.inc). Paths are relative to this script, so mutation controls can run
it against a modified copy of the tree.
"""
from pathlib import Path
import re
import shutil
import subprocess
from unittest import TestCase, main, skipUnless

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / 'arch/x86_64'
CALL = 'storage_stress_c_hold(r);'
# function: (signature start, the operation whose private result is held, the final cancellation check)
SITES = {
    'open_at': ('static int64_t open_at(', 'walk(s, n->ino, len, &st);', 'int final = finish(s, &scope);'),
    'stat_node': ('static int stat_node(', 'r = node_stat(s, n->ino, &st);', 'int final = finish(s, &scope);'),
    'read_at': ('static int64_t read_at(', 'r = outcome(s, ext2_read(volume.fs, n->ino, off, s->stage, len, &done));',
                'if (!r && !(r = outcome(s, 0))) {'),             # EXT2_OK is 0 after preprocessing
    'next_entry': ('static int next_entry(', 'int err = ext2_readdir(volume.fs, n->ino, &pos, &ent);',
                   'int final = finish(s, &scope);'),
}
PUBLISH = ('publish(', 'issue(&next_token)', 'copy_to_user(', 'memcpy(out', '*cookie = pos', 'memcpy(s->stage')
TRAILING = {
    'open_at': '    r = n->generation != volume.generation ? -FILE_ESTALE : walk(s, n->ino, len, &st); FILE_PUBLICATION_HOLD(r)\n',
    'stat_node': '    r = node_stat(s, n->ino, &st); FILE_PUBLICATION_HOLD(r)\n    int final = finish(s, &scope);\n',
    'next_entry': '        eof = 0;\n        break;\n    } FILE_PUBLICATION_HOLD(r)\n    int final = finish(s, &scope);\n',
}
INCLUDE = ('        r = outcome(s, ext2_read(volume.fs, n->ino, off, s->stage, len, &done));\n'
           '    /* Commit before the no-wait finish erases staging. IF=0: no stop can arise\n'
           '     * between this last outcome check, copy and finish. Never copy on error. */\n'
           '#include "file_hold.inc"\n'
           '    if (!r && !(r = outcome(s, EXT2_OK))) {\n')
GUARD = '#if defined(VOS_TEST) && defined(VOS_TEST_CASE_storage_stress_c)'


def preprocess(src, defs):
    p = subprocess.run(['gcc', '-std=gnu11', '-ffreestanding', '-m64', '-mgeneral-regs-only', '-E', '-P', *defs,
                        '-I', str(src), str(Path(src) / 'file.c')], capture_output=True, text=True, timeout=60)
    if p.returncode:
        raise AssertionError(p.stderr[:400])
    return p.stdout


def body(text, start):
    i = text.index(start)
    return text[i:text.index('\n}\n', i)]


def flat(text):
    """Code as one whitespace-normalized string (comments are already gone after preprocessing)."""
    return re.sub(r'\s+', ' ', text)


def ordering_problems(gatec):
    """Ordering in the preprocessed Gate C image."""
    found = []
    if gatec.count(CALL) != 4:
        found.append('expected four holds in the Gate C image, found %d' % gatec.count(CALL))
    for name, (start, op, check) in SITES.items():
        try:
            f = flat(body(gatec, start))
        except ValueError:
            found.append(name + ': function missing')
            continue
        if f.count(CALL) != 1:
            found.append('%s: %d holds' % (name, f.count(CALL)))
            continue
        h = f.index(CALL)
        if f.count(op) != 1 or f.index(op) > h:
            found.append(name + ': hold does not follow the operation')
        if f.count(check) != 1 or f[h + len(CALL):].lstrip()[:len(check)] != check:
            found.append(name + ': hold is not immediately before the final cancellation check')
        if any(p in f[:h] for p in PUBLISH):
            found.append(name + ': publication before the hold')
        if not any(p in f[h:] for p in PUBLISH):
            found.append(name + ': no publication after the check')
        if check.startswith('int final') and not f[h:].startswith(CALL + ' ' + check + ' if (final) r = final;'):
            found.append(name + ': cancellation result does not override the operation result')
        if name == 'read_at' and 'if (!r) r = outcome(s, ext2_read(' not in f[:h]:
            found.append(name + ': the read is not complete before the hold')
    return found


def source_problems(src):
    """The hooks' zero-line-shift forms, the cancellation check, and the hold's inertness."""
    found = []
    text = (Path(src) / 'file.c').read_text()
    for name, want in TRAILING.items():
        if text.count(want) != 1:
            found.append(name + ': hook does not trail the line completing the operation')
    if text.count(INCLUDE) != 1:
        found.append('read_at: hook is not the single directive line before the final check')
    if text.count('FILE_PUBLICATION_HOLD(r)') != 3 or text.count('#include "file_hold.inc"') != 1:
        found.append('unexpected hook count in file.c')
    inc = (Path(src) / 'file_hold.inc').read_text()
    code = [l.strip() for l in inc.splitlines() if l.strip() and not l.lstrip().startswith(('/*', '*'))]
    if code != [GUARD, 'FILE_PUBLICATION_HOLD(r)', '#endif']:
        found.append('file_hold.inc: hold not confined to the Gate C image')
    header = (Path(src) / 'file.h').read_text()
    if not re.search(re.escape(GUARD) + r'\nvoid storage_stress_c_hold\(int op_result\);\n'
                     r'#define FILE_PUBLICATION_HOLD\(r\) storage_stress_c_hold\(r\);\n#else\n'
                     r'#define FILE_PUBLICATION_HOLD\(r\)\n#endif\n#endif\n$', header):
        found.append('file.h: hold macro not empty outside the Gate C image, or not after every declaration')
    outcome = body(text, 'static int outcome(')
    if not re.search(r'if \(\(s && \(s->closing \|\| s->owner->kill_reason\)\) \|\| stop == IO_SCOPE_INTERRUPTED\)\n'
                     r'\s+return -FILE_EINTR;', outcome):
        found.append('outcome: a killed owner does not fail with -FILE_EINTR first')
    if 'int r = outcome(s, EXT2_OK);\n    io_scope_end(scope);' not in body(text, 'static int finish('):
        found.append('finish: does not begin with the cancellation check')
    hold = body((Path(src) / 'storage_stress_c.c').read_text(), 'void storage_stress_c_hold(int op_result) {')
    if 'if(hold_state!=1 || thread_current()->id!=hold_tid) return;' not in hold:
        found.append('hold: not inert unless armed for the victim thread')
    if re.findall(r'\b(io_scope_wait|thread_block_until|thread_sleep|thread_yield)\(', hold) != ['io_scope_wait']:
        found.append('hold: must wait only through the cancellable scope wait')
    if re.search(r'\b(file_|publish|copy_to_user|memcpy|kill_reason\s*=)', hold):
        found.append('hold: touches file state or publication')
    return found


@skipUnless(shutil.which('gcc'), 'gcc required')
class HoldPoints(TestCase):
    src = SRC

    def test_hold_points_precede_cancellation_and_publication(self):
        self.assertEqual(ordering_problems(preprocess(self.src, ['-DVOS_TEST', '-DVOS_TEST_CASE_storage_stress_c'])), [])

    def test_hook_forms_cancellation_and_inertness(self):
        self.assertEqual(source_problems(self.src), [])

    def test_only_the_gate_c_image_compiles_holds(self):
        for defs in ([], ['-DVOS_TEST'], ['-DVOS_TEST', '-DVOS_TEST_CASE_storage_stress_b'],
                     ['-DVOS_TEST', '-DVOS_TEST_CASE_storage_stress'], ['-DVOS_TEST', '-DVOS_TEST_CASE_file'],
                     ['-DVOS_TEST_CASE_storage_stress_c']):
            with self.subTest(defs):
                self.assertNotIn('storage_stress', preprocess(self.src, defs))


if __name__ == '__main__':
    main()

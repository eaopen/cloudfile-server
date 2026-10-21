#!/usr/bin/env python3
"""Compile the complete production lock barrier and real fileop dispatcher.

Only config/database I/O is doubled. This proves disabled Checkout issues zero
SQL and active reservations/SQL failures refuse native unscoped writes.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix='cf-lock-test-') as folder:
    build = Path(folder)
    # Keep every production statement; replace only external include boundaries.
    source = re.sub(r'^#include[^\n]*\n', '', (root/'common/cf-lock.c').read_text(), flags=re.M)
    policy = (root/'server/cloudfile-policy.c').read_text()
    start = policy.index('int\ncf_policy_check_unleased_write (')
    end = policy.index('\n}\n', start) + 3
    (build/'production.c').write_text(source + '\n' + policy[start:end])
    flags = subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'], text=True).split()
    subprocess.run(['cc','-std=c99','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
        '-I'+str(build),'-I'+str(root/'common'),'-I'+str(root/'include'),
        str(root/'tests/cf-lock/fixture.c'),str(root/'common/cf-fileop.c'),
        str(root/'common/cf-path.c'),'-o',str(build/'test'),*flags],check=True)
    subprocess.run([str(build/'test')],check=True)

#!/bin/bash
# Native credential rotation must be checked by the same gates that serve bytes.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
python3 "$here/test-token.py"

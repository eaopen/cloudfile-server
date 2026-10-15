#!/bin/bash
# Compile the production qualifier and exercise the same role vectors as Hub.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
python3 "$here/test-qualification.py"

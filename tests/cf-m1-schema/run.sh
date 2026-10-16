#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
python3 "$here/test-native-boundary.py"

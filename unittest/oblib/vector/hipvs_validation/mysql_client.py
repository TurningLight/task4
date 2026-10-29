"""Pinned, offline SQL validation client; not a seekdb runtime dependency."""
import hashlib
from pathlib import Path
import sys

wheel=Path(__file__).with_name('vendor')/'pymysql-1.1.2-py3-none-any.whl'
expected='e6b1d89711dd51f8f74b1631fe08f039e7d76cf67a42a323d3178f0f25762ed9'
if not wheel.is_file() or hashlib.sha256(wheel.read_bytes()).hexdigest()!=expected:
    raise RuntimeError('Missing or modified bundled PyMySQL validation client; restore the committed vendor wheel')
sys.path.insert(0,str(wheel))
import pymysql  # noqa: E402

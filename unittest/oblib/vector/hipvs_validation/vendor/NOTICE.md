# Offline validation dependency

`pymysql-1.1.2-py3-none-any.whl` is the unmodified PyMySQL 1.1.2 pure-Python wheel
from https://pypi.org/project/PyMySQL/1.1.2/ . Its MIT license is included inside
the wheel and copied to `PyMySQL-LICENSE` here. This dependency is used only by
the SQL validation clients, not by the seekdb server or GPU worker.

SHA-256 (checked against the PyPI release metadata):
`e6b1d89711dd51f8f74b1631fe08f039e7d76cf67a42a323d3178f0f25762ed9`.

The validation loader verifies this digest and imports directly from the wheel.
It does not invoke pip, download packages, or change the system Python environment.
NumPy 2.4.6 is already provided by the tested competition image.

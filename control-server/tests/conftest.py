"""시험이 어느 controlplane 을 import 하는지 정한다.

변이 시험(tests/mutate.py)은 원본을 임시 폴더에 복사해 고친 뒤 CP_SRC 로 그 폴더를 넘긴다.
원본이 대신 import 되면 변이가 조용히 살아남은 것처럼 보이므로, 실제로 import 된 위치를
확인하고 어긋나면 시험 전체를 멈춘다.
"""

import os
import sys
from pathlib import Path

_SRC = Path(os.environ.get("CP_SRC") or Path(__file__).resolve().parents[1]).resolve()
sys.path.insert(0, str(_SRC))

import controlplane  # noqa: E402
import pytest  # noqa: E402

_loaded = Path(controlplane.__file__).resolve()
if not _loaded.is_relative_to(_SRC):
    raise RuntimeError(f"controlplane 을 {_SRC} 가 아니라 {_loaded} 에서 읽었다")


def pytest_collection_modifyitems(config, items):
    """건너뛰는 행은 store 표시가 있는 것뿐이어야 한다. 다른 이유의 skip 은 수집에서 멈춘다."""
    bad = [
        item.nodeid
        for item in items
        if any(item.iter_markers(name="skip")) or any(item.iter_markers(name="skipif"))
        if not any(item.iter_markers(name="store"))
    ]
    if bad:
        raise pytest.UsageError(f"store 표시 없이 건너뛰는 시험이 있다: {bad}")

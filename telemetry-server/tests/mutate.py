"""텔레메트리 서비스의 변이 시험. 실행기는 control-server/tests/mutate.py 를 그대로 쓴다. 판정 규칙은 그 파일 첫머리다.

여기서는 원본 폴더, 패키지 이름, 변이 목록 폴더, 고친 패키지를 넘기는 변수만 바꾼다. 변이 목록은 tests/mutants/<표>.py 다.

    python tests/mutate.py [--store] [-j N] [표 이름 ...]      (telemetry-server 에서)
"""

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
_spec = importlib.util.spec_from_file_location("cp_mutate", ROOT.parent / "control-server" / "tests" / "mutate.py")
mutate = importlib.util.module_from_spec(_spec)
sys.modules["cp_mutate"] = mutate  # dataclass 가 모듈을 찾는다
_spec.loader.exec_module(mutate)

mutate.CP_ROOT = ROOT
mutate.PACKAGE = "telemetry"
mutate.MUTANT_DIR = ROOT / "tests" / "mutants"
mutate.SRC_ENV = "TM_SRC"

if __name__ == "__main__":
    sys.exit(mutate.main(sys.argv[1:]))

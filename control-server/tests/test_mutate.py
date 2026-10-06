"""변이 실행기 자체의 판정 시험. 실행기가 틀리면 모든 변이가 조용히 KILLED 로 보인다."""

import pytest

import mutate
from mutate import Mutant, judge

K1 = "tests/test_x.py::test_case[a]"
K2 = "tests/test_x.py::test_case[b]"


def raw(**over):
    base = dict(id="m1", path="controlplane/x.py", old="a", new="b", kills=[K1], why="규칙")
    base.update(over)
    return base


class TestJudge:
    def test_all_kills_failed(self):
        out = f"FAILED {K1} - AssertionError: x\nFAILED {K2}\n1 failed"
        assert judge(1, out, (K1, K2)).verdict == "KILLED"

    def test_some_kills_failed(self):
        assert judge(1, f"FAILED {K1} - x\n", (K1, K2)).verdict == "PARTIAL"

    def test_only_other_rows_failed(self):
        assert judge(1, "FAILED tests/test_x.py::test_case[z] - x\n", (K1,)).verdict == "SURVIVED"

    def test_all_passed(self):
        assert judge(0, "3 passed", (K1,)).verdict == "SURVIVED"

    @pytest.mark.parametrize("code", [2, 3, 4, 5])
    def test_other_exit_codes_are_error(self, code):
        # 수집 실패(2)나 시험 없음(5)을 KILLED 로 읽으면 import 를 깨는 변이가 전부 통과한다.
        assert judge(code, f"ERROR {K1}\n", (K1,)).verdict == "ERROR"

    def test_timeout_is_error(self):
        assert judge(None, "", (K1,)).verdict == "ERROR"

    def test_exit_one_without_failed_lines_is_error(self):
        assert judge(1, "something odd", (K1,)).verdict == "ERROR"

    def test_error_line_is_not_a_kill(self):
        # 준비 단계에서 터진 행은 변이가 그 행을 돌렸다는 증거가 아니다.
        assert judge(1, f"ERROR {K1} - RuntimeError\n", (K1,)).verdict == "ERROR"

    def test_error_on_kill_row_beats_other_failures(self):
        out = f"FAILED {K2} - x\nERROR {K1} - RuntimeError\n"
        assert judge(1, out, (K1, K2)).verdict == "ERROR"

    def test_error_on_other_row_does_not_hide_kill(self):
        out = f"FAILED {K1} - x\nERROR tests/test_x.py::test_case[z] - y\n"
        assert judge(1, out, (K1,)).verdict == "KILLED"

    def test_prefix_of_node_id_is_not_a_match(self):
        out = "FAILED tests/test_x.py::test_case[a-long] - x\n"
        assert judge(1, out, (K1,)).verdict == "SURVIVED"


class TestApply:
    def test_replaces_single_occurrence(self):
        assert mutate.apply("x = a + 1", Mutant("m", "controlplane/x.py", "a + 1", "a", (K1,), "w")) == "x = a"

    @pytest.mark.parametrize("text", ["nothing here", "a + 1; a + 1"])
    def test_stale_or_ambiguous_old_is_rejected(self, text):
        with pytest.raises(ValueError):
            mutate.apply(text, Mutant("m", "controlplane/x.py", "a + 1", "a", (K1,), "w"))


class TestToMutant:
    def test_valid(self):
        assert mutate.to_mutant(raw(), "s").kills == (K1,)

    @pytest.mark.parametrize("over", [
        {"id": "한글"},
        {"id": "a b"},
        {"path": "tests/test_x.py"},
        {"path": "controlplane/../x.py"},
        {"old": ""},
        {"old": "same", "new": "same"},
        {"kills": []},
        {"kills": ["tests/test_x.py"]},
        {"kills": "tests/test_x.py::t"},
        {"why": "  "},
        {"extra": 1},
    ])
    def test_invalid(self, over):
        with pytest.raises(ValueError):
            mutate.to_mutant(raw(**over), "s")

    def test_missing_key(self):
        bad = raw()
        del bad["why"]
        with pytest.raises(ValueError):
            mutate.to_mutant(bad, "s")


class TestLoad:
    def test_empty_mutant_dir_is_an_error(self, tmp_path, monkeypatch):
        # 변이를 하나도 못 찾았는데 0/0 으로 pass 를 내면 안 된다.
        monkeypatch.setattr(mutate, "MUTANT_DIR", tmp_path)
        with pytest.raises(ValueError):
            mutate.load_mutants()

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


class TestArgs:
    def test_names_only(self):
        assert mutate.parse_args(["room_id", "http"]) == (["room_id", "http"], [], mutate.DEFAULT_JOBS)

    def test_store_flag(self):
        assert mutate.parse_args(["--store", "store"]) == (["store"], ["--store"], mutate.DEFAULT_JOBS)

    def test_store_flag_once(self):
        assert mutate.parse_args(["--store", "--store"]) == ([], ["--store"], mutate.DEFAULT_JOBS)

    @pytest.mark.parametrize("arg", ["--stor", "-s", "--store-endpoint", "--store=1", "-"])
    def test_unknown_flag_is_rejected(self, arg):
        # 모르는 옵션을 변이 파일 이름으로 읽거나 조용히 버리면 저장소 없이 돈 결과를 믿게 된다.
        with pytest.raises(ValueError):
            mutate.parse_args([arg, "store"])


    @pytest.mark.parametrize("argv,jobs", [(["-j", "4"], 4), (["-j4"], 4), (["-j", "1", "room_id"], 1)])
    def test_jobs(self, argv, jobs):
        assert mutate.parse_args(argv)[2] == jobs

    @pytest.mark.parametrize("argv", [["-j"], ["-j", "0"], ["-j", "65"], ["-j", "x"], ["-j", "-1"], ["-j", "４"]])
    def test_bad_jobs_is_rejected(self, argv):
        with pytest.raises(ValueError):
            mutate.parse_args(argv)


class TestRunOne:
    def test_original_failing_alone_is_error(self, monkeypatch, tmp_path):
        # 원본이 그 행들을 단독으로 통과하지 못하면 변이가 무엇이든 FAIL 로 보여 KILLED 로 잘못 읽힌다.
        calls = []

        def fake(src, targets, flags=(), report="-rfE"):
            calls.append(src)
            return 1, f"FAILED {K1} - x\n"

        monkeypatch.setattr(mutate, "run_pytest", fake)
        m = Mutant("m", "controlplane/x.py", "a", "b", (K1,), "w")
        assert mutate.run_one(m, tmp_path).verdict == "ERROR"
        assert calls == [tmp_path]

    def test_runs_only_kill_nodes(self, monkeypatch, tmp_path):
        seen = []

        def fake(src, targets, flags=(), report="-rfE"):
            seen.append(list(targets))
            return (0, "") if src == tmp_path else (1, f"FAILED {K1} - x\nFAILED {K2} - y\n")

        monkeypatch.setattr(mutate, "run_pytest", fake)
        monkeypatch.setattr(mutate, "copy_package", lambda dest: (dest / "controlplane").mkdir() or (dest / "controlplane" / "x.py").write_text("a", encoding="utf-8"))
        m = Mutant("m", "controlplane/x.py", "a", "b", (K2, K1, K1), "w")
        assert mutate.run_one(m, tmp_path).verdict == "KILLED"
        assert seen == [sorted({K1, K2}), sorted({K1, K2})]


class TestBaselineRows:
    M = Mutant("m", "controlplane/x.py", "a", "b", (K1, K2), "w")

    def test_all_kills_passed(self):
        out = f"PASSED {K1}\nPASSED {K2}\n2 passed"
        assert mutate.unrun_kills(out, [self.M]) == []

    def test_skipped_kill_row_is_reported(self):
        # --store 없이 돌린 저장소 행은 건너뛰어 PASSED 줄이 없다. 그대로 두면 모든 변이가 SURVIVED 다.
        out = f"PASSED {K1}\nSKIPPED [1] tests/test_x.py:3: no store\n1 passed, 1 skipped"
        assert mutate.unrun_kills(out, [self.M]) == [K2]

    def test_prefix_is_not_a_pass(self):
        out = f"PASSED {K1}\nPASSED {K2[:-1]}x]\n"
        assert mutate.unrun_kills(out, [self.M]) == [K2]

    def test_failed_is_not_a_pass(self):
        assert mutate.unrun_kills(f"PASSED {K1}\nFAILED {K2} - x\n", [self.M]) == [K2]


class TestRunPytest:
    def test_flags_reach_pytest(self, monkeypatch, tmp_path):
        seen = {}

        class Done:
            returncode = 0
            stdout = ""
            stderr = ""

        def fake_run(cmd, **kwargs):
            seen["cmd"] = cmd
            return Done()
        monkeypatch.setattr(mutate.subprocess, "run", fake_run)
        mutate.run_pytest(tmp_path, ["tests/test_x.py"], ["--store"])
        assert "--store" in seen["cmd"]
        assert seen["cmd"][-1] == "tests/test_x.py"

    def test_baseline_asks_for_pass_lines(self, monkeypatch, tmp_path):
        seen = {}

        def fake_run_pytest(src, targets, flags=(), report="-rfE", timeout_s=mutate.TIMEOUT_S):
            seen.update(flags=list(flags), report=report)
            return 0, ""
        monkeypatch.setattr(mutate, "run_pytest", fake_run_pytest)
        monkeypatch.setattr(mutate, "copy_package", lambda dest: None)
        mutate.baseline([Mutant("m", "controlplane/x.py", "a", "b", (K1,), "w")], ["--store"])
        assert seen == {"flags": ["--store"], "report": "-rA"}


class TestBaselineTimeout:
    def test_baseline_uses_its_own_timeout_and_says_so(self, monkeypatch):
        # 원본 시험은 파일 전체라 변이 한 번보다 길다. 같은 상한을 쓰면 정상 원본이 '통과 못함' 으로 보인다.
        seen = {}

        def fake(src, targets, flags=(), report="-rfE", timeout_s=mutate.TIMEOUT_S):
            seen["timeout_s"] = timeout_s
            return None, "....."

        monkeypatch.setattr(mutate, "run_pytest", fake)
        ok, out = mutate.baseline([Mutant("m", "controlplane/x.py", "a", "b", (K1,), "w")])
        assert not ok
        assert seen["timeout_s"] == mutate.BASELINE_TIMEOUT_S > mutate.TIMEOUT_S
        assert "끝나지 않았다" in out

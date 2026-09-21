"""docgate 의 판정 함수 검증. 구현보다 먼저 쓴다 (design-audit 6장 규칙 7).

각 케이스는 "정상 구현이 통과하는가"(규칙 3)와 "결함을 잡는가"를 쌍으로 확인한다.
"""

import shutil
import tempfile
import unittest
from pathlib import Path

import docgate


def write(root: Path, rel: str, text: str) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


CLEAN_KOR = """# 제목

본문.

## 절

| 열 |
|----|
| 값 |

```bash
# 이것은 헤딩이 아니다
echo hi
```

[링크](other.md)
"""

CLEAN_ENG = """# Title

Body.

## Section

| col |
|-----|
| val |

```bash
# this is not a heading
echo hi
```

[link](other.md)
"""


class GateCase(unittest.TestCase):
    def setUp(self) -> None:
        self.root = Path(tempfile.mkdtemp(prefix="docgate-test-"))
        self.addCleanup(shutil.rmtree, self.root, True)

    def build(self, kor: str = CLEAN_KOR, eng: str = CLEAN_ENG) -> None:
        write(self.root, "docs/kor/a.md", kor)
        write(self.root, "docs/eng/a.md", eng)
        write(self.root, "docs/kor/other.md", "# 다른 문서\n")
        write(self.root, "docs/eng/other.md", "# Other\n")

    def run_gate(self) -> docgate.Report:
        return docgate.run(self.root)


class TestHeadings(unittest.TestCase):
    def test_fenced_hash_is_not_a_heading(self) -> None:
        text = "# real\n\n```\n# fake\n```\n\n## real two\n"
        self.assertEqual(docgate.headings(text), [(1, "real"), (2, "real two")])

    def test_tilde_fence(self) -> None:
        text = "# real\n\n~~~\n# fake\n~~~\n"
        self.assertEqual(docgate.headings(text), [(1, "real")])

    def test_indented_hash_inside_fence_with_language(self) -> None:
        text = "```powershell\n# fake\n```\n# real\n"
        self.assertEqual(docgate.headings(text), [(1, "real")])

    def test_hash_without_space_is_not_a_heading(self) -> None:
        # "#5 항목" 같은 본문은 헤딩이 아니다.
        self.assertEqual(docgate.headings("#5 not a heading\n# yes\n"), [(1, "yes")])

    def test_seven_hashes_is_not_a_heading(self) -> None:
        self.assertEqual(docgate.headings("####### too deep\n"), [])


class TestLinkExtraction(unittest.TestCase):
    def test_skips_external_and_anchor_only(self) -> None:
        text = "[a](https://x.test) [b](mailto:x@y.z) [c](#section) [d](real.md)"
        self.assertEqual(docgate.local_links(text), [(1, "real.md")])

    def test_strips_anchor_and_decodes_percent(self) -> None:
        text = "[a](dir/파일.md#절) [b](a%20b.md)"
        self.assertEqual(docgate.local_links(text), [(1, "dir/파일.md"), (1, "a b.md")])

    def test_ignores_image_and_reference_noise(self) -> None:
        self.assertEqual(docgate.local_links("![img](pic.png)"), [(1, "pic.png")])

    def test_ignores_link_inside_fence(self) -> None:
        self.assertEqual(docgate.local_links("```\n[a](x.md)\n```\n"), [])


class TestGate(GateCase):
    def test_clean_repo_passes(self) -> None:
        # 규칙 3: 정상 구현이 통과하는지부터 확인한다.
        self.build()
        self.assertTrue(self.run_gate().ok)

    def test_heading_level_change_fails_even_when_count_matches(self) -> None:
        self.build(eng=CLEAN_ENG.replace("## Section", "### Section"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any(f.check == "parity" for f in report.findings))

    def test_missing_heading_fails(self) -> None:
        self.build(eng=CLEAN_ENG.replace("## Section\n", ""))
        self.assertFalse(self.run_gate().ok)

    def test_table_row_mismatch_fails(self) -> None:
        self.build(eng=CLEAN_ENG.replace("| val |", "| val |\n| val2 |"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any("표" in f.detail for f in report.findings))

    def test_code_block_mismatch_fails(self) -> None:
        self.build(eng=CLEAN_ENG + "\n```\nextra\n```\n")
        self.assertFalse(self.run_gate().ok)

    def test_link_count_mismatch_fails(self) -> None:
        self.build(eng=CLEAN_ENG + "\n[extra](other.md)\n")
        self.assertFalse(self.run_gate().ok)

    def test_missing_mirror_file_fails(self) -> None:
        self.build()
        (self.root / "docs/eng/other.md").unlink()
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any(f.check == "mirror" for f in report.findings))

    def test_extra_mirror_file_fails(self) -> None:
        self.build()
        write(self.root, "docs/eng/orphan.md", "# Orphan\n")
        self.assertFalse(self.run_gate().ok)

    def test_dangling_link_fails(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(gone.md)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(gone.md)"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any(f.check == "link" for f in report.findings))

    def test_allowed_dangling_link_passes(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(experiments.md)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(experiments.md)"))
        self.assertTrue(self.run_gate().ok)

    def test_link_to_existing_directory_passes(self) -> None:
        write(self.root, "docs/kor/decisions/0001-가.md", "# 가\n")
        write(self.root, "docs/eng/decisions/0001-a.md", "# A\n")
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(decisions/)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(decisions/)"))
        report = self.run_gate()
        # decisions/ 아래 파일 이름은 언어마다 다르므로 미러 짝 검사 대상이 아니다.
        self.assertTrue(report.ok, report.text())

    def test_korean_filename_link_resolves(self) -> None:
        # 실제 저장소의 ADR 이 이 모양이다. 번호는 같고 이름은 언어마다 다르다.
        write(self.root, "docs/kor/decisions/0002-리바인딩-복구-미보장.md", "# 결정\n")
        write(self.root, "docs/eng/decisions/0002-no-rebinding-recovery.md", "# Decision\n")
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(decisions/0002-리바인딩-복구-미보장.md)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(decisions/0002-no-rebinding-recovery.md)"))
        report = self.run_gate()
        self.assertTrue(report.ok, report.text())

    def test_unpairable_names_without_number_fail(self) -> None:
        # 번호가 없으면 짝을 맞출 수 없다. 조용히 통과시키지 않고 막는다.
        self.build()
        write(self.root, "docs/kor/결정.md", "# 결정\n")
        write(self.root, "docs/eng/decision.md", "# Decision\n")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(sum(1 for f in report.findings if f.check == "mirror"), 2)

    def test_parent_relative_link_resolves(self) -> None:
        write(self.root, "docs/kor/sub/x.md", "# 하위\n\n[위로](../other.md)\n")
        write(self.root, "docs/eng/sub/x.md", "# Sub\n\n[up](../other.md)\n")
        self.build()
        self.assertTrue(self.run_gate().ok)

    def test_claim_scan_is_report_only(self) -> None:
        self.build(kor=CLEAN_KOR + "\n이것은 실측이며 전부 확인했다.\n",
                   eng=CLEAN_ENG + "\nThis was measured and all verified.\n")
        report = self.run_gate()
        self.assertTrue(report.ok)
        self.assertTrue(report.claims, "주장 문구는 보고되어야 한다")


class TestPairing(GateCase):
    def test_date_prefixed_history_pairs_one_to_one(self) -> None:
        # 앞자리가 같은 파일이 한 열쇠로 뭉쳐 사라지면 안 된다.
        self.build()
        for day in ("2026-09-10-a", "2026-09-14-b", "2026-09-20-c"):
            write(self.root, f"docs/kor/commit_history/{day}.md", "# 기록\n")
            write(self.root, f"docs/eng/commit_history/{day}.md", "# Record\n")
        report = self.run_gate()
        self.assertTrue(report.ok, report.text())
        self.assertEqual(report.pairs, 5)

    def test_history_file_missing_on_one_side_is_caught(self) -> None:
        self.build()
        write(self.root, "docs/kor/commit_history/2026-09-10-a.md", "# 기록\n")
        write(self.root, "docs/kor/commit_history/2026-09-14-b.md", "# 기록\n")
        write(self.root, "docs/eng/commit_history/2026-09-10-a.md", "# Record\n")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(sum(1 for f in report.findings if f.check == "mirror"), 1)

    def test_different_dates_are_not_paired_by_year(self) -> None:
        # 한쪽에만 있는 기록 두 건이 앞자리 연도로 붙으면 결함이 통과한다.
        self.build()
        write(self.root, "docs/kor/commit_history/2026-09-10-a.md", "# 기록\n")
        write(self.root, "docs/eng/commit_history/2026-09-14-b.md", "# Record\n")
        report = self.run_gate()
        self.assertFalse(report.ok, report.text())
        self.assertEqual(sum(1 for f in report.findings if f.check == "mirror"), 2)

    def test_adr_number_pairing_still_works_next_to_dates(self) -> None:
        self.build()
        write(self.root, "docs/kor/commit_history/2026-09-10-a.md", "# 기록\n")
        write(self.root, "docs/eng/commit_history/2026-09-10-a.md", "# Record\n")
        write(self.root, "docs/kor/decisions/0003-가.md", "# 가\n")
        write(self.root, "docs/eng/decisions/0003-c.md", "# C\n")
        report = self.run_gate()
        self.assertTrue(report.ok, report.text())

    def test_same_number_twice_is_not_silently_paired(self) -> None:
        self.build()
        write(self.root, "docs/kor/decisions/0001-가.md", "# 가\n")
        write(self.root, "docs/kor/decisions/0001-나.md", "# 나\n")
        write(self.root, "docs/eng/decisions/0001-a.md", "# A\n")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertEqual(sum(1 for f in report.findings if f.check == "mirror"), 3)

    def test_every_file_is_accounted_for(self) -> None:
        self.build()
        report = self.run_gate()
        korean = len(list((self.root / "docs/kor").rglob("*.md")))
        english = len(list((self.root / "docs/eng").rglob("*.md")))
        unmatched = sum(1 for f in report.findings if f.check == "mirror")
        self.assertEqual(report.pairs * 2 + unmatched, korean + english)


class TestVerdictLine(GateCase):
    def test_report_text_ends_with_single_verdict(self) -> None:
        self.build()
        lines = self.run_gate().text().strip().splitlines()
        self.assertEqual(lines[-1], "VERDICT: pass")
        self.assertEqual(sum(1 for line in lines if line.startswith("VERDICT:")), 1)

    def test_failing_verdict_line(self) -> None:
        self.build(eng=CLEAN_ENG.replace("## Section", "### Section"))
        self.assertEqual(self.run_gate().text().strip().splitlines()[-1], "VERDICT: fail")



class TestSkippedRegions(unittest.TestCase):
    def test_indented_code_block_is_not_body(self) -> None:
        text = "본문\n\n    # 가짜\n    | t |\n    [x](x.md)\n\n# 진짜\n"
        self.assertEqual(docgate.headings(text), [(1, "진짜")])
        self.assertEqual(docgate.table_rows(text), 0)
        self.assertEqual(docgate.local_links(text), [])

    def test_shallow_indented_table_row_still_counts(self) -> None:
        # 3칸까지는 표 행이다. 4칸부터는 CommonMark 가 코드로 본다.
        self.assertEqual(docgate.table_rows("| a |\n   | b |\n"), 2)

    def test_four_space_indented_table_row_is_not_a_row(self) -> None:
        self.assertEqual(docgate.table_rows("| a |\n    | b |\n"), 1)

    def test_html_comment_hides_its_contents(self) -> None:
        self.assertEqual(docgate.headings("<!--\n# 가짜\n-->\n# 진짜\n"), [(1, "진짜")])

    def test_single_line_html_comment_does_not_swallow_the_file(self) -> None:
        self.assertEqual(docgate.headings("<!-- 한 줄 -->\n# 진짜\n"), [(1, "진짜")])

    def test_yaml_front_matter_is_skipped(self) -> None:
        self.assertEqual(docgate.headings("---\ntitle: X\n# key\n---\n# 진짜\n"), [(1, "진짜")])

    def test_dashes_later_in_the_file_are_not_front_matter(self) -> None:
        self.assertEqual(docgate.headings("# 진짜\n\n---\n\n## 둘\n"), [(1, "진짜"), (2, "둘")])

    def test_setext_heading_is_ignored_on_purpose(self) -> None:
        # 지원하지 않는다고 정한 것이다. 몰라서 빠진 것과 구분하려고 고정한다.
        self.assertEqual(docgate.headings("제목\n=====\n"), [])

    def test_table_without_leading_pipe_is_not_counted_on_purpose(self) -> None:
        self.assertEqual(docgate.table_rows("a | b\n--- | ---\n1 | 2\n"), 0)

    def test_tilde_and_backtick_fences_do_not_close_each_other(self) -> None:
        self.assertEqual(docgate.headings("```\n~~~\n# 가짜\n```\n# 진짜\n"), [(1, "진짜")])

    def test_unclosed_fence_swallows_the_rest(self) -> None:
        self.assertEqual(docgate.headings("```\n# 가짜\n"), [])

    def test_longer_closing_fence_closes(self) -> None:
        self.assertEqual(docgate.headings("```\n# 가짜\n````\n# 진짜\n"), [(1, "진짜")])


class TestLinkRules(GateCase):
    def test_empty_link_is_caught(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "()"),
                   eng=CLEAN_ENG.replace("(other.md)", "()"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any("빈 링크" in f.detail for f in report.findings))

    def test_absolute_target_is_rejected(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(/etc/passwd)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(/etc/passwd)"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any("절대 경로" in f.detail for f in report.findings))

    def test_drive_letter_target_is_rejected(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(C:/x.md)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(C:/x.md)"))
        self.assertFalse(self.run_gate().ok)

    def test_backslash_target_is_rejected(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(sub\\other.md)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(sub\\other.md)"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any("역슬래시" in f.detail for f in report.findings))

    def test_case_mismatch_fails_on_every_platform(self) -> None:
        # Windows 에서 통과하고 Linux 에서 깨지는 종류다. 여기서 잡는다.
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(Other.md)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(Other.md)"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any(f.check == "link" for f in report.findings))

    def test_escaping_the_repository_is_rejected(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(../../../../etc/passwd)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(../../../../etc/passwd)"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any("저장소 밖" in f.detail for f in report.findings))

    def test_experiments_exception_only_at_the_mirror_location(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(../../nowhere/experiments.md)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(../../nowhere/experiments.md)"))
        report = self.run_gate()
        self.assertFalse(report.ok, report.text())

    def test_reference_definition_is_resolved(self) -> None:
        self.build(kor=CLEAN_KOR.replace("[링크](other.md)", "[링크][표]\n\n[표]: gone.md"),
                   eng=CLEAN_ENG.replace("[link](other.md)", "[link][ref]\n\n[ref]: gone.md"))
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any("끊긴 링크" in f.detail for f in report.findings))

    def test_reference_usage_is_not_counted_on_purpose(self) -> None:
        self.assertEqual(docgate.local_links("[글][표]\n"), [])


class TestClaimScan(GateCase):
    def test_negated_and_longer_words_are_not_flagged(self) -> None:
        self.build(kor=CLEAN_KOR + "\n미해결이고 미완료다.\n",
                   eng=CLEAN_ENG + "\nUnresolved after install, incomplete.\n")
        report = self.run_gate()
        self.assertTrue(report.ok, report.text())
        self.assertEqual(report.claims, [])

    def test_real_claims_are_flagged(self) -> None:
        self.build(kor=CLEAN_KOR + "\n실측으로 전부 확인했다.\n",
                   eng=CLEAN_ENG + "\nWe measured all of it.\n")
        phrases = {phrase for _, _, phrase in self.run_gate().claims}
        self.assertEqual(phrases, {"실측", "전부", "measured", "all"})


class TestCommandLine(GateCase):
    def test_exit_zero_and_last_line_on_clean(self) -> None:
        self.build()
        self.assertEqual(docgate.main(["--root", str(self.root)]), 0)

    def test_exit_one_on_failure(self) -> None:
        self.build(eng=CLEAN_ENG.replace("## Section", "### Section"))
        self.assertEqual(docgate.main(["--root", str(self.root)]), 1)

    def test_missing_root_does_not_report_pass(self) -> None:
        # 판정할 수 없는 것을 통과로 적지 않는다.
        missing = self.root / "nowhere"
        self.assertEqual(docgate.main(["--root", str(missing)]), 2)

    def test_root_without_eng_tree_does_not_report_pass(self) -> None:
        self.build()
        shutil.rmtree(self.root / "docs" / "eng")
        with self.assertRaises(docgate.GateInputError):
            docgate.run(self.root)
        self.assertEqual(docgate.main(["--root", str(self.root)]), 2)

    def test_claims_mode_keeps_one_verdict_line(self) -> None:
        self.build(kor=CLEAN_KOR + "\n실측했다.\n", eng=CLEAN_ENG + "\nWe measured it.\n")
        lines = self.run_gate().text(show_claims=True).strip().splitlines()
        self.assertEqual(lines[-1], "VERDICT: pass")
        self.assertEqual(sum(1 for line in lines if line.startswith("VERDICT:")), 1)
        self.assertTrue(lines[0].startswith("SUMMARY: "))

    def test_findings_are_sorted(self) -> None:
        self.build(eng=CLEAN_ENG.replace("## Section", "### Section"))
        write(self.root, "docs/kor/zzz.md", "# 마지막\n")
        report = self.run_gate()
        self.assertEqual([f.key() for f in sorted(report.findings, key=docgate.Finding.key)],
                         sorted(f.key() for f in report.findings))


class TestBom(GateCase):
    def test_bom_prefixed_file_is_read_as_markdown(self) -> None:
        self.build()
        (self.root / "docs/kor/a.md").write_text("\ufeff" + CLEAN_KOR, encoding="utf-8")
        self.assertTrue(self.run_gate().ok, self.run_gate().text())


class TestPinnedEdges(GateCase):
    def test_numbered_heading_forms(self) -> None:
        self.assertEqual(docgate.headings("# 5. 절\n"), [(1, "5. 절")])
        self.assertEqual(docgate.headings("#5. 절\n"), [])
        self.assertEqual(docgate.headings("######5 깊은 것\n"), [])

    def test_claim_counted_once_per_line_per_phrase(self) -> None:
        self.build(kor=CLEAN_KOR + "\n실측 실측 실측.\n", eng=CLEAN_ENG)
        korean = [c for c in self.run_gate().claims if c[2] == "실측"]
        self.assertEqual(len(korean), 1)

    def test_blockquoted_fence_is_not_tracked_on_purpose(self) -> None:
        # 인용 안의 펜스는 펜스로 보지 않는다. 인용 안의 `> #` 도 헤딩이 아니므로 구멍은 없다.
        self.assertEqual(docgate.headings("> ```\n> # 인용\n> ```\n# 진짜\n"), [(1, "진짜")])


class TestInlineLinkScanner(GateCase):
    def test_angle_bracket_destination_with_space(self) -> None:
        self.assertEqual(docgate.inline_links("[글](<빈 칸.md>)"), ["빈 칸.md"])

    def test_nested_brackets_in_link_text(self) -> None:
        self.assertEqual(docgate.inline_links("[본 [초안]](경로.md)"), ["경로.md"])

    def test_title_after_destination(self) -> None:
        self.assertEqual(docgate.inline_links('[글](other.md "제목")'), ["other.md"])

    def test_image_and_multiple_links_on_one_line(self) -> None:
        self.assertEqual(docgate.inline_links("![그림](pic.png) 과 [글](a.md)"), ["pic.png", "a.md"])

    def test_escaped_bracket_is_not_a_link(self) -> None:
        self.assertEqual(docgate.inline_links("\\[링크 아님](x.md)"), [])

    def test_bracket_without_parenthesis_is_not_a_link(self) -> None:
        self.assertEqual(docgate.inline_links("[그냥 대괄호] 뒤 글"), [])

    def test_spaced_destination_is_checked_not_skipped(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(<없는 파일.md>)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(<없는 파일.md>)"))
        report = self.run_gate()
        self.assertFalse(report.ok, report.text())
        self.assertTrue(any(f.check == "link" for f in report.findings))

    def test_nested_bracket_link_is_checked_not_skipped(self) -> None:
        self.build(kor=CLEAN_KOR.replace("[링크](other.md)", "[본 [초안]](gone.md)"),
                   eng=CLEAN_ENG.replace("[link](other.md)", "[see [draft]](gone.md)"))
        report = self.run_gate()
        self.assertFalse(report.ok, report.text())

    def test_query_string_is_stripped_before_resolving(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(other.md?plain=1)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(other.md?plain=1)"))
        self.assertTrue(self.run_gate().ok, self.run_gate().text())

    def test_angle_bracket_destination_resolves_when_present(self) -> None:
        self.build(kor=CLEAN_KOR.replace("(other.md)", "(<other.md>)"),
                   eng=CLEAN_ENG.replace("(other.md)", "(<other.md>)"))
        self.assertTrue(self.run_gate().ok, self.run_gate().text())


class TestCodeSpans(GateCase):
    def test_link_inside_code_span_is_not_a_link(self) -> None:
        self.assertEqual(docgate.local_links("`[글](없는.md)` 는 예시다\n"), [])

    def test_real_link_next_to_a_code_span_is_kept(self) -> None:
        self.assertEqual(docgate.local_links("`[예시](x.md)` 와 [진짜](other.md)\n"),
                         [(1, "other.md")])

    def test_double_backtick_span_can_hold_a_backtick(self) -> None:
        self.assertEqual(docgate.local_links("``[글](`.md)`` 뒤\n"), [])

    def test_unclosed_backtick_is_not_a_span(self) -> None:
        self.assertEqual(docgate.local_links("` [글](other.md)\n"), [(1, "other.md")])

    def test_document_that_quotes_link_syntax_passes(self) -> None:
        # 이 저장소의 커밋 기록이 실제로 이렇게 쓴다. 정상 문서가 떨어지면 안 된다 (규칙 3).
        self.build(kor=CLEAN_KOR + "\n| 예 | `[글](<빈 칸.md>)` 가 빠진다 |\n",
                   eng=CLEAN_ENG + "\n| ex | `[x](<a file.md>)` is skipped |\n")
        self.assertTrue(self.run_gate().ok, self.run_gate().text())


class TestHtmlCommentsAndReferences(GateCase):
    def test_single_line_comment_hides_a_link(self) -> None:
        self.assertEqual(docgate.local_links("<!-- [초안](없는.md) -->\n"), [])

    def test_single_line_comment_hides_a_table_row(self) -> None:
        self.assertEqual(docgate.table_rows("<!-- | a | -->\n"), 0)

    def test_trailing_comment_keeps_the_heading(self) -> None:
        self.assertEqual(docgate.headings("# 진짜 <!-- 메모 -->\n"), [(1, "진짜")])

    def test_comment_opened_and_closed_across_lines(self) -> None:
        text = "# 진짜\n\n<!-- 시작\n[x](없는.md)\n끝 --> [진짜링크](other.md)\n"
        self.assertEqual(docgate.headings(text), [(1, "진짜")])
        self.assertEqual(docgate.local_links(text), [(5, "other.md")])

    def test_commented_out_document_passes(self) -> None:
        self.build(kor=CLEAN_KOR + "\n<!-- [초안](없는.md) -->\n",
                   eng=CLEAN_ENG + "\n<!-- [draft](missing.md) -->\n")
        self.assertTrue(self.run_gate().ok, self.run_gate().text())

    def test_reference_definition_with_spaced_angle_destination(self) -> None:
        self.assertEqual(docgate.local_links("[표]: <빈 칸.md>\n"), [(1, "빈 칸.md")])

    def test_spaced_reference_definition_is_checked(self) -> None:
        self.build(kor=CLEAN_KOR.replace("[링크](other.md)", "[링크][표]\n\n[표]: <없는 파일.md>"),
                   eng=CLEAN_ENG.replace("[link](other.md)", "[link][ref]\n\n[ref]: <a missing file.md>"))
        report = self.run_gate()
        self.assertFalse(report.ok, report.text())
        self.assertTrue(any(f.check == "link" for f in report.findings))

    def test_reference_definition_with_title_after_destination(self) -> None:
        self.assertEqual(docgate.local_links('[표]: other.md "제목"\n'), [(1, "other.md")])


class TestCodeBlockCounting(GateCase):
    def test_fence_inside_html_comment_is_not_counted(self) -> None:
        self.assertEqual(docgate.code_blocks("<!--\n```\nx\n```\n-->\n"), 0)

    def test_fence_inside_front_matter_is_not_counted(self) -> None:
        self.assertEqual(docgate.code_blocks("---\n```\n---\n# 진짜\n"), 0)

    def test_commented_out_fence_on_one_side_still_passes(self) -> None:
        self.build(kor=CLEAN_KOR + "\n<!--\n```\n옛 예시\n```\n-->\n", eng=CLEAN_ENG)
        self.assertTrue(self.run_gate().ok, self.run_gate().text())

    def test_real_fence_difference_still_fails(self) -> None:
        self.build(kor=CLEAN_KOR + "\n```\n추가\n```\n", eng=CLEAN_ENG)
        self.assertFalse(self.run_gate().ok)

    def test_plain_fence_count(self) -> None:
        self.assertEqual(docgate.code_blocks("```\na\n```\n\n~~~\nb\n~~~\n"), 2)


class TestCommentMarkerInCodeSpan(GateCase):
    def test_literal_comment_marker_does_not_hide_the_rest(self) -> None:
        text = "`<!--` 는 주석 시작이다\n\n# 진짜\n\n[글](없는.md)\n"
        self.assertEqual(docgate.headings(text), [(1, "진짜")])
        self.assertEqual(docgate.local_links(text), [(5, "없는.md")])

    def test_literal_comment_marker_does_not_hide_a_broken_link(self) -> None:
        self.build(kor=CLEAN_KOR + "\n`<!--` 설명\n\n[끊김](gone.md)\n",
                   eng=CLEAN_ENG + "\n`<!--` note\n\n[broken](gone.md)\n")
        report = self.run_gate()
        self.assertFalse(report.ok, report.text())
        self.assertTrue(any(f.check == "link" for f in report.findings))

    def test_real_comment_still_hides(self) -> None:
        self.assertEqual(docgate.local_links("<!-- [x](gone.md) -->\n"), [])

    def test_close_marker_in_code_span_does_not_end_a_comment(self) -> None:
        # 주석 안에서는 백틱이 글자다. `-->` 가 코드 스팬 안이어도 주석은 거기서 끝난다.
        self.assertEqual(docgate.headings("<!--\n`-->`\n# 진짜\n"), [(1, "진짜")])

    def test_code_span_with_comment_pair_inside(self) -> None:
        self.assertEqual(docgate.headings("`<!-- x -->` 뒤\n# 진짜\n"), [(1, "진짜")])


class TestImages(GateCase):
    def test_existing_image_passes(self) -> None:
        self.build(kor=CLEAN_KOR + "\n![그림](pic.png)\n", eng=CLEAN_ENG + "\n![img](pic.png)\n")
        (self.root / "docs/kor/pic.png").write_bytes(b"x")
        (self.root / "docs/eng/pic.png").write_bytes(b"x")
        self.assertTrue(self.run_gate().ok, self.run_gate().text())

    def test_missing_image_is_caught(self) -> None:
        self.build(kor=CLEAN_KOR + "\n![그림](gone.png)\n", eng=CLEAN_ENG + "\n![img](gone.png)\n")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any(f.check == "link" for f in report.findings))

    def test_image_on_one_side_only_is_a_parity_finding(self) -> None:
        self.build(kor=CLEAN_KOR + "\n![그림](pic.png)\n", eng=CLEAN_ENG)
        (self.root / "docs/kor/pic.png").write_bytes(b"x")
        report = self.run_gate()
        self.assertFalse(report.ok)
        self.assertTrue(any(f.check == "parity" for f in report.findings))


class TestIndentedCodeStart(GateCase):
    def test_indented_code_right_after_a_heading(self) -> None:
        text = "# 제목\n    [x](없는.md)\n    | t |\n"
        self.assertEqual(docgate.local_links(text), [])
        self.assertEqual(docgate.table_rows(text), 0)
        self.assertEqual(docgate.headings(text), [(1, "제목")])

    def test_indented_code_right_after_a_fence(self) -> None:
        text = "```\nx\n```\n    # 가짜\n    [x](없는.md)\n"
        self.assertEqual(docgate.headings(text), [])
        self.assertEqual(docgate.local_links(text), [])

    def test_indented_line_inside_a_paragraph_is_not_code(self) -> None:
        # 들여쓴 코드는 문단을 끊지 못한다 (CommonMark).
        self.assertEqual(docgate.local_links("본문 이어짐\n    [x](other.md)\n"), [(2, "other.md")])

    def test_indented_sample_after_a_heading_does_not_fail_a_mirror(self) -> None:
        self.build(kor=CLEAN_KOR + "\n## 예시\n    [끊김](없는.md)\n",
                   eng=CLEAN_ENG + "\n## Example\n    [broken](gone.md)\n")
        self.assertTrue(self.run_gate().ok, self.run_gate().text())


if __name__ == "__main__":
    unittest.main()

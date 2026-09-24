#!/usr/bin/env python3
"""Lint Loom RFC files (docs/rfcs/NNNN-*.md).

Enforces the contract described in .claude/skills/rfc/SKILL.md:
  - filename number matches the ``rfc:`` frontmatter field
  - status is a recognised stage
  - required frontmatter fields exist
  - stage-specific required sections / tables are present
  - no placeholder (TBD/FIXME) survives past ``implementable``
  - RFC numbers are unique and the index in docs/README.md is current

Exit code is 0 when every RFC passes, 1 otherwise. No third-party deps.
"""

from __future__ import annotations

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
RFC_DIR = ROOT / "docs" / "rfcs"
README = ROOT / "docs" / "README.md"

VALID_STATUS = {
    "provisional",
    "accepted",
    "implementable",
    "implemented",
    "deferred",
    "rejected",
    "withdrawn",
    "replaced",
}

FRONTMATTER_REQUIRED = ("title", "status", "owners", "created")
REQUIRED_SECTIONS = (
    "Summary",
    "Motivation",
    "Goals",
    "Non-Goals",
    "Proposal",
    "Alternatives",
)
# Some required concepts use established synonymous headings.
SECTION_ALIASES = {
    "Proposal": {"Proposal", "Target architecture", "Design overview"},
    "Alternatives": {"Alternatives", "Alternatives considered"},
}
IMPLEMENTABLE_EXTRA = (
    "Phases and graduation criteria",
    "Production Readiness Review",
    "Rollout and rollback",
    "Testing and verification plan",
    "Implementation History",
)

FNAME_RE = re.compile(r"^(\d{4})-[a-z0-9][a-z0-9-]*\.md$")
FM_RE = re.compile(r"^---\n(.*?)\n---\n", re.S)
FIELD_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_-]*):\s*(.*)$")
HEADING_RE = re.compile(r"^#{1,3}\s+(.*?)\s*$", re.M)


def parse_frontmatter(text: str) -> dict[str, str]:
    m = FM_RE.match(text)
    if not m:
        return {}
    fields: dict[str, str] = {}
    for line in m.group(1).splitlines():
        fm = FIELD_RE.match(line)
        if fm:
            fields[fm.group(1)] = fm.group(2).strip()
    return fields


def headings(text: str) -> set[str]:
    out: set[str] = set()
    for m in HEADING_RE.finditer(text):
        h = m.group(1).strip()
        h = re.sub(r"^\d+(?:\.\d+)*[.)]?\s+", "", h)  # strip "1. " / "2.3 "
        out.add(h)
    return out


def has_section(hs: set[str], name: str) -> bool:
    allowed = {name} | SECTION_ALIASES.get(name, set())
    return bool(allowed & hs)


def lint_file(path: pathlib.Path) -> list[str]:
    errs: list[str] = []
    name = path.name
    m = FNAME_RE.match(name)
    if not m:
        return [f"filename must be NNNN-kebab-slug.md, got {name!r}"]
    num = int(m.group(1))

    text = path.read_text(encoding="utf-8")
    fm = parse_frontmatter(text)
    if not fm:
        return ["missing YAML frontmatter delimiters (--- … ---)"]

    for field in FRONTMATTER_REQUIRED:
        if not fm.get(field):
            errs.append(f"missing frontmatter field: {field}")

    if fm.get("rfc"):
        rfc = fm["rfc"].strip()
        if not rfc.isdigit():
            errs.append(f"frontmatter 'rfc' must be digits, got {rfc!r}")
        elif int(rfc) != num:
            errs.append(f"rfc: {rfc} disagrees with filename number {num:04d}")
    else:
        errs.append("missing frontmatter field: rfc")

    status = fm.get("status", "").strip().lower()
    if status not in VALID_STATUS:
        errs.append(f"status {status!r} not one of {sorted(VALID_STATUS)}")
        return errs  # section checks depend on a valid stage

    hs = headings(text)
    for section in REQUIRED_SECTIONS:
        if not has_section(hs, section):
            errs.append(f"missing required section: ## {section}")

    if status in {"accepted", "implementable", "implemented"}:
        if "Evidence" not in hs and "evidence" not in text.lower():
            errs.append("accepted+ RFCs need an ## Evidence block with measurements")

    if status in {"implementable", "implemented"}:
        for section in IMPLEMENTABLE_EXTRA:
            if not has_section(hs, section):
                errs.append(f"{status} RFC missing section: ## {section}")
        if "| Phase |" not in text:
            errs.append("implementable RFC must contain the phase table header '| Phase |'")
        if "issue #" not in text and not fm.get("tracking"):
            errs.append("implementable RFC needs a tracking issue (frontmatter or text)")

    if status in {"implementable", "implemented"}:
        placeholders = [w for w in ("TBD", "FIXME", "XXX", "<!-- TODO") if w in text]
        if placeholders:
            errs.append(f"unresolved placeholder(s) at status={status}: {sorted(set(placeholders))}")

    if status == "replaced" and not fm.get("superseded-by"):
        errs.append("status 'replaced' requires frontmatter 'superseded-by: NNNN'")
    if status in {"deferred", "rejected", "withdrawn"}:
        if "## Rationale" not in hs and "rationale" not in text.lower():
            errs.append(f"status {status!r} requires a Rationale section/statement")

    return errs


def main() -> int:
    if not RFC_DIR.is_dir():
        print(f"rfc_lint: {RFC_DIR} not found", file=sys.stderr)
        return 1

    # RFCs are top-level NNNN-*.md; RFC design attachments live in
    # attachments/ and are deliberately not linted as RFCs.
    files = sorted(RFC_DIR.glob("*.md"))
    if not files:
        print("rfc_lint: no RFCs found")
        return 0

    all_errs: list[str] = []
    seen_numbers: dict[int, pathlib.Path] = {}
    for path in files:
        for e in lint_file(path):
            all_errs.append(f"{path.relative_to(ROOT)}: {e}")
        m = FNAME_RE.match(path.name)
        if m:
            num = int(m.group(1))
            if num in seen_numbers:
                all_errs.append(
                    f"duplicate RFC number {num:04d}: "
                    f"{seen_numbers[num].name} and {path.name}"
                )
            seen_numbers[num] = path

    index_missing: list[str] = []
    if README.exists():
        readme = README.read_text(encoding="utf-8")
        for path in files:
            if path.name not in readme:
                index_missing.append(path.name)
        if index_missing:
            all_errs.append(
                "docs/README.md RFC index missing: " + ", ".join(index_missing)
            )

    if all_errs:
        print("RFC lint FAILED:")
        for e in all_errs:
            print(f"  - {e}")
        return 1

    print(f"RFC lint OK: {len(files)} file(s), {len(seen_numbers)} unique number(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

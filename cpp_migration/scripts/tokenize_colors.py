#!/usr/bin/env python3
"""tokenize_colors.py — Hard-coded color → UI20 design token replacement scanner.

Scans cpp_migration/src/**/*.cppm for hard-coded RGB/ANSI/hex colors, finds the
nearest UI20 Palette token by Euclidean distance, and produces:

  build/tokens_scan_summary.csv   — file, line_no, original, token, distance
  build/tokens_autopatch.diff     — suggested git-apply diff (dry-run default)
  build/tokens_unmapped.txt       — colors outside threshold (human review)

Usage:  python3 tokenize_colors.py [--root ../src] [--threshold 8] [--apply]
Python 3.10+ stdlib only.
"""

from __future__ import annotations
import argparse, csv, math, re, sys
from dataclasses import dataclass
from difflib import unified_diff
from pathlib import Path
from typing import Optional

# --- 1. UI20 Palette tokens (copied verbatim from design_tokens.cppm) --------

_D = {  # dark palette
    "primary":(215,119,87), "primary_shimmer":(232,164,134),
    "info":(110,151,255), "success":(78,186,101), "warning":(255,193,7),
    "danger":(255,107,128), "muted":(153,153,153), "subtle":(87,87,87),
    "suggestion":(173,216,255), "text":(230,237,243), "inverse_text":(32,33,36),
    "background":(32,33,36), "chrome":(55,57,61),
    "rate_limit_fill":(80,140,255), "rate_limit_empty":(60,60,80),
    "brief_label":(215,119,87), "diff_added":(46,160,67),
    "diff_removed":(248,81,73), "diff_added_word":(63,185,80),
    "diff_removed_word":(250,109,93), "merged":(163,113,247),
    "rainbow0":(255,99,132), "rainbow1":(255,159,64), "rainbow2":(255,205,86),
    "rainbow3":(75,192,192), "rainbow4":(54,162,235), "rainbow5":(153,102,255),
    "rainbow6":(201,203,207), "rainbow_shimmer":(230,230,250),
}

PALETTES: dict[str, dict[str, tuple[int,int,int]]] = {
    "dark": dict(_D),
    "light": dict(_D, info=(53,113,232), success=(40,150,70),
        warning=(200,140,10), danger=(210,60,80), muted=(120,120,120),
        subtle=(190,190,190), suggestion=(30,90,180), text=(32,33,36),
        inverse_text=(245,245,245), background=(245,245,245),
        chrome=(220,220,220), rate_limit_empty=(225,230,240),
        diff_added=(38,130,60), diff_removed=(210,50,45),
        diff_added_word=(50,160,75), diff_removed_word=(230,80,70),
        merged=(120,60,210)),
    "dark_daltonized": dict(_D, primary=(198,128,80),
        primary_shimmer=(225,170,130), info=(100,140,230),
        success=(90,150,200), warning=(210,170,40), danger=(200,120,140),
        diff_added=(90,130,180), diff_removed=(180,110,120),
        diff_added_word=(110,160,200), diff_removed_word=(200,130,140),
        merged=(150,120,220)),
    "light_daltonized": dict(_D, success=(60,120,170),
        warning=(180,140,20), danger=(180,90,110),
        diff_added=(70,120,170), diff_removed=(170,80,95),
        diff_added_word=(90,140,190), diff_removed_word=(190,100,115),
        merged=(120,100,200)),
    "monochrome": {k:(128,128,128) for k in _D},
}
PALETTES["monochrome"].update(text=(255,255,255), primary=(255,255,255),
    background=(0,0,0), inverse_text=(0,0,0), muted=(100,100,100),
    subtle=(60,60,60), chrome=(60,60,60))

ALL: list[tuple[str,str,tuple[int,int,int]]] = [
    (pn, tn, c) for pn, p in PALETTES.items() for tn, c in p.items()]

# Semantic aliases used by resolve_color() string form
_ALIASES = {"primary":"primary","info":"info","success":"success",
    "warning":"warning","danger":"danger","muted":"muted","subtle":"subtle",
    "suggestion":"suggestion","text":"text","background":"background",
    "chrome":"chrome","inverse_text":"inverseText","diff_added":"diffAdded",
    "diff_removed":"diffRemoved","merged":"merged","brief_label":"claude",
    "primary_shimmer":"claudeShimmer"}

# --- 2. Regex patterns + parsers --------------------------------------------

def _pal256(m: re.Match) -> tuple[int,int,int]:
    n = int(m.group("n")) & 0xFF
    if n < 16:
        b = [(0,0,0),(128,0,0),(0,128,0),(128,128,0),(0,0,128),(128,0,128),
             (0,128,128),(192,192,192),(128,128,128),(255,0,0),(0,255,0),
             (255,255,0),(0,0,255),(255,0,255),(0,255,255),(255,255,255)]
        return b[n]
    if n < 232:
        i = n - 16
        r,g,b = i//36,(i//6)%6,i%6
        return tuple(0 if x==0 else 55+40*x for x in (r,g,b))
    g = 8+(n-232)*10; return (g,g,g)

PATTERNS = [  # (name, regex, parser)
    ("RGB(call)", re.compile(
        r"(?P<prefix>(?:ftxui::)?Color::RGB\s*\(\s*)"
        r"(?P<r>\d+)\s*,\s*(?P<g>\d+)\s*,\s*(?P<b>\d+)"
        r"(?P<suffix>\s*\))"),
     lambda m: (int(m["r"])&0xFF, int(m["g"])&0xFF, int(m["b"])&0xFF)),
    ("Palette256", re.compile(
        r"(?P<prefix>(?:ftxui::)?Color::Palette256\s*\(\s*)"
        r"(?P<n>\d+)(?P<suffix>\s*\))"), _pal256),
    ("hex_string", re.compile(
        r'(?P<prefix>"#)(?P<hex>[0-9a-fA-F]{6})(?P<suffix>")'),
     lambda m: (int(m["hex"][0:2],16), int(m["hex"][2:4],16),
                int(m["hex"][4:6],16))),
    ("rgb_string", re.compile(
        r'(?P<prefix>"rgb\s*\(\s*)'
        r'(?P<r>\d+)\s*,\s*(?P<g>\d+)\s*,\s*(?P<b>\d+)'
        r'(?P<suffix>\s*\)")'),
     lambda m: (int(m["r"])&0xFF, int(m["g"])&0xFF, int(m["b"])&0xFF)),
]

# --- 3. Match record + nearest-token search ---------------------------------

@dataclass
class Rec:
    file: Path; line_no: int; col: int; form: str
    original_text: str; original_rgb: tuple[int,int,int]
    token_name: str; palette: str; distance: float
    replacement: Optional[str]

def _dist(a, b):
    return math.sqrt((a[0]-b[0])**2+(a[1]-b[1])**2+(a[2]-b[2])**2)

def nearest(rgb: tuple[int,int,int]
            ) -> tuple[str, str, float]:
    bt, bp, bd = None, "dark", float("inf")
    for pn, tn, c in ALL:
        d = _dist(rgb, c)
        if d < bd: bd, bt, bp = d, tn, pn
    return (bt or "unknown"), bp, bd

def replacement(form: str, tok: str) -> Optional[str]:
    if form in ("RGB(call)", "Palette256"):
        return f"palette.{tok}"
    if form in ("hex_string", "rgb_string"):
        return f'"{_ALIASES.get(tok, tok)}"'
    return None

# --- 4. Scanner --------------------------------------------------------------

def scan(path: Path, threshold: float) -> list[Rec]:
    out: list[Rec] = []
    try: lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        lines = path.read_text(encoding="latin-1", errors="replace").splitlines()
    for ln, raw in enumerate(lines, 1):
        # Strip // comments before scanning (heuristic)
        slash = raw.find("//")
        region = raw[:slash] if slash >= 0 else raw
        for form, pat, parser in PATTERNS:
            for m in pat.finditer(region):
                try: rgb = parser(m)
                except Exception: continue
                tok, pal, d = nearest(rgb)
                mapped = d <= threshold
                repl = replacement(form, tok) if mapped else None
                out.append(Rec(path, ln, m.start(), form, m.group(0), rgb,
                    tok if mapped else f"[unmapped] {tok}", pal, d, repl))
    return out

# --- 5. Report writers -------------------------------------------------------

def write_csv(recs: list[Rec], p: Path) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["file","line","col","form","original","rgb",
                    "token","palette","distance","replacement"])
        for r in recs:
            w.writerow([r.file, r.line_no, r.col, r.form, r.original_text,
                        f"rgb{r.original_rgb}", r.token_name, r.palette,
                        f"{r.distance:.2f}", r.replacement or ""])

def write_unmapped(recs: list[Rec], p: Path) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    u: dict[tuple[int,int,int], set[str]] = {}
    for r in recs:
        if r.replacement is None:
            u.setdefault(r.original_rgb, set()).add(r.original_text)
    items = []
    for rgb, occ in u.items():
        _, _, d = nearest(rgb); tn, _, _ = nearest(rgb)
        items.append((d, rgb, tn, occ))
    items.sort(reverse=True)
    with p.open("w", encoding="utf-8") as f:
        f.write("# Colors outside threshold — human review\n")
        f.write(f"# {'RGB':<14} {'Nearest':<22} {'Dist':>6}  Samples\n")
        for d, rgb, tn, occ in items:
            f.write(f"  rgb{rgb}  {tn:<20}  {d:5.1f}  "
                    f"{', '.join(sorted(occ)[:3])}\n")

def write_diff(recs: list[Rec], p: Path, root: Path, apply_: bool) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    by: dict[Path, list[Rec]] = {}
    for r in recs:
        if r.replacement: by.setdefault(r.file, []).append(r)
    diff_lines: list[str] = []
    for fp, rs in sorted(by.items()):
        try: orig = fp.read_text(encoding="utf-8").splitlines(keepends=True)
        except UnicodeDecodeError: continue
        new = list(orig)
        for r in sorted(rs, key=lambda x: (x.line_no, -x.col)):
            i = r.line_no - 1
            if i >= len(new): continue
            pos = new[i].find(r.original_text, r.col) if r.col < len(new[i]) else -1
            if pos < 0: pos = new[i].find(r.original_text)
            if pos >= 0:
                new[i] = new[i][:pos] + r.replacement + new[i][pos+len(r.original_text):]
        try:
            # Paths relative to cpp_migration/ (src/commands/theme.cppm …) so the
            # diff is directly `git apply`-able from cpp_migration/.
            rel = str(fp.relative_to(root.parent))
        except ValueError:
            rel = str(fp)
        diff_lines.extend(unified_diff(orig, new, fromfile=rel, tofile=rel, n=2))
        if apply_: fp.write_text("".join(new), encoding="utf-8")
    with p.open("w", encoding="utf-8") as f:
        f.writelines(diff_lines)
    if not diff_lines:
        p.write_text("# (no mappable replacements within threshold)\n",
                     encoding="utf-8")

# --- 6. CLI ------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    here = Path(__file__).resolve().parent
    ap.add_argument("--root", type=Path, default=here.parent/"src")
    ap.add_argument("--tokens", type=Path, default=None)
    ap.add_argument("--threshold", type=float, default=8.0)
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--build-dir", type=Path, default=here.parent/"build")
    args = ap.parse_args()

    if not args.root.exists():
        print(f"ERROR: root {args.root} missing", file=sys.stderr); return 2
    files = sorted(args.root.rglob("*.cppm"))
    print(f"Scanning {len(files)} .cppm under {args.root} "
          f"(threshold={args.threshold:.1f}, apply={args.apply})…")

    all_r: list[Rec] = []
    for fp in files:
        all_r.extend(scan(fp, args.threshold))

    mp = sum(1 for r in all_r if r.replacement)
    um = len(all_r) - mp
    print(f"  {len(all_r)} refs: {mp} mapped, {um} unmapped.")

    args.build_dir.mkdir(parents=True, exist_ok=True)
    write_csv(all_r,       args.build_dir/"tokens_scan_summary.csv")
    write_diff(all_r,      args.build_dir/"tokens_autopatch.diff",
               args.root, args.apply)
    write_unmapped(all_r,  args.build_dir/"tokens_unmapped.txt")

    tag = " (APPLIED!)" if args.apply else " (dry-run, add --apply)"
    print(f"  CSV  → {args.build_dir/'tokens_scan_summary.csv'}")
    print(f"  Diff → {args.build_dir/'tokens_autopatch.diff'}{tag}")
    print(f"  UM   → {args.build_dir/'tokens_unmapped.txt'}")
    return 0

if __name__ == "__main__":
    sys.exit(main())

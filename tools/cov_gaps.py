#!/usr/bin/env python3
"""Cobertura (OpenCppCoverage export) -> per-file line coverage for project src/.

Self-contained: stdlib only (xml.etree / json / argparse). Given a cobertura XML
as produced by `OpenCppCoverage --export_type cobertura:... --sources <repo>\\src
--excluded_sources <repo>\\src\\gui|probes|cli`, it computes:
  - overall line coverage across project src/ files (gui/probes/cli excluded
    defensively here as well),
  - per-file covered/total rate and the uncovered line numbers,
writes the result as JSON and prints a summary.

Usage:
  python tools/cov_gaps.py [--xml integration-results/coverage.xml]
                           [--out integration-results/cov-gaps.json]
                           [--threshold 96.1]

Exit codes: 0 = pass (or no threshold given), 1 = coverage below threshold,
2 = bad input (missing/unparsable XML, no tracked files).
"""
import argparse
import json
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SRC = REPO / "src"
EXCLUDED_TOP = {"gui", "probes", "cli"}  # dirs under src/ not counted
ABS_PATH = re.compile(r"^([A-Za-z]:/|/)")  # "C:/..." or posix-absolute


def _norm(p):
    return p.replace("\\", "/")


def project_rel(filename, sources):
    """Map a cobertura <class filename> to a path relative to src/, or None.

    OpenCppCoverage writes <source>C:</source> plus drive-relative filenames
    like "tjf\\github\\phantom-bt\\src\\ble\\aes.cpp"; some versions emit full
    paths. Normalize everything to forward slashes and try every base.

    Two defensive rules (both reproduced by check-tools G8b fixtures):
    - An ABSOLUTE filename is never glued onto a base (gluing an SDK path like
      "C:/Program Files/..." onto the src/ root yielded "src/C:/Program
      Files/..." and counted foreign files).
    - The project src/ root is anchored at its LAST occurrence in the
      candidate: gluing a src-rooted <source> onto a drive-rel filename embeds
      a second repo path, and that embedded copy lacks the drive letter, so
      both the absolute form ("C:/.../src/") and the repo-relative tail
      ("/.../src/") are tried, last match wins (the tail-anchored src/ is the
      project one).
    """
    fn = _norm(filename).strip("/")
    if ABS_PATH.match(fn):
        candidates = [fn]
    else:
        bases = [_norm(s).rstrip("/") + "/" for s in sources]
        bases += [_norm(str(REPO)) + "/", _norm(str(SRC)) + "/"]
        candidates = [b + fn for b in bases]
    src_abs = _norm(str(SRC)).lower() + "/"
    _drive, _sep, tail = src_abs.partition(":/")
    src_tail = "/" + tail if tail else src_abs  # drive letter dropped
    for cand in candidates:
        low = cand.lower()
        anchors = [(low.rfind(src_abs), len(src_abs)),
                   (low.rfind(src_tail), len(src_tail))]
        i, n = max(anchors)  # last occurrence wins (see docstring)
        if i >= 0:
            rel = cand[i + n:]
            top = rel.split("/")[0]
            return None if top in EXCLUDED_TOP else rel
    return None  # outside project src (stl/sdk headers, excluded dirs, ...)


def parse(xml_path):
    """Return (files, header) where files maps rel-path -> {lines: {no: hits}}."""
    tree = ET.parse(xml_path)  # ET.ParseError propagates as input error
    root = tree.getroot()
    sources = [s.text or "" for s in root.findall("./sources/source")]
    files = {}
    for cls in root.iter("class"):
        rel = project_rel(cls.get("filename", ""), sources)
        if rel is None:
            continue
        lines = files.setdefault(rel, {})
        for ln in cls.findall("./lines/line"):
            try:
                no, hits = int(ln.get("number")), int(ln.get("hits", 0))
            except (TypeError, ValueError):
                continue
            lines[no] = max(lines.get(no, 0), hits)  # branch dup lines
    header = {
        "line_rate": float(root.get("line-rate", 0)),
        "lines_covered": int(root.get("lines-covered", 0)),
        "lines_valid": int(root.get("lines-valid", 0)),
    }
    return files, header


def summarize(files):
    out = []
    for rel, lines in sorted(files.items()):
        total = len(lines)
        covered = sum(1 for h in lines.values() if h > 0)
        out.append({
            "file": "src/" + rel,
            "total": total,
            "covered": covered,
            "rate": round(covered / total, 4) if total else 1.0,
            "uncovered": sorted(n for n, h in lines.items() if h <= 0),
        })
    out.sort(key=lambda f: (f["rate"], f["file"]))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--xml", default=str(REPO / "integration-results/coverage.xml"),
                    help="cobertura XML from OpenCppCoverage")
    ap.add_argument("--out", default=str(REPO / "integration-results/cov-gaps.json"),
                    help="JSON report to write")
    ap.add_argument("--threshold", type=float, default=None,
                    help="overall line-coverage %% gate; exit 1 if below")
    args = ap.parse_args()

    xml_path = Path(args.xml)
    if not xml_path.is_file():
        print(f"error: cobertura XML not found: {xml_path}", file=sys.stderr)
        return 2
    try:
        files, header = parse(xml_path)
    except ET.ParseError as e:
        print(f"error: cannot parse {xml_path}: {e}", file=sys.stderr)
        return 2

    per_file = summarize(files)
    total = sum(f["total"] for f in per_file)
    covered = sum(f["covered"] for f in per_file)
    if total == 0:
        print("error: no project src/ files found in report "
              "(wrong --sources/--excluded_sources?)", file=sys.stderr)
        return 2
    pct = 100.0 * covered / total
    hdr_pct = 100.0 * header["line_rate"]
    hdr_txt = (f"{hdr_pct:.2f}% ({header['lines_covered']}/{header['lines_valid']})"
               if header["lines_valid"] else "n/a")

    report = {
        "overall": round(pct / 100, 4),
        "pct": round(pct, 2),
        "total_lines": total,
        "covered_lines": covered,
        "file_count": len(per_file),
        "files": per_file,
    }
    if args.threshold is not None:
        report["threshold_pct"] = args.threshold
        report["pass"] = pct >= args.threshold
    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    print(f"src line coverage: {pct:.2f}% ({covered}/{total}) "
          f"across {len(per_file)} files  [cobertura header: {hdr_txt}]")
    worst = [f for f in per_file if f["rate"] < 1.0]
    print(f"{len(worst)} file(s) below 100%; worst first:")
    for f in worst[:10]:
        print(f"  {f['file']:<40} {100*f['rate']:6.2f}%  "
              f"{f['covered']}/{f['total']}  gaps: {len(f['uncovered'])}")
    if len(worst) > 10:
        print(f"  ... and {len(worst) - 10} more (see {out_path})")

    if args.threshold is None:
        print(f"report written: {out_path}")
        return 0
    if report["pass"]:
        print(f"GATE: PASS ({pct:.2f}% >= {args.threshold}%) -> {out_path}")
        return 0
    print(f"GATE: FAIL ({pct:.2f}% < {args.threshold}%)")
    print("files below threshold:")
    for f in per_file:
        if 100 * f["rate"] < args.threshold:
            gaps = ",".join(map(str, f["uncovered"][:20]))
            more = "" if len(f["uncovered"]) <= 20 else ",..."
            print(f"  {f['file']:<40} {100*f['rate']:6.2f}%  "
                  f"uncovered: {gaps}{more}")
    print(f"report written: {out_path}")
    return 1


if __name__ == "__main__":
    sys.exit(main())

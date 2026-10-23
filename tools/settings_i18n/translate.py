#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
# SPDX-License-Identifier: BSL-1.0
"""Build-time English settings-page generator.

The Japanese pages remain the upstream-maintained sources. The Gemini product
build runs this deterministic transformer and embeds/serves the result. The
catalog is checked in so a firmware build needs no network access.

Usage:
  translate.py CATALOG INPUT OUTPUT [... INPUT OUTPUT ...]
  translate.py --check CATALOG FILE [...]

If optional overrides.json and polish.json files sit next to CATALOG, the
overrides replace generated translations and the polish entries run as a
second, exact-string copyediting pass over the resulting English. All files are
checked in; builds never call a translation service.

A source string that is still Japanese after all catalog replacements is a
hard error: adding a new Japanese UI string upstream must also add its English
translation here rather than silently shipping a mixed-language page.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from urllib.parse import quote

JA = re.compile(
    r"[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\uff66-\uff9f]"
)

# Google Cloud icon geometry from the official multicolour cloud mark in
# https://commons.wikimedia.org/wiki/File:Google_Cloud_logo.svg. Keeping the
# vector inline makes the firmware-served SoftAP page fully offline while
# adding only a few kilobytes to the already-injected HTML.
GOOGLE_CLOUD_ICON_SVG = (
    '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 39 28">'
    '<path fill="#ea4335" d="M21.85,7.41l1,0,2.85-2.85.14-1.21A12.81,12.81,0,0,0,5,9.6a1.55,1.55,0,0,1,1-.06l5.7-.94s.29-.48.44-.45a7.11,7.11,0,0,1,9.73-.74Z"/>'
    '<path fill="#4285f4" d="M29.76,9.6a12.84,12.84,0,0,0-3.87-6.24l-4,4A7.11,7.11,0,0,1,24.5,13v.71a3.56,3.56,0,1,1,0,7.12H17.38l-.71.72v4.27l.71.71H24.5A9.26,9.26,0,0,0,29.76,9.6Z"/>'
    '<path fill="#34a853" d="M10.25,26.49h7.12v-5.7H10.25a3.54,3.54,0,0,1-1.47-.32l-1,.31L4.91,23.63l-.25,1A9.21,9.21,0,0,0,10.25,26.49Z"/>'
    '<path fill="#fbbc05" d="M10.25,8A9.26,9.26,0,0,0,4.66,24.6l4.13-4.13a3.56,3.56,0,1,1,4.71-4.71l4.13-4.13A9.25,9.25,0,0,0,10.25,8Z"/>'
    '</svg>'
)
GOOGLE_CLOUD_FAVICON = 'data:image/svg+xml,' + quote(GOOGLE_CLOUD_ICON_SVG, safe='')
GOOGLE_CLOUD_HEADER_ICON = GOOGLE_CLOUD_ICON_SVG.replace(
    '<svg ', '<svg width="34" height="24" style="flex:none" aria-hidden="true" focusable="false" ', 1
)
GOOGLE_CLOUD_TITLE = 'Google Cloud StackChan Settings'

# Copyediting regressions that must not reach either the firmware page or the
# hosted BLE page. These are deliberately concrete: the translation catalog is
# large and exact-string replacements make broad grammar checks unreliable.
BAD_ENGLISH = re.compile(
    "|".join(
        re.escape(phrase)
        for phrase in (
            "Stuck-chan",
            "Stack Chan",
            "save & restart",
            "save and restart",
            "acquisition failure",
            "retrieval failure",
            "old farm",
            "new farm",
            "aircraft",
            "farm",
            "unloaded",
            "Unregistered",
            "transmission",
            "Transmission",
            "acquisition",
            "Acquisition",
            "retrieval",
            "Retrieval",
            "reflection",
            "Obtained",
            "Obtaining",
            "Retrieved",
            "HTTP connecting",
            "Wi-Fi: Connecting",
            "Immediate reflection",
            "saved/unloaded",
            "Saved/Not loaded",
            "Farm Defaults",
            "Farm default",
            "ms/times",
            "Status Read failure",
            "conversation function",
            "because it is exclusive",
            "Apply completed",
            "Loaded —",
            "Not applicable",
            "to will change",
            "When configured, When configured,",
            ".。",
            "※",
            "Jyugobyo",
            "Ojikan",
            "Yahoo",
        )
    )
)


def read_catalog(path: Path) -> dict[str, str]:
    raw = json.loads(path.read_text(encoding="utf-8"))
    entries = raw.get("replacements", raw)
    if not isinstance(entries, dict):
        raise SystemExit(f"{path}: expected a JSON object of Japanese -> English strings")
    result: dict[str, str] = {}
    for source, target in entries.items():
        if not isinstance(source, str) or not isinstance(target, str):
            raise SystemExit(f"{path}: every catalog entry must map strings to strings")
        if source and source != target:
            result[source] = target
    return result


def sorted_pairs(entries: dict[str, str]) -> list[tuple[str, str]]:
    # Longest first makes overlapping phrases deterministic. Exact whole-string
    # overrides win before this sort; the polish pass is separate so its
    # English -> English rules cannot be fed back into the Japanese catalog.
    return sorted(entries.items(), key=lambda pair: (-len(pair[0]), pair[0]))


def load_catalog(path: Path) -> list[tuple[str, str]]:
    entries = read_catalog(path)
    overrides = path.with_name("overrides.json")
    if overrides.is_file():
        entries.update(read_catalog(overrides))
    return sorted_pairs(entries)


def load_polish(path: Path) -> list[tuple[str, str]]:
    polish = path.with_name("polish.json")
    return sorted_pairs(read_catalog(polish)) if polish.is_file() else []


def replace_all(text: str, pairs: list[tuple[str, str]]) -> str:
    """Replace all matches in one pass, longest match first.

    A plain sequence of str.replace() calls also applies shorter catalog rules
    to English emitted by longer ones. That can silently rewrite a carefully
    reviewed sentence and make the polish pass miss it. Matching only against
    the original input keeps replacement output opaque to later rules.
    """
    by_initial: dict[str, list[tuple[str, str]]] = {}
    for source, target in pairs:
        by_initial.setdefault(source[0], []).append((source, target))

    output: list[str] = []
    position = 0
    while position < len(text):
        match: tuple[str, str] | None = None
        for candidate in by_initial.get(text[position], ()):
            if text.startswith(candidate[0], position):
                match = candidate
                break
        if match is None:
            output.append(text[position])
            position += 1
            continue
        output.append(match[1])
        position += len(match[0])
    return "".join(output)


def apply_google_branding(text: str) -> str:
    """Apply the Gemini product branding to a fully translated settings page."""
    title_pattern = re.compile(
        r"<title>\s*Stack-chan settings(?: \(Wi-Fi\))?\s*</title>", re.IGNORECASE
    )
    heading_pattern = re.compile(
        r"<h1>\s*(?:🤖\s*)?Stack-chan settings(?: \(Wi-Fi\))?\s*</h1>",
        re.IGNORECASE,
    )
    favicon = f'\n<link rel="icon" type="image/svg+xml" href="{GOOGLE_CLOUD_FAVICON}">'
    heading = (
        '<h1 style="display:flex;align-items:center;gap:0.55rem">'
        f'{GOOGLE_CLOUD_HEADER_ICON}<span>{GOOGLE_CLOUD_TITLE}</span></h1>'
    )
    text, title_count = title_pattern.subn(f'<title>{GOOGLE_CLOUD_TITLE}</title>{favicon}', text, count=1)
    text, heading_count = heading_pattern.subn(heading, text, count=1)
    if title_count != 1 or heading_count != 1:
        raise SystemExit(
            "error: settings branding markers not found; update apply_google_branding() "
            "for the changed upstream title/header markup"
        )
    # The upstream English catalog retains the project spelling. Apply the
    # customer product name only to the generated Gemini settings page. Keep
    # JavaScript identifiers such as StackchanSettings untouched.
    text = text.replace("Stack-chan", "StackChan")
    text = text.replace("Stackchan-XXXXXX", "StackChan-XXXXXX")
    text = text.replace("Stackchan AP", "StackChan AP")
    return text


def translate(text: str, pairs: list[tuple[str, str]], polish: list[tuple[str, str]]) -> str:
    # Some catalog entries intentionally retain interpolated fragments, such as
    # `${enabled ? '有効' : '無効'}`. Repeat only the Japanese-to-English scan so
    # those fragments are translated without ever feeding English output
    # through English copyediting rules.
    japanese_pairs = [(source, target) for source, target in pairs if JA.search(source)]
    for _ in range(16):
        translated = replace_all(text, japanese_pairs)
        if translated == text or not JA.search(translated):
            text = translated
            break
        text = translated
    text = replace_all(text, polish)
    text = text.replace('<html lang="ja">', '<html lang="en">')
    text = text.replace("<html lang='ja'>", "<html lang='en'>")
    return strip_japanese_comments(text)


def strip_script_comments(segment: str) -> str:
    """Remove Japanese JS/CSS comments without treating comment text as code.

    A naive ``/\\*.*?\\*/`` pass corrupts real pages: ``// ... /mcp/*`` is a
    line comment, not the beginning of a block comment. Scan strings and line
    comments first so only actual comment spans containing Japanese are
    removed. Comment-looking text inside regex literals is uncommon and is
    left intact unless it looks like a complete block comment; this is safe
    because we only remove spans that contain Japanese, and user-visible
    strings have already been translated.
    """
    output: list[str] = []
    i = 0
    quote: str | None = None
    while i < len(segment):
        c = segment[i]
        if quote is not None:
            output.append(c)
            if c == "\\" and i + 1 < len(segment):
                output.append(segment[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue

        if c in ("'", '"', "`"):
            quote = c
            output.append(c)
            i += 1
            continue

        if c == "/" and i + 1 < len(segment) and segment[i + 1] == "/":
            end = segment.find("\n", i)
            if end < 0:
                end = len(segment)
            comment = segment[i:end]
            output.append(" " if JA.search(comment) else comment)
            i = end
            continue

        if c == "/" and i + 1 < len(segment) and segment[i + 1] == "*":
            end = segment.find("*/", i + 2)
            if end < 0:
                end = len(segment) - 2
            end += 2
            comment = segment[i:end]
            if JA.search(comment):
                # Keep line count stable while ensuring adjacent tokens do not
                # become one token on an inline comment removal.
                output.append("".join(ch if ch == "\n" else "" for ch in comment) or " ")
            else:
                output.append(comment)
            i = end
            continue

        output.append(c)
        i += 1
    return "".join(output)


def strip_japanese_comments(text: str) -> str:
    """Remove Japanese comments left in the generated artifact.

    The source pages keep their Japanese comments for upstream maintenance.
    The shipped Gemini artifacts do not need them, and removing them keeps a
    simple residual-character audit meaningful. Only comments containing
    Japanese are touched; English comments (including license headers) stay.
    """

    # HTML comments can span lines. A comment is not user-visible, but ships in
    # the page source, so drop it when it still contains Japanese.
    def html_comment(match: re.Match[str]) -> str:
        return "" if JA.search(match.group(0)) else match.group(0)

    text = re.sub(r"<!--.*?-->", html_comment, text, flags=re.DOTALL)

    # Preset DSL sources are serialized into a JSON string, so their comments
    # appear as literal "\n-- ..." sequences rather than physical lines.
    def escaped_dsl_comment(match: re.Match[str]) -> str:
        return "\\n" if JA.search(match.group(0)) else match.group(0)

    text = re.sub(r'[ \t]*--[^\\\n]*(?=\\n|")', escaped_dsl_comment, text)

    # Process script/style bodies as code. Matching the whole HTML with a block
    # comment regex is unsafe: '/mcp/*' inside a JavaScript line comment would
    # swallow code through the next unrelated '*/'.
    def script_body(match: re.Match[str]) -> str:
        return match.group(1) + strip_script_comments(match.group(2)) + match.group(3)

    for tag in ("script", "style"):
        text = re.sub(
            rf"(<{tag}\b[^>]*>)(.*?)(</{tag}\s*>)",
            script_body,
            text,
            flags=re.DOTALL | re.IGNORECASE,
        )
    return text


def residual_lines(text: str) -> list[tuple[int, str]]:
    return [(i, line.rstrip()) for i, line in enumerate(text.splitlines(), 1) if JA.search(line)]


def copyedit_lines(text: str) -> list[tuple[int, str]]:
    return [(i, line.rstrip()) for i, line in enumerate(text.splitlines(), 1) if BAD_ENGLISH.search(line)]


def branding_lines(text: str) -> list[tuple[int, str]]:
    issues: list[tuple[int, str]] = []
    if f"<title>{GOOGLE_CLOUD_TITLE}</title>" not in text:
        issues.append((0, "missing branded document title"))
    if "data:image/svg+xml," not in text:
        issues.append((0, "missing inline SVG favicon"))
    if f"<span>{GOOGLE_CLOUD_TITLE}</span>" not in text:
        issues.append((0, "missing branded page heading"))
    return issues


def report(path: Path, text: str, require_branding: bool = False) -> bool:
    failed = False
    checks = [("Japanese", residual_lines(text)), ("banned English", copyedit_lines(text))]
    if require_branding:
        checks.append(("missing Google Cloud branding", branding_lines(text)))
    for kind, lines in checks:
        if not lines:
            continue
        failed = True
        print(f"error: {path}: {kind} remains in {len(lines)} line(s)", file=sys.stderr)
        for line_no, line in lines[:40]:
            print(f"  {line_no}: {line}", file=sys.stderr)
        if len(lines) > 40:
            print(f"  ... {len(lines) - 40} more", file=sys.stderr)
    return failed


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("catalog", type=Path)
    parser.add_argument("--check", action="store_true", help="check files without writing")
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()

    pairs = load_catalog(args.catalog)
    polish = load_polish(args.catalog)
    if args.check:
        failed = False
        for path in args.paths:
            is_html = path.suffix.lower() in (".html", ".htm")
            if report(path, path.read_text(encoding="utf-8"), require_branding=is_html):
                failed = True
        return 1 if failed else 0

    if len(args.paths) % 2:
        parser.error("translation mode needs INPUT OUTPUT pairs")
    failed = False
    for input_name, output_name in zip(args.paths[::2], args.paths[1::2]):
        source = input_name.read_text(encoding="utf-8")
        result = translate(source, pairs, polish)
        is_html = input_name.suffix.lower() in (".html", ".htm")
        if is_html:
            result = apply_google_branding(result)
        if report(input_name, result, require_branding=is_html):
            failed = True
            continue
        output_name.parent.mkdir(parents=True, exist_ok=True)
        output_name.write_text(result, encoding="utf-8")
        print(f"[settings-i18n] {input_name} -> {output_name}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())

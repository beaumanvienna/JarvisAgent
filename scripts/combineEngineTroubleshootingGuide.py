#!/usr/bin/env python3
# @jarvis-script
# @short: Combine engine troubleshooting sections into a single guide
# @params: code244JsonPath — path to the code 244 {title, mermaid} JSON
#   code250JsonPath — path to the code 250 {title, mermaid} JSON
#   code301JsonPath — path to the code 301 {title, mermaid} JSON
#   outputMdPath — path to write combined guide
# @description: Reads the three schema-validated JSON outputs from the per-code
#   ai_call tasks ({"title": "...", "mermaid": "flowchart TD ..."}) and emits
#   one Markdown document with the Mermaid source wrapped in ```mermaid fences.
#   The AI never produces fences itself — this script owns the wrapping — so
#   no variation in the AI reply can accidentally yield an unfenced diagram.
# @outputs: outputMdPath — the combined troubleshooting guide
from __future__ import annotations

import json
import re
from pathlib import Path


# Defensive mermaid syntax normalization. Haiku occasionally emits '--|"label"|'
# for a labeled edge, which mermaid 10.x rejects — the correct token is
# '-->|"label"|'. The AI prompt asks for '-->' explicitly and gives a WRONG
# counter-example, but the model still slips this through. Fixing it once in
# the combiner is cheaper than retrying per-task when we already hold the text.
_BROKEN_EDGE_RE = re.compile(r'--(\s*\|)')


def _normalizeMermaid(source: str) -> str:
    return _BROKEN_EDGE_RE.sub(r'-->\1', source)


def _readJsonFile(pathString: str) -> dict:
    filePath = Path(pathString)
    if filePath.exists() is False:
        raise FileNotFoundError(f"Input JSON file not found: {filePath}")
    return json.loads(filePath.read_text(encoding="utf-8"))


def _writeTextFile(pathString: str, content: str) -> None:
    filePath = Path(pathString)
    filePath.parent.mkdir(parents=True, exist_ok=True)
    filePath.write_text(content, encoding="utf-8")


def _appendCodeSection(outputLines: list[str], anchor: str, section: dict) -> None:
    title = str(section.get("title", anchor)).strip()
    mermaidSource = _normalizeMermaid(str(section.get("mermaid", "")).strip())
    outputLines.append(f"## {title}")
    outputLines.append("")
    outputLines.append("```mermaid")
    outputLines.append(mermaidSource)
    outputLines.append("```")
    outputLines.append("")


def buildEngineTroubleshootingGuide(
    code244JsonPath: str,
    code250JsonPath: str,
    code301JsonPath: str,
    outputMdPath: str,
    **kwargs,
) -> dict:
    code244 = _readJsonFile(code244JsonPath)
    code250 = _readJsonFile(code250JsonPath)
    code301 = _readJsonFile(code301JsonPath)

    outputLines: list[str] = []
    outputLines.append("# engine troubleshooting guide")
    outputLines.append("")
    outputLines.append("## table of contents")
    outputLines.append(f"- [{code244.get('title', 'code 244')}](#code-244)")
    outputLines.append(f"- [{code250.get('title', 'code 250')}](#code-250)")
    outputLines.append(f"- [{code301.get('title', 'code 301')}](#code-301)")
    outputLines.append("")
    outputLines.append("## introduction")
    outputLines.append(
        "This troubleshooting guide is used to repair the vehicle. "
        "Usage: read out the engine codes and follow the troubleshooting steps."
    )
    outputLines.append("")

    _appendCodeSection(outputLines, "code 244", code244)
    _appendCodeSection(outputLines, "code 250", code250)
    _appendCodeSection(outputLines, "code 301", code301)

    combined = "\n".join(outputLines)
    _writeTextFile(outputMdPath, combined)

    return {
        "outputMdPath": outputMdPath
    }

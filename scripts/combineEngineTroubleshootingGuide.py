#!/usr/bin/env python3
# @jarvis-script
# @short: Combine engine troubleshooting sections into a single guide
# @params: code244MdPath — path to code 244 markdown file
#   code250MdPath — path to code 250 markdown file
#   code301MdPath — path to code 301 markdown file
#   outputMdPath — path to write combined guide
# @description: Merges per-error-code troubleshooting Markdown files into a
#   single navigable guide with a table of contents.
#   Called as a JCWF python task via buildEngineTroubleshootingGuide().
# @outputs: outputMdPath — the combined troubleshooting guide
from __future__ import annotations

from pathlib import Path


def _readTextFile(pathString: str) -> str:
    filePath = Path(pathString)
    if filePath.exists() is False:
        raise FileNotFoundError(f"Input markdown file not found: {filePath}")
    return filePath.read_text(encoding="utf-8")


def _writeTextFile(pathString: str, content: str) -> None:
    filePath = Path(pathString)
    filePath.parent.mkdir(parents=True, exist_ok=True)
    filePath.write_text(content, encoding="utf-8")


def buildEngineTroubleshootingGuide(
    code244MdPath: str,
    code250MdPath: str,
    code301MdPath: str,
    outputMdPath: str,
    **kwargs,
) -> dict:
    code244Content = _readTextFile(code244MdPath).strip()
    code250Content = _readTextFile(code250MdPath).strip()
    code301Content = _readTextFile(code301MdPath).strip()

    outputLines: list[str] = []
    outputLines.append("# engine troubleshooting guide")
    outputLines.append("")
    outputLines.append("## table of contents")
    outputLines.append("- [code 244](#code-244)")
    outputLines.append("- [code 250](#code-250)")
    outputLines.append("- [code 301](#code-301)")
    outputLines.append("")
    outputLines.append("## introduction")
    outputLines.append(
        "This troubleshooting guide is used to repair the vehicle."
        "Usage: read out the engine codes and follow the troubleshooting steps."
    )
    outputLines.append("")

    outputLines.append("## code 244")
    outputLines.append("")
    outputLines.append(code244Content)
    outputLines.append("")

    outputLines.append("## code 250")
    outputLines.append("")
    outputLines.append(code250Content)
    outputLines.append("")

    outputLines.append("## code 301")
    outputLines.append("")
    outputLines.append(code301Content)
    outputLines.append("")

    combined = "\n".join(outputLines)
    _writeTextFile(outputMdPath, combined)

    return {
        "outputMdPath": outputMdPath
    }

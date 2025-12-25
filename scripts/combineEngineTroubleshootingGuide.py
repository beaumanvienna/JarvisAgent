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
        "This troubleshooting guide is used to reoair the vehicle. "
        "Usage: read out the engine codes and follow the troubleshooitng steps."
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

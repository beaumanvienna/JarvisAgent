from __future__ import annotations

import subprocess
from pathlib import Path


def convertMarkdownToPdf(inputMdPath: str, outputPdfPath: str) -> dict:
    inputPath = Path(inputMdPath)
    if inputPath.exists() is False:
        raise FileNotFoundError(f"Input markdown file not found: {inputPath}")

    outputPath = Path(outputPdfPath)
    outputPath.parent.mkdir(parents=True, exist_ok=True)

    subprocess.run(
        ["md2pdf", str(inputPath), "-o", str(outputPath)],
        check=True,
    )

    return {
        "outputPdfPath": str(outputPath)
    }

#!/usr/bin/env python3
# @jarvis-script
# @short: Convert Markdown to PDF using md2pdf
# @params: inputMdPath — path to the input Markdown file
#   outputPdfPath — path to the output PDF file
# @description: Converts a Markdown file to PDF by invoking the md2pdf CLI.
#   Called as a JCWF python task via convertMarkdownToPdf().
# @outputs: outputPdfPath — the generated PDF file
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

"""Pre-process a portfolio position: uppercase all text, compute metadata.

Used by the portfolioPythonAnalysis workflow to demonstrate parallel Python
execution across the PythonEnginePool (N sub-interpreters).
"""

import os
import hashlib


def run(**kwargs):
    context = kwargs.get("context", {})
    # Filter-injected values use the binding prefix (e.g. "pos.Symbol")
    symbol = kwargs.get("pos.Symbol", "")
    name = kwargs.get("pos.Name", "")
    percentage = kwargs.get("pos.Percentage", "")

    # --- CPU-bound work: string processing ---
    upper_symbol = symbol.upper()
    upper_name = name.upper()
    words = upper_name.split()
    char_count = len(upper_name)
    word_count = len(words)

    # Compute a fingerprint (exercises hashlib, mild CPU)
    fingerprint = hashlib.sha256(f"{symbol}:{name}:{percentage}".encode()).hexdigest()[:12]

    report = (
        f"SYMBOL: {upper_symbol}\n"
        f"NAME: {upper_name}\n"
        f"ALLOCATION: {percentage}\n"
        f"CHARACTER COUNT: {char_count}\n"
        f"WORD COUNT: {word_count}\n"
        f"FINGERPRINT: {fingerprint}\n"
    )

    # --- I/O-bound work: write output file ---
    task_dir = context.get("_task_working_directory", ".")
    output_path = os.path.join(task_dir, f"preprocessed_{upper_symbol}.txt")
    with open(output_path, "w") as f:
        f.write(report)

    return {
        "formatted": report,
        "outputPath": output_path,
    }

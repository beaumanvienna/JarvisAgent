import subprocess
from helpers.log import log_info, log_warn, log_error

# Requires: pip install markitdown

def convert_pdf_to_markdown(input_pdf, output_md):
    log_info(f"Running MarkItDown on: {input_pdf}")

    try:
        result = subprocess.run(
            ["markitdown", input_pdf, "--output", output_md],
            capture_output=True,
            text=True,
            check=False
        )

        if result.returncode != 0:
            log_error(f"MarkItDown failed: {result.stderr}")
            return False

        log_info(f"PDF converted successfully: {output_md}")
        return True

    except FileNotFoundError:
        log_error("markitdown not found. Install it using: pip install markitdown")
        return False

    except Exception as e:
        log_error(f"MarkItDown exception: {e}")
        return False

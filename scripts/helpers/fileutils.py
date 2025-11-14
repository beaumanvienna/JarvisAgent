import os

def is_pdf(path):
    return path.lower().endswith(".pdf")

def is_binary_file(path, block_size=512):
    try:
        with open(path, "rb") as f:
            chunk = f.read(block_size)
            return b'\0' in chunk
    except Exception:
        return False

def read_text_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()

def write_text_file(path, content):
    with open(path, "w", encoding="utf-8") as f:
        f.write(content)

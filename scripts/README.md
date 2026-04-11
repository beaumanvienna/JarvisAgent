# JarvisAgent Python Scripting Layer — Technical Documentation

This document describes the **exact Python components** used by the JarvisAgent engine.  
It is intentionally concise and strictly reflects the code you provided.

Contents:
1. `main.py`
2. Logging helpers
3. File‑type utilities
4. MarkItDown conversion tools
5. Markdown chunker
6. Chunk combiner

---

# 1. main.py — Python Scripting Layer Entry Point

**Responsibilities**
- Initialize Python→C++ communication callbacks:
  - `JarvisPyStatus` — reports errors to the C++ engine.
  - `JarvisRedirect` — redirects Python `stdout` and `stderr` to C++ logging.
- Install a global `sys.excepthook` to catch all uncaught Python exceptions.
- Implement `OnStart`, `OnEvent`, and `OnShutdown` hooks called by the engine.
- Detect file additions:
  - Convert documents → Markdown using the MarkItDown CLI.
  - Chunk large Markdown files.
  - Combine chunk outputs once all are present.

## 1.1 Python → C++ Error Forwarding

`_JarvisPyStatus` loads `JarvisPyStatus(char*)` from the process symbol table:

```python
C = ctypes.CDLL(None)
C.JarvisPyStatus.argtypes = [ctypes.c_char_p]
C.JarvisPyStatus.restype = None
```

Used by:
- `notify_python_error(msg)`
- All helper modules

Purpose:
- Immediately notify C++ about Python-side failures so the engine can enter “Python Offline” mode.

## 1.2 stdout / stderr Redirection

`_JarvisRedirect` replaces `sys.stdout` and `sys.stderr`.  
It:
- Buffers partial lines
- Emits complete newline‑terminated lines to C++ via `JarvisRedirect(char*)`
- Prevents partial/mixed Python output

## 1.3 Global Exception Hook

```python
sys.excepthook = _global_exception_hook
```

Any unhandled exception:
- Is formatted
- Logged
- Forwarded to C++

## 1.4 File Event Handling (OnEvent)

### Document Conversion
Triggered when `FileAdded` and file is:
- PDF
- DOC/DOCX
- XLS/XLSX/CSV
- PPT/PPTX

Calls:

```python
md_path = convert_with_markitdown(file_path)
```

### Chunk Output Combination
Triggered when file matches:

```
chunk_<num>.output.md
```

Calls:

```python
handle_chunk_output_added(file_path)
```

### Markdown Chunking
For any `.md` file (except `.output.md` or chunk outputs):
- Skips if `.output.md` exists and is newer
- Otherwise calls:

```python
chunk_markdown_if_needed(file_path)
```

---

# 2. helpers/log.py — Logging Helpers

Functions:
- `log_info(msg)`
- `log_warn(msg)`
- `log_error(msg)`

All send timestamped messages to Python stdout, which is redirected to C++.

Format example:

```
[PY][INFO 12:30:15] Message
```

No additional behavior.

---

# 3. helpers/fileutils.py — File Type + File IO Utilities

### File‑type Detection
- `is_pdf(path)`
- `is_docx(path)`
- `is_xlsx(path)`
- `is_pptx(path)`
- `is_binary_file(path)`

`is_binary_file()` reads a block and checks for NUL bytes.  
On failure → reports to C++ via `_JarvisPyStatusHelper`.

### Text File Helpers
- `read_text_file(path)` → returns UTF‑8 text with replacement on errors
- `write_text_file(path, content)`  
  On failure → reports error to C++ and re‑raises.

---

# 4. helpers/markitdown_tools.py — MarkItDown CLI Wrapper

**Purpose:** Convert documents → Markdown using the external CLI tool `markitdown`.

Main function:

```python
convert_with_markitdown(input_path) -> Path
```

Behavior:
1. Computes `<input>.md`
2. Skips conversion if output exists and is newer
3. Runs:

```
markitdown <input>
```

4. Writes `.md` output
5. On missing CLI → generates an error `.md` file and notifies C++
6. On other failures → logs and writes an error file

All exceptions notify C++ via `_JarvisPyStatusHelper`.

---

# 5. helpers/md_chunker.py — Markdown Chunking

**Purpose:** Split oversized Markdown files into `chunk_###.md` files.

Entry point:

```python
chunk_markdown_if_needed(markdown_file_path)
```

Workflow:

1. Loads `config.json` from project root.
2. Reads `"max file size in kB"`.
3. If file size ≤ limit → no action.
4. Reads entire Markdown file.
5. Splits text into “sections” using heading boundaries.
6. Sections exceeding max size are further split into paragraphs.
7. Paragraphs exceeding max size are recursively byte‑split on UTF‑8 boundaries.
8. Writes chunks into:

```
<file>.md_chunks/chunk_###.md
```

9. Logs stale-chunk warnings but never deletes old chunks.

All failures call `_notify_chunker_error()` → Python error → C++ notified.

---

# 6. helpers/chunk_combiner.py — Chunk Output Combiner

**Purpose:** Combine:

```
chunk_###.output.md
```

into a single:

```
<file>.output.md
```

Triggered by `main.py` via `handle_chunk_output_added()`.

Workflow:

1. Scan folder for matching `chunk_###.md` inputs.
2. Ensure corresponding `chunk_###.output.md` exist AND are newer than inputs.
3. Determine combined output location:
   - Parent folder
   - Input folder name must end with `_chunks`
4. Skip recombination if combined output is already up-to-date.
5. Read and concatenate chunk outputs in index order.
6. Write final combined output.
7. Hard‑fail if folder naming or IO errors occur.

---

# Summary

| Component | Purpose |
|----------|---------|
| `main.py` | Engine-facing Python layer; error forwarding; document conversion; chunking; combining |
| `helpers/log.py` | Timestamped Python logging (redirected to C++) |
| `helpers/fileutils.py` | File-type detection + safe text IO |
| `helpers/markitdown_tools.py` | MarkItDown CLI wrapper with robust error handling |
| `helpers/md_chunker.py` | Split large Markdown files into manageable chunks |
| `helpers/chunk_combiner.py` | Detect when all chunk outputs exist and combine them |

All modules use the same Python→C++ failure reporting mechanism and avoid silent failures.


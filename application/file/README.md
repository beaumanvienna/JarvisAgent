# JarvisAgent File System Subsystem Documentation

## Overview
The file subsystem provides:
- Continuous monitoring of the queue directory.
- Categorization of files into meaningful groups.
- Hash‑based change detection.
- Automatic classification and filtering of files for AI consumption.
- Event forwarding into the engine (added/modified/removed).

It consists of several components:
- `FileWatcher`
- `FileCategorizer`
- `TrackedFile`
- `ProbUtils` (PROB filename parsing)
- File category enums and containers

---

## FileWatcher

### Purpose
Asynchronously monitors a directory (recursively) and emits engine events:
- `FileAddedEvent`
- `FileModifiedEvent`
- `FileRemovedEvent`
- Shutdown request if folder disappears

### Behavior
- Runs inside a thread‑pool task.
- Sleeps for the configured interval (`100ms` default).
- Initial scan fires `FileAddedEvent` for all existing files.
- Skips:
  - directories
  - dotfiles (e.g., Geany temp files)
- Detects:
  - new files → `FileAddedEvent`
  - time‑updated files → `FileModifiedEvent`
  - removed files → `FileRemovedEvent`

### Stop/Start
- `Start()` launches the watcher.
- `Stop()` terminates gracefully and waits for the task to finish.

---

## FileCategorizer

### Purpose
Categorizes files so SessionManagers and the AI system understand what a file represents.

### Categories
- **Settings** (`STNG_*`)
- **Context** (`CNTX_*`)
- **Task** (`TASK_*`)
- **Provider** (`PROV_*`) — provider configuration (never sent to AI)
- **Requirement** (default for non‑binary, non‑ignored files)
- **SubFolder**
- **Ignored**

### Special Handling

#### PROB files
- Filenames: `PROB_<id>_<timestamp>.txt` or `.output.txt`
- Parsed via `ProbUtils`.
- Stale PROB files (timestamp < app startup) are **ignored silently**.
- Non‑stale PROB files are always treated as **requirements**.

#### Binary detection
Rejects files with common binary signatures:
- PDF (`%PDF`)
- Office formats (`.docx`, `.xlsx`, `.pptx`, `.odt`)
- PNG, JPEG, GIF, BMP
- ELF, PE
- ZIP/archives

These are:
- silently ignored (PDF, Office)
- or logged as binary and ignored (others)

#### Text check
- Reads first 256 bytes.
- If >10% non‑text → file treated as binary → ignored.

#### Size limit
If file exceeds `m_MaxFileSizekB`:
- Creates an `.output.txt` file with explanation.
- Original file is ignored.

### Modification Handling
`ModifyFile()`:
- Re‑categorizes the file.
- Compares hashes via `TrackedFile::CheckIfContentChanged()`.
- Marks category as dirty.
- Increments modified count.

---

## TrackedFile

### Purpose
Stores metadata for each tracked file:
- File path
- Category
- Hash (SHA‑256 if available, otherwise `std::hash`)
- “Modified” state

### Behavior
- Upon creation: file is hashed and marked modified.
- `CheckIfContentChanged()` updates hash and returns true/false.
- `GetContent()` returns file text and resets the modified flag.

---

## ProbUtils

### Purpose
Parse PROB filenames.

### Behavior
Accepts:
- `PROB_<id>_<timestamp>.txt`
- `PROB_<id>_<timestamp>.output.txt`

Returns:
- `id` (uint64)
- `timestamp` (int64)
- `isOutput` flag

Used heavily by:
- FileCategorizer
- JarvisAgent (for routing output back into ChatMessagePool)

---

## Data Structures

### CategorizedFiles
Holds per‑category `TrackedFiles`:
- Settings
- Context
- Tasks
- Requirements
- Subfolders
- Ignored

Each `TrackedFiles` object tracks:
- `m_Map` (filename → TrackedFile)
- `m_Dirty` flag
- modified-file counters

---

## Interaction Flow

1. **FileWatcher** watches the `queue/` and `scripts/` folders, emitting `FileAddedEvent` / `FileModifiedEvent` / `FileRemovedEvent`.
2. **JarvisAgent** routes events to `TriggerEngine::NotifyFileEvent` (JCWF `file_watch` triggers) and to the script registry.
3. **FileCategorizer** classifies queue files by 4-letter prefix (STNG/CNTX/TASK/PROV/PROB); `.output` and `.transcript` stems are ignored.
4. **TrackedFile** performs hashing and content tracking.

Runtime `ai_call` tasks dispatch through a typed `AiInvocation` envelope — `AiCallTaskExecutor` writes the queue files to disk for replay/audit and calls `AiRequestPool::Submit` directly; the reply callback writes `<prob>.output.{txt|json}` + `<prob>.transcript.json` and signals completion inline. The `queue/` `FileWatcher` is still in place for `TriggerEngine` and for `AiJcwfService`'s editor-side pipelines (Explain / FixScript / Generate).

---

## Reliability Guarantees
- No unhandled exceptions propagate from file IO.
- Stale PROB files are never processed.
- Binary and oversized files cannot reach the AI.
- All modifications are tracked via secure hashing.
- All category transitions mark their groups dirty.

---

## File Index
- `file/fileWatcher.h/.cpp`
- `file/fileCategorizer.h/.cpp`
- `file/trackedFile.h/.cpp`
- `file/probUtils.h/.cpp`
- `file/fileCategory.h`


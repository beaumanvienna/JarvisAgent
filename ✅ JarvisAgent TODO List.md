✅ JarvisAgent TODO List (Updated Nov 15, 2025)
❗ 1. Status Line Renderer overwrites log messages (NEW, needs fix)

Severity: Critical
You already saw it:
The live status line (the moving “progress bar” that prints in-place) overwrites log output.

Result:
Important log messages are destroyed or never visible.

This is dangerous because you may miss error logs (file errors, curl failures, etc.).

Fix options:
Option A — Disable status line entirely (fastest)

Probably smart for now, because you do most inspection in the terminal.

Option B — Only update status when stdout is a TTY

If redirected to file → disable.
If normal terminal → update with ANSI cursor up/down movement.

Option C — Use a dedicated logging stream

Log always goes to stderr;
Status renderer goes to stdout.

I recommend Option C (takes 10 minutes to implement).

❗ 2. Move PythonEngine events off the main thread (NOT FIXED)

Large PDFs still freeze the main thread for seconds because MarkItDown runs synchronously.

Fix:

Create:

class PythonJobQueue


Main thread pushes events → worker thread consumes them.

This will:

remove 2–10 second UI freezes

avoid watchdog “JarvisAgent stalled”

fix timing issues during mass PDF processing

❗ 3. Force cancellation of CURL requests on shutdown (PARTIALLY FIXED)

Pressing q during long GPT calls still blocks shutdown.

What is working:

Setting the shutdown flag works.

Threads eventually stop.

What’s still broken:

Threads cannot exit until curl finishes.

If network stalls → JarvisAgent hangs for 30–120 seconds.

Fix:

Add:

CURLOPT_TIMEOUT_MS = 15000
CURLOPT_CONNECTTIMEOUT_MS = 5000


Optionally:

curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);


This will guarantee threads break out fast.

⚠ 4. Ignore stale PROB files older than startup time (MOSTLY FIXED, needs cleanup)

Your new startup timestamp check works, but:

Remaining issues:

The log still prints the timestamp comparison for every PROB file on startup.

Should be silent.

Should skip further processing entirely (no log spam).

Fix:

Just remove:

LOG_APP_INFO("\"{}\" startupTimestamp …")


and ensure early return in FileCategorizer.

⚠ 5. Markdown file size limit is correct, but “PDF too big” message misleading (FIXED but refactoring recommended)

It now uses the config value.
Behavior is correct.

Remaining task:

Apply the same size limit dynamically in the Python-generated .output.txt.

This is optional cleanup.

⚠ 6. PROB output → reply parser “no content” warning (NOT FIXED)

When the response contains reasoning tokens without user-facing text:

warning: output discarded because it had no content


This is correct but too noisy.

Fix:

If:

message.content[*].text.empty() == true
→ write a harmless stub into .output.txt instead of warning.

Something like:

The model returned no answer (empty response).


This prevents confusion.

⚠ 7. Minor cleanup: variable names, consistent filtering, file categorizer rearrangement

You already fixed many, but:

Remaining inconsistencies:

Review Categorizer for leftover abbreviation names.

“Ignoring oversized file” log appears before Python conversion — correct for .md, but not for PDFs.

Low priority.

🧭 Priority Ordered Roadmap
Highest priority (do these next):

Status line overwriting logs → fix immediately

Python job queue → remove UI freeze

Curl timeout + abort thread on shutdown

Medium priority:

Remove PROB timestamp logs on startup

Improve empty GPT output handling

Low priority:

Cleanup categorizer ordering and variable names

Polish PDF/MD size logging logic

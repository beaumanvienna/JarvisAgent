# JarvisAgent — TODO List

## 1. Status Line Renderer overwrites log messages
**Severity: Critical**

The animated status/progress line overwrites `spdlog` output.

Fix options:
- Disable status line entirely  
- Or: log to stderr and render status line on stdout  
- Or: only draw the status line when stdout is a TTY  

---

## 2. GPT response parser: “no content” warning is too noisy
When the model returns only reasoning tokens, no visible answer text is produced.

Fix:  
If content is empty, write this instead (no warning):

```
The model returned no answer (empty response).
```

---

## 3. Minor cleanup (low priority)
- naming consistency  
- remove leftover logs  
- small categorizer polish  

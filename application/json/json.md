# JarvisAgent JSON Parsing & ReplyParser Module Documentation

## Overview
The JSON subsystem provides:
- Structured parsing of OpenAI‑style responses (API1 and API2).
- A recursive object/array walker (`JsonObjectParser`).
- A version‑agnostic reply dispatcher (`ReplyParser`).

The implementation is strictly logging‑driven: JSON content is not transformed except where needed for reply extraction. Unknown fields are logged, never ignored silently.

---

## JsonObjectParser

### Purpose
A lightweight recursive JSON walker used to:
- Safely traverse arbitrary objects/arrays.
- Log each field with indentation.
- Detect unhandled JSON fields.
- Handle parse errors gracefully.

### Key Behavior
- Accepts a key, a `simdjson::ondemand::value`, a warning label, and an indentation level.
- Logs each primitive type:
  - string, number, boolean, null.
- Recursively descends into:
  - arrays → `ParseArray()`
  - objects → `ParseObject()`
- Tracks `m_HasError` when a simdjson exception occurs.

### Error Handling
- Any field extraction error logs a warning with the provided `warningText`.
- Parsing continues even after errors in nested structures.

---

## ReplyParser (base class)

### Purpose
Abstract interface for all response formats.

### Responsibilities
- Stores raw JSON string.
- Tracks error state (`m_HasError`).
- Defines:
  - `HasContent()`
  - `GetContent()`

### Factory
`ReplyParser::Create(interfaceType, jsonString)`  
Selects the correct parser implementation:
- `ReplyParserAPI1`
- `ReplyParserAPI2`

---

## ReplyParserAPI1  
(Used for classic ChatCompletion‑style responses)

### Expected JSON Format
```
{
  "id": "...",
  "object": "...",
  "created": <int>,
  "model": "...",
  "choices": [ ... ],
  "usage": { ... }
}
```

### What It Extracts
- `id`
- `object`
- `created`
- `model`
- `choices[n].message.content`
- Usage tokens

### Error Handling
If `"error"` exists:
- Logs detailed diagnostic.
- Captures message/type/code/param.
- Marks reply discarded.

### Content Extraction
`GetContent(index)` returns the text from the corresponding choice.

---

## ReplyParserAPI2  
(Used for GPT‑5‑style structured responses)

### Expected JSON Format
```
{
  "id": "...",
  "object": "response",
  "created_at": <int>,
  "status": "...",
  "model": "...",
  "output": [ ... ],
  "usage": { ... }
}
```

### What It Extracts
- `id`
- `object`
- `created_at`
- `status`
- `model`
- Output blocks
  - For each output item:
    - `type`
    - `status`
    - `role`
    - Multiple content entries
- Only `output_text` content is returned by `GetContent`.

### Error Handling
If `"error"` exists:
- Fully parsed (message/type/code/param).
- Marks reply discarded.

---

## High‑Level Flow

1. **Raw JSON arrives from Python/OpenAI.**
2. **ReplyParser::Create()** picks API1 or API2.
3. Parser converts raw JSON → `simdjson::padded_string`.
4. Walks top‑level fields:
   - Known fields → extracted normally.
   - Unknown fields → passed into `JsonObjectParser`.
5. If `"error"` appears at any depth:
   - Reply marked as failed.
   - `GetContent()` becomes unusable.
6. On success:
   - Response content extracted cleanly.
   - Token usage tracked.

---

## Logging Behavior
Every JSON field is printed to the terminal log:
- API1: `LOG_APP_INFO("key: {}", value)`
- API2: similar structured logs
- Unknown fields delegated to `JsonObjectParser` for recursive inspection.

Output text is also printed to stdout (matching current engine behavior).

---

## Reliability Guarantees
The JSON subsystem guarantees:
- No exceptions propagate upward.
- Reply structures validated against expected types (via `CORE_ASSERT`).
- Unknown JSON is never silently ignored.
- Parsing failures always logged with context.

---

## Integration Notes
- Both API parsers share a common base class.
- Selection is controlled by engine configuration.
- Parser does not interpret semantics—only structures.

---

## File Index
- `json/jsonObjectParser.h/.cpp`
- `json/replyParser.h/.cpp`
- `json/replyParserAPI1.h/.cpp`
- `json/replyParserAPI2.h/.cpp`


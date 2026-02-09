# per_item Task Expansion, Filters & Fan-Out

**JCWF version:** 1.1 (minor bump — additive feature, backward-compatible)

---

## 1. Overview

This feature introduces three new first-class concepts to the workflow
graph:

1. **Filter node** — selects items from a data source (CSV rows, text
   lines, or Lucene-style query hits).  Filters support range selection,
   boolean grouping, and field matching.
2. **Fan-out node** — represents the parallel expansion of a task template
   over the items produced by a filter.  One fan-out instance per matched
   item.
3. **per_item mode** — a task mode that binds a task template to a filter
   via a fan-out node, producing N independent task instances at runtime.

```
 [Filter Node]          ← select items (CSV / text / query)
       │
  [Fan-Out Node]        ← parallel expansion (N items)
    ┌──┼──┐
   [T0][T1]...[Tn]     ← task instances (ai_call / shell / internal / python)
    │   │       │
   f-0 f-1 ... f-n     ← output files: filterID-itemNumber.txt
```

Freshness is checked **per item**: if `output[k]` is newer than the
source, that item is skipped.

---

## 2. Filter Node

Filters are declared at the workflow level under a new `"filters"` key.
Each filter has an ID and appears as a distinct node in the workflow
editor.

### 2.1 JCWF Schema

```jsonc
"filters": [
  {
    "id": "reviewed-reqs",
    "source": { ... },
    "binding": "item"
  }
]
```

A filter node has no `depends_on`, no executor — it is purely
declarative.  It defines *what* items to iterate over.

### 2.2 Source Kinds

#### 2.2.1 CSV File (`csv`)

```jsonc
{
  "id": "csv-items",
  "source": {
    "kind": "csv",
    "path": "data/items.csv",
    "delimiter": ",",
    "has_header": true,
    "range": "10-20"
  },
  "binding": "row"
}
```

- **`range`** (optional) — row range (1-based, inclusive).  Examples:
  `"10-20"` (rows 10–20), `"5-"` (row 5 to end), `"-50"` (first 50).
  Omit for all rows.
- **Iteration:** Each selected row becomes one item.

**Item shape:**

| Key               | Value                                   |
|-------------------|-----------------------------------------|
| `row.index`       | 0-based index within the selected range |
| `row.row_number`  | 1-based row number in the source file   |
| `row.<col_name>`  | Cell value (header names, if present)   |
| `row.col_0` …     | Cell value by positional index          |
| `row.line`        | Full CSV line as raw string             |

**Freshness input:** file modification time of `path`.

#### 2.2.2 Text File Lines (`text_lines`)

```jsonc
{
  "id": "req-lines",
  "source": {
    "kind": "text_lines",
    "path": "data/requirements.txt",
    "skip_empty": true
  },
  "binding": "line"
}
```

**Item shape:**

| Key            | Value                       |
|----------------|-----------------------------|
| `line.index`   | 0-based line number         |
| `line.text`    | Full line content (trimmed) |

**Freshness input:** file modification time of `path`.

#### 2.2.3 Lucene-Style Query (`query`)

```jsonc
{
  "id": "reviewed-reqs",
  "source": {
    "kind": "query",
    "index_path": "data/index",
    "query": "(type:requirement OR type:defect) AND tags:JC AND created:[20230101 TO 20231231]",
    "fields": ["id", "title", "body", "status"]
  },
  "binding": "hit"
}
```

**Query language features:**

| Feature                | Syntax                                      | Example                                          |
|------------------------|---------------------------------------------|--------------------------------------------------|
| Field match            | `field:value`                               | `tags:JC`                                        |
| Boolean AND            | `expr AND expr`                             | `tags:JC AND status:reviewed`                    |
| Boolean OR             | `expr OR expr`                              | `type:requirement OR type:defect`                |
| Grouping               | `(expr)`                                    | `(type:requirement OR type:defect) AND tags:JC`  |
| Range (inclusive)       | `field:[lo TO hi]`                          | `created:[20230101 TO 20231231]`                 |
| Range (exclusive)       | `field:{lo TO hi}`                          | `priority:{1 TO 5}`                              |
| Wildcard               | `field:val*`                                | `title:sys*`                                     |
| Negation               | `NOT expr` or `-field:value`                | `NOT status:archived`                            |

**Item shape:**

| Key              | Value                                    |
|------------------|------------------------------------------|
| `hit.index`      | 0-based hit number                       |
| `hit.<field>`    | Stored field value (e.g. `hit.id`)       |
| `hit.doc_path`   | Filesystem path of the source doc        |

**Freshness input:** see §4 Freshness Naming Scheme.

---

## 3. Fan-Out Node

The fan-out node is the visual + runtime bridge between a filter and the
per_item task instances.

### 3.1 JCWF Representation

Fan-out is implicit: any task with `"mode": "per_item"` and a `"filter"`
reference creates a fan-out relationship.

```jsonc
{
  "id": "summarize_req",
  "type": "ai_call",
  "mode": "per_item",
  "filter": "reviewed-reqs",
  "params": { ... },
  "inputs": {
    "req_id":    { "type": "string" },
    "req_body":  { "type": "string" }
  },
  "outputs": {
    "summary":   { "type": "string" }
  }
}
```

The `"filter"` field (new in v1.1) references a filter node by ID.

### 3.2 Editor Representation

In the workflow editor, the fan-out is rendered as a **dedicated node**
between the filter node and the task node:

```
┌─────────────────┐
│  Filter Node    │  ← "reviewed-reqs" (query source)
│  (filter icon)  │
└────────┬────────┘
         │
┌────────▼────────┐
│  Fan-Out Node   │  ← auto-generated, shows item count at runtime
│  (parallel icon)│
└────────┬────────┘
         │
┌────────▼────────┐
│  Task Node      │  ← "summarize_req" (ai_call, per_item)
│  (task icon)    │
└─────────────────┘
```

- The fan-out node is **auto-created** when a per_item task references a
  filter; it cannot be manually added or removed.
- At runtime, the fan-out node displays the item count (e.g. "47 items")
  and progress (e.g. "32/47 done, 2 failed").
- Clicking the fan-out node shows the list of item instances with their
  individual statuses.

### 3.3 Runtime Expansion

When a per_item task becomes ready (all `depends_on` satisfied):

1. **Evaluate filter** — the runtime enumerates items from the filter's
   source, applying range/query constraints.
2. **Create task instances** — for each `items[k]`, create a task instance
   with ID `taskId#k` (e.g. `summarize_req#0`, `summarize_req#1`, …).
3. **Inject inputs** — item key-value pairs are merged into the instance's
   resolved inputs under the binding prefix.
4. **Check freshness per item** — each instance is independently checked
   (see §4).  If up-to-date, it is skipped.
5. **Dispatch** — non-skipped instances are dispatched to the thread pool.
6. **Aggregate** — parent task succeeds when all instances succeed or are
   skipped.  Configurable failure policy (see §7).

### 3.4 Output File Convention

Each per_item instance produces an output file named:

```
<filterID>-<itemNumber>.txt
```

Example: `reviewed-reqs-0.txt`, `reviewed-reqs-1.txt`, …

Written to the task's `working_directory`.  Contents may be plain text or
Markdown.

Built-in variables injected into each instance:

| Key                    | Value                                      |
|------------------------|--------------------------------------------|
| `_item.index`          | 0-based item number                        |
| `_item.output_path`    | Resolved output file path for this instance|
| `_item.filter_id`      | ID of the filter node                      |

---

## 4. Freshness Naming Scheme

### 4.1 Filter Result Manifest

Each filter evaluation writes a **manifest file** to the filter's own
directory (named after the filter ID, like any other task folder):

```
<workflowBaseDir>/<filterID>/<filterID>.manifest.json
```

Contents:

```jsonc
{
  "filter_id": "reviewed-reqs",
  "evaluated_at": "2026-02-08T16:00:00Z",
  "query_hash": "sha256:abc123...",
  "item_count": 47,
  "items": [
    { "index": 0, "key": "REQ-001", "source_path": "docs/REQ-001.md", "source_mtime": "..." },
    { "index": 1, "key": "REQ-002", "source_path": "docs/REQ-002.md", "source_mtime": "..." }
  ]
}
```

- **`query_hash`** — SHA-256 of the normalized filter expression.  If the
  filter expression changes, all items are considered stale.
- **`key`** — a stable item identity (e.g. first field value, row number).
  Used to detect item additions/removals between runs.

### 4.2 Per-Item Freshness Rules

For each instance `k`:

| Source kind    | Input timestamp                            |
|----------------|--------------------------------------------|
| `csv`          | file mtime of `path`                       |
| `text_lines`   | file mtime of `path`                       |
| `query`        | mtime of `hit.doc_path` (if available)     |

- **Output timestamp** = mtime of `<filterID>-<k>.txt`
- **Up-to-date** iff:
  1. Output file exists, AND
  2. Output mtime ≥ input mtime, AND
  3. `query_hash` in manifest matches current filter expression
- If `query_hash` changed → **all items stale** (full re-run).
- If `doc_path` unavailable for query hits → that item is always stale.

### 4.3 Manifest Staleness for Query Sources

Because query result sets can change (new items added, items removed)
without any single file changing:

- The manifest itself is the freshness anchor.
- On each run, the filter is re-evaluated and the new item list is
  compared to the manifest.
- **New items** (not in previous manifest) → stale, must run.
- **Removed items** (in manifest but not in new results) → output files
  are orphaned; the runtime logs a warning but does not delete them.
- **Existing items** (in both) → freshness checked per §4.2.

---

## 5. Frontend: Filter Input Dialog

### 5.1 Filter Builder UI

The workflow editor provides a **filter builder dialog** for constructing
complex Lucene-style queries visually:

```
┌─────────────────────────────────────────────────┐
│  Filter Builder: "reviewed-reqs"                │
├─────────────────────────────────────────────────┤
│                                                 │
│  Source: [query ▼]   Index: [data/index       ] │
│                                                 │
│  ┌─ AND ──────────────────────────────────────┐ │
│  │  ┌─ OR ────────────────────────────────┐   │ │
│  │  │  [type    ] [=  ▼] [requirement   ] │   │ │
│  │  │  [type    ] [=  ▼] [defect        ] │   │ │
│  │  │  [+ Add condition]                  │   │ │
│  │  └─────────────────────────────────────┘   │ │
│  │  [tags    ] [=  ▼] [JC              ]      │ │
│  │  [created ] [range] [20230101] [20231231]  │ │
│  │  [+ Add condition]                         │ │
│  └────────────────────────────────────────────┘ │
│                                                 │
│  Preview query:                                 │
│  (type:requirement OR type:defect) AND tags:JC  │
│   AND created:[20230101 TO 20231231]            │
│                                                 │
│  Fields to extract: [id, title, body, status  ] │
│  Binding prefix:    [hit                      ] │
│                                                 │
│  [Test Filter (dry run)]    [Save]    [Cancel]  │
└─────────────────────────────────────────────────┘
```

**Key features:**
- **Visual grouping** — nested AND/OR groups via drag-and-drop or
  add/remove buttons.
- **Operator selector** — `=`, `!=`, `range`, `wildcard`, `exists`.
- **Range inputs** — two-field input for `[lo TO hi]` ranges.
- **Live preview** — renders the Lucene query string in real time.
- **Test (dry run)** — evaluates the filter and shows matching item count
  without running tasks.
- **CSV mode** — when source kind is `csv`, shows file path picker, row
  range input, and column preview.
- **Text mode** — when source kind is `text_lines`, shows file path and
  skip-empty toggle.

### 5.2 Filter Node in Editor

Filters appear as a distinct node type in the workflow graph:

- **Visual style:** Different color/shape from task nodes (e.g. diamond or
  hexagonal shape, filter icon).
- **Inspector panel:** Opens the filter builder dialog.
- **Output port:** Connects to a fan-out node (auto-created).
- **No input ports:** Filters are source nodes (no `depends_on`).

### 5.3 Fan-Out Node in Editor

- **Auto-created** when a per_item task references a filter.
- **Visual style:** Distinct shape (e.g. trident/fork icon) showing
  parallel expansion.
- **Runtime overlay:** Displays item count, progress bar, pass/fail
  counts.
- **Click action:** Opens a list view of all item instances with
  individual status, output path, and error messages.
- **Not user-deletable:** Removing the per_item mode from the task removes
  the fan-out node automatically.

---

## 6. JCWF Schema Changes (v1.0 → v1.1)

| Change                              | Location                   | Kind     |
|-------------------------------------|----------------------------|----------|
| `"filters"` top-level array         | `$.filters`                | New      |
| Filter `id`, `source`, `binding`    | `$.filters[].{id,source,binding}` | New |
| Source kinds: `csv`, `text_lines`, `query` | `$.filters[].source.kind` | New |
| CSV `range` field                   | `$.filters[].source.range` | New      |
| Query syntax (AND/OR/range/group)   | `$.filters[].source.query` | New      |
| `"filter"` field on tasks           | `$.tasks.<id>.filter`      | New      |
| Version bump                        | `$.version`                | `"1.1"`  |

All changes are additive.  A v1.0 file with no filters remains valid
under v1.1.

---

## 7. Implementation Plan

### Phase 1: Data Structures and Parsing

**Files:** `workflowTypes.h`, `workflowJsonParser.cpp`

1. Add `FilterSource` struct:
   ```cpp
   struct FilterSource
   {
       std::string m_Kind;       // "csv", "text_lines", "query"
       std::string m_Path;       // source file (csv, text_lines)
       std::string m_Delimiter;  // csv only, default ","
       bool m_HasHeader{true};   // csv only
       std::string m_Range;      // csv only: "10-20", "5-", "-50"
       bool m_SkipEmpty{true};   // text_lines only
       std::string m_IndexPath;  // query only
       std::string m_Query;      // query only (Lucene expression)
       std::vector<std::string> m_Fields; // query only
   };
   ```
2. Add `FilterDef` struct:
   ```cpp
   struct FilterDef
   {
       std::string m_Id;
       FilterSource m_Source;
       std::string m_Binding;    // prefix for injected inputs
   };
   ```
3. Store `std::vector<FilterDef> m_Filters;` in `WorkflowDefinition`.
4. Add `std::string m_Filter;` to `TaskDef` (references filter ID).
5. Parse `"filters"` array and `"filter"` field in parser.
6. Bump known minor version to `1`.

### Phase 2: Filter Engine (CSV + Text Lines)

**New files:** `filterEngine.h` / `filterEngine.cpp`

```cpp
struct FilterItem
{
    size_t m_Index;         // 0-based within result set
    size_t m_SourceIndex;   // original index in source (row number, line number)
    std::unordered_map<std::string, std::string> m_Values;
    std::string m_SourcePath; // for freshness
    std::string m_Key;        // stable identity (e.g. first field value)
};

class FilterEngine
{
public:
    std::vector<FilterItem> Evaluate(
        FilterDef const& filter,
        std::string const& workflowBaseDir,
        std::string& errorMessage) const;

private:
    std::vector<FilterItem> EvaluateCsv(...) const;
    std::vector<FilterItem> EvaluateTextLines(...) const;
    std::vector<FilterItem> EvaluateQuery(...) const;

    // CSV range parser: "10-20" → {10, 20}
    static bool ParseRange(std::string const& range,
                           size_t& outStart, size_t& outEnd);
};
```

- **CSV:** C++ parser, split on delimiter, apply row range filter.
- **Text lines:** `std::getline`, optionally skip empty.
- **Query:** Stub in Phase 2; implemented in Phase 4.

### Phase 3: Filter Result Manifest + Freshness

**New file:** `filterManifest.h` / `filterManifest.cpp`

- Write `<workflowBaseDir>/<filterID>/<filterID>.manifest.json` after
  each filter evaluation.
- Compare previous manifest to detect new/removed/changed items.
- Compute `query_hash` for expression change detection.
- Integrate with `TaskFreshnessChecker` for per-item checks.

### Phase 4: Lucene Query Engine

**New files:** `queryParser.h` / `queryParser.cpp`

Recursive descent parser for the Lucene-style query language:

```
query     := orExpr
orExpr    := andExpr ("OR" andExpr)*
andExpr   := unaryExpr ("AND" unaryExpr)*
unaryExpr := "NOT" unaryExpr | "(" query ")" | fieldExpr
fieldExpr := FIELD ":" ( value | rangeExpr | wildcardExpr )
rangeExpr := "[" value "TO" value "]" | "{" value "TO" value "}"
```

The parser produces an AST that is evaluated against each candidate
document (row, line, or indexed record).  For CSV/text sources, the
query acts as a post-filter.  For indexed sources (future), it maps to
the index's native query API.

### Phase 5: Polarion REST API Client

**New files:** `polarionClient.h` / `polarionClient.cpp`

C++ class that performs paginated work-item queries against the
Polarion ALM REST API using libcurl (already vendored).

```
GET {base_url}/rest/v1/projects/{project_id}/workitems
  ?query={url_encoded_query}
  &fields[workitems]={comma_separated_fields}
  &page[size]={page_size}
  &page[number]={N}
```

Responsibilities:

- URL-encode the query expression and field list.
- HTTP Basic authentication via `key_name` → `KeyManager` lookup
  (same pattern as AI provider keys — credentials never in JCWF).
- Paginate automatically (`page[size]` default 100).  Stop when the
  API returns fewer items than `page_size` or `max_items` is reached.
- Parse the JSON:API response, extract work-item attributes per page.
- Write each item to `<filterID>/<filterID>-<k>.json` as it goes,
  bounding peak memory to one page of results.
- Return the full item list to `FilterEngine` for manifest creation.

Integration with `FilterEngine`:

- `FilterEngine::Evaluate` dispatches to `PolarionClient::FetchAll`
  when `source.kind == "polarion_query"`.
- The returned items follow the same `FilterItem` shape as other
  source kinds (index, binding fields, key for stable identity).

Error handling:

- HTTP errors (4xx/5xx) → filter evaluation fails, task does not
  fan out.
- Network timeouts → configurable via `timeout_ms` on the filter
  (reuses the task-level timeout semantic).
- Partial page failure → retry the page up to 3 times, then fail.

### Phase 6: Runtime Expansion in WorkflowRuntimeManager

**File:** `workflowRuntimeManager.cpp`

1. When a `per_item` task becomes ready, look up its `m_Filter` →
   `FilterDef`.
2. Call `FilterEngine::Evaluate` to produce `items[]`.
3. Write/update the filter manifest.
4. For each `items[k]`, synthesize instance ID `taskId#k`.
5. Insert `TaskInstanceState` entries into `m_TaskStates`.
6. Per-item freshness check using manifest + output file mtime.
7. Dispatch non-stale instances to thread pool.
8. Aggregate results into parent task state.

### Phase 7: Frontend — Filter Builder + Editor Nodes

**Files:** `WorkflowEditorView.tsx`, new `FilterBuilderDialog.tsx`

1. **Filter builder dialog** — visual AND/OR grouping, operator
   selectors, range inputs, live query preview, dry-run test button.
2. **Filter node** — new node type in the editor canvas with distinct
   visual style, inspector panel that opens filter builder.
3. **Fan-out node** — auto-created between filter and per_item task.
   Shows runtime item count and progress.  Click to expand instance
   list.
4. **Serialization** — read/write the `"filters"` array and `"filter"`
   task field in the editor's JCWF import/export.

### Phase 8: Spec Update + Tests

- Update `JC_Workflow_Specification.md` sections 3.2.3, 3.3.2, 6.3.
- Add new section for filter nodes and query language.
- Bump spec version to `"1.1"`.
- Create example: `example/workflows/per_item_example.jcwf`.
- Unit tests:
  - CSV parsing (header / no-header / custom delimiter / row range).
  - Text line iteration (skip empty / keep empty).
  - Query parser (AND, OR, NOT, grouping, range, wildcard).
  - Polarion client (mock HTTP, pagination, error handling).
  - Filter manifest write/read/diff.
  - Task expansion (N items → N instances).
  - Per-item freshness (skip / re-run / expression change).
  - Aggregate success / failure.

---

## 8. Execution Order

| #  | Phase                          | Effort  | Dependencies |
|----|--------------------------------|---------|--------------|
| 1  | Data structures + parsing      | Small   | None         |
| 2  | Filter engine (CSV + text)     | Medium  | Phase 1      |
| 3  | Filter manifest + freshness    | Medium  | Phase 2      |
| 4  | Lucene query parser            | Medium  | Phase 1      |
| 5  | Polarion REST API client       | Medium  | Phase 1      |
| 6  | Runtime expansion              | Large   | Phase 2 + 3  |
| 7  | Frontend (filter + fan-out)    | Large   | Phase 1      |
| 8  | Spec update + tests + example  | Small   | All          |

---

## 9. Resolved Design Decisions

1. **Max items cap:** 10000 default.  If set to 0 → no limit.
   Configured per filter via `"max_items": 10000`.
2. **Parallel item dispatch:** Reuse existing thread pool limits.
3. **Error policy:** `continue` — remaining items keep running even if
   one fails.  Parent task reports partial failure with item-level
   error details.
4. **Downstream fan-in (file-driven philosophy):**
   - **CSV / text_lines:** Downstream tasks consume the source file
     directly.  Freshness is checked via the source file's mtime.
     Each row/line is processed independently; no intermediate
     aggregation file is needed.
   - **Query / database sources:** The filter engine writes a small
     text-file snippet per item (the `<filterID>-<k>.txt` output).
     Downstream tasks consume these snippet files.  Freshness is
     checked via snippet file mtime vs source document mtime.
   - **Polarion query sources:** The filter engine writes a JSON file
     per item (`<filterID>-<k>.json`).  Downstream tasks consume
     these files.  Freshness is re-evaluated every run (remote source
     has no local mtime).
   - This keeps the entire pipeline file-driven: every piece of data
     flowing between tasks is a file on disk.
5. **Query index format:** **Python bridge** (Whoosh / pylucene via
   `PythonEngine`).  The C++ `FilterEngine` calls into a Python
   module `jcwf_query.py` that wraps the index library.  The bridge
   returns JSON-lines to C++.  This path was chosen to support
   large-scale index use cases planned for the future.
6. **Orphan cleanup:** No automatic deletion.  Runtime logs a warning
   for output files whose source items are no longer in the result set.
7. **Polarion REST API:** C++ `PolarionClient` class using libcurl
   (already vendored).  Paginated fetch with `page[size]=100`.
   Credentials via `KeyManager` (`key_name` field).  Peak memory
   bounded to one page.  Items written to disk incrementally.

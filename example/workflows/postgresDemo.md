# postgresDemo Workflow -- PostgreSQL Database Integration

## Executive Summary

The **postgresDemo** workflow demonstrates how JarvisAgent interacts with **PostgreSQL** through the cloud integration layer using the `db_query` task type.

At its core, this workflow shows:

- how `db_query` tasks execute SQL statements against a PostgreSQL database via libpq,
- how a named **CloudConnection** centralizes database credentials and host config,
- how results are written to disk in CSV and JSON formats,
- and how tasks chain via `depends_on` to build a create-insert-query pipeline.

---

## Prerequisites

1. A running PostgreSQL instance (local or remote)
2. A CloudConnection named `local-pg` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `postgres` |
| Endpoint | `localhost:5432` |
| Key | A KeyManager credential with username and password |
| Auth Type | `basic_auth` |
| Database | `j9t_test` |
| SSL Mode | `disable` (for local) or `require` (for remote) |

Credentials are stored as **BasicAuth**: username and password in the KeyManager.

---

## Pipeline Overview

```
+-----------------+
|  create_table   |
|  db_query: DDL  |
|  (01_create)    |
+--------+--------+
         |
         v
+-----------------+
|  insert_data    |
|  db_query: INS  |
|  (02_insert)    |
+--------+--------+
         |
    +----+----+
    |         |
    v         v
+--------+ +-------------+
|query   | |query_json   |
|csv     | |db_query:JSON |
|(03_)   | |(04_)         |
+--------+ +-------------+
```

---

## Task Details

### 1. create_table -- create test table

Drops and recreates a `j9t_demo` table with columns for id, name, score, and timestamp.

| Field | Value |
|-------|-------|
| Type | `db_query` |
| Connection | `local-pg` |
| Query | `DROP TABLE IF EXISTS j9t_demo; CREATE TABLE j9t_demo (id SERIAL PRIMARY KEY, name TEXT NOT NULL, score INTEGER, created_at TIMESTAMP DEFAULT NOW())` |
| Working dir | `postgresDemo/01_create_table` |

### 2. insert_data -- insert sample rows

Inserts five sample rows into the table.

| Field | Value |
|-------|-------|
| Type | `db_query` |
| Connection | `local-pg` |
| Query | `INSERT INTO j9t_demo (name, score) VALUES ('Alice', 95), ('Bob', 82), ('Charlie', 91), ('Diana', 78), ('Eve', 99)` |
| Working dir | `postgresDemo/02_insert` |
| Depends on | `create_table` |

### 3. query_csv -- query results as CSV

Queries all rows ordered by score descending and writes results to CSV format.

| Field | Value |
|-------|-------|
| Type | `db_query` |
| Connection | `local-pg` |
| Query | `SELECT name, score, created_at FROM j9t_demo ORDER BY score DESC` |
| Format | `csv` |
| Output file | `scores.csv` |
| Working dir | `postgresDemo/03_query` |
| Depends on | `insert_data` |

### 4. query_json -- query top scorers as JSON

Queries rows with score >= 90 and writes results to JSON format.

| Field | Value |
|-------|-------|
| Type | `db_query` |
| Connection | `local-pg` |
| Query | `SELECT name, score FROM j9t_demo WHERE score >= 90 ORDER BY score DESC` |
| Format | `json` |
| Output file | `top_scorers.json` |
| Working dir | `postgresDemo/04_query_json` |
| Depends on | `insert_data` |

---

## db_query Task Type Reference

The `db_query` task type is backed by `DbQueryCloudTaskExecutor`, which extends `ICloudTaskExecutor`. The base class automatically resolves the named connection and BasicAuth credentials before delegating to the PostgreSQL-specific logic.

### Common Params

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `postgres`) |
| `query` | yes | | SQL query or statement(s) |
| `format` | no | `csv` | Output format: `csv` or `json` |
| `output_file` | no | `result.csv`/`result.json` | Output filename |

### Output Formats

- **CSV**: RFC 4180 compliant (proper quoting/escaping). NULL values produce empty fields.
- **JSON**: Array of objects with column names as keys. NULL values produce JSON `null`.

---

## Running

```bash
# Start via REST API or web UI
curl -s -X POST http://localhost:8080/api/workflows/postgresDemo/run
```

Or click the play button in the workflow editor / dashboard.

---

## Expected Execution

### Task States at Completion

| Task | Final State | Notes |
|------|-------------|-------|
| `create_table` | Succeeded | Table created (or recreated) |
| `insert_data` | Succeeded | 5 rows inserted |
| `query_csv` | Succeeded | All rows exported to CSV |
| `query_json` | Succeeded | Top scorers exported to JSON |

### Output Files

| Path | Content |
|------|---------|
| `postgresDemo/03_query/scores.csv` | All 5 rows sorted by score descending |
| `postgresDemo/04_query_json/top_scorers.json` | 3 rows (Alice 95, Charlie 91, Eve 99) |

### Sample CSV Output

```csv
name,score,created_at
Eve,99,2026-04-09 18:30:00
Alice,95,2026-04-09 18:30:00
Charlie,91,2026-04-09 18:30:00
Bob,82,2026-04-09 18:30:00
Diana,78,2026-04-09 18:30:00
```

### Sample JSON Output

```json
[
  {"name": "Eve", "score": 99},
  {"name": "Alice", "score": 95},
  {"name": "Charlie", "score": 91}
]
```

---

## Key Concepts Demonstrated

- **Cloud task executor pattern** -- `ICloudTaskExecutor` resolves connection + credentials, delegates to `ExecuteCloud()`
- **Named connections** -- PostgreSQL host, database, SSL mode, and credentials are centralized in the Connections tab
- **libpq C API** -- direct PostgreSQL wire protocol, no ORM or wrapper library
- **Dual output formats** -- CSV for spreadsheet workflows, JSON for downstream API consumption
- **DDL + DML in same pipeline** -- tasks can create schema, populate data, and query results in a single workflow
- **DAG dependency** -- both query tasks fan out in parallel after insert completes

# snowflakeQueryDemo Workflow -- Snowflake SQL REST API Integration

## Executive Summary

The **snowflakeQueryDemo** workflow demonstrates how JarvisAgent executes SQL queries against **Snowflake** through the cloud integration layer using the Snowflake SQL REST API with RSA JWT authentication.

At its core, this workflow shows:

- how `snowflake_query` tasks submit SQL statements and poll for async results,
- how RSA key-pair authentication works via `JwtGenerator` (no passwords or shared secrets),
- how a named **CloudConnection** centralizes Snowflake account, warehouse, and credential config,
- how results are written to disk in both CSV and JSON formats,
- and how tasks chain via `depends_on` to build a DDL-then-DML-then-query pipeline.

---

## Prerequisites

1. A Snowflake account with SQL REST API access
2. An RSA key pair (2048-bit minimum) assigned to the Snowflake user:
   ```bash
   # Generate key pair
   openssl genrsa -out snowflake_rsa_key.pem 2048
   openssl rsa -in snowflake_rsa_key.pem -pubout -out snowflake_rsa_key.pub

   # Assign public key to Snowflake user (run in Snowflake)
   ALTER USER SVC_JARVIS SET RSA_PUBLIC_KEY='<public key without header/footer>';
   ```
3. A KeyManager credential (type `key_pair`) containing the RSA private key PEM
4. A CloudConnection named `my-snowflake` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `snowflake` |
| Endpoint | `xy12345.us-east-1` (account locator with region) |
| Key | A KeyManager credential with RSA private key |
| Auth Type | `jwt_rsa` |
| Account | `xy12345` |
| User | `SVC_JARVIS` |
| Warehouse | `COMPUTE_WH` |
| Database | `ANALYTICS` |
| Schema | `PUBLIC` |

---

## Pipeline Overview

```
+-----------------+
|  create_table   |
|  snowflake: DDL |
|  (01_create)    |
+--------+--------+
         |
         v
+-----------------+
|  insert_data    |
|  snowflake: INS |
|  (02_insert)    |
+--------+--------+
         |
    +----+----+
    |         |
    v         v
+--------+ +-------------+
|query   | |query_json   |
|csv     | |snowflake:JSON|
|(03_)   | |(04_)         |
+--------+ +-------------+
```

---

## Trigger

This workflow uses a **manual trigger** only. It will not start automatically when j9t loads -- it must be started explicitly via the web UI or REST API.

---

## Task Details

### 1. create_table -- create demo table

Creates a `j9t_demo` table with company name, region, revenue, and timestamp columns.

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `CREATE TABLE IF NOT EXISTS j9t_demo (id INTEGER AUTOINCREMENT, ...)` |
| Working dir | `snowflakeQueryDemo/01_create_table` |

### 2. insert_data -- insert sample rows

Inserts five sample company records with revenue data.

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `INSERT INTO j9t_demo (name, region, revenue) VALUES (...)` |
| Working dir | `snowflakeQueryDemo/02_insert` |
| Depends on | `create_table` |

### 3. query_csv -- full revenue report as CSV

Queries all rows ordered by revenue descending and writes results to CSV.

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `SELECT name, region, revenue, created_at FROM j9t_demo ORDER BY revenue DESC` |
| Output format | `csv` |
| Output file | `revenue_report.csv` |
| Working dir | `snowflakeQueryDemo/03_query_csv` |
| Depends on | `insert_data` |

### 4. query_json -- top revenue as JSON

Queries companies with revenue >= 1M and writes results to JSON.

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `SELECT name, region, revenue FROM j9t_demo WHERE revenue >= 1000000 ORDER BY revenue DESC` |
| Output format | `json` |
| Output file | `top_revenue.json` |
| Working dir | `snowflakeQueryDemo/04_query_json` |
| Depends on | `insert_data` |

---

## snowflake_query Task Type Reference

The `snowflake_query` task type is backed by `SnowflakeCloudTaskExecutor`, which extends `ICloudTaskExecutor`. The base class resolves the named connection and generates a fresh JWT before delegating to the Snowflake-specific logic.

### Execution Flow

1. Generate JWT via `JwtGenerator::GenerateSnowflakeJwt()`
2. `POST /api/v2/statements` with SQL body -- receive `statementHandle`
3. Poll `GET /api/v2/statements/{handle}` until `message == "Statement executed successfully."`
4. Parse result set (jsonv2 format): column names from `resultSetMetaData.rowType`, row values from `data` array
5. Write to output file (CSV or JSON) in the task working directory
6. Save raw Snowflake response to `response.json`

### Common Params

| Param | Required | Default | Description |
|-------|----------|---------|-------------|
| `connection` | yes | | Named CloudConnection (type `snowflake`) |
| `query` | yes | | SQL statement |
| `warehouse` | no | connection default | Override warehouse for this query |
| `database` | no | connection default | Override database |
| `schema` | no | connection default | Override schema |
| `output_format` | no | `csv` | `csv` or `json` |
| `output_file` | no | auto | Output filename |
| `timeout` | no | 3600 | Statement timeout (seconds) |
| `poll_interval` | no | 2 | Async poll interval (seconds) |

---

## JWT Authentication

All requests include:
- `Authorization: Bearer <JWT>` -- RS256-signed JWT with 1-hour expiry
- `X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT` -- tells Snowflake to validate against the user's assigned public key

The JWT contains:
- `iss`: `{ACCOUNT}.{USER}.SHA256:{public_key_fingerprint}`
- `sub`: `{ACCOUNT}.{USER}`
- `iat`: current Unix time
- `exp`: current + 3600 seconds

No passwords, no shared secrets -- authentication is purely RSA key-pair based.

---

## Running

```bash
# Manual trigger only
curl -s -X POST http://localhost:8080/api/workflows/snowflakeQueryDemo/run
```

Or click the play button in the workflow editor / dashboard.

---

## Expected Execution

### Task States at Completion

| Task | Final State | Notes |
|------|-------------|-------|
| `create_table` | Succeeded | Table created (or already exists) |
| `insert_data` | Succeeded | 5 rows inserted |
| `query_csv` | Succeeded | All rows exported to CSV |
| `query_json` | Succeeded | High-revenue companies exported to JSON |

### Output Files

| Path | Content |
|------|---------|
| `snowflakeQueryDemo/03_query_csv/revenue_report.csv` | All 5 rows sorted by revenue |
| `snowflakeQueryDemo/04_query_json/top_revenue.json` | 3 rows (Cyberdyne, Initech, Acme Corp) |
| `snowflakeQueryDemo/*/response.json` | Raw Snowflake API response per task |

---

## Key Concepts Demonstrated

- **RSA JWT authentication** -- no passwords, cryptographic key-pair auth via `JwtGenerator`
- **Async SQL execution** -- submit-then-poll pattern for long-running queries
- **Cooperative cancellation** -- workflow cancel propagates to Snowflake `POST .../cancel`
- **Cloud task executor pattern** -- `ICloudTaskExecutor` resolves connection + credentials
- **Named connections** -- account, warehouse, database, schema centralized in Connections tab
- **Dual output formats** -- CSV for spreadsheets, JSON for downstream API consumption
- **DAG dependency** -- both query tasks fan out in parallel after insert completes

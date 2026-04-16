# snowflakeQueryDemo Workflow -- Snowflake Per-Item Round-Trip

## Executive Summary

The **snowflakeQueryDemo** workflow is a true Snowflake round-trip demo with AI-driven per-item classification. It creates a database and two tables, inserts 8 sales records, queries aggregated region stats to CSV, fans out to an AI classifier that labels each region STRONG/MODERATE/WEAK, then INSERTs the verdict back into a dedicated analysis table per region. A final verify task SELECTs counts from both tables to confirm the round-trip completed. Round-trip = read external -> process with AI -> write back to the same external system.

The AI classifier is constrained to output a single word (STRONG, MODERATE, or WEAK) so its `captured_stdout` can be wired straight into the downstream `write_analysis` INSERT statement via template variable -- no Python parsing step needed.

---

## Prerequisites

### 1. Snowflake account

A Snowflake account with SQL REST API access. You need the **org-account identifier** (e.g. `TVXEFHO-JHB68153`), which you can find in the Snowflake UI under **Admin -> Accounts**.

### 2. RSA key pair for JWT authentication

Snowflake JWT auth uses RSA key pairs -- no passwords, no shared secrets.

```bash
# Generate 2048-bit RSA key pair
openssl genrsa -out snowflake_rsa_key.pem 2048
openssl rsa -in snowflake_rsa_key.pem -pubout -out snowflake_rsa_key.pub

# Assign public key to your Snowflake user (run in Snowflake worksheet)
ALTER USER SVC_JARVIS SET RSA_PUBLIC_KEY='<public key without header/footer>';
```

### 3. Store the RSA private key in KeyManager

```bash
curl -sk -X POST https://localhost:8443/api/settings/providers \
  -H 'Content-Type: application/json' \
  -d '{
    "name":"snowflake-key",
    "display_name":"Snowflake RSA Key",
    "api_key":"<paste entire PEM including BEGIN/END lines>",
    "credential_type":"key_pair",
    "api_type":"snowflake"
  }'
curl -sk -X POST https://localhost:8443/api/settings/providers/save -d '{}'
```

### 4. Create the `my-snowflake` CloudConnection

```bash
curl -sk -X POST https://localhost:8443/api/connections \
  -H 'Content-Type: application/json' \
  -d '{
    "name":"my-snowflake",
    "type":"snowflake",
    "endpoint":"TVXEFHO-JHB68153",
    "key_name":"snowflake-key",
    "auth_type":"jwt_rsa",
    "params":{
      "account":"TVXEFHO-JHB68153",
      "user":"SVC_JARVIS",
      "warehouse":"COMPUTE_WH",
      "database":"J9T_DEMO",
      "schema":"PUBLIC"
    }
  }'
curl -sk -X POST https://localhost:8443/api/connections/save -d '{}'
curl -sk -X POST https://localhost:8443/api/connections/my-snowflake/test -d '{}'
# -> {"ok":true}
```

| Field | Example |
|-------|---------|
| Type | `snowflake` |
| Endpoint | `TVXEFHO-JHB68153` (org-account identifier) |
| Key | A KeyManager credential with RSA private key PEM |
| Auth type | `jwt_rsa` |
| Account | `TVXEFHO-JHB68153` |
| User | `SVC_JARVIS` |
| Warehouse | `COMPUTE_WH` |
| Database | `J9T_DEMO` |
| Schema | `PUBLIC` |

---

## Trigger

Manual only.

```bash
curl -sk -X POST https://localhost:8443/api/workflows/snowflakeQueryDemo/run -d '{}'
```

---

## Task Graph

```
create_db (snowflake: CREATE DATABASE)
    |
    +------+------+
    |             |
    v             v
create_table  create_analysis_table
(snowflake:   (snowflake:
 j9t_demo)     j9t_demo_analysis)
    |             |
    +------+------+
           |
           v
      insert_data (snowflake: INSERT 8 rows)
           |
           v
      query_regions (snowflake: SELECT GROUP BY region -> CSV)
           |
           +---> filter "region-stats" (csv, binding: region)
           |
           v
      ai_analyze (per_item, AI classifies STRONG/MODERATE/WEAK)
           |
           v
      write_analysis (per_item, snowflake: INSERT verdict back)
           |
           v
        verify (snowflake: SELECT counts from both tables)
```

---

## Task Details

### 1. create_db -- create demo database

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `CREATE DATABASE IF NOT EXISTS J9T_DEMO` |
| Working dir | `snowflakeQueryDemo/00_create_db` |

Creates the `J9T_DEMO` database if it does not already exist. All downstream tasks use fully-qualified table names (`J9T_DEMO.PUBLIC.table`) so the connection-level database/schema defaults are not relied upon.

### 2. create_table -- create sales data table

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `CREATE OR REPLACE TABLE J9T_DEMO.PUBLIC.j9t_demo (id INTEGER AUTOINCREMENT, name STRING, region STRING, revenue FLOAT, created_at TIMESTAMP_NTZ DEFAULT CURRENT_TIMESTAMP())` |
| Working dir | `snowflakeQueryDemo/01_create_table` |
| Depends on | `create_db` |

### 3. create_analysis_table -- create analysis results table

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `CREATE OR REPLACE TABLE J9T_DEMO.PUBLIC.j9t_demo_analysis (id INTEGER AUTOINCREMENT, region STRING NOT NULL, verdict STRING NOT NULL, analysis_file STRING, created_at TIMESTAMP_NTZ DEFAULT CURRENT_TIMESTAMP())` |
| Working dir | `snowflakeQueryDemo/01b_create_analysis` |
| Depends on | `create_db` |

Runs in parallel with `create_table` -- both depend only on `create_db`.

### 4. insert_data -- insert 8 sample sales records

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `INSERT INTO J9T_DEMO.PUBLIC.j9t_demo (name, region, revenue) VALUES ('Acme Corp', 'EMEA', 1250000.50), ('Globex Inc', 'APAC', 890000.00), ('Initech', 'AMER', 2100000.75), ('Umbrella LLC', 'EMEA', 430000.25), ('Cyberdyne', 'AMER', 3200000.00), ('Soylent Corp', 'APAC', 670000.00), ('Tyrell Corp', 'EMEA', 980000.00), ('Weyland Corp', 'APAC', 1450000.50)` |
| Working dir | `snowflakeQueryDemo/02_insert` |
| Depends on | `create_table`, `create_analysis_table` |

Waits for both table creation tasks before inserting. Three regions (AMER, EMEA, APAC) with 2-3 companies each.

### 5. query_regions -- aggregate region stats to CSV

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `SELECT region AS "region", COUNT(*) AS "company_count", ROUND(SUM(revenue)) AS "total_revenue", ROUND(AVG(revenue)) AS "avg_revenue", ROUND(MIN(revenue)) AS "min_revenue", ROUND(MAX(revenue)) AS "max_revenue" FROM J9T_DEMO.PUBLIC.j9t_demo GROUP BY region ORDER BY "total_revenue" DESC` |
| Output format | `csv` |
| Output file | `region_stats.csv` |
| Working dir | `snowflakeQueryDemo/03_query_regions` |
| Depends on | `insert_data` |

The column aliases are **lowercase and double-quoted** (`AS "region"`, not `AS REGION`). This is critical -- Snowflake uppercases unquoted identifiers, and the downstream CSV filter bindings use lowercase names (`{{region.region}}`, `{{region.total_revenue}}`). Without quoting, the CSV headers would be `REGION`, `TOTAL_REVENUE`, etc. and the template variables would not resolve.

The resulting CSV has 3 rows (one per region) and feeds the `region-stats` filter.

### 6. ai_analyze -- AI classifies each region (per-item)

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `per_item` |
| Filter | `region-stats` (csv, binding: `region`) |
| Working dir | `../../queue/snowflakeQueryDemo/04_ai_analyze` |
| Depends on | `query_regions` |
| Queue binding | STNG + CNTX + TASK + PROB files |

The filter sources `03_query_regions/region_stats.csv` and binds each row as `region`. The AI runs once per region (3 times total) with PROB files templated per item:

```
Region: {{region.region}}
Company Count: {{region.company_count}}
Total Revenue: {{region.total_revenue}}
...
```

The system prompt constrains the AI to output **exactly one word**: STRONG, MODERATE, or WEAK. The thresholds are embedded in the TASK file (STRONG = total above 2500000 or avg above 1000000, WEAK = total below 1000000 or avg below 500000). `captured_stdout` ends up as the literal string `"STRONG"`, `"MODERATE"`, or `"WEAK"`.

### 7. write_analysis -- INSERT verdict back to Snowflake (per-item)

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Mode | `per_item` |
| Filter | `region-stats` |
| Connection | `my-snowflake` |
| Query | `INSERT INTO J9T_DEMO.PUBLIC.j9t_demo_analysis (region, verdict, analysis_file) VALUES ('{{region.region}}', '{{ai_analyze.captured_stdout}}', '{{ai_analyze.output_file}}')` |
| Working dir | `snowflakeQueryDemo/05_write_analysis` |
| Depends on | `ai_analyze` |

Runs once per region, inserting the AI's verdict and the path to its output file. Both `{{ai_analyze.captured_stdout}}` and `{{ai_analyze.output_file}}` resolve to the matching item's output for the same row index -- the runtime tracks per-item correspondence automatically.

### 8. verify -- confirm round-trip with row counts

| Field | Value |
|-------|-------|
| Type | `snowflake_query` |
| Connection | `my-snowflake` |
| Query | `SELECT 'j9t_demo' AS table_name, COUNT(*) AS row_count FROM J9T_DEMO.PUBLIC.j9t_demo UNION ALL SELECT 'j9t_demo_analysis', COUNT(*) FROM J9T_DEMO.PUBLIC.j9t_demo_analysis` |
| Output format | `csv` |
| Output file | `verification.csv` |
| Working dir | `snowflakeQueryDemo/06_verify` |
| Depends on | `write_analysis` |

Expected output: `j9t_demo` has 8 rows, `j9t_demo_analysis` has 3 rows (one per region).

---

## Expected Execution

### Task States at Completion

| Task | Final State | Notes |
|------|-------------|-------|
| `create_db` | Succeeded | Database created (or already exists) |
| `create_table` | Succeeded | Sales table created |
| `create_analysis_table` | Succeeded | Analysis table created (parallel with above) |
| `insert_data` | Succeeded | 8 rows inserted |
| `query_regions` | Succeeded | 3-row CSV: AMER, EMEA, APAC |
| `ai_analyze` | Succeeded (x3) | Per-item: STRONG, MODERATE, or WEAK per region |
| `write_analysis` | Succeeded (x3) | Per-item: INSERT into analysis table per region |
| `verify` | Succeeded | 8 sales rows + 3 analysis rows confirmed |

### Output Files

| Path | Content |
|------|---------|
| `snowflakeQueryDemo/03_query_regions/region_stats.csv` | 3 rows: region, company_count, total_revenue, avg_revenue, min_revenue, max_revenue |
| `snowflakeQueryDemo/06_verify/verification.csv` | 2 rows: j9t_demo=8, j9t_demo_analysis=3 |
| `snowflakeQueryDemo/*/response.json` | Raw Snowflake API response per task |
| `queue/snowflakeQueryDemo/04_ai_analyze/PROB_*.output.txt` | AI classification output per region |

---

## Key Concepts Demonstrated

- **True round-trip pattern** -- create tables, query data, AI classifies, write verdicts back to the same Snowflake database
- **RSA JWT authentication** -- no passwords, no shared secrets; RS256-signed JWT with `X-Snowflake-Authorization-Token-Type: KEYPAIR_JWT` header, validated against the user's assigned public key
- **Snowflake SQL REST API async polling** -- `POST /api/v2/statements` returns a `statementHandle`, executor polls `GET /api/v2/statements/{handle}` until completion; each request carries a single SQL statement (Snowflake REST API limitation)
- **User-Agent header required** -- Snowflake's REST API rejects requests without a `User-Agent` header; the executor sets this automatically
- **Per-item fan-out with AI classification** -- CSV filter spawns one AI call per region row; strict-output prompt constrains the model to a single word
- **Per-item output piping for write-back** -- `{{ai_analyze.captured_stdout}}` and `{{ai_analyze.output_file}}` resolve to the matching item's output for the same row index, no Python glue needed
- **Fully-qualified table names** -- `J9T_DEMO.PUBLIC.j9t_demo` avoids reliance on connection-level database/schema defaults, making the workflow portable across warehouse configurations
- **Lowercase quoted column aliases** -- `SELECT region AS "region"` ensures CSV headers match the lowercase template binding names (`{{region.region}}`); without quoting, Snowflake uppercases identifiers and bindings would fail to resolve
- **Single statement per request** -- the Snowflake SQL REST API does not support multi-statement batches; the workflow uses separate tasks for CREATE DATABASE, CREATE TABLE, INSERT, and SELECT

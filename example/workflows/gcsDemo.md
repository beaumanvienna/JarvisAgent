# gcsDemo Workflow -- GCS Round-Trip with Per-Item AI Analysis

## Executive Summary

The **gcsDemo** workflow demonstrates a complete Google Cloud Storage round-trip
with per-item fan-out: upload regional sales data, download it, fan out per region
for AI sales analysis, and upload each AI report back to the bucket.

This workflow shows:

- `gcs_upload` / `gcs_download` task types with service account JWT auth,
- named **CloudConnection** for centralized GCS credentials,
- **per-item fan-out** via CSV filter -- one AI call per region,
- **per-item output piping** via `{{ai_analyze.output_file}}` for cloud write-back,
- and linear + fan-out task chaining.

---

## Prerequisites

1. Google Cloud Storage or fake-gcs-server emulator
2. A CloudConnection named `my-gcs` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `gcs` |
| Endpoint | `http://localhost:4443` (fake-gcs-server) or empty for production GCS |
| Key | A KeyManager credential (KeyPair) with service account private key PEM |
| Auth Type | `jwt_rsa` |
| Bucket | `j9t-demo` |
| Service Account Email | `j9t@myproject.iam.gserviceaccount.com` |

For local testing with fake-gcs-server:
```bash
docker run -d --name fake-gcs -p 4443:4443 \
  fsouza/fake-gcs-server -scheme http

# Create the bucket:
curl -X POST "http://localhost:4443/storage/v1/b" \
  -H "Content-Type: application/json" \
  -d '{"name": "j9t-demo"}'
```

With fake-gcs-server, the JWT is used directly as a bearer token (no token
exchange). Any valid KeyPair credential will work -- the emulator does not
verify signatures.

---

## Pipeline Overview

```
+----------------+     +----------------+     +-------------------+     +-------------------+
|  upload_data   | --> |  download_data | --> |  ai_analyze       | --> |  upload_reports   |
|  gcs: upload   |     |  gcs: download |     |  ai_call          |     |  gcs: upload      |
|                |     |                |     |  per_item (5x)    |     |  per_item (5x)    |
|  (01_upload)   |     |  (02_download) |     |  (03_ai_analyze)  |     |  (04_upload_rpts) |
+----------------+     +----------------+     +-------------------+     +-------------------+
                                                     |
                                              CSV filter: regions
                                              (5 regions fan-out)
```

---

## Task Details

### 1. upload_data -- upload sample CSV to GCS

Uploads `regional_sales.csv` (bundled in the .jcwf) to Google Cloud Storage.

| Field | Value |
|-------|-------|
| Type | `gcs_upload` |
| Connection | `my-gcs` |
| Bucket | `j9t-demo` |
| Object name | `regional_sales.csv` |

### 2. download_data -- download from GCS

Downloads the CSV back to provide input for the CSV filter.

| Field | Value |
|-------|-------|
| Type | `gcs_download` |
| Connection | `my-gcs` |
| Bucket | `j9t-demo` |
| Object name | `regional_sales.csv` |

### 3. ai_analyze -- AI analyzes each region (per-item)

A CSV filter fans out 5 items (one per region). Each AI instance receives
the region's revenue, growth, rep count, and top product, and writes a 2-3
sentence sales analysis.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `per_item` |
| Filter | `regions` (CSV: `regional_sales.csv`) |
| Context | `{{reg.region}}`, `{{reg.q1_revenue_k}}`, etc. |

### 4. upload_reports -- upload each AI report back (per-item)

Each per-item instance uploads the corresponding AI analysis back to
GCS under `reports/{region}_analysis.txt`.

| Field | Value |
|-------|-------|
| Type | `gcs_upload` |
| Mode | `per_item` |
| Object name | `reports/{{reg.region}}_analysis.txt` |
| Local path | `{{ai_analyze.output_file}}` |

---

## Sample Data

The workflow includes `regional_sales.csv`:

```csv
region,q1_revenue_k,q2_revenue_k,growth_pct,rep_count,top_product
EMEA,1200,1350,12.5,45,Enterprise Suite
APAC,800,920,15.0,32,Cloud Platform
Americas,2100,2050,-2.4,68,Enterprise Suite
LATAM,340,410,20.6,12,Starter Pack
DACH,550,580,5.5,18,Enterprise Suite
```

---

## Expected Output

After a successful run, the GCS bucket `j9t-demo` contains:

| Object | Description |
|--------|-------------|
| `regional_sales.csv` | Original regional sales data |
| `reports/EMEA_analysis.txt` | AI analysis: 12.5% growth, strong performance |
| `reports/APAC_analysis.txt` | AI analysis: 15% growth, expanding market |
| `reports/Americas_analysis.txt` | AI analysis: -2.4% decline, needs attention |
| `reports/LATAM_analysis.txt` | AI analysis: 20.6% growth, fastest growing |
| `reports/DACH_analysis.txt` | AI analysis: 5.5% growth, steady performance |

---

## Key Concepts Demonstrated

- **GCS round-trip** -- upload, download, AI process, upload results back
- **Service account JWT** -- RS256-signed JWT exchanged for OAuth2 access token
- **Per-item fan-out** -- CSV filter creates 5 parallel AI instances
- **Per-item output piping** -- `{{ai_analyze.output_file}}` pipes each AI output to the matching upload
- **Named connections** -- GCS bucket, service account email, and credentials centralized
- **fake-gcs-server compatible** -- fully testable locally with Docker emulator

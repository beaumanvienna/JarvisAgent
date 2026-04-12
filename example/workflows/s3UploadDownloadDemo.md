# s3UploadDownloadDemo Workflow -- S3 Round-Trip with AI Analysis

## Executive Summary

The **s3UploadDownloadDemo** workflow demonstrates a complete S3 round-trip:
upload sample data, download it, have AI analyze the content, and upload the
AI report back to S3.

This workflow shows:

- `s3` task types for upload, download, and list operations with SigV4 signing,
- named **CloudConnection** for centralized S3 credentials and endpoint config,
- AI processing of cloud-sourced data,
- and linear task chaining via `depends_on`.

---

## Prerequisites

1. An S3-compatible service (AWS S3, MinIO, Wasabi, Cloudflare R2, etc.)
2. A CloudConnection named `my-s3` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `s3` |
| Endpoint | *(empty for AWS, or e.g. `http://localhost:9000` for MinIO)* |
| Key | A KeyManager credential with access key ID and secret key |
| Auth Type | `sigv4` |
| Region | `us-east-1` |
| Bucket | `j9t-test` |

For local testing with MinIO:
```bash
docker run -d --name minio -p 9000:9000 -p 9001:9001 \
  -e MINIO_ROOT_USER=minioadmin -e MINIO_ROOT_PASSWORD=minioadmin123 \
  minio/minio server /data --console-address ":9001"
docker exec minio mc mb /data/j9t-test
```

---

## Pipeline Overview

```
+----------------+     +----------------+     +-----------------+     +------------------+     +----------------+
|  upload_data   | --> |  download_data | --> |  ai_analyze     | --> |  upload_report   | --> |  list_objects  |
|  s3: upload    |     |  s3: download  |     |  ai_call        |     |  s3: upload      |     |  s3: list      |
|  (01_upload)   |     |  (02_download) |     |  (03_ai_analyze)|     |  (04_upload_rpt) |     |  (05_list)     |
+----------------+     +----------------+     +-----------------+     +------------------+     +----------------+
```

---

## Task Details

### 1. upload_data -- upload sample CSV to S3

Uploads `server_metrics.csv` (bundled in the .jcwf) to S3.

| Field | Value |
|-------|-------|
| Type | `s3` |
| Connection | `my-s3` |
| Operation | `upload` |
| Key | `j9t-demo/server_metrics.csv` |
| File path | `workflows/s3UploadDownloadDemo/server_metrics.csv` |

### 2. download_data -- download from S3

Downloads the CSV back to verify the upload and provide input for AI analysis.

| Field | Value |
|-------|-------|
| Type | `s3` |
| Connection | `my-s3` |
| Operation | `download` |
| Key | `j9t-demo/server_metrics.csv` |
| File path | `workflows/s3UploadDownloadDemo/02_download/metrics.csv` |

### 3. ai_analyze -- AI analyzes server metrics

The downloaded CSV is provided as context to the AI, which identifies servers
needing attention and recommends actions.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Context | Downloaded `metrics.csv` |
| Role | DevOps engineer analyzing server health |
| Output | Plain text analysis |

### 4. upload_report -- upload AI report back to S3

Uploads the AI-generated health report to S3, completing the round-trip.

| Field | Value |
|-------|-------|
| Type | `s3` |
| Connection | `my-s3` |
| Operation | `upload` |
| Key | `j9t-demo/ai_health_report.txt` |
| File path | AI output file |

### 5. list_objects -- verify bucket contents

Lists all objects under the `j9t-demo/` prefix to confirm both the original
data and the AI report exist in the bucket.

| Field | Value |
|-------|-------|
| Type | `s3` |
| Connection | `my-s3` |
| Operation | `list` |
| Prefix | `j9t-demo/` |

---

## Sample Data

The workflow includes `server_metrics.csv`:

```csv
server,cpu_pct,mem_pct,disk_pct,status
web-01,82,65,45,healthy
web-02,95,78,52,warning
db-01,45,88,71,healthy
db-02,38,92,85,critical
cache-01,12,34,22,healthy
```

---

## Expected Output

After a successful run, the S3 bucket contains:

| Object | Size | Description |
|--------|------|-------------|
| `j9t-demo/server_metrics.csv` | ~160 bytes | Original server metrics |
| `j9t-demo/ai_health_report.txt` | ~500 bytes | AI-generated health analysis |

The AI report identifies web-02 (warning: high CPU) and db-02 (critical: high
memory + disk) as needing immediate attention, with recommended actions.

---

## Key Concepts Demonstrated

- **S3 round-trip** -- upload, download, AI process, upload result back
- **Named connections** -- S3 endpoint, region, bucket, credentials centralized in Connections tab
- **SigV4 signing** -- every request is cryptographically signed (HMAC-SHA256)
- **S3-compatible** -- works with AWS S3, MinIO, Wasabi, R2, and any S3-compatible API
- **Disk-first output** -- all responses written to `response.json` in task working directory
- **AI integration** -- cloud-sourced data processed by AI, results written back to cloud

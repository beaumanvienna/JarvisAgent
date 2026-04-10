# s3UploadDownloadDemo Workflow -- S3 Object Storage Integration

## Executive Summary

The **s3UploadDownloadDemo** workflow demonstrates how JarvisAgent interacts with **S3-compatible object storage** through the cloud integration layer.

At its core, this workflow shows:

- how `s3` task types upload and download objects using SigV4-signed requests,
- how a named **CloudConnection** centralizes S3 credentials and endpoint config,
- how `list` operations enumerate bucket contents,
- and how tasks chain via `depends_on` to build upload-then-download pipelines.

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
| Bucket | `my-bucket` |

Credentials can be stored as:
- **BasicAuth**: username = access key ID, password = secret access key
- **ApiKey**: `ACCESS_KEY_ID:SECRET_ACCESS_KEY` format

---

## Pipeline Overview

```
+-----------------+
|  create_file    |
|  shell: echo    |
|  (01_create)    |
+--------+--------+
         |
         v
+-----------------+
|  upload         |
|  s3: upload     |
|  (02_upload)    |
+--------+--------+
         |
    +----+----+
    |         |
    v         v
+--------+ +-------------+
|download| |list_objects  |
|s3: down| |s3: list      |
|(03_)   | |(04_list)     |
+--------+ +-------------+
```

---

## Task Details

### 1. create_file -- generate sample data

Creates a sample text file with a timestamp to upload.

| Field | Value |
|-------|-------|
| Type | `shell` |
| Script | `scripts/run.sh` |
| Args | `echo`, `Hello from j9t S3 demo at $(date)` |
| Working dir | `s3UploadDownloadDemo/01_create` |
| Output | `stdout.txt` |

### 2. upload -- upload to S3

Uploads the generated file to the S3 bucket under the key `j9t-demo/hello.txt`.

| Field | Value |
|-------|-------|
| Type | `s3` |
| Connection | `my-s3` |
| Operation | `upload` |
| Key | `j9t-demo/hello.txt` |
| File path | `s3UploadDownloadDemo/01_create/stdout.txt` |
| Working dir | `s3UploadDownloadDemo/02_upload` |
| Depends on | `create_file` |

The task executor reads the local file, computes its SHA-256 hash for SigV4 signing, and sends a `PUT` request to S3.

### 3. download -- download from S3

Downloads the same object back to a local file to verify the round-trip.

| Field | Value |
|-------|-------|
| Type | `s3` |
| Connection | `my-s3` |
| Operation | `download` |
| Key | `j9t-demo/hello.txt` |
| File path | `s3UploadDownloadDemo/03_download/downloaded.txt` |
| Working dir | `s3UploadDownloadDemo/03_download` |
| Depends on | `upload` |

### 4. list_objects -- list bucket contents

Lists all objects under the `j9t-demo/` prefix. The response is an S3 `ListBucketResult` XML document written to `response.json` in the task working directory.

| Field | Value |
|-------|-------|
| Type | `s3` |
| Connection | `my-s3` |
| Operation | `list` |
| Prefix | `j9t-demo/` |
| Working dir | `s3UploadDownloadDemo/04_list` |
| Depends on | `upload` |

---

## S3 Task Type Reference

The `s3` task type is backed by `S3CloudTaskExecutor`, which extends `ICloudTaskExecutor`. The base class automatically resolves the named connection and SigV4 credentials before delegating to the S3-specific logic.

### Supported Operations

| Operation | Required Params | Description |
|-----------|----------------|-------------|
| `upload` | `key`, `file_path` | PUT object to S3 |
| `download` | `key`, `file_path` | GET object from S3 to local file |
| `list` | `prefix` (optional) | ListObjectsV2 |
| `delete` | `key` | DELETE object |

### Common Params

| Param | Required | Description |
|-------|----------|-------------|
| `connection` | yes | Named CloudConnection (type `s3`) |
| `operation` | yes | One of: `upload`, `download`, `list`, `delete` |
| `bucket` | no | Override the connection's default bucket |

---

## SigV4 Authentication

All S3 requests are authenticated using **AWS Signature Version 4** (`SigV4Signer`). The signer:

1. Builds a canonical request (method, path, query, headers, payload hash)
2. Derives a signing key from the secret access key + date + region + service
3. Produces an `Authorization` header with the HMAC-SHA256 signature

This is compatible with AWS S3, MinIO, Wasabi, and other S3-compatible services.

---

## Running

```bash
# Manual start only (manual_start: true)
curl -s -X POST http://localhost:8080/api/workflows/s3UploadDownloadDemo/run
```

---

## Expected Execution

### Task States at Completion

| Task | Final State | Notes |
|------|-------------|-------|
| `create_file` | Succeeded | Generated sample text file |
| `upload` | Succeeded | Uploaded to `s3://bucket/j9t-demo/hello.txt` |
| `download` | Succeeded | Downloaded to local `downloaded.txt` |
| `list_objects` | Succeeded | Listed objects under `j9t-demo/` prefix |

### Output Files

| Path | Content |
|------|---------|
| `s3UploadDownloadDemo/01_create/stdout.txt` | Sample text with timestamp |
| `s3UploadDownloadDemo/02_upload/response.json` | `{"ok":true,"operation":"upload",...}` |
| `s3UploadDownloadDemo/03_download/downloaded.txt` | Copy of the uploaded file |
| `s3UploadDownloadDemo/03_download/response.json` | `{"ok":true,"operation":"download",...}` |
| `s3UploadDownloadDemo/04_list/response.json` | S3 ListBucketResult XML |

---

## Key Concepts Demonstrated

- **Cloud task executor pattern** -- `ICloudTaskExecutor` resolves connection + credentials, delegates to `ExecuteCloud()`
- **Named connections** -- S3 endpoint, region, bucket, and credentials are centralized in the Connections tab
- **SigV4 signing** -- every request is cryptographically signed using HMAC-SHA256
- **S3-compatible** -- works with AWS S3, MinIO, Wasabi, R2, and any S3-compatible API
- **Disk-first output** -- all responses are written to `response.json` in the task working directory
- **DAG dependency** -- download and list both depend on upload completing first

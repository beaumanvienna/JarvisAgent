# azureBlobDemo Workflow -- Azure Blob Round-Trip with Per-Item AI Analysis

## Executive Summary

The **azureBlobDemo** workflow demonstrates a complete Azure Blob Storage round-trip
with per-item fan-out: upload project budget data, download it, fan out per project
for AI risk assessment, and upload each AI report back to the container.

This workflow shows:

- `azure_blob_upload` / `azure_blob_download` task types with Shared Key signing,
- named **CloudConnection** for centralized Azure Storage credentials,
- **per-item fan-out** via CSV filter -- one AI call per project,
- **per-item output piping** via `{{ai_analyze.output_file}}` for cloud write-back,
- and linear + fan-out task chaining.

---

## Prerequisites

1. Azure Blob Storage or Azurite emulator
2. A CloudConnection named `my-azure-blob` configured in the **Connections** tab:

| Field | Example |
|-------|---------|
| Type | `azure_blob` |
| Endpoint | `http://127.0.0.1:10000/devstoreaccount1` (Azurite) |
| Key | A KeyManager credential with the Base64-encoded account key |
| Auth Type | `azure_shared_key` |
| Account Name | `devstoreaccount1` |
| Container | `j9t-demo` |

For local testing with Azurite:
```bash
docker run -d --name azurite -p 10000:10000 -p 10001:10001 -p 10002:10002 \
  mcr.microsoft.com/azure-storage/azurite

# Create the container (Azurite well-known dev key):
curl -X PUT "http://127.0.0.1:10000/devstoreaccount1/j9t-demo?restype=container" \
  -H "x-ms-version: 2024-11-04" \
  -H "x-ms-date: $(date -u '+%a, %d %b %Y %H:%M:%S GMT')" \
  -H "Authorization: SharedKey devstoreaccount1:<signature>"
```

Azurite credentials:
- Account name: `devstoreaccount1`
- Account key: `Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==`

---

## Pipeline Overview

```
+----------------+     +----------------+     +-------------------+     +-------------------+
|  upload_data   | --> |  download_data | --> |  ai_analyze       | --> |  upload_reports   |
|  azure_blob:   |     |  azure_blob:   |     |  ai_call          |     |  azure_blob:      |
|  upload        |     |  download      |     |  per_item (5x)    |     |  upload per_item  |
|  (01_upload)   |     |  (02_download) |     |  (03_ai_analyze)  |     |  (04_upload_rpts) |
+----------------+     +----------------+     +-------------------+     +-------------------+
                                                     |
                                              CSV filter: projects
                                              (5 projects fan-out)
```

---

## Task Details

### 1. upload_data -- upload sample CSV to Azure Blob

Uploads `project_budgets.csv` (bundled in the .jcwf) to Azure Blob Storage.

| Field | Value |
|-------|-------|
| Type | `azure_blob_upload` |
| Connection | `my-azure-blob` |
| Container | `j9t-demo` |
| Blob name | `project_budgets.csv` |

### 2. download_data -- download from Azure Blob

Downloads the CSV back to provide input for the CSV filter.

| Field | Value |
|-------|-------|
| Type | `azure_blob_download` |
| Connection | `my-azure-blob` |
| Container | `j9t-demo` |
| Blob name | `project_budgets.csv` |

### 3. ai_analyze -- AI risk-assesses each project (per-item)

A CSV filter fans out 5 items (one per project). Each AI instance receives
the project's budget, spend, status, and deadline, and writes a 2-3 sentence
risk assessment.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `per_item` |
| Filter | `projects` (CSV: `project_budgets.csv`) |
| Context | `{{proj.project}}`, `{{proj.budget_k}}`, etc. |

### 4. upload_reports -- upload each AI report back (per-item)

Each per-item instance uploads the corresponding AI risk assessment back to
Azure Blob Storage under `reports/{project}_risk.txt`.

| Field | Value |
|-------|-------|
| Type | `azure_blob_upload` |
| Mode | `per_item` |
| Blob name | `reports/{{proj.project}}_risk.txt` |
| Local path | `{{ai_analyze.output_file}}` |

---

## Sample Data

The workflow includes `project_budgets.csv`:

```csv
project,budget_k,spent_k,status,deadline
Atlas,500,480,at_risk,2026-06-01
Beacon,300,120,on_track,2026-09-15
Citadel,750,800,over_budget,2026-04-30
Delta,200,95,on_track,2026-12-01
Echo,400,390,at_risk,2026-05-15
```

---

## Expected Output

After a successful run, the Azure Blob container `j9t-demo` contains:

| Blob | Description |
|------|-------------|
| `project_budgets.csv` | Original project budget data |
| `reports/Atlas_risk.txt` | AI risk assessment for Atlas (96% burn, at risk) |
| `reports/Beacon_risk.txt` | AI risk assessment for Beacon (40% burn, on track) |
| `reports/Citadel_risk.txt` | AI risk assessment for Citadel (107% burn, over budget) |
| `reports/Delta_risk.txt` | AI risk assessment for Delta (48% burn, on track) |
| `reports/Echo_risk.txt` | AI risk assessment for Echo (98% burn, at risk) |

---

## Key Concepts Demonstrated

- **Azure Blob round-trip** -- upload, download, AI process, upload results back
- **Shared Key signing** -- HMAC-SHA256 authorization header per Azure Storage spec
- **Per-item fan-out** -- CSV filter creates 5 parallel AI instances
- **Per-item output piping** -- `{{ai_analyze.output_file}}` pipes each AI output to the matching upload
- **Named connections** -- Azure Storage account, container, and credentials centralized
- **Azurite compatible** -- fully testable locally with the Azure Storage emulator

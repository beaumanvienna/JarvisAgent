# Example Workflows

Ready-to-use JCWF workflow examples. Copy any `.jcwf` file to the `workflows/` folder to load it into j9t.

## AI Workflows

| Workflow | Description |
|----------|-------------|
| [aiCarMaintenancePipeline](aiCarMaintenancePipeline.md) | Multi-step car maintenance analysis with AI |
| [aiZipDemo](aiZipDemo.md) | Three parallel AI explanations + zip |
| [bookSummaryPipeline](bookSummaryPipeline.md) | Book chapter summarization pipeline |
| [jarvisCppDocu](jarvisCppDocu.md) | Auto-generate C++ header documentation |
| [vehicleTroubleshootingGuide](vehicleTroubleshootingGuide.md) | Vehicle diagnostic guide with PDF input |

## Integration Workflows

| Workflow | Description |
|----------|-------------|
| [hamburg-tourist-day-planner](hamburg-tourist-day-planner.md) | Webhook-triggered tourist day planner (n8n integration) |
| [goKartComplianceCheck](goKartComplianceCheck.md) | Polarion requirements compliance check with AI |

## Cloud Integration Demos

| Workflow | Task Types | Description |
|----------|-----------|-------------|
| [postgresDemo](postgresDemo.md) | `db_query` | Create table, insert, query to CSV/JSON |
| [s3UploadDownloadDemo](s3UploadDownloadDemo.md) | `s3` | Upload, download, list S3 objects |
| [oneDriveUploadDownloadDemo](oneDriveUploadDownloadDemo.md) | `onedrive_upload/download` | OneDrive file transfer via Graph API |
| [snowflakeQueryDemo](snowflakeQueryDemo.md) | `snowflake_query` | SQL queries with JWT RSA auth |
| [slackQAndABot](slackQAndABot.md) | `slack_read`, `ai_call`, `slack_message` | Slack round-trip: read user question, AI answers as C++/Vulkan expert, reply in thread |
| [emailDemo](emailDemo.md) | `email_read`, `email_send` | Email round-trip: IMAP read + AI reply + SMTP send |
| [gitHubIssueDemo](gitHubIssueDemo.md) | `github_issue` | Create/list GitHub issues |
| [jiraIssueDemo](jiraIssueDemo.md) | `jira_issue` | Create Jira issues |
| [redmineTriageBot](redmineTriageBot.md) | `redmine_issue`, `ai_call`, `python` | Redmine round-trip: list issues, AI classifies backend/frontend developers, AI writes triage comment, per-item update assigns + comments |
| [sheetsQuizGrader](sheetsQuizGrader.md) | `sheets_read/write` + `ai_call` | Read quiz from Google Sheets, AI grades answers, write results back |

## Build & Test Workflows

| Workflow | Description |
|----------|-------------|
| [exampleMakefile4](exampleMakefile4.md) | C compilation pipeline |
| [exampleMakefile5](exampleMakefile5.md) | Multi-file C build with dependencies |
| [make-example](make-example.md) | Simple make example with sub-workflow |
| [inputResolutionTest](inputResolutionTest.md) | Path resolution test workflow |

## Sample Data

| File | Used By |
|------|---------|
| [sheetsQuizGrader_sample_data.csv](sheetsQuizGrader_sample_data.csv) | sheetsQuizGrader — C++/Vulkan quiz questions |

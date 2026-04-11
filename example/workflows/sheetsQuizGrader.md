# sheetsQuizGrader Workflow -- Google Sheets + AI Grading

## Executive Summary

The **sheetsQuizGrader** workflow demonstrates the full Google Sheets read/write cycle combined with AI evaluation. It reads C++ and Vulkan coding quiz questions from a Google Sheet, uses an AI model to grade each answer, and writes the grades back to the sheet.

This workflow shows:

- how `sheets_read` reads a spreadsheet range to CSV,
- how `ai_call` evaluates structured data and produces CSV output,
- how `sheets_write` uploads CSV results back to the spreadsheet,
- and how the three task types chain together in a read-process-write pipeline.

---

## Prerequisites

1. A Google Sheets spreadsheet with the sample quiz data
2. A CloudConnection named `my-sheets` configured in the **Connections** tab
3. An AI provider configured (e.g., OpenAI GPT-4.1-mini)

### Setting up the quiz spreadsheet

1. Create a new Google Sheet
2. Import the sample data from `example/workflows/sheetsQuizGrader_sample_data.csv`:
   - The sheet should have **Column A** = Question, **Column B** = Answer
   - Row 1 is the header, rows 2-11 are the 10 quiz items
3. Note the spreadsheet ID from the URL: `https://docs.google.com/spreadsheets/d/{SPREADSHEET_ID}/edit`
4. Configure the `my-sheets` connection with this spreadsheet ID

### Quiz content

The sample data contains 10 C++ and Vulkan questions. Some answers are correct, some are deliberately wrong:

| # | Topic | Answer Quality |
|---|-------|---------------|
| 1 | VkInstance purpose | Correct |
| 2 | std::span in C++20 | Correct |
| 3 | GPU representation in Vulkan | Correct (VkDevice, though VkPhysicalDevice is more precise) |
| 4 | What std::move does | **Incorrect** (it casts to rvalue, it does not copy) |
| 5 | Vulkan render pass | Correct |
| 6 | RAII in C++ | Correct |
| 7 | VkBuffer vs VkImage | Correct |
| 8 | volatile keyword | Partially correct (missing the "no reordering" aspect) |
| 9 | Vulkan descriptor set | **Incorrect** (descriptor sets are not garbage-collected) |
| 10 | std::unique_ptr | Correct |

---

## Pipeline Overview

```
+-----------------+     +-----------------+     +-----------------+
|  read_quiz      | --> |  grade_answers  | --> |  write_grades   |
|  sheets_read    |     |  ai_call        |     |  sheets_write   |
|  (01_read)      |     |  (02_grade)     |     |  (03_write)     |
+-----------------+     +-----------------+     +-----------------+
```

---

## Trigger

Manual trigger only -- will not start at j9t startup.

---

## Task Details

### 1. read_quiz -- read quiz from Google Sheets

Reads the quiz questions and answers from the spreadsheet.

| Field | Value |
|-------|-------|
| Type | `sheets_read` |
| Connection | `my-sheets` |
| Range | `Sheet1!A1:B11` |
| Output Format | `csv` |
| Output File | `quiz_data.csv` |

### 2. grade_answers -- AI evaluates each answer

An AI model grades each answer as Correct, Partially Correct, or Incorrect with a brief explanation.

| Field | Value |
|-------|-------|
| Type | `ai_call` |
| Mode | `one_shot` |
| Output | `grades.csv` (3 columns: Grade, Score, Explanation) |
| Depends on | `read_quiz` |

The AI prompt includes all 10 questions and answers, asking the model to produce a CSV with grades (0/5/10), and one-sentence explanations.

### 3. write_grades -- write grades back to sheet

Writes the AI's grading CSV to Column C of the spreadsheet, adding Grade, Score, and Explanation columns alongside the original data.

| Field | Value |
|-------|-------|
| Type | `sheets_write` |
| Connection | `my-sheets` |
| Range | `Sheet1!C1` |
| Input File | `grades.csv` |
| Value Input Option | `USER_ENTERED` |
| Depends on | `grade_answers` |

---

## Running

```bash
curl -s -X POST http://localhost:8080/api/workflows/sheetsQuizGrader/run
```

---

## Expected Output

After the workflow completes, the Google Sheet will have 5 columns:

| A (Question) | B (Answer) | C (Grade) | D (Score) | E (Explanation) |
|---|---|---|---|---|
| What is VkInstance... | VkInstance is the connection... | Correct | 10 | Accurately describes VkInstance's role... |
| ... | ... | ... | ... | ... |
| What does std::move do... | std::move copies the object... | Incorrect | 0 | std::move performs an rvalue cast, not a copy... |
| ... | ... | ... | ... | ... |

Expected scores: ~70/100 (7 correct, 1 partial, 2 incorrect).

---

## Key Concepts Demonstrated

- **Google Sheets read + write** -- full round-trip: read CSV from sheet, process, write results back
- **AI-powered evaluation** -- `ai_call` task grades structured quiz data with explanations
- **Pipeline chaining** -- `sheets_read` -> `ai_call` -> `sheets_write` via `depends_on`
- **Sample data included** -- `sheetsQuizGrader_sample_data.csv` ready to import into Google Sheets
- **Mixed correctness** -- deliberately includes wrong answers to demonstrate AI grading accuracy

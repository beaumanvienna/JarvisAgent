# JarvisAgent Engine: Curl Wrapper Module Documentation

## Overview
The Curl Wrapper module provides a thread‑local, RAII‑safe wrapper around **libcurl**, used by JarvisAgent to send HTTP POST requests to the OpenAI API.  
It ensures:

- Global libcurl initialization happens once across the entire process  
- Each worker thread gets its own `CURL*` handle (thread‑local)  
- Clean shutdown and memory safety via RAII wrappers  
- Uniform request building (headers, POST body, write callback)  
- Centralized error logging and response buffering  
- API key extraction from `OPENAI_API_KEY` environment variable

This document covers the following components:

- `CurlManager`
- `CurlWrapper`
- Thread‑local CURL instances
- Query execution pipeline
- Header list RAII (`CurlSlist`)
- API key validation
- Response buffering
- Cleanup behavior

---

## 1. `CurlManager`
### Purpose
Provides a **thread-local instance** of `CurlWrapper`.

```cpp
static CurlWrapper& GetThreadCurl()
{
    thread_local CurlWrapper curl;
    return curl;
}
```

### Notes
- Each thread receives exactly one `CurlWrapper` instance.
- Ensures concurrency without locks, because `CURL*` handles are not shared.

---

## 2. `CurlWrapper` Class

### Responsibilities
- Initialize libcurl once globally (`curl_global_init`)
- Create and manage a per‑instance `CURL*` handle
- Configure headers and POST body
- Install write callback
- Accumulate the response body into an internal buffer
- Report errors through logger
- Delete/cleanup resources correctly

### Key Members
```cpp
CURL* m_Curl;               // Owned libcurl handle
bool m_Initialized;         // Whether initialization succeeded
std::string m_ReadBuffer;   // Accumulates response body
static std::string m_ApiKey;
static std::atomic<uint32_t> m_QueryCounter;
```

### Construction Behavior

1. Reads API key from environment:  
   `OPENAI_API_KEY`
2. Validates minimum length (`>= 8`)
3. Performs **global** libcurl initialization (once per process)
4. Creates a thread‑local `CURL*` handle via `curl_easy_init`

### Destruction Behavior
- Cleans up `m_Curl` using `curl_easy_cleanup`
- **Does not** call global cleanup; that is done explicitly via `CurlWrapper::GlobalCleanup()`

---

## 3. Sending a Query

### Input Structure
```cpp
struct QueryData
{
    std::string m_Url;   // Endpoint URL
    std::string m_Data;  // JSON POST body
    bool IsValid() const;
};
```

### Validation
A query is valid only if:

- URL is non‑empty  
- JSON data string is non‑empty

Otherwise logs show:

```
CurlWrapper::QueryData::IsValid(): url empty
CurlWrapper::QueryData::IsValid(): data empty
```

### Query Execution Flow

1. Validate initialization + query data  
2. Construct header list via `CurlSlist`  
3. Install headers:
   - `Authorization: Bearer <api_key>`
   - `Content-Type: application/json`
4. Install POST body and write callback
5. Call:
   ```cpp
   curl_easy_perform(m_Curl);
   ```
6. Log success or error
7. Response body available through `GetBuffer()`

---

## 4. Response Buffering
The write callback appends incoming chunks to the internal buffer:

```cpp
buffer->append(static_cast<char*>(contents), totalSize);
```

After the request:

```cpp
std::string& GetBuffer();
void Clear();
```

allow the engine to consume or reset the buffer.

---

## 5. `CurlSlist` (Header Management)

RAII wrapper around `curl_slist`.

```cpp
class CurlSlist
{
    curl_slist* m_List{nullptr};
    ~CurlSlist() { curl_slist_free_all(m_List); }
};
```

Ensures:

- No leaks
- Header allocation always paired with cleanup
- Safe even if `curl_slist_append` fails

---

## 6. API Key Handling

The API key is read **once globally**:

```cpp
char* apiKeyEnv = std::getenv("OPENAI_API_KEY");
```

Validation:
```cpp
bool CurlWrapper::IsValidKey(std::string const& key)
{
    return key.size() >= 8;
}
```

Failure logs a critical error.

---

## 7. Global Cleanup

Must be invoked at application shutdown:

```cpp
CurlWrapper::GlobalCleanup();
```

This calls:

```cpp
curl_global_cleanup();
```

---

## 8. Threading Model

- `curl_global_init` is called once under a mutex.
- Each thread receives its own `CurlWrapper` instance via thread_local storage.
- No CURL handle is shared across threads.

This design avoids:

- Races
- Cross‑thread reuse errors
- Manual locking

---

## 9. Error Handling & Logging

Uses engine‑side logging (CORE logger):

Examples:

```
thread 1234 got a good curl
curl_easy_init() failed
curl error: <error_string>
Response:
{...raw JSON...}
```

On failure, `Query()` returns:

```cpp
false
```

---

## 10. Summary of Guarantees

| Guarantee | Mechanism |
|----------|-----------|
| Thread-safe CURL handles | `thread_local CurlWrapper` |
| No global double-init | Static mutex + boolean gate |
| Memory safety | RAII (`CurlSlist`, destructor cleanup) |
| Unified logging | `LOG_CORE_INFO / CRITICAL / ERROR` |
| Valid API key enforcement | Length check |
| Response buffering | Internal `std::string` |

---

## 11. Minimal Usage Example

```cpp
auto& curl = CurlManager::GetThreadCurl();
curl.Clear();

CurlWrapper::QueryData q;
q.m_Url = "https://api.openai.com/v1/chat/completions";
q.m_Data = R"({"model":"gpt-5.1","messages":[{"role":"user","content":"hi"}]})";

if (curl.Query(q))
{
    std::string response = curl.GetBuffer();
    // parse via ReplyParser...
}
```

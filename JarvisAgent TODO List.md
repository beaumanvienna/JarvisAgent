# JarvisAgent TODO List (Core Items Only)

This list tracks the remaining essential work for JarvisAgent.

---

## 1. Status Line Renderer Cleanup  
**Problem:** Status line overwrites other log messages.  
**Goal:** Ensure logs remain readable.  
**Current plan:** Disable or restrict status line output until ANSI-based version is implemented.

---

## 2. File Categorizer Polish (Log Cleanliness)  
**Goal:**  
Reduce noise by removing unnecessary logs, especially for document formats handled by Python (PDF, DOCX, XLSX, PPTX).

---

## 3. Add GitHub Ubuntu CI Pipeline  
**Goal:**  
Automatic build + run tests on push/PR.  
- Build on Ubuntu  
- Run unit tests  
- Run formatting checks  
- Cache dependencies  
- Fails early if compilation breaks

---

## 4. Compile on Windows  
**Goal:**  
Enable full Windows compatibility.  
- Configure MSVC build  
- Resolve filesystem path differences  
- Add GitHub Windows CI

---

## 5. Dockerize JarvisAgent  
**Goal:**  
Provide a reproducible container environment.  
- Ubuntu base image  
- All required dependencies preinstalled  

---

## 6. Remove OnUpdate Python calls 
**Goal:**  
- OnUpdate() not used
- Events jam event queuw during long file conversion


---


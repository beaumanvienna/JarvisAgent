# JarvisAgent TODO List

This list tracks the remaining work for JarvisAgent.

---

## 1. Add GitHub Ubuntu CI Pipeline
**Goal:**  
Automatic build + run tests on push/PR.
- Build on Ubuntu  
- Fail early if compilation breaks
- Run unit tests  

---

## 2. Compile on Windows
**Goal:**  
Enable full Windows compatibility.
- Configure MSVC build  
- Resolve filesystem path differences  
- Add GitHub Windows CI

---

## 3. Dockerize JarvisAgent
**Goal:**  
Provide a reproducible container environment.
- Ubuntu base image  
- All required dependencies preinstalled  (markitdown and python 3.12)

---

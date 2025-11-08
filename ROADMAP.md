# JarvisAgent Roadmap

<br>

This document outlines the **next development milestones** for JarvisAgent.  
The focus is on expanding file format support, introducing a Python-based scripting layer, and adding a lightweight web interface for human–AI interaction.

---

## 1. Python Integration

**Goal:** Introduce Python as a flexible scripting backend for preprocessing and postprocessing tasks.  

- Embed Python (via CPython API) for optional automation and data transformation.
- Replace Lua with Python for higher ecosystem compatibility.
- Scripts can:
  - Preprocess input (e.g., convert documents to Markdown)
  - Postprocess responses (e.g., format output, apply templates)
  - Chain AI queries into automated pipelines.

If Python is unavailable, JarvisAgent will continue to operate in standalone mode without scripting.

---

## 2. MarkItDown Integration

**Goal:** Extend file format support beyond plain text.

- Integrate **Microsoft’s open-source MarkItDown** for automatic Markdown conversion of:
  - `.pdf`
  - `.docx`
  - `.pptx`
  - `.xlsx`
- Extract readable text while skipping binary/image content.
- Preserve meaningful references (e.g., figure captions, “see Figure 3”).
- Optionally generate local thumbnails for referenced images.

This will enable JarvisAgent to act as a **universal AI document processor** for structured and unstructured input formats.

---

## 3. Embedded Web Server

**Goal:** Provide an interactive browser-based chatbot interface.

- Implement a **minimal local web server** (e.g., `cpp-httplib` or `crow`).
- Serve a simple HTML/JS frontend for chat-style interaction.
- Each message sent through the chatbot:
  - Is written as a `.txt` file into the queue directory.
  - Triggers normal processing via JarvisAgent’s core engine.
  - Streams or polls the response for display in the browser.

JarvisAgent thus becomes both a **daemon** and a **conversational AI node** accessible through a web browser.

---

## 4. Planned Enhancements

- [ ] WebSocket-based response streaming  
- [ ] Python plugin system for task automation  
- [ ] Web dashboard for active queries and performance metrics  
- [ ] Persistent cache for processed outputs  
- [ ] Support for additional AI backends (Anthropic, local models)  

---

## 5. Long-Term Vision

JarvisAgent evolves into a **unified AI processing hub** — combining:
- On-disk file orchestration  
- Intelligent dependency tracking  
- Scriptable AI workflows via Python  
- Live conversational access via a local web interface  

The guiding principle remains the same:  
**Do only the work that’s necessary — but do it intelligently.**

---

GPL-3.0 License © 2025 JC Technolabs

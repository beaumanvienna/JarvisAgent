# Introduction
<br>
JarvisAgent is a console application that operates as a background service (agent). It monitors a queue folder for prompt and instruction files, sends them to an AI provider through a REST API, and stores the results in an output directory. <br>
<br>
It can perform AI-driven tasks and serve as a component for workflow automation.<br>
<br>

| Layer            | Responsibility                                                                                                           | Status |
| ---------------- | ------------------------------------------------------------------------------------------------------------------------ | ------ |
| **Engine**       | Networking (`libcurl`), logging (`spdlog`), JSON parsing (`simdjson`), threading (`BS thread-pool`), profiling (`tracy`) | ✅     |
| **Event System** | Thread-safe atomic event queue and dispatcher system for cross-thread communication                                      | ✅     |
| **Application**  | Orchestrates queue handling, event dispatching, and task flow                                                            | 🚧     |
| **Config**       | `config.json` with folder paths, thread count, AI backend model, and other settings                                      | ✅     |
| **I/O**          | Input queue folder watcher, output folder, file categorization                                                           | 🚧     |
| **Networking**   | ChatGPT REST API requests and response handling                                                                          | 🚧     |


<br> JarvisAgent processes different types of prompt files located in the queue folder defined in the configuration file `config.json`. Each file category serves a specific purpose, and files are identified using 4-letter, all-caps prefixes.<br>

| Category                  | Description                                                                    | Prefix                 | Example Filename                                          |
| ------------------------- | ------------------------------------------------------------------------------ | ---------------------- | --------------------------------------------------------- |
| **Settings**              | Style, behavior, or tone modifiers (e.g., “write succinct”, “use formal tone”) | `STNG`                 | `STNG_write_succinct.txt`                                 |
| **Context / Description** | Provides contextual or background information for AI prompts                   | `CNXT`                 | `CNXT_project_overview.txt`                               |
| **Task**                  | Defines the main task or instruction for the AI                                | `TASK`                 | `TASK_compare_requirements.txt`                           |
| **Subfolders**            | Contain additional prompt or requirement files, processed recursively          | *(folder name itself)* | `../queue/subproject/`                                    |
| **Requirements**          | Requirement files such as customer or system requirements                      | *(no prefix)*          | `REQ_vehicle_speed.txt` or `customer_requirement_001.txt` |

🧠 Categories STNG, CNXT, and TASK are combined into an environment used alongside each individual requirement file during processing.<br>

# Architecture & Design Overview

* **Environment Files** — Files in categories STNG (Settings), CNXT (Context/Description), and TASK (Tasks). These form the shared environment or knowledge base.
* **Query Files (Requirement Files)** — Each represents a smaller task/requirement that we want the AI to process using the environment.
* **File Watcher** — Monitors additions/changes/removals in the queue folder (including environment files and query files).
* **File Categorizer & Tracker** — Tracks which files are environment vs query, tracks modification status, provides content retrieval.
* **CurlWrapper / REST Interface** — Handles communication with the AI provider’s API (e.g., GPT-4/5) via HTTP.
* **Thread Pool / Parallel Processing** — Configured by max threads in config.json; handles multiple query tasks in parallel.
* **JarvisAgent Application** — Orchestrates startup, event handling, file watcher, file categorization, query dispatching.
* **Core Engine** — Provides globally shared components (thread pool, event queue, logger, config, etc.).

# Contributions
<br>
In this project, you're welcome to contribute. Please be sure to enable clang formatting in your IDE. The coding style is Allman. Member fields of structs and classes are written as `m_` + PascalCase.

# Development
<br>
To clone the project, use<br>

`git clone --recurse-submodules https://github.com/beaumanvienna/JarvisAgent`<br>
<br>
JarvisAgent is cross-platform. The project is defined in a Lua file for permake5.<br>
<br>
Run <br>
`premake5 gmake` to get a Makefile<br>
`premake5 vs2022` to get a VS solution<br>
`premake5 xcode4` for Xcode on MacOS<br>
<br>
On Ubuntu, install the required development libraries with<br>
`sudo apt install libssl-dev zlib1g-dev`<br>
<br>
If you created a Makefile, build the project with<br>
`make config=release verbose=1 && make config=debug verbose=1`<br>
<br>
Run the executable with<br>
`./bin/Release/jarvisAgent` or `./bin/Debug/jarvisAgent`<br>
<br>
To update the source code, use<br>
`git pull && git submodule update --init --recursive`<br>
<br>
Use `premake5 clean` to clean the project from build artifacts.<br>
<br>
<br>
TODO: HTTP/2 is currently not enabled to keep the prototype simple. Enable it to improve performance.

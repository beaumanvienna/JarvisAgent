# Introduction
<br>
Jarvis can perform AI tasks. It can be used to automate workflows.<br>
<br>

| Layer | Responsibility | Status |
|--------|----------------|--------|
| **Engine** | Networking (`libcurl`), logging (`spdlog`), JSON parsing (`simdjson`), threading | 🚧 |
| **Application** | Orchestrates queue handling and task flow | 🚧 |
| **Config** | `config.json` with folder paths + thread count | ✅ |
| **I/O** | Input queue, output folder | 🚧 |
| **Networking** | ChatGPT requests/responses | 🚧 |

# Contributions
<br>
In this project, you're welcome to contribute. Please be sure to enable clang formatting in your IDE. The coding style is Allmann. Member fields of structs and classes are written as `m_` + PascalCase.

# Development
<br>
To clone the project, use<br>

`git clone --recurse-submodules https://github.com/beaumanvienna/jarvis`<br>
<br>
Jarvis is cross-platform. The project is defined in a Lua file for permake5.<br>
<br>
Run <br>
`premake5 gmake` to get a Makefile<br>
`premake5 vs2022` to get a VS solution<br>
`premake5 xcode4` for Xcode on MacOS<br>
<br>
If you created a Makefile, build the project with<br>
`make config=release verbose=1 && make config=debug verbose=1`<br>
<br>
Run the executable with<br>
`./bin/Release/jarvis` or `./bin/Debug/jarvis`<br>
<br>
To update the source code, use<br>
`git pull && git submodule update --init --recursive`<br>
<br>
Use `premake5 clean` to clean the project from build artifacts.<br>

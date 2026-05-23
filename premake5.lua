-- Copyright (c) 2025 JC Technolabs
-- License: GPL-3.0

-- premake5.lua
workspace "AIAssistant"
    -- Default to x86_64 (emits -m64 under gmake). On ARM hosts (Apple Silicon,
    -- linux/arm64 Docker) switch to ARM64 so premake doesn't pass -m64 to GCC
    -- — clang on macOS tolerates -m64 as a no-op, but GCC on linux/arm64
    -- rejects it with "unrecognized command-line option '-m64'".
    local uname_m = os.outputof("uname -m 2>/dev/null") or ""
    uname_m = uname_m:gsub("%s+$", "")
    if uname_m == "arm64" or uname_m == "aarch64" then
        architecture "ARM64"
    else
        architecture "x86_64"
    end
    configurations { "Debug", "Release" }
    startproject "jarvisAgent"

    -- Wrap static libs with --start-group/--end-group so the linker re-scans
    -- archives for cross-TU references (e.g. cipher_idea_hw.o in libcrypto.a
    -- calls IDEA_*_encrypt defined in i_*.o in the same libcrypto.a; a single
    -- left-to-right scan misses this on strict linkers like Ubuntu arm64's).
    -- GNU ld (Linux) only — Apple's ld on macOS rejects these flags as unknown.
    filter "system:linux"
        linkgroups "On"
    filter {}

-- ================================================================
-- Options
-- ================================================================
newoption {
    trigger = "tracy",
    description = "Enable Tracy profiler instrumentation"
}

newoption {
    trigger = "studio",
    description = "Build the Studio edition (explicit — same as default)."
}

newoption {
    trigger = "engine",
    description = "Build the Engine edition (lean production server, no editor/AI tooling)."
}

-- Opt-in clang toolchain on Linux.  CI uses gcc by default (matches Ubuntu/Rocky/Arch/Docker
-- behaviour); this flag is for local dev when JC wants clang instead.  Pairs with
-- libc++-18-dev (clang 18's libstdc++ headers can't see <expected> due to a known
-- __cpp_concepts version mismatch — clang 18 reports 201907L, libstdc++ requires 202002L —
-- so we route through libc++ which ships its own <expected>).  Remove this option when
-- clang ≥19 lands in Ubuntu LTS.
newoption {
    trigger = "clang",
    description = "Use clang + libc++ instead of gcc/libstdc++ (Linux only)."
}

-- SecureString-only HTTP-path audit (Sitting 8c).  Builds test/security/heapScan_test.cpp
-- into the binary and routes engine.cpp through RunHeapScanAudit() before normal startup;
-- the binary scans /proc/self/mem for planted nonce-secrets across every auth style shipped
-- by 8a + 8b, then exits with a PASS/FAIL code.  Off by default — pre-1.0 audit artifact
-- only, not a runtime feature.
newoption {
    trigger = "heapscan",
    description = "Compile the SecureString heap-scan audit into the binary (pre-1.0 artifact)."
}

project "jarvisAgent"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++23"

    -- Local clang toolchain override (Linux only).  CXX must also be exported in
    -- the env so the generated Makefile picks up clang++ (premake's `toolset "clang"`
    -- writes `CXX = clang++` into the Makefile).
    if _OPTIONS["clang"] then
        toolset "clang"
        print(">>> Toolchain: clang + libc++  (opt-in via --clang)")
    end

    targetdir "bin/%{cfg.buildcfg}"

    -- Each edition gets its own objdir so switching editions triggers a full rebuild.
    if _OPTIONS["engine"] then
        objdir ("bin-int/engine/%{cfg.buildcfg}")
    else
        objdir ("bin-int/studio/%{cfg.buildcfg}")
    end

    defines
    {
        "JARVIS_AGENT_VERSION=\"0.8.5\"",
        "CROW_ENFORCE_WS_SPEC",
        "CROW_ENABLE_SSL",
        "CROW_USE_LOCALTIMEZONE",
        "PDC_WIDE",
        "MINIZ_NO_ZLIB_COMPATIBLE_NAMES"
    }

    ------------------------------------
    -- Tracy toggle
    ------------------------------------
    if _OPTIONS["tracy"] then
        defines { "TRACY_ENABLE" }
        print(">>> Tracy profiling: ENABLED")
    else
        print(">>> Tracy profiling: DISABLED")
    end

    ------------------------------------
    -- Edition toggle
    -- Default = Studio.  --engine opts into the lean build.
    -- --studio is accepted for clarity but is a no-op.
    ------------------------------------
    if _OPTIONS["engine"] then
        targetname "jarvisAgent-engine"
        print(">>> Edition: Engine  ->  jarvisAgent-engine")
        io.writefile(".build-edition", "engine\n")
    else
        targetname "jarvisAgent-studio"
        defines { "J9T_STUDIO" }
        print(">>> Edition: Studio  ->  jarvisAgent-studio")
        io.writefile(".build-edition", "studio\n")
    end

    files
    {
        "application/**.h",
        "application/**.cpp",
        "engine/**.h",
        "engine/**.cpp",
        "vendor/simdjson/simdjson.cpp",
        "vendor/simdjson/simdjson.h",
        "vendor/date/src/tz.cpp",
    }

    -- Heap-scan audit (Sitting 8c).  Only pulled in when --heapscan is set; the
    -- production binary never compiles the audit code.  The audit's RunHeapScanAudit()
    -- is invoked from engine.cpp under the same J9T_HEAPSCAN_BUILD #ifdef.
    if _OPTIONS["heapscan"] then
        defines { "J9T_HEAPSCAN_BUILD" }
        files {
            "test/security/heapScan_test.h",
            "test/security/heapScan_test.cpp",
            -- Cloud-side scenarios live in an isolated TU because application/cloud/
            -- sigV4Signer.h defines AIAssistant::SigV4Signer which collides with the
            -- engine signer of the same name in engine/curlWrapper/awsSigV4.h.  No TU
            -- includes both headers.
            "test/security/heapScan_cloud_scenarios.cpp",
        }
        print(">>> Heap-scan audit: ENABLED  (binary will run audit then exit)")
    end

    -- Engine edition: drop Studio-only modules from the compile list.
    -- removefiles MUST come after files() — applying it earlier removes from an
    -- empty set and silently does nothing, which would link Studio code into
    -- the engine binary (cybersecurity gap).
    if _OPTIONS["engine"] then
        removefiles {
            "application/assistant/**",
            "application/web/aiJcwfService.h",
            "application/web/aiJcwfService.cpp",
            "application/web/webServer_studio.cpp",
        }
    else
        -- Studio edition: drop the Engine-only stubs.  Same security argument as
        -- above for the symmetry — without this, both Studio and Engine impls of
        -- InitEditionSpecific / HandleAssistantWebSocketMessage would link into
        -- Studio and the no-op Engine version could shadow the real one.
        removefiles {
            "application/web/webServer_engine.cpp",
        }
    end

    -- Embed doc/jcwf.schema.json + doc/jcwf_generation_guide.md as C++ constants.
    -- Runs at premake-generation time so the generated headers exist before the
    -- compiler looks for them.  Generated headers are gitignored; the source of
    -- truth is the on-disk doc/ files.
    local function embedAsHeader(sourceRelativePath, outputRelativePath, constantName)
        local sourcePath = path.join(_MAIN_SCRIPT_DIR, sourceRelativePath)
        local outputPath = path.join(_MAIN_SCRIPT_DIR, outputRelativePath)
        local sourceFile = io.open(sourcePath, "rb")
        if not sourceFile then
            print(">>> WARN: embed source missing, skipping: " .. sourceRelativePath)
            return
        end
        local content = sourceFile:read("*all")
        sourceFile:close()
        if content:sub(-1) ~= "\n" then content = content .. "\n" end
        -- Raw-string terminator is ")DELIM\"" — pick one not present in content.
        local delimiter = "JCWF"
        for _, candidate in ipairs({"JCWF", "JCWFX", "JARVIS", "JARVIS9T"}) do
            if not content:find(")" .. candidate .. "\"", 1, true) then
                delimiter = candidate
                break
            end
        end
        -- MSVC caps a single string literal at 16380 bytes.  Split into adjacent
        -- raw-string literals so the preprocessor can concatenate them back
        -- into a single char const*.  We target 12000 bytes per chunk and
        -- break on newlines for readability of the generated header.
        local maxChunkBytes = 12000
        local chunks = {}
        local pos = 1
        local total = #content
        while pos <= total do
            local chunkEnd = pos + maxChunkBytes - 1
            if chunkEnd >= total then
                chunkEnd = total
            else
                -- Walk back to the nearest newline so chunks break on line boundaries.
                while chunkEnd > pos and content:sub(chunkEnd, chunkEnd) ~= "\n" do
                    chunkEnd = chunkEnd - 1
                end
                if chunkEnd == pos then
                    -- No newline in window — fall back to hard split.
                    chunkEnd = pos + maxChunkBytes - 1
                end
            end
            table.insert(chunks, content:sub(pos, chunkEnd))
            pos = chunkEnd + 1
        end

        local literalParts = {}
        for _, chunk in ipairs(chunks) do
            table.insert(literalParts,
                string.format("        R\"%s(%s)%s\"", delimiter, chunk, delimiter))
        end
        local joinedLiterals = table.concat(literalParts, "\n")

        local body = string.format(
            "/* Copyright (c) 2026 JC Technolabs\n" ..
            "   License: GPL-3.0\n" ..
            "   AUTO-GENERATED by premake5.lua prebuild — do not edit by hand.\n" ..
            "   Source: %s\n*/\n\n#pragma once\n\n" ..
            "namespace AIAssistant\n{\n" ..
            "    inline constexpr char const* %s =\n%s;\n" ..
            "} // namespace AIAssistant\n",
            sourceRelativePath, constantName, joinedLiterals)
        local existing = io.open(outputPath, "rb")
        if existing then
            local existingContent = existing:read("*all")
            existing:close()
            if existingContent == body then return end
        end
        os.mkdir(path.getdirectory(outputPath))
        local outputFile = io.open(outputPath, "wb")
        if not outputFile then
            error("could not write " .. outputPath)
        end
        outputFile:write(body)
        outputFile:close()
        print(">>> Embedded: " .. sourceRelativePath .. " -> " .. outputRelativePath)
    end
    embedAsHeader("doc/jcwf.schema.json", "application/json/jcwfSchema.generated.h",
                   "kJcwfSchemaJson")
    embedAsHeader("doc/jcwf_generation_guide.md", "application/json/jcwfGenerationGuide.generated.h",
                   "kJcwfGenerationGuide")

    includedirs
    {
        "engine/",
        "application/",
        "test/",
        "vendor/",
        "vendor/spdlog/include",
        "vendor/curl/include",
        "vendor/nghttp2/lib/includes",
        "vendor/thread-pool/include",
        "vendor/tracy/include",
        "vendor/openssl/include",
        "vendor/crow/include/crow",
        "vendor/asio/asio/include",
        "vendor/pdcursesmod",
        "vendor/date/include",
        "vendor/miniz"
    }

    defines { "NGHTTP2_STATICLIB" }

    filter "system:linux"

        linkoptions {
            "-fno-pie -no-pie",
            "-rdynamic",
            -- Let ld resolve versioned symbols via transitive shared-lib deps
            -- (e.g. libpq → libcrypto.so.3 provides RC2_cfb64_encrypt@@OPENSSL_3.0.0).
            -- Required on Ubuntu arm64 where the default is --no-copy-dt-needed-entries;
            -- on amd64 the older default lets this work without the flag.
            "-Wl,--copy-dt-needed-entries"
        }

        --
        -- Use python3-config --includes to discover Python include path.
        --
        -- IMPORTANT:
        -- Premake executes Lua at generation-time, even inside a filter.
        -- So we must guard Linux-only host calls (like python3-config)
        -- to avoid crashing when premake runs on Windows/macOS.
        --
        if os.ishost("linux") then
            local py_includes = os.outputof("python3-config --includes")
            if not py_includes then
                error("python3-config not found (needed to locate Python includes on Linux).")
            end

            -- Extract the first include path:
            local py_incdir = py_includes:match("-I([^%s]+)")
            if not py_incdir then
                error("Failed to extract Python include directory from python3-config --includes")
            end

            -- Extract the final folder name (e.g. "python3.12")
            local py_libname = py_incdir:match("([^/]+)$")
            if not py_libname then
                error("Failed to determine Python library name from include path: " .. py_incdir)
            end

            local py_link = py_libname

            includedirs {
                py_incdir
            }

            --
            -- Use pkg-config to discover libpq (PostgreSQL client library).
            --
            local pq_cflags = os.outputof("pkg-config --cflags libpq 2>/dev/null")
            if pq_cflags and pq_cflags ~= "" then
                local pq_incdir = pq_cflags:match("-I([^%s]+)")
                if pq_incdir then
                    includedirs { pq_incdir }
                end
                print(">>> libpq (Linux): " .. pq_cflags:gsub("%s+$", ""))
            else
                print(">>> libpq: not found via pkg-config — PostgreSQL connector will not be available")
            end

            links {
                "curl",
                "nghttp2",
                "pthread",
                "dl",
                "ssl",
                "crypto",
                "z",
                "m",
                py_link,
                "pq",
                "pdcursesmod",
                "miniz"
            }
        end

        defines {
            "LINUX",
            "USE_OS_TZDB=1",
            "HAS_REMOTE_API=0"
        }

    filter "system:macosx"
        defines {
            "USE_OS_TZDB=1",
            "HAS_REMOTE_API=0"
        }
    
        --
        -- Robust Python discovery on macOS:
        -- Queries sysconfig for include/lib locations and the link library name.
        -- Uses the python3 that matches python3-config on PATH (via --prefix),
        -- falling back to plain "python3" if python3-config is unavailable.
        --
        -- Why: `brew install python` installs Homebrew's python3-config first in
        -- PATH but does not necessarily override Apple's /usr/bin/python3 (from
        -- Command Line Tools), which advertises "Python3.framework" and ships no
        -- Python.h. Trusting PATH for python3 breaks embedding builds.
        -- On CI, actions/setup-python installs both consistently, so the
        -- python3-config --prefix lookup still resolves to the intended install.
        --
        -- IMPORTANT:
        -- Premake executes Lua at generation-time, even inside a filter.
        -- So we must guard macOS-only host calls to avoid crashing when premake runs elsewhere.
        --
        if os.ishost("macosx") then
            local py_bin = "python3"
            local py_prefix = os.outputof("python3-config --prefix 2>/dev/null")
            if py_prefix and py_prefix ~= "" then
                py_prefix = py_prefix:gsub("%s+$", "")
                local candidate = py_prefix .. "/bin/python3"
                if os.isfile(candidate) then
                    py_bin = candidate
                end
            end
            print(">>> Python (macOS): using " .. py_bin)

            local pythonInfo = os.outputof(py_bin .. [[ -c "import sys, sysconfig;
print(sysconfig.get_path('include') or '');
print(sysconfig.get_config_var('LIBDIR') or '');
print(sys.base_prefix or '');
print(sysconfig.get_config_var('LDLIBRARY') or '');
print(sysconfig.get_config_var('PYTHONFRAMEWORK') or '');
print(sysconfig.get_config_var('PYTHONFRAMEWORKPREFIX') or '')"]])

            if not pythonInfo then
                error("python3 not found on PATH. On CI, ensure actions/setup-python ran before premake5.")
            end

            local lines = {}
            for line in pythonInfo:gmatch("([^\r\n]+)") do
                lines[#lines + 1] = line
            end

            if #lines < 6 then
                error("Failed to query Python sysconfig paths on macOS. Output was: " .. pythonInfo)
            end

            local pyIncludeDir = lines[1]
            local pyLibDir = lines[2]
            local pyBasePrefix = lines[3]
            local pyLdLibrary = lines[4]
            local pyFrameworkName = lines[5]
            local pyFrameworkPrefix = lines[6]

            if pyIncludeDir == "" then
                error("Failed to determine Python include directory on macOS.")
            end

            includedirs { pyIncludeDir }

            local useFramework = (pyFrameworkName ~= "")

            if useFramework then
                --
                -- Framework build (common on GitHub Actions macOS):
                -- Link via -framework Python rather than -lPython.
                --
                print(">>> Python (macOS): linking as framework: " .. pyFrameworkName)

                if pyFrameworkPrefix ~= "" then
                    -- Usually /Library/Frameworks; add explicit search path to be safe.
                    linkoptions { "-F" .. pyFrameworkPrefix }
                end

                linkoptions { "-framework " .. pyFrameworkName }
            else
                --
                -- Non-framework build: link against libpythonX.Y in LIBDIR.
                --
                if pyLibDir == "" then
                    -- Fallback: <base_prefix>/lib
                    pyLibDir = path.join(pyBasePrefix, "lib")
                end

                local pyLibName = pyLdLibrary
                -- Convert e.g. "libpython3.12.dylib" -> "python3.12"
                pyLibName = pyLibName:gsub("^lib", "")
                pyLibName = pyLibName:gsub("%.dylib$", "")
                pyLibName = pyLibName:gsub("%.a$", "")

                if pyLibName == "" then
                    error("Failed to determine Python link library name on macOS (LDLIBRARY was: " .. pyLdLibrary .. ").")
                end

                libdirs { pyLibDir }

                -- Ensure the runtime can find libpython without extra env vars.
                linkoptions { "-Wl,-rpath," .. pyLibDir }

                links { pyLibName }
            end

            -- macOS frameworks needed by libcurl (SystemConfiguration/CoreFoundation)
            linkoptions { "-framework CoreFoundation", "-framework SystemConfiguration" }

            --
            -- Use pkg-config to discover libpq (PostgreSQL client library).
            -- On macOS, brew installs libpq to a non-standard prefix; pkg-config handles this.
            --
            local pq_cflags = os.outputof("pkg-config --cflags libpq 2>/dev/null")
            if pq_cflags and pq_cflags ~= "" then
                local pq_incdir = pq_cflags:match("-I([^%s]+)")
                if pq_incdir then
                    includedirs { pq_incdir }
                end
                local pq_libs = os.outputof("pkg-config --libs-only-L libpq 2>/dev/null")
                if pq_libs and pq_libs ~= "" then
                    local pq_libdir = pq_libs:match("-L([^%s]+)")
                    if pq_libdir then
                        libdirs { pq_libdir }
                    end
                end
                print(">>> libpq (macOS): " .. pq_cflags:gsub("%s+$", ""))
            else
                print(">>> libpq: not found via pkg-config — PostgreSQL connector will not be available")
            end

            links {
                "curl",
                "nghttp2",
                "ssl",
                "crypto",
                "z",
                "pq",
                "pdcursesmod",
                "miniz"
            }

        end


    filter { "action:gmake*", "configurations:Debug" }
        buildoptions { "-ggdb -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter { "action:gmake*", "configurations:Release" }
        buildoptions { "-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    -- Clang opt-in (Linux dev): route through libc++ instead of libstdc++ so
    -- <expected> resolves cleanly under clang 18.  Applies to both build and link
    -- phases — the driver pulls in libc++abi + libunwind automatically.
    if _OPTIONS["clang"] then
        filter { "action:gmake*" }
            buildoptions { "-stdlib=libc++" }
            linkoptions  { "-stdlib=libc++" }
    end
    filter {}

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/utf-8", "/bigobj", "/MP" }

        -- Tell libcurl headers that we're linking against the static library.
        defines { "CURL_STATICLIB", "NOMINMAX" }

    -- MSVC natively supports C++20 chrono timezone — exclude vendor/date/tz.cpp.
    -- MinGW/Clang-on-Windows compile tz.cpp like Linux/macOS.
    filter { "system:windows", "action:vs*" }
        removefiles { "vendor/date/src/tz.cpp" }

    -- Non-MSVC Windows builds (MinGW, etc.) need the date library's tz.cpp.
    filter { "system:windows", "action:gmake*" }
        defines { "USE_OS_TZDB=0", "HAS_REMOTE_API=0" }

    filter "system:windows"

        --
        -- Windows system libs (always).
        --
        links { "wldap32", "advapi32", "crypt32", "secur32", "ws2_32", "normaliz", "pdcursesmod", "winmm", "curl", "nghttp2", "ssl", "crypto", "miniz" }

        --
        -- Robust Python discovery on Windows:
        -- Uses whatever "python" is on PATH (e.g. provided by actions/setup-python),
        -- and queries sysconfig for include/lib locations and the import-lib name.
        --
        -- IMPORTANT:
        -- Premake executes Lua at generation-time, even inside a filter.
        -- So we must guard Windows-only host calls to avoid crashing when premake runs elsewhere.
        --
        if os.ishost("windows") then
            local pythonInfo = os.outputof([[python -c "import sys, sysconfig; print(sysconfig.get_path('include')); print(sys.base_prefix); print('python{}{}'.format(sys.version_info[0], sys.version_info[1]))"]])

            if not pythonInfo then
                error("Python not found on PATH. On CI, ensure actions/setup-python ran before premake5.")
            end

            local lines = {}
            for line in pythonInfo:gmatch("([^\r\n]+)") do
                lines[#lines + 1] = line
            end

            if #lines < 3 then
                error("Failed to query Python sysconfig paths. Output was: " .. pythonInfo)
            end

            local pyIncludeDir = lines[1]
            local pyBasePrefix = lines[2]
            local pyLibName = lines[3]

            local pyLibDir = path.join(pyBasePrefix, "libs")

            includedirs { pyIncludeDir }
            libdirs { pyLibDir }
            links { pyLibName }

            --
            -- libpq on Windows: check LIBPQ_DIR env var or default PostgreSQL install path.
            --
            local pqDir = os.getenv("LIBPQ_DIR") or "C:/Program Files/PostgreSQL/17"
            local pqIncDir = path.join(pqDir, "include")
            local pqLibDir = path.join(pqDir, "lib")
            if os.isdir(pqIncDir) then
                includedirs { pqIncDir }
                libdirs { pqLibDir }
                links { "libpq" }
                print(">>> libpq (Windows): " .. pqDir)
            else
                print(">>> libpq: not found — set LIBPQ_DIR or install PostgreSQL; PostgreSQL connector will not be available")
            end
        end


    filter "configurations:Debug"
        defines
        {
            "DEBUG"
        }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines
        {
            "NDEBUG"
        }
        runtime "Release"
        optimize "on"

    filter {}

    if _ACTION == "clean" then
        print("clean the build...")

        ----------------------------------------------------
        -- Top-level build folders
        ----------------------------------------------------
        os.rmdir("bin")
        os.rmdir("bin-int")
        os.rmdir("bin-int/engine")
        os.rmdir("bin-int/studio")
        os.remove(".build-edition")

        ----------------------------------------------------
        -- Remove all generated Makefiles (.make)
        ----------------------------------------------------
        os.remove("*.make")
        os.remove("**/*.make")
        os.remove("Makefile")
        os.remove("vendor/Makefile")

        ----------------------------------------------------
        -- Curl build folders
        ----------------------------------------------------
        os.rmdir("vendor/curl/bin")
        os.rmdir("vendor/curl/bin-int")
        ----------------------------------------------------
        -- Remove generated curl config
        ----------------------------------------------------
        os.remove("vendor/curl/lib/curl_config.h")

        ----------------------------------------------------
        -- nghttp2 build folders
        ----------------------------------------------------
        os.rmdir("vendor/nghttp2/bin")
        os.rmdir("vendor/nghttp2/bin-int")


        ----------------------------------------------------
        -- OpenSSL build folders
        ----------------------------------------------------
        os.rmdir("vendor/openssl/bin")
        os.rmdir("vendor/openssl/bin-int")

        ----------------------------------------------------
        -- PDCursesMod build folders
        ----------------------------------------------------
        os.rmdir("vendor/pdcursesmod/bin")
        os.rmdir("vendor/pdcursesmod/bin-int")
        os.remove("vendor/pdcursesmod/Makefile")

        ----------------------------------------------------
        -- miniz build folders
        ----------------------------------------------------
        os.rmdir("vendor/miniz/bin")
        os.rmdir("vendor/miniz/bin-int")

        ----------------------------------------------------
        -- React apps (Dashboard + Workflow Editor)
        ----------------------------------------------------
        os.rmdir("dashboard/ui/dist")
        os.rmdir("dashboard/ui/node_modules")
        os.rmdir("workflow-editor/ui/dist")
        os.rmdir("workflow-editor/ui/node_modules")

        ----------------------------------------------------
        -- MCP sidecar (TypeScript → dist/, npm → node_modules/)
        ----------------------------------------------------
        os.rmdir("mcp/dist")
        os.rmdir("mcp/node_modules")

        print("done.")
    end

    include "vendor/curl.lua"
    include "vendor/nghttp2.lua"
    include "vendor/openssl/crypto.lua"
    include "vendor/openssl/ssl.lua"
    include "vendor/pdcursesmod/pdcursesmod.lua"
    include "vendor/miniz/miniz.lua"

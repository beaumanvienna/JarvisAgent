-- Copyright (c) 2025 JC Technolabs
-- License: GPL-3.0

-- premake5.lua
workspace "AIAssistant"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "jarvisAgent"

-- ================================================================
-- TRACY TOGGLE (OFF by default)
-- Usage:
--   premake5 gmake2 --tracy
--   make config=release
-- ================================================================
newoption {
    trigger = "tracy",
    description = "Enable Tracy profiler instrumentation"
}

project "jarvisAgent"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"

    targetdir "bin/%{cfg.buildcfg}"
    objdir ("bin-int/%{cfg.buildcfg}")

    defines
    {
        "JARVIS_AGENT_VERSION=\"0.1\"",
        "CROW_ENFORCE_WS_SPEC",
        "PDC_WIDE"
    }

    ------------------------------------
    -- Tracy toggle logic (added)
    ------------------------------------
    if _OPTIONS["tracy"] then
        defines { "TRACY_ENABLE" }
        print(">>> Tracy profiling: ENABLED")
    else
        print(">>> Tracy profiling: DISABLED")
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

    includedirs
    {
        "engine/",
        "application/",
        "vendor/",
        "vendor/spdlog/include",
        "vendor/curl/include",
        "vendor/thread-pool/include",
        "vendor/tracy/include",
        "vendor/openssl/include",
        "vendor/crow/include/crow",
        "vendor/asio/asio/include",
        "vendor/pdcursesmod",
        "vendor/date/include"
    }

    filter "system:linux"

        linkoptions {
            "-fno-pie -no-pie",
            "-rdynamic"
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

            links {
                "curl",
                "pthread",
                "dl",
                "ssl",
                "crypto",
                "z",
                "m",
                py_link,
                "pdcursesmod"
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
        -- Uses whatever "python3" is on PATH (e.g. provided by actions/setup-python),
        -- and queries sysconfig for include/lib locations and the link library name.
        --
        -- IMPORTANT:
        -- Premake executes Lua at generation-time, even inside a filter.
        -- So we must guard macOS-only host calls to avoid crashing when premake runs elsewhere.
        --
        if os.ishost("macosx") then
            local pythonInfo = os.outputof([[python3 -c "import sys, sysconfig; 
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

            links {
                "curl",
                "ssl",
                "crypto",
                "z",
                "pdcursesmod"
            }

        end


    filter { "action:gmake*", "configurations:Debug" }
        buildoptions { "-ggdb -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter { "action:gmake*", "configurations:Release" }
        buildoptions { "-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter "system:windows"
        systemversion "latest"
        buildoptions { "/utf-8", "/bigobj" }

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
        links { "wldap32", "advapi32", "crypt32", "ws2_32", "normaliz", "pdcursesmod", "winmm", "curl", "ssl", "crypto" }

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

        print("done.")
    end

    include "vendor/curl.lua"
    include "vendor/openssl/crypto.lua"
    include "vendor/openssl/ssl.lua"
    include "vendor/pdcursesmod/pdcursesmod.lua"

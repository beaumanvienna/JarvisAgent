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
    staticruntime "on"

    targetdir "bin/%{cfg.buildcfg}"
    objdir ("bin-int/%{cfg.buildcfg}")
    
    defines
    {
        --"TRACY_ENABLE",
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
        "vendor/pdcursesmod"
    }

    filter "system:linux"

        linkoptions {
            "-fno-pie -no-pie",
            "-rdynamic"
        }

        --
        -- Use python3-config --includes to discover Python include path.
        --
        local py_includes = os.outputof("python3-config --includes")

        -- Example output:
        --   "-I/usr/include/python3.12 -I/usr/include/python3.12"
        --
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

        -- Create linker library name, e.g. "-lpython3.12"
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
            py_link,      -- e.g. python3.12
            "pdcursesmod"
        }

        defines {
            "LINUX"
        }


	filter "system:macosx"

        includedirs {
            "/opt/homebrew/opt/python@3.12/Frameworks/Python.framework/Versions/3.12/include/python3.12"
        }
		links {
			"curl",
			"ssl",
			"crypto",
			"z",
			"-framework CoreFoundation",
			"-framework SystemConfiguration",
            "python3.12",
            "pdcursesmod"
		}

    filter { "action:gmake*", "configurations:Debug"}
        buildoptions { "-ggdb -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter { "action:gmake*", "configurations:Release"}
        buildoptions { "-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter "system:windows"
        systemversion "latest"
        links { "wldap32", "advapi32", "crypt32", "ws2_32", "normaliz", "python312", "pdcursesmod" }
        
        includedirs {
            "C:/Users/%{os.getenv('USERNAME')}/AppData/Local/Programs/Python/Python312/include"
        }
        libdirs {
            "C:/Users/%{os.getenv('USERNAME')}/AppData/Local/Programs/Python/Python312/libs"
        }

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



-- premake5.lua
workspace "Chat"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "jarvisAgent"

project "jarvisAgent"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"

    targetdir "bin/%{cfg.buildcfg}"
    objdir ("bin-int/%{cfg.buildcfg}")
    
    defines
    {
        "JARVIS_AGENT_VERSION=\"0.1\"",
        "CROW_ENFORCE_WS_SPEC"
    }

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
        "vendor/asio/asio/include"
    }

    filter "system:linux"

        linkoptions { "-fno-pie -no-pie" }

        files 
        { 
        }
        includedirs 
        {
        }
        links
        {
			"curl",
			"pthread",
			"dl",
			"ssl",
			"crypto",
			"z"
        }
        libdirs
        {
        }
        defines
        {
            "LINUX"
        }

	filter "system:macosx"
		links {
			"curl",
			"ssl",
			"crypto",
			"z",
			"-framework CoreFoundation",
			"-framework SystemConfiguration"
		}

    filter { "action:gmake*", "configurations:Debug"}
        buildoptions { "-ggdb -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter { "action:gmake*", "configurations:Release"}
        buildoptions { "-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter "system:windows"
        systemversion "latest"
        links { "wldap32", "advapi32", "crypt32", "ws2_32", "normaliz" }

    filter "configurations:Debug"
        defines
        { 
			"DEBUG", 
            "TRACY_ENABLE"
        }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines
        {
			"NDEBUG",
            "TRACY_ENABLE"
		}
        runtime "Release"
        optimize "on"

    if _ACTION == 'clean' then
        print("clean the build...")
        os.rmdir("./bin")
        os.rmdir("./bin-int")
        os.remove("./**.make")
        os.remove("./Makefile")
        os.remove("./vendor/Makefile")
        os.rmdir("./vendor/curl/bin")
        os.rmdir("./vendor/curl/bin-int")
        os.rmdir("./vendor/openssl/bin")
        os.rmdir("./vendor/openssl/bin-int")
        print("done.")
    end

	include "vendor/curl.lua"
	include "vendor/openssl/crypto.lua"
	include "vendor/openssl/ssl.lua"

-- premake5.lua
workspace "Chat"
    architecture "x86_64"
    configurations { "Debug", "Release" }
    startproject "jarvis"

project "jarvis"
    kind "ConsoleApp"
    language "C++"
    cppdialect "C++20"
    staticruntime "on"

    targetdir "bin/%{cfg.buildcfg}"
    objdir ("bin-int/%{cfg.buildcfg}")
    
    defines
    {
        "JARVIS_VERSION=\"0.1\""
    }

    files
    {
    
        "application/**.h", 
        "application/**.cpp",
        "engine/**.h",
        "engine/**.cpp"
    }

    includedirs
    {
        "engine/",
        "application/",
        "vendor/",
        "vendor/spdlog/include"
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
			"curl"
        }
        libdirs
        {
        }
        defines
        {
            "LINUX"
        }

    filter { "action:gmake*", "configurations:Debug"}
        buildoptions { "-ggdb -Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter { "action:gmake*", "configurations:Release"}
        buildoptions { "-Wall -Wextra -Wpedantic -Wshadow -Wno-unused-parameter -Wno-reorder -Wno-expansion-to-defined" }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Debug"
        defines { "DEBUG" }
        runtime "Debug"
        symbols "on"

    filter "configurations:Release"
        defines { "NDEBUG" }
        runtime "Release"
        optimize "on"

    if _ACTION == 'clean' then
        print("clean the build...")
        os.rmdir("./bin")
        os.rmdir("./bin-int")
        os.remove("./**.make")
        os.remove("./Makefile")
        print("done.")
    end

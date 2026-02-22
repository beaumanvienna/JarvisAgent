
-- 2025 JC Technolabs

----------------------------------------------------
-- Select correct curl_config.h per host OS (gmake only)
----------------------------------------------------
if _ACTION and _ACTION:match("^gmake") then
    if os.ishost("linux") then
        print(">>> curl: using Linux curl_config")
        os.copyfile(
            "curl/lib/curl_config_Linux.h",
            "curl/lib/curl_config.h"
        )
    elseif os.ishost("macosx") then
        print(">>> curl: using macOS curl_config")
        os.copyfile(
            "curl/lib/curl_config_macOS.h",
            "curl/lib/curl_config.h"
        )
    end
end


project "curl"
    kind "StaticLib"
    language "C++"
    cppdialect "C++20"

    targetdir "curl/bin/%{cfg.buildcfg}"
    objdir ("curl/bin-int/%{cfg.buildcfg}")

    files
    {
        "curl/lib/**.c",
        "curl/include/**.h"
    }

    includedirs
    {
        "curl/include",
        "curl/lib",
        "openssl/include"
    }
    
    defines
    {
        "CURL_STATICLIB",
        "USE_MANUAL",
        "CURL_HIDDEN_SYMBOLS",
        "BUILDING_LIBCURL"
    }
    
    filter "system:windows"
        systemversion "latest"
        defines { "WIN32", "_WINDOWS", "USE_OPENSSL" }

    filter "system:linux"
        defines
        {
            "_GNU_SOURCE",
            "LINUX",
            "HAVE_CONFIG_H"
        }

    filter "system:macosx"
        defines 
        { 
            "_DARWIN_C_SOURCE",
            "MACOSX",
            "HAVE_CONFIG_H"
        }

    filter { "action:gmake*", "configurations:Debug"}
        buildoptions { "-ggdb" }

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

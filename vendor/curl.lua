
-- 2025 JC Technolabs

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
        defines { "WIN32", "_WINDOWS" }

    filter "system:linux"
        defines
        {
			"_GNU_SOURCE",
			"HAVE_CONFIG_H"
		}

    filter "system:macosx"
        defines { "_DARWIN_C_SOURCE" }

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

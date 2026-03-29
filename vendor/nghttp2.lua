
-- 2026 JC Technolabs

project "nghttp2"
    kind "StaticLib"
    language "C"

    targetdir "nghttp2/bin/%{cfg.buildcfg}"
    objdir    "nghttp2/bin-int/%{cfg.buildcfg}"

    files { "nghttp2/lib/*.c" }

    includedirs { "nghttp2/lib/includes" }

    defines { "NGHTTP2_STATICLIB" }

    filter "system:linux"
        defines { "_GNU_SOURCE", "HAVE_ARPA_INET_H", "HAVE_NETINET_IN_H" }

    filter "system:macosx"
        defines { "HAVE_ARPA_INET_H", "HAVE_NETINET_IN_H" }

    filter "system:windows"
        systemversion "latest"
        defines { "WIN32", "HAVE_WINDOWS_H" }

    filter "configurations:Release"
        runtime "Release"
        optimize "on"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

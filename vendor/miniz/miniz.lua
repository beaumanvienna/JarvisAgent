
-- 2026 JC Technolabs

project "miniz"
    kind "StaticLib"
    language "C"

    targetdir "bin/%{cfg.buildcfg}"
    objdir    "bin-int/%{cfg.buildcfg}"

    files {
        "miniz.c",
        "miniz_tdef.c",
        "miniz_tinfl.c",
        "miniz_zip.c"
    }

    includedirs { "." }

    filter "system:windows"
        systemversion "latest"

    filter "configurations:Release"
        runtime "Release"
        optimize "on"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

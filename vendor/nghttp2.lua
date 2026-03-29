
-- 2026 JC Technolabs

----------------------------------------------------
-- Generate nghttp2ver.h from nghttp2ver.h.in
-- (nghttp2's own .gitignore excludes the generated file;
--  we produce it at premake-time so CI machines don't need cmake/autoconf)
----------------------------------------------------
local verIn  = "nghttp2/lib/includes/nghttp2/nghttp2ver.h.in"
local verOut = "nghttp2/lib/includes/nghttp2/nghttp2ver.h"

local function generateNghttp2VerHeader()
    local fin = io.open(verIn, "r")
    if not fin then return end  -- .in missing — nothing to generate
    local src = fin:read("*a")
    fin:close()

    -- Check if the output already exists and is identical (avoid touching the timestamp).
    local fout_check = io.open(verOut, "r")
    if fout_check then
        local existing = fout_check:read("*a")
        fout_check:close()
        -- If already generated, skip — the version never changes for a vendored snapshot.
        if existing ~= "" then return end
    end

    -- Substitute cmake-style @VARIABLE@ placeholders with the fixed version values.
    local version    = "1.68.90"
    local versionNum = "0x01445a"
    src = src:gsub("@PACKAGE_VERSION@",     version)
    src = src:gsub("@PACKAGE_VERSION_NUM@", versionNum)

    local fout = io.open(verOut, "w")
    if fout then
        fout:write(src)
        fout:close()
        print(">>> nghttp2: generated " .. verOut)
    end
end

generateNghttp2VerHeader()

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
        -- ssize_t is POSIX-only; MSVC doesn't define it in sys/types.h.
        -- ptrdiff_t is signed, pointer-sized, and already available via <stddef.h>
        -- (included by nghttp2.h before any ssize_t typedef).
        defines { "WIN32", "HAVE_WINDOWS_H", "ssize_t=ptrdiff_t" }

    filter "configurations:Release"
        runtime "Release"
        optimize "on"

    filter "configurations:Debug"
        runtime "Debug"
        symbols "on"

-- PDCursesMod Premake Script
-- Win:   WINCON backend
-- Linux: VT backend
-- macOS: VT backend

project "pdcursesmod"
    kind "StaticLib"
    language "C"
    staticruntime "on"

    targetdir ("bin/%{cfg.buildcfg}")
    objdir    ("bin-int/%{cfg.buildcfg}")

    --
    -- Core sources (shared)
    --
    files {
        "pdcurses/*.c",
        "common/*.c",
        "panel.h",
        "curses.h",
        "curspriv.h",
        "term.h"
    }

    includedirs {
        ".",
        "common",
        "pdcurses"
    }

    ----------------------------------------------------------------------
    -- WINDOWS → WINCON BACKEND
    ----------------------------------------------------------------------
    filter "system:windows"
        defines { "PDC_WIDE", "PDC_WINCON" }
        files { "wincon/*.c" }
        includedirs { "wincon" }

    ----------------------------------------------------------------------
    -- LINUX + MACOS → VT BACKEND
    ----------------------------------------------------------------------
    filter { "system:linux or system:macosx" }
        defines { "PDC_WIDE", "PDC_VT" }
        files { "vt/*.c" }
        includedirs { "vt" }

        --
        -- Exclude files not valid for VT backend
        --
        removefiles {
            "common/dosutil.c",    -- DOS only
            "common/mouse.c",      -- requires mouse backend (not supported in VT)
            "vt/getch2.c",         -- includes <conio.h>
            "common/winclip.c"     -- Windows clipboard API (must not compile)
        }

    filter {}

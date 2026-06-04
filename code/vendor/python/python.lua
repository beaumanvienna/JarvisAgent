-- python.lua: locate system Python installation
-- [jc 11/13/2025] this file is not used - instead the filter values are directly included in the main premake5.lua file

local python = {}

function python.import()

    -----------------------------------------------------
    -- Linux (Ubuntu / Zorin / Debian)
    -----------------------------------------------------
    filter { "system:linux" }
        includedirs {
            "/usr/include/python3.12"
        }
        links {
            "python3.12"
        }
        linkoptions {
            "-ldl",
            "-lm"
        }

    -----------------------------------------------------
    -- macOS (Homebrew)
    -----------------------------------------------------
    filter { "system:macosx" }
        includedirs {
            "/opt/homebrew/opt/python@3.12/Frameworks/Python.framework/Versions/3.12/include/python3.12"
        }
        links { "python3.12" }

    -----------------------------------------------------
    -- Windows (user installation)
    -----------------------------------------------------
    filter { "system:windows" }
        includedirs {
            "C:/Users/%{os.getenv('USERNAME')}/AppData/Local/Programs/Python/Python312/include"
        }
        libdirs {
            "C:/Users/%{os.getenv('USERNAME')}/AppData/Local/Programs/Python/Python312/libs"
        }
        links { "python312" }

    filter {}
end

return python

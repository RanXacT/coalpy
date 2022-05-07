local SourceDir = "Source"
local ScriptsDir = "Source/scripts/coalpy"
local ImguiDir = "Source/imgui"
local LibJpgDir = "Source/libjpeg"
local LibPngDir = "Source/libpng"
local ZlibDir = "Source/zlib"
local SpirvReflectDir = "Source/spirvreflect"
local DxcDir = "External/dxc/v1.6.2112/"
local DxcIncludes = DxcDir.."inc/"
local DxcLibDir = DxcDir.."lib/x64/"
local DxcBinaryCompiler = DxcDir.."bin/x64/dxcompiler.dll"
local DxcBinaryCompilerSo = DxcDir.."bin/x64/libdxcompiler.so"
local DxcBinaryIl = DxcDir.."bin/x64/dxil.dll"
local PythonDir = "External/Python39-win64/"
local OpenEXRDir = "External/OpenEXR/"
local OpenEXRLibDir = "External/OpenEXR/staticlib/"
local PixDir ="External/WinPixEventRuntime_1.0.210818001"
local PixBinaryDll ="External/WinPixEventRuntime_1.0.210818001/bin/WinPixEventRuntime.dll"


local LibIncludes = {
    {
        PythonDir .. "Include",
        Config = "win64-*-*"
    },
    {
        "/usr/include/python3.9",
        Config = "linux-*-*"
    },
    ImguiDir,
    LibJpgDir,
    LibPngDir,
}

local Libraries = {
    {
        PythonDir.."python39_d.lib",
        "Half-2_5_d.lib",
        "Iex-2_5_d.lib",
        "IexMath-2_5_d.lib",
        "IlmImf-2_5_d.lib",
        "IlmImfUtil-2_5_d.lib",
        "IlmThread-2_5_d.lib",
        "Imath-2_5_d.lib",
        Config = { "win64-msvc-debug-*" }
    },
    {
        PythonDir.."python39.lib",
        "Half-2_5.lib",
        "Iex-2_5.lib",
        "IexMath-2_5.lib",
        "IlmImf-2_5.lib",
        "IlmImfUtil-2_5.lib",
        "IlmThread-2_5.lib",
        "Imath-2_5.lib",
        Config = { "win64-msvc-release", "win64-msvc-production"  }
    },
    {
        "User32.lib",
        Config = "win64-msvc-*"
    },
    {
        "pthread",
        "dl",
        "vulkan",
        "SDL2",
        "Half",
        "Iex",
        "IexMath",
        "IlmImf",
        "IlmImfUtil",
        "IlmThread",
        "Imath",
        "dxclib",
        "LLVMDxcSupport",
        Config = "linux-*-*"
    }
}

local LibPaths = {
    OpenEXRLibDir,
    DxcLibDir
}

local Binaries = {
    DxcBinaryCompiler,
    DxcBinaryCompilerSo,
    DxcBinaryIl,
    PixBinaryDll
}

-- Build zlib
local zlibLib = StaticLibrary {
    Name = "zlib",
    Pass = "BuildCode",
    Includes = { ZlibDir },
    Sources = {
        Glob {
            Dir = ZlibDir,
            Extensions = { ".c", ".h" },
            Recursive =  true
        }
    },
    IdeGenerationHints = _G.GenRootIdeHints("zlib");
}

Default(zlibLib)

-- Build imgui
local imguiLib = StaticLibrary {
    Name = "imgui",
    Pass = "BuildCode",
    Includes = {
        ImguiDir,
        {
            "/usr/include/SDL2/",
            Config = { "linux-gcc-*" }
        }
    },
    Sources = {
        Glob {
            Dir = ImguiDir,
            Extensions = { ".cpp", ".h", ".hpp" },
            Recursive = false
        },
        Glob {
            Dir = ImguiDir.."/cpp/",
            Extensions = { ".cpp", ".h", ".hpp" },
            Recursive = true
        },
        {
            Config = { "win64-msvc-*" },
            {
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_dx12.cpp",
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_dx12.h",
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_win32.h",
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_win32.cpp",
            }
        },
        {
            Config = { "linux-gcc-*" },
            {
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_sdl.h",
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_sdl.cpp",
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_vulkan.h",
                ImguiDir.."$(SEP)backends$(SEP)imgui_impl_vulkan.cpp",
            }
        }
    },
    IdeGenerationHints = _G.GenRootIdeHints("imgui");
}

Default(imguiLib)

-- Build libjpeg
local libjpegLib = StaticLibrary {
    Name = "libjpeg",
    Pass = "BuildCode",
    Includes = LibJpgDir,
    Sources = {
        Glob {
            Dir = LibJpgDir,
            Extensions = { ".c", ".h" },
            Recursive =  true
        }
    },
    IdeGenerationHints = _G.GenRootIdeHints("libjpeg");
}

Default (libjpegLib)

-- Build spirvreflect
local spirvreflect = StaticLibrary {
    Name = "spirvreflect",
    Pass = "BuildCode",
    Includes = SpirvReflectDir,
    Sources = {
        Glob {
            Dir = SpirvReflectDir,
            Extensions = { ".c", ".h" },
            Recursive = true
        }
    }
}

Default (spirvreflect)

-- Build libpng
local libpngLib = StaticLibrary {
    Name = "libpng",
    Pass = "BuildCode",
    Includes = { LibPngDir, ZlibDir },
    Depends = { zlibLib },
    Sources = {
        Glob {
            Dir = LibPngDir,
            Extensions = { ".c", ".h" },
            Recursive =  true
        }
    },
    IdeGenerationHints = _G.GenRootIdeHints("libpng");
}

Default (libpngLib)

-- C++ module table-> module name with its dependencies
local CoalPyModuleTable = {
    core    = {},
    tasks   = { "core" },
    files   = { "core", "tasks" },
    texture = { "core", "tasks", "files", "render" },
    render  = { "core", "tasks", "files", "window" },
    window  = { "core" }
}

-- C++ module external includes
local CoalPyModuleIncludes = {
    render = {
        DxcIncludes,
        ImguiDir,
        SpirvReflectDir
    },
    texture = { LibJpgDir, LibPngDir, ZlibDir,
        {
            OpenEXRDir.."include/OpenEXR",
            Config = "win64-*-*"
        },
        {
            "/usr/include/OpenEXR",
            Config = "linux-*-*"
        }
    }
}

local CoalPyModuleDeps = {
    render = { imguiLib, spirvreflect },
    texture = { zlibLib, libpngLib, libjpegLib }
}

-- Module list for the core coalpy module
local CoalPyModules = { "core", "tasks", "render", "files", "window", "texture", "libjpeg", "libpng", "zlib"  }
local SrcDLL = "$(OBJECTDIR)$(SEP)gpu.dll"
local SrcSO = "$(OBJECTDIR)$(SEP)libgpu.so"

_G.BuildModules(SourceDir, CoalPyModuleTable, CoalPyModuleIncludes, CoalPyModuleDeps)
_G.BuildPyLib("gpu", "pymodules/gpu", SourceDir, LibIncludes, CoalPyModules, Libraries, { CoalPyModuleDeps.render, CoalPyModuleDeps.texture, "core" }, LibPaths)
_G.BuildProgram("coalpy_tests", "tests", { "CPY_ASSERT_ENABLED=1" }, SourceDir, LibIncludes, CoalPyModules, Libraries, LibPaths)
_G.DeployPyPackage("coalpy", "gpu", SrcDLL, SrcSO, Binaries, ScriptsDir)

-- Deploy PIP package
_G.DeployPyPackage("coalpy_pip/src/coalpy", "gpu", SrcDLL, SrcSO, Binaries, ScriptsDir)
_G.DeployPyPipFiles("coalpy_pip", SourceDir.."/../README.md", SourceDir.."/pipfiles")

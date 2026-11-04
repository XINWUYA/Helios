@echo off
REM ===========================================================================
REM  Helios CMake Project Generator (Windows)
REM
REM  Usage: Win-GenProj-CMake.bat [options]
REM
REM    ninja                use the Ninja generator (needs ninja.exe + MSVC)
REM    vs, vs2022, ...      auto-detect Visual Studio (default behaviour)
REM    debug|release|shipping
REM                         build configuration (default: debug)
REM    --dir <path>         build directory (default: build)
REM    --clean, -c          delete the build directory before configuring
REM    --no-pause           do not wait for a key press at the end
REM    -h, --help, /?       show this help
REM
REM  Notes:
REM   * With no generator argument the newest Visual Studio installed on this
REM     machine is used, provided the selected CMake knows that generator.
REM   * The CMake shipped with Visual Studio is preferred, because it is the
REM     only CMake guaranteed to support the matching VS generator.
REM ===========================================================================

setlocal enabledelayedexpansion

set "RC=1"
set "SCRIPT_NAME=%~nx0"

REM ---------------------------------------------------------------------------
REM Always work in the directory that holds this script.
REM Double-clicking from Explorer may leave the current directory anywhere, and
REM every path below is relative to the project root.
REM ---------------------------------------------------------------------------
set "PROJECT_ROOT=%~dp0"
if "%PROJECT_ROOT:~-1%"=="\" set "PROJECT_ROOT=%PROJECT_ROOT:~0,-1%"
set "PUSHED="
pushd "%PROJECT_ROOT%" 2>nul
if errorlevel 1 goto :err_no_dir
set "PUSHED=1"

echo ========================================
echo Helios CMake Project Generator (Windows)
echo ========================================
echo Project root: %PROJECT_ROOT%
echo.

REM ---------------------------------------------------------------------------
REM Use the Windows tool binaries explicitly. Dev machines usually have a
REM Git/Cygwin/MSYS "sort" earlier on PATH, which would shadow sort.exe and
REM fail with "sort: cannot read: /R".
REM The variables stay unquoted on purpose: SystemRoot never contains spaces,
REM and a leading quote inside `for /f ... in (`cmd`)` gets stripped by cmd.
REM ---------------------------------------------------------------------------
set "FINDSTR=%SystemRoot%\System32\findstr.exe"
set "SORTEXE=%SystemRoot%\System32\sort.exe"
set "WHEREEXE=%SystemRoot%\System32\where.exe"
if not exist "%FINDSTR%" set "FINDSTR=findstr"
if not exist "%SORTEXE%" set "SORTEXE=sort"
if not exist "%WHEREEXE%" set "WHEREEXE=where"

REM ---------------------------------------------------------------------------
REM Defaults
REM ---------------------------------------------------------------------------
set "BUILD_TYPE=Debug"
set "BUILD_DIR=build"
set "GENERATOR="
set "VS_GEN="
set "CLEAN=0"
set "NO_PAUSE=0"
set "VS_NAME="
set "VS_PATH="
set "VS_MAJOR="
set "CMAKE_EXE="
set "CMAKE_VER="
set "VS_OUT="
set "VS_RESULT="
set "NINJA_EXE="
set "NINJA_VER="
set "ABORT="

REM ---------------------------------------------------------------------------
REM Parse arguments.
REM Every branch jumps out of the if-block: a plain `set` inside a one-line
REM block would keep the trailing space before "&", corrupting the value.
REM ---------------------------------------------------------------------------
:parse_args
if "%~1"=="" goto :end_parse
set "ARG=%~1"

if /i "!ARG!"=="-h"         goto :usage
if /i "!ARG!"=="--help"     goto :usage
if /i "!ARG!"=="/?"         goto :usage
if /i "!ARG!"=="--no-pause" ( set "NO_PAUSE=1" & goto :next_arg )
if /i "!ARG!"=="--clean"    ( set "CLEAN=1" & goto :next_arg )
if /i "!ARG!"=="-c"         ( set "CLEAN=1" & goto :next_arg )
if /i "!ARG!"=="ninja"      ( set "GENERATOR=Ninja" & goto :next_arg )
if /i "!ARG!"=="debug"      ( set "BUILD_TYPE=Debug" & goto :next_arg )
if /i "!ARG!"=="release"    ( set "BUILD_TYPE=Release" & goto :next_arg )
if /i "!ARG!"=="shipping"   ( set "BUILD_TYPE=Shipping" & goto :next_arg )
if /i "!ARG!"=="--dir" (
    set "BUILD_DIR=%~2"
    shift
    if not defined BUILD_DIR goto :err_bad_dir
    goto :next_arg
)
REM "vs", "vs2019", "vs2022", ... : kept for compatibility, auto-detect decides.
if /i "!ARG:~0,2!"=="vs" (
    echo NOTE: Visual Studio version is auto-detected, ignoring "!ARG!".
    goto :next_arg
)
echo [ERROR] Unknown argument: "!ARG!"
set "UNKNOWN=1"
goto :usage

:next_arg
shift
goto :parse_args
:end_parse

if not defined BUILD_DIR set "BUILD_DIR=build"

REM ---------------------------------------------------------------------------
REM Locate vswhere.exe (ships with the VS 2017+ installer)
REM ---------------------------------------------------------------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE="

call :detect_vs

REM ---------------------------------------------------------------------------
REM Prefer the CMake bundled with Visual Studio, fall back to PATH
REM ---------------------------------------------------------------------------
if defined VS_PATH (
    set "VS_CMAKE=!VS_PATH!\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    if exist "!VS_CMAKE!" set "CMAKE_EXE=!VS_CMAKE!"
)

if not defined CMAKE_EXE (
    for /f "usebackq delims=" %%i in (`%WHEREEXE% cmake 2^>nul`) do (
        if not defined CMAKE_EXE set "CMAKE_EXE=%%i"
    )
)

if not defined CMAKE_EXE goto :err_no_cmake

set "VER_OUT=!TEMP!\helios_ver_!RANDOM!!RANDOM!.txt"
"!CMAKE_EXE!" --version > "!VER_OUT!" 2>nul
for /f "usebackq delims=" %%i in ("!VER_OUT!") do if not defined CMAKE_VER set "CMAKE_VER=%%i"
del "!VER_OUT!" 2>nul
if not defined CMAKE_VER set "CMAKE_VER=unknown version"
echo CMake:   !CMAKE_EXE!
echo Version: !CMAKE_VER!

REM ---------------------------------------------------------------------------
REM Pick a generator
REM ---------------------------------------------------------------------------
if not defined GENERATOR call :auto_pick_generator
if not defined GENERATOR goto :err_no_generator

if /i "!GENERATOR!"=="Ninja" (
    call :pick_ninja
    if not defined NINJA_EXE goto :err_no_ninja
    echo Ninja:  !NINJA_EXE! ^(version !NINJA_VER!^)
)

REM ---------------------------------------------------------------------------
REM Set up the MSVC environment.
REM Required by Ninja, harmless-but-useful for the VS generators.
REM ---------------------------------------------------------------------------
if defined VS_PATH (
    set "VCVARS=!VS_PATH!\VC\Auxiliary\Build\vcvarsall.bat"
    if exist "!VCVARS!" (
        echo.
        echo Setting up MSVC x64 environment...
        call "!VCVARS!" x64 >nul
        REM Record the result now: any echo in between would reset ERRORLEVEL
        if errorlevel 1 set "VC_FAIL=1"
    ) else (
        echo.
        echo [WARNING] vcvarsall.bat not found at:
        echo           !VCVARS!
        if /i "!GENERATOR!"=="Ninja" goto :err_no_msvc
    )
)
if defined VC_FAIL echo [WARNING] vcvarsall.bat reported an error - continuing.

if /i "!GENERATOR!"=="Ninja" (
    %WHEREEXE% cl >nul 2>nul
    if errorlevel 1 (
        echo.
        echo [WARNING] cl.exe is not on PATH. If configure fails, install the
        echo           "Desktop development with C++" workload, or run this
        echo           script from a Visual Studio Developer Prompt.
    )
)

REM ---------------------------------------------------------------------------
REM VS 2017 and newer take the architecture through -A. Older generators encode
REM it in the generator name instead.
REM ---------------------------------------------------------------------------
set "VS_GEN=!GENERATOR!"
set "ARCH_ARGS=-A x64"
if defined VS_MAJOR if !VS_MAJOR! LSS 15 (
    set "VS_GEN=!GENERATOR! Win64"
    set "ARCH_ARGS="
)

echo.
echo Using generator: !GENERATOR!
echo Build type:      !BUILD_TYPE!
echo Build directory: !BUILD_DIR!
echo.

REM ---------------------------------------------------------------------------
REM A build directory keeps its original generator and CMake refuses to switch.
REM Offer to wipe it instead of printing a raw generator mismatch error.
REM ---------------------------------------------------------------------------
set "EXISTING_GENERATOR="
if exist "!BUILD_DIR!\CMakeCache.txt" (
    for /f "usebackq tokens=1,* delims==" %%a in (`%FINDSTR% /B /C:"CMAKE_GENERATOR:INTERNAL=" "!BUILD_DIR!\CMakeCache.txt"`) do set "EXISTING_GENERATOR=%%b"
)

if defined EXISTING_GENERATOR if /i not "!EXISTING_GENERATOR!"=="!GENERATOR!" call :resolve_conflict
if defined ABORT goto :aborted

if "!CLEAN!"=="1" if exist "!BUILD_DIR!" (
    echo Removing "!BUILD_DIR!" ...
    rmdir /s /q "!BUILD_DIR!"
    if exist "!BUILD_DIR!" goto :err_clean
)

REM ---------------------------------------------------------------------------
REM Configure
REM ---------------------------------------------------------------------------
if not exist "!BUILD_DIR!" mkdir "!BUILD_DIR!"
if not exist "!BUILD_DIR!" goto :err_mkdir

if /i "!GENERATOR!"=="Ninja" (
    echo ^> cmake -S . -B !BUILD_DIR! -G Ninja -DCMAKE_MAKE_PROGRAM=!NINJA_EXE! -DCMAKE_BUILD_TYPE=!BUILD_TYPE!
    echo.
    "!CMAKE_EXE!" -S "%PROJECT_ROOT%" -B "!BUILD_DIR!" -G Ninja -DCMAKE_MAKE_PROGRAM="!NINJA_EXE!" -DCMAKE_BUILD_TYPE=!BUILD_TYPE!
) else (
    echo ^> cmake -S . -B !BUILD_DIR! -G "!VS_GEN!" !ARCH_ARGS!
    echo.
    "!CMAKE_EXE!" -S "%PROJECT_ROOT%" -B "!BUILD_DIR!" -G "!VS_GEN!" !ARCH_ARGS!
)

if errorlevel 1 goto :err_configure

REM ---------------------------------------------------------------------------
REM Report the real solution / project file instead of guessing its name
REM ---------------------------------------------------------------------------
set "SOLUTION="
for %%f in ("!BUILD_DIR!\*.slnx") do if not defined SOLUTION set "SOLUTION=%%~nxf"
if not defined SOLUTION for %%f in ("!BUILD_DIR!\*.sln") do if not defined SOLUTION set "SOLUTION=%%~nxf"

echo.
echo ========================================
echo CMake project files generated successfully!
echo Build directory: !BUILD_DIR!
echo ========================================
echo.
echo Next steps:

if /i "!GENERATOR!"=="Ninja" (
    echo   Build:  cmake --build !BUILD_DIR!
) else (
    if defined SOLUTION echo   Open:   !BUILD_DIR!\!SOLUTION!
    echo   IDE:    build the solution in Visual Studio ^(x64^)
    echo   Build:  cmake --build !BUILD_DIR! --config !BUILD_TYPE!
)
echo.

set "RC=0"
goto :cleanup

REM ===========================================================================
REM Subroutines
REM ===========================================================================

REM --- read the newest installed Visual Studio -------------------------------
REM NOTE: vswhere output goes through a redirect instead of
REM `for /f ... in (`cmd`)`. cmd wraps backquoted commands in `cmd /c "..."`,
REM and that strips the quotes around "C:\Program Files\..." so the command
REM breaks on installs whose path contains spaces.
:detect_vs
if not defined VSWHERE goto :eof
call :vs_query -latest -products * -property installationPath
if not defined VS_RESULT goto :eof
set "VS_PATH=!VS_RESULT!"
call :vs_query -latest -products * -property displayName
if defined VS_RESULT set "VS_NAME=!VS_RESULT!"
echo Detected: !VS_NAME!
echo Install:  !VS_PATH!
goto :eof

REM --- build the candidate list from "cmake --help" and probe each VS major ---
:auto_pick_generator
set "HELP_FILE=!TEMP!\helios_help_!RANDOM!!RANDOM!.txt"
set "RAW_LIST=!TEMP!\helios_gen_!RANDOM!!RANDOM!.txt"
set "SORTED_LIST=!TEMP!\helios_gen_sorted_!RANDOM!!RANDOM!.txt"

"!CMAKE_EXE!" --help > "!HELP_FILE!" 2>nul
if not exist "!HELP_FILE!" goto :apg_done
type nul > "!RAW_LIST!"
if not exist "!RAW_LIST!" goto :apg_done

REM One "Visual Studio NN YYYY" line per generator this CMake can emit.
REM The default generator is marked with a leading "*", so the columns shift.
for /f "usebackq delims=" %%L in (`%FINDSTR% /R /C:"Visual Studio [0-9][0-9] [0-9][0-9][0-9][0-9]" "!HELP_FILE!"`) do (
    call :append_generator "%%L"
)

REM Newest major first (two-digit majors sort correctly as plain strings)
"%SORTEXE%" /R < "!RAW_LIST!" > "!SORTED_LIST!" 2>nul
if not exist "!SORTED_LIST!" goto :apg_done

for /f "usebackq tokens=1,* delims=|" %%M in ("!SORTED_LIST!") do (
    if not defined GENERATOR call :try_vs_major "%%M" "%%N"
)

:apg_done
if defined HELP_FILE del "!HELP_FILE!" 2>nul
if defined RAW_LIST del "!RAW_LIST!" 2>nul
if defined SORTED_LIST del "!SORTED_LIST!" 2>nul
goto :eof

REM %1 = raw "cmake --help" line -> append "MAJOR|Visual Studio MAJOR YEAR"
:append_generator
set "LINE=%~1"
if "!LINE:~0,1!"=="*" set "LINE=!LINE:~2!"
set "G_MAJOR="
set "G_NAME="
for /f "tokens=1-4" %%a in ("!LINE!") do (
    set "G_MAJOR=%%c"
    set "G_NAME=%%a %%b %%c %%d"
)
if not defined G_MAJOR goto :eof
>> "!RAW_LIST!" echo !G_MAJOR!^|!G_NAME!
goto :eof

REM %1 = major version, %2 = generator name; succeeds when that VS is installed
:try_vs_major
set "TRY_MAJOR=%~1"
set "TRY_NAME=%~2"
if not defined TRY_MAJOR goto :eof

set "TRY_PATH="
if defined VSWHERE (
    set /a TRY_NEXT=TRY_MAJOR+1
    call :vs_query -version "[!TRY_MAJOR!.0,!TRY_NEXT!.0)" -latest -products * -property installationPath
    if defined VS_RESULT set "TRY_PATH=!VS_RESULT!"
)
if not defined TRY_PATH goto :eof

set "GENERATOR=!TRY_NAME!"
set "VS_PATH=!TRY_PATH!"
set "VS_MAJOR=!TRY_MAJOR!"
set "VS_NAME="
call :vs_query -path "!VS_PATH!" -property displayName
if defined VS_RESULT set "VS_NAME=!VS_RESULT!"
if defined VS_NAME echo Matched installed: !VS_NAME!
echo Auto-selected generator: !GENERATOR!
goto :eof

REM --- vswhere wrapper: first output line of the query lands in VS_RESULT ----
REM %* = vswhere arguments
:vs_query
set "VS_RESULT="
set "VS_OUT=!TEMP!\helios_vswhere_!RANDOM!!RANDOM!.txt"
"!VSWHERE!" %* > "!VS_OUT!" 2>nul
for /f "usebackq delims=" %%i in ("!VS_OUT!") do if not defined VS_RESULT set "VS_RESULT=%%i"
del "!VS_OUT!" 2>nul
goto :eof

REM --- choose a ninja that can actually run -------------------------------
REM "where ninja" may list shell-script shims first: depot_tools ships both a
REM "ninja" bash script and a "ninja.bat" wrapper. Running them just spawns
REM python/bash and CMake later reports a bare "unknown error", so only real
REM .exe candidates are considered, and each must answer "--version".
:pick_ninja
set "NINJA_EXE="
set "NINJA_VER="
for /f "usebackq delims=" %%i in (`%WHEREEXE% ninja 2^>nul`) do call :probe_ninja "%%i"
goto :eof

:probe_ninja
if defined NINJA_EXE goto :eof
if /i not "%~x1"==".exe" goto :eof
if not exist "%~1" goto :eof
set "NV_OUT=!TEMP!\helios_nv_!RANDOM!!RANDOM!.txt"
"%~1" --version > "!NV_OUT!" 2>nul
set "NV_RC=!ERRORLEVEL!"
set "NV="
for /f "usebackq delims=" %%v in ("!NV_OUT!") do if not defined NV set "NV=%%v"
del "!NV_OUT!" 2>nul
if not "!NV_RC!"=="0" goto :eof
if not defined NV goto :eof
set "NINJA_EXE=%~1"
set "NINJA_VER=!NV!"
goto :eof

REM --- generator mismatch in an existing build directory ---------------------
:resolve_conflict
echo.
echo [WARNING] "!BUILD_DIR!" was configured with generator:
echo             !EXISTING_GENERATOR!
echo           but this run selects:
echo             !GENERATOR!
echo           CMake cannot switch generators inside one build directory.
if "!CLEAN!"=="1" goto :eof
set "ANS="
set /p "ANS=Delete !BUILD_DIR! and regenerate? [y/N] "
if /i "!ANS!"=="y" ( set "CLEAN=1" & goto :eof )
if /i "!ANS!"=="yes" ( set "CLEAN=1" & goto :eof )
echo Cancelled - nothing was changed.
echo Pass --clean to do this non-interactively.
set "ABORT=1"
goto :eof

REM ===========================================================================
REM Exit paths
REM ===========================================================================
:usage
echo.
echo Usage: !SCRIPT_NAME! [ninja^|vs] [debug^|release^|shipping] [--dir ^<path^>] [--clean] [--no-pause]
echo.
echo   ninja            use the Ninja generator ^(needs ninja.exe and MSVC^)
echo   vs               auto-detect Visual Studio ^(default^)
echo   debug/release/shipping
echo                    build configuration, default debug
echo   --dir ^<path^>     build directory, default build
echo   --clean          delete the build directory before configuring
echo   --no-pause       do not wait for a key press at the end
echo.
set "NO_PAUSE=1"
set "RC=0"
if defined UNKNOWN set "RC=1"
goto :cleanup

:aborted
set "RC=1"
goto :cleanup

:err_no_dir
echo [ERROR] Cannot enter project directory: %PROJECT_ROOT%
goto :fatal
:err_bad_dir
echo [ERROR] --dir needs a directory name.
goto :fatal
:err_no_cmake
echo.
echo [ERROR] CMake was not found.
echo         Install CMake or the "Desktop development with C++" workload of
echo         Visual Studio, then run this script again.
goto :fatal
:err_no_ninja
echo.
echo [ERROR] No usable ninja.exe was found.
echo         Note that a "ninja" on PATH can be a shell script shim - depot_tools
echo         ships one - which CMake cannot run. Only executables that answer
echo         "ninja --version" are accepted here.
echo         Install a real build:  winget install Ninja-build.Ninja
echo         Or drop the "ninja" argument to use Visual Studio instead.
goto :fatal
:err_no_msvc
echo.
echo [ERROR] The Ninja generator requires the MSVC environment.
goto :fatal
:err_no_generator
echo.
echo [ERROR] No installed Visual Studio matches a generator this CMake knows.
echo         Installed CMake: !CMAKE_VER!
echo         Fix one of these:
echo           - Install Visual Studio 2019/2022/2026 with C++ tools
echo           - Use a newer CMake that knows your Visual Studio
echo           - Run:  !SCRIPT_NAME! ninja
goto :fatal
:err_clean
echo [ERROR] Could not remove "!BUILD_DIR!". Close Visual Studio or any process
echo         holding files in it, then try again.
goto :fatal
:err_mkdir
echo [ERROR] Could not create build directory "!BUILD_DIR!".
goto :fatal
:err_configure
echo.
echo ========================================
echo CMake configuration failed!
echo ========================================
goto :fatal

:fatal
set "RC=1"

:cleanup
echo.
if "!NO_PAUSE!"=="0" pause
if defined PUSHED popd
endlocal & exit /b %RC%

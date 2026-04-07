# Builds WebKit WinCairo for ARM32 Windows via vcpkg + MSVC.
#
# Strategy: vcpkg manages ALL library dependencies so no feature is disabled
# for lack of a stub.  A custom arm-windows-webkit triplet targets ARM MSVC
# with static library linkage and dynamic CRT (/MD), which is the same ABI
# as the rest of the ARM32 toolchain.
#
# Prerequisites (once per machine):
#   - Visual Studio 2019/2022 with ARM32 build tools
#   - Ninja (winget install Ninja-build.Ninja)
#   - CMake >= 3.20 (winget install Kitware.CMake)
#   - Git for Windows (includes Perl + gperf in usr\bin)
#   - RubyInstaller (rubyinstaller.org) - needed by WebKit code-gen scripts
#
# Run from an x64 Developer Command Prompt (or let Common-ARM32.ps1 set it up).

$ErrorActionPreference = 'Stop'

# Transcript captures ALL output (Write-Host, native stdout, stderr) to the log.
# Start-Transcript fails silently on Z: (VBoxSvr share) so we write to C: first,
# then copy to Z: at the end so Linux can see it.
$transcriptLog = 'C:\webkit-build-live.log'
$sharedLog     = 'Z:\prism-browser\deps\build-logs\webkit-build-live.log'
Start-Transcript -Path $transcriptLog -Append -Force | Out-Null

$depsDir  = $PSScriptRoot
$repoRoot = Split-Path $depsDir -Parent        # Z:\prism-browser
$srcDir   = "$depsDir\src\webkit"
$bldDir   = "$depsDir\build\webkit-arm32"
$outDir   = "$repoRoot\arm32"                  # final install tree

# vcpkg MUST live on a local drive - VBoxSvr (Z:\) network shares block
# writing .exe files.  C:\vcpkg-webkit is writable by all VS tools.
$vcpkgDir = "C:\vcpkg-webkit"

. "$depsDir\Common-ARM32.ps1"                  # sets $clExe, $cmake, $ninja, $armasmExe, env

# ── ARM32 build environment (INCLUDE / LIB / PATH) ───────────────────────────
# Common-ARM32.ps1 locates cl.exe but does not call vcvarsall.bat.
# vcpkg with VCPKG_CHAINLOAD_TOOLCHAIN_FILE also skips vcvarsall.
# Without it, INCLUDE is empty and cl.exe can't find stddef.h / stdio.h.
#
# Derive vcvarsall.bat from $clExe:
#   ...\VC\Tools\MSVC\<ver>\bin\HostX64\arm\cl.exe
#   ...\VC\Auxiliary\Build\vcvarsall.bat
$vsRoot    = $clExe -replace [regex]::Escape('\VC\Tools\MSVC\') + '.*', ''
$vcvarsall = "$vsRoot\VC\Auxiliary\Build\vcvarsall.bat"

if (!(Test-Path $vcvarsall)) {
    # Fallback: search via vswhere
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vsRoot    = & $vswhere -latest -property installationPath
        $vcvarsall = "$vsRoot\VC\Auxiliary\Build\vcvarsall.bat"
    }
}

if (Test-Path $vcvarsall) {
    Write-Host "==> Setting up ARM32 build environment ($vcvarsall x64_arm)..."
    # Run vcvarsall in cmd, dump the resulting env with 'set', parse back into PS.
    $envLines = cmd /c "`"$vcvarsall`" x64_arm > nul 2>&1 && set" 2>&1
    foreach ($line in $envLines) {
        if ($line -match '^([^=]+)=(.*)$') {
            [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
    Write-Host "    INCLUDE now has $( ($env:INCLUDE -split ';').Count ) directories."
} else {
    Write-Warning "vcvarsall.bat not found - INCLUDE/LIB may be empty and builds will fail."
    Write-Warning "Install the 'MSVC vXXX - C++ ARM build tools' component in VS Installer."
}

# ── Directory junctions for spaceless SDK paths ───────────────────────────────
# ARM32 SDK paths contain spaces ("Program Files", "Windows Kits", MSVC version
# number directories).  When autoconf passes $CFLAGS / $LDFLAGS unquoted to a
# compile/link test in bash, the shell word-splits the flags and breaks the paths.
# Fix: create Windows directory junctions (mklink /J) at a spaceless root under
# $vcpkgDir so every tool, including autoconf configure scripts, sees clean paths.
# No administrator rights are needed for directory junctions on the same volume.
# We then rewrite $env:INCLUDE and $env:LIB to use the junction-backed paths so
# $arm32IFlags / $arm32LibFlags (built below) are already spaceless.
$junctionRoot = "$vcpkgDir\arm32-junctions"
New-Item $junctionRoot -ItemType Directory -Force | Out-Null

function Get-SpacelessPath {
    param([string]$Path)
    if ($Path -notmatch ' ') { return $Path }
    # Use a stable 8-char hex hash of the lowercased path as the junction name.
    $h  = [System.Math]::Abs($Path.ToLower().GetHashCode()).ToString("X8")
    $jn = "$junctionRoot\$h"
    if (!(Test-Path $jn)) {
        # mklink /J requires cmd; PowerShell's New-Item -ItemType Junction works
        # on PS 5+ but silently fails if the target has a trailing backslash.
        $target = $Path.TrimEnd('\')
        New-Item -ItemType Junction -Path $jn -Target $target -Force | Out-Null
    }
    return $jn
}

if ($env:INCLUDE) {
    $env:INCLUDE = ($env:INCLUDE -split ';' | Where-Object { $_ -ne '' } |
        ForEach-Object { Get-SpacelessPath $_ }) -join ';'
    Write-Host "==> INCLUDE rewritten through spaceless junctions ($( ($env:INCLUDE -split ';' | Where-Object { $_ -ne '' }).Count ) dirs)"
}
if ($env:LIB) {
    $env:LIB = ($env:LIB -split ';' | Where-Object { $_ -ne '' } |
        ForEach-Object { Get-SpacelessPath $_ }) -join ';'
    Write-Host "==> LIB rewritten through spaceless junctions ($( ($env:LIB -split ';' | Where-Object { $_ -ne '' }).Count ) dirs)"
}

# ══════════════════════════════════════════════════════════════════════════════
# 1.  vcpkg bootstrap
# ══════════════════════════════════════════════════════════════════════════════

$git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
       Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$git) { $git = "git" }

if (!(Test-Path "$vcpkgDir\.git")) {
    Write-Host "==> Cloning vcpkg to $vcpkgDir..."
    & $git clone https://github.com/microsoft/vcpkg.git $vcpkgDir
    if ($LASTEXITCODE -ne 0) { throw "git clone vcpkg failed" }
}

if (!(Test-Path "$vcpkgDir\vcpkg.exe")) {
    Write-Host "==> Bootstrapping vcpkg..."
    & "$vcpkgDir\bootstrap-vcpkg.bat" -disableMetrics
    # bootstrap.ps1 downloads vcpkg.exe from GitHub releases.  If the
    # write still fails (antivirus/SmartScreen on the account), download directly.
    if (!(Test-Path "$vcpkgDir\vcpkg.exe")) {
        Write-Host "  bootstrap.ps1 write failed - downloading vcpkg.exe directly..."
        # Pin to a specific release so the binary matches the cloned source.
        $tag = (& $git -C $vcpkgDir log -1 --format="%as" HEAD) -replace '-','/'
        # Fallback to a known-good release that supports arm community triplet.
        $vcpkgExeUrl = "https://github.com/microsoft/vcpkg-tool/releases/download/2025-04-14/vcpkg.exe"
        Invoke-WebRequest $vcpkgExeUrl -OutFile "$vcpkgDir\vcpkg.exe" -UseBasicParsing
        if (!(Test-Path "$vcpkgDir\vcpkg.exe")) { throw "Could not obtain vcpkg.exe" }
    }
}

Write-Host "==> vcpkg: $(& "$vcpkgDir\vcpkg.exe" version 2>&1 | Select-Object -First 1)"

# ── custom overlay triplet + chainload toolchain ─────────────────────────────
# ARM32 Windows was moved to "community" status in modern vcpkg, and vcpkg's
# own VS toolchain detection won't find ARM32 unless the full MSBuild ARM32
# platform is registered (not just the cross-compiler cl.exe files).
#
# Fix: VCPKG_CHAINLOAD_TOOLCHAIN_FILE bypasses vcpkg's detection entirely and
# gives cmake the compiler paths we already resolved via Common-ARM32.ps1.

$overlayDir  = "$depsDir\vcpkg-triplets"
$tripletName = "arm-windows-webkit"
$tripletFile = "$overlayDir\$tripletName.cmake"
New-Item $overlayDir -ItemType Directory -Force | Out-Null

# ── Dummy m.lib (ARM32) ───────────────────────────────────────────────────────
# Several packages (notably brotli) unconditionally link the Unix math library
# "m" against CLI tool targets.  On Windows/MSVC there is no m.lib; the math
# functions are part of the CRT.  We cannot disable those exe targets (brotli
# 1.2.0 ignores BROTLI_BUILD_PROGRAMS), so we satisfy the linker by providing
# an empty stub m.lib.  The exe will link and vcpkg_copy_tools will succeed;
# we don't care whether the installed exe actually runs.
# The stub's directory is baked into CMAKE_EXE_LINKER_FLAGS_INIT so it is
# visible to the linker even when vcpkg resets the process environment.
$dummyLibDir = "$depsDir\build\arm32-dummy-libs"
New-Item $dummyLibDir -ItemType Directory -Force | Out-Null
$mLibPath = "$dummyLibDir\m.lib"
if (!(Test-Path $mLibPath)) {
    # Compile a minimal stub object then archive it.
    $stubSrc = "$dummyLibDir\m_stub.c"
    Set-Content $stubSrc "/* stub: satisfies MSVC linker when packages link -lm */" -Encoding ASCII
    $stubObj = "$dummyLibDir\m_stub.obj"
    & $clExe /c /TC /nologo "/Fo$stubObj" $stubSrc 2>&1 | Out-Null
    $libExe = $clExe -replace '\\cl\.exe$', '\lib.exe'
    & $libExe /machine:ARM /nologo "/out:$mLibPath" $stubObj 2>&1 | Out-Null
    if (Test-Path $mLibPath) {
        Write-Host "==> Created dummy ARM32 m.lib at $mLibPath"
    } else {
        Write-Warning "Failed to create dummy m.lib - brotli link may fail."
    }
} else {
    Write-Host "==> Dummy ARM32 m.lib already present."
}
$dummyLibDirFwd = $dummyLibDir -replace '\\', '/'

# Generate the chainload toolchain file using the actual $clExe path.
# Use forward slashes - CMake requires them even on Windows.
$chainloadFile = "$overlayDir\arm32-msvc-chainload.cmake"
$clFwd         = $clExe     -replace '\\','/'
$armAsmFwd     = $armasmExe -replace '\\','/'

# Bake INCLUDE dirs as explicit /I flags into CMAKE_C_FLAGS_INIT.
# When vcpkg invokes "cmake --build" as a subprocess it resets the environment,
# so $env:INCLUDE is gone and cl.exe can't find stddef.h / stdio.h.
# Embedding the paths in FLAGS_INIT writes them into build.ninja at configure
# time, making them available regardless of the environment at build time.
$arm32IFlags = ""
if ($env:INCLUDE) {
    $arm32IFlags = ($env:INCLUDE -split ';' |
        Where-Object { $_ -ne '' } |
        ForEach-Object {
            $p = $_ -replace '\\', '/'
            '/I\"' + $p + '\"'
        }) -join ' '
    Write-Host "    Embedding $( ($env:INCLUDE -split ';' | Where-Object { $_ -ne '' }).Count ) /I paths in CMAKE_C_FLAGS_INIT."
} else {
    Write-Warning "INCLUDE is empty - chainload will have no /I flags; CRT headers may not be found."
}

# Bake LIB dirs as /LIBPATH: flags into CMAKE_EXE_LINKER_FLAGS_INIT.
# Same problem as INCLUDE: vcpkg's cmake --build subprocess resets the env,
# so $env:LIB is gone and the linker can't find kernel32.lib etc.
# We also prepend the dummy-libs dir so stub m.lib is found before the SDK dirs.
$arm32LibFlags = '/LIBPATH:\"' + $dummyLibDirFwd + '\"'
if ($env:LIB) {
    $arm32LibFlags += ' ' + ($env:LIB -split ';' |
        Where-Object { $_ -ne '' } |
        ForEach-Object {
            $p = $_ -replace '\\', '/'
            '/LIBPATH:\"' + $p + '\"'
        }) -join ' '
    Write-Host "    Embedding $( ($env:LIB -split ';' | Where-Object { $_ -ne '' }).Count ) /LIBPATH entries in CMAKE_EXE_LINKER_FLAGS_INIT."
} else {
    Write-Warning "LIB is empty - linker may not find kernel32.lib / CRT libs."
}

# NOTE: We intentionally do NOT set $env:LDFLAGS here.
# vcpkg_configure_make builds CMAKE_SHARED_LINKER_FLAGS_INIT into LDFLAGS.
# Even though $env:LIB now uses spaceless junction paths, LDFLAGS would still
# contain /LIBPATH: entries which MSVC's link.exe does not understand when invoked
# via bash's autoconf glue (libtool/icu configure do their own link.exe wrapping).
# For MSVC, $env:LIB (rewritten to spaceless junctions above) is sufficient for
# the linker to find ARM32 CRT and SDK libs.
# Autoconf configure tests are bypassed via --cache-file= (arm32-icu-cache.sh).

# Find rc.exe from the Windows SDK.
# cmake's ARM chainload auto-detection falls back to "cl.exe rc" which cannot
# find rc.exe in PATH during vcpkg's subprocess, so the .res file is never
# produced (silent failure → LNK1181 at lib.exe step).
# RC compilation is host-architecture work; always use the x64 rc.exe.
$rcExe = $null
foreach ($incPath in ($env:INCLUDE -split ';' | Where-Object { $_ -ne '' })) {
    if ($incPath -match '(.*Windows Kits.10)\\Include\\(\d+\.\d+\.\d+\.\d+)') {
        $kitRoot    = $Matches[1]
        $kitVersion = $Matches[2]
        $candidate  = "$kitRoot\bin\$kitVersion\x64\rc.exe"
        if (Test-Path $candidate) { $rcExe = $candidate; break }
        $candidate  = "$kitRoot\bin\x64\rc.exe"
        if (Test-Path $candidate) { $rcExe = $candidate; break }
    }
}
if (!$rcExe) {
    # Broad fallback scan
    $rcExe = Get-ChildItem "C:\Program Files (x86)\Windows Kits\10\bin" `
        -Filter rc.exe -Recurse -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match 'x64' } |
        Select-Object -ExpandProperty FullName -First 1
}
if ($rcExe) {
    Write-Host "    RC compiler: $rcExe"
} else {
    Write-Warning "rc.exe not found - .rc resource files may fail to compile."
    $rcExe = "rc.exe"
}
$rcFwd = $rcExe -replace '\\', '/'

# nmake.exe lives in the x64 host bin dir (sibling of the arm\ subdir where cl.exe is).
# We pre-populate the NMAKE cmake cache variable in the chainload file so that
# vcpkg_build_nmake's find_program(NMAKE nmake REQUIRED) skips the PATH search
# entirely and uses our known path (avoids environment-propagation issues).
$hostBinDir = Split-Path (Split-Path $clExe)   # ...\bin\HostX64
$nmakeDir   = Join-Path $hostBinDir "x64"       # ...\bin\HostX64\x64
$nmakeExe   = $null

# Search multiple candidate locations.
$nmakeCandidates = @(
    "$nmakeDir\nmake.exe",
    # Some VS layouts put nmake in the VS Common Tools dir
    "$(Split-Path $vsRoot)\Common7\Tools\nmake.exe",
    "$vsRoot\Common7\Tools\nmake.exe"
)
foreach ($c in $nmakeCandidates) {
    if (Test-Path $c) { $nmakeExe = $c; break }
}
# Final fallback: search PATH
if (!$nmakeExe) {
    $nmakeCmd = Get-Command "nmake.exe" -ErrorAction SilentlyContinue
    if ($nmakeCmd) { $nmakeExe = $nmakeCmd.Source }
}
if ($nmakeExe) {
    Write-Host "    nmake: $nmakeExe"
} else {
    Write-Warning "nmake.exe not found in any expected location - openssl build will fail."
    $nmakeExe = "nmake.exe"  # hope it ends up in PATH
}
$nmakeFwd    = ($nmakeDir) -replace '\\', '/'
$nmakeExeFwd = $nmakeExe   -replace '\\', '/'
$vcpkgInstalledFwd = "$vcpkgDir/installed/$tripletName" -replace '\\', '/'

Set-Content $chainloadFile @"
# ARM32 MSVC chainload for vcpkg - generated by build-webkit-arm32.ps1
# Bypasses vcpkg's VS toolchain detection; uses the compiler already located
# by Common-ARM32.ps1.
set(CMAKE_SYSTEM_NAME      Windows)
set(CMAKE_SYSTEM_PROCESSOR ARM)
set(CMAKE_C_COMPILER   "$clFwd")
set(CMAKE_CXX_COMPILER "$clFwd")
set(CMAKE_ASM_MASM_COMPILER "$armAsmFwd")
# Cross-compiling ARM32 on x64: skip the link step in cmake's compiler tests.
# cmake cannot link ARM32 executables that run on the x64 host, and the test
# doesn't need to run - just compile to a .lib to verify the compiler works.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# Bake CRT include paths into every build.ninja that vcpkg generates.
# vcpkg resets the process environment between configure and build, so the
# INCLUDE variable set by vcvarsall.bat is gone when cl.exe actually runs.
# CMAKE_C_FLAGS_INIT / CMAKE_CXX_FLAGS_INIT survive into build.ninja.
set(CMAKE_C_FLAGS_INIT   "$arm32IFlags")
set(CMAKE_CXX_FLAGS_INIT "$arm32IFlags")
# Explicit rc.exe path: cmake's ARM chainload detection falls back to cl.exe
# which silently fails to produce a .res (no rc.exe in PATH in subprocess).
# RC compilation is host-architecture work so x64 rc.exe is always correct.
set(CMAKE_RC_COMPILER    "$rcFwd")
set(CMAKE_RC_FLAGS_INIT  "$arm32IFlags")
# ── Per-package cmake option overrides ────────────────────────────────────────
# libxml2: xmlmodule.c includes dl.h (Unix dlopen header) when WITH_MODULES=ON.
# cmake's cross-compile feature checks incorrectly detect HAVE_DL_H=1 on the
# host, causing a compile error on the ARM32 Windows target.  WebKit does not
# use libxml2's module-loading API so disabling it has no functional impact.
set(LIBXML2_WITH_MODULES OFF CACHE BOOL "Disable libxml2 dynamic module loading" FORCE)
# Bake all ARM32 LIB paths into linker flags as /LIBPATH: entries.
# vcpkg resets the process environment, so $env:LIB (from vcvarsall x64_arm) is
# gone when link.exe runs.  Without these paths the linker can't find kernel32.lib,
# msvcrt.lib, etc.  The dummy-libs dir is prepended so stub m.lib is found first.
#
# CMAKE_EXE_LINKER_FLAGS_INIT works correctly and is picked up by cmake-get-vars.
#
# CMAKE_SHARED_LINKER_FLAGS_INIT does NOT work when CMAKE_TRY_COMPILE_TARGET_TYPE
# is STATIC_LIBRARY: cmake never runs a shared-library link test, so it never
# initialises CMAKE_SHARED_LINKER_FLAGS from the _INIT variable.
# cmake-get-vars therefore reports only /machine:ARM (from cmake's MSVC ARM
# platform detection) and vcpkg writes that into the meson cross file's
# c_link_args — omitting all /LIBPATH: entries.  Without them, link.exe falls
# back to the ambient x64 LIB that vcpkg carries from its host-tools build,
# finds x64 kernel32.lib / MSVCRT.lib, and fails with LNK4272 + LNK1120.
#
# Fix: use CACHE STRING FORCE so the variable is set unconditionally in the
# cmake cache regardless of compile-test mode.  We include /machine:ARM
# explicitly because cmake's platform detection (which normally appends it via
# the INIT mechanism) is superseded by our FORCE set.
set(CMAKE_EXE_LINKER_FLAGS_INIT    "$arm32LibFlags advapi32.lib ws2_32.lib")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "$arm32LibFlags advapi32.lib ws2_32.lib")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "$arm32LibFlags advapi32.lib ws2_32.lib")
# nmake.exe is needed by the openssl port (vcpkg_build_nmake uses find_program).
# Pre-populate the NMAKE cmake cache variable so find_program(NMAKE nmake REQUIRED)
# skips its PATH search entirely and uses this resolved path directly.
# Also add to CMAKE_PROGRAM_PATH and ENV{PATH} as belt-and-suspenders.
# Note: `$ENV{PATH} uses a PS backtick-escape so cmake receives the literal
# cmake variable reference, not the PS value of `$env:PATH`.
set(NMAKE "$nmakeExeFwd" CACHE FILEPATH "NMake executable" FORCE)
list(APPEND CMAKE_PROGRAM_PATH "$nmakeFwd")
set(ENV{PATH} "$nmakeFwd;`$ENV{PATH}")
# vcpkg installed dir: make find_path/find_library directly search these dirs.
# CMAKE_INCLUDE_PATH and CMAKE_LIBRARY_PATH are searched directly by cmake's
# find_path/find_library, bypassing the CMAKE_FIND_ROOT_PATH prepend behavior
# that causes issues in non-cross-compile mode (both host+target = Windows).
list(INSERT CMAKE_INCLUDE_PATH 0
    "$vcpkgInstalledFwd/include"
)
list(INSERT CMAKE_LIBRARY_PATH 0
    "$vcpkgInstalledFwd/lib"
    "$vcpkgInstalledFwd/bin"
)
"@ -Encoding UTF8

$chainloadFwd = $chainloadFile -replace '\\','/'

# Hash the chainload content and embed it in the triplet file as a comment.
# vcpkg computes the per-triplet compiler hash from the TRIPLET FILE CONTENT
# (not the chainload content directly).  Without this, changing the chainload
# never invalidates vcpkg's cmake-get-vars cache, so the meson cross file
# c_link_args keeps being generated from stale detected flags.
# By including the hash in the triplet, every chainload change triggers a fresh
# cmake-get-vars run that reflects the new CMAKE_SHARED_LINKER_FLAGS_INIT.
$chainloadHash = (Get-FileHash $chainloadFile -Algorithm MD5).Hash
Write-Host "==> Chainload MD5: $chainloadHash (embedded in triplet for cache invalidation)"

Set-Content $tripletFile @"
set(VCPKG_TARGET_ARCHITECTURE arm)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "$chainloadFwd")
# Chainload MD5 (cache-bust): $chainloadHash
"@ -Encoding UTF8

# ── Invalidate ARM32 package cache when chainload changes ─────────────────────
# vcpkg ABI hashes are based on triplet content + compiler fingerprint.
# We embed the chainload MD5 in the triplet (above), so a new chainload hash
# means a new triplet hash --> new ABI --> all ARM32 cached packages are stale.
# Pre-check sentinel headers here so we can skip the installed-tree wipe if
# all deps are already present (avoids forcing a full dep rebuild just because
# a minor path/hash change triggered a chainload invalidation).
$earlyDepSentinels = @(
    "$vcpkgDir\installed\$tripletName\include\zlib.h",
    "$vcpkgDir\installed\$tripletName\include\unicode\uchar.h",
    "$vcpkgDir\installed\$tripletName\include\harfbuzz\hb.h",
    "$vcpkgDir\installed\$tripletName\include\cairo\cairo.h",
    "$vcpkgDir\installed\$tripletName\include\libpsl.h"
)
$earlyDepsOk = ($earlyDepSentinels | Where-Object { -not (Test-Path $_) }).Count -eq 0

$chainloadHashFile = "$overlayDir\chainload.md5"
$prevChainloadHash = if (Test-Path $chainloadHashFile) { Get-Content $chainloadHashFile -Raw } else { "" }
if ($prevChainloadHash.Trim() -ne $chainloadHash.Trim()) {
    if ($earlyDepsOk) {
        Write-Host "==> Chainload changed but all deps installed -- skipping installed-tree wipe, clearing cmake caches only."
    } else {
        Write-Host "==> Chainload changed ($($prevChainloadHash.Trim()) -> $chainloadHash) - purging ARM32 build cache..."
        foreach ($stale in @(
                "$vcpkgDir\installed\$tripletName",
                "$vcpkgDir\packages\*_$tripletName")) {
            Get-Item $stale -ErrorAction SilentlyContinue | Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    # Always clear cmake-get-vars caches so new chainload flags take effect.
    Get-ChildItem "$vcpkgDir\buildtrees" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object {
            $cmakeVars = "$($_.FullName)\cmake-get-vars_C_CXX-$tripletName-rel.cmake"
            if (Test-Path $cmakeVars) { Remove-Item $cmakeVars -Force }
            $cmakeVarsRel = "$($_.FullName)\cmake-vars-$tripletName-rel.cmake"
            if (Test-Path $cmakeVarsRel) { Remove-Item $cmakeVarsRel -Force }
        }
    Set-Content $chainloadHashFile $chainloadHash -Encoding ASCII
    if (-not $earlyDepsOk) {
        Write-Host "==> ARM32 cache purged. Full rebuild will run (packages are cached per-ABI so most will restore from binary cache)."
    }
} else {
    Write-Host "==> Chainload unchanged ($chainloadHash) - vcpkg package cache is valid."
}

# ── install dependencies ───────────────────────────────────────────────────────
# --allow-unsupported is required because many ports added explicit guards
# against arm (32-bit) after vcpkg deprecated the triplet.  The C/C++ code
# still compiles fine; the guards are just conservative policy choices.
# --overlay-triplets points to our local triplet file.

$vcpkg = "$vcpkgDir\vcpkg.exe"
$t     = $tripletName
$overlayFlag = "--overlay-triplets=$overlayDir"

# Guarantee the x64 MSVC host tools directory is in PATH before invoking vcpkg.
# vcvarsall x64_arm adds the ARM cross-compiler dir but not always the x64 host
# dir (HostX64\x64\) where nmake.exe lives.  vcpkg's cmake portfile scripts call
# find_program(nmake) and search PATH, so it must be present.
if ($nmakeExe -and (Test-Path $nmakeExe)) {
    $nmakeExeDir = Split-Path $nmakeExe
    if ($env:PATH -notlike "*$nmakeExeDir*") {
        $env:PATH = "$nmakeExeDir;$env:PATH"
        Write-Host "==> Prepended $nmakeExeDir to PATH (nmake.exe)"
    }
}

# ── Patch vcpkg_build_nmake.cmake ─────────────────────────────────────────────
# vcpkg portfiles run in cmake -P (script mode), which does NOT load toolchain
# files.  Everything we set in the chainload (CMAKE_PROGRAM_PATH, ENV{PATH},
# NMAKE cache) is invisible to the portfile cmake process.
# Direct patch: inject a pre-set for NMAKE before find_program() so cmake never
# has to search PATH.  find_program skips the search if the variable is cached.
if ($nmakeExe -and (Test-Path $nmakeExe)) {
    $nmakeCmakeFile = "$vcpkgDir\scripts\cmake\vcpkg_build_nmake.cmake"
    if (Test-Path $nmakeCmakeFile) {
        $content = Get-Content $nmakeCmakeFile -Raw
        $marker  = '# ARM32 cross: nmake pre-set by build-webkit-arm32.ps1'
        if ($content -notmatch [regex]::Escape($marker)) {
            $injection = "    $marker`r`n    if(NOT NMAKE)`r`n        set(NMAKE `"$nmakeExeFwd`" CACHE FILEPATH `"`" FORCE)`r`n    endif()`r`n"
            # Insert before the find_program(NMAKE ...) line
            $content = $content -replace '([ \t]*find_program\(NMAKE\b)', "$injection`$1"
            Set-Content $nmakeCmakeFile $content -Encoding UTF8
            Write-Host "==> Patched vcpkg_build_nmake.cmake with NMAKE=$nmakeExeFwd"
        } else {
            Write-Host "==> vcpkg_build_nmake.cmake already patched."
        }
    }
}

# ── Patch compile wrapper: inject INCLUDE as -I flags ────────────────────────
# The vcpkg-make 'compile' wrapper script is called for every cl.exe invocation
# during autoconf configure, including the preprocessor sanity check:
#   $CPP $CPPFLAGS conftest.c  →  compile cl.exe -E conftest.c
# CPPFLAGS is empty; cl.exe -E can't find limits.h without -I paths.
# Fix: inject -I flags from the INCLUDE env var so cl.exe always finds headers.
#
# Strategy: restore from a known-good local source every run, THEN patch.
# We copy from vcpkg's MSYS2 download or Git for Windows' bundled automake —
# both ship the standard compile wrapper, always clean.
# Only patch if the restored file is a sane size (< 200 KB).
$compileWrapper = "$vcpkgDir\installed\x64-windows\share\vcpkg-make\wrappers\compile"
$cwMarker = '# ARM32: inject INCLUDE env var as -I flags'

# Restore compile wrapper unconditionally before every ICU build attempt.
# Search order (highest to lowest confidence):
#   1. vcpkg's own MSYS2 download (same automake version vcpkg-make uses)
#   2. Git for Windows' bundled automake (always clean, compatible version)
# We NEVER rely on the installed file being clean - it has been corrupted by
# earlier (now-fixed) patch logic that incorrectly matched internal #! lines.
$cwRestored = $false

# --- Candidate 1: vcpkg's downloaded MSYS2 (tools/msys2/<hash>/usr/share/automake-*/compile)
$msys2Root = Get-ChildItem "$vcpkgDir\downloads\tools\msys2" -Directory 2>$null | Select-Object -First 1
if ($msys2Root) {
    $cwCandidate = Get-ChildItem "$($msys2Root.FullName)\usr\share" -Recurse -Filter "compile" -File 2>$null |
        Where-Object { $_.Length -gt 5000 -and $_.Length -lt 200000 -and $_.DirectoryName -match 'automake' } |
        Select-Object -First 1
    if ($cwCandidate) {
        Copy-Item $cwCandidate.FullName $compileWrapper -Force
        Write-Host "==> Restored compile wrapper from vcpkg MSYS2: $($cwCandidate.FullName) ($($cwCandidate.Length) bytes)"
        $cwRestored = $true
    }
}

# --- Candidate 2: Git for Windows bundled automake
if (-not $cwRestored) {
    $gitRoots = @("C:\Program Files\Git", "C:\Program Files (x86)\Git")
    foreach ($gr in $gitRoots) {
        if (!(Test-Path $gr)) { continue }
        $cwCandidate = Get-ChildItem "$gr\usr\share" -Recurse -Filter "compile" -File 2>$null |
            Where-Object { $_.Length -gt 5000 -and $_.Length -lt 200000 -and $_.DirectoryName -match 'automake' } |
            Select-Object -First 1
        if ($cwCandidate) {
            Copy-Item $cwCandidate.FullName $compileWrapper -Force
            Write-Host "==> Restored compile wrapper from Git for Windows: $($cwCandidate.FullName) ($($cwCandidate.Length) bytes)"
            $cwRestored = $true
            break
        }
    }
}

if (-not $cwRestored) {
    Write-Warning "Could not find a clean compile wrapper in MSYS2 or Git for Windows."
    Write-Warning "Current wrapper: $(if (Test-Path $compileWrapper) { (Get-Item $compileWrapper).Length } else { 'MISSING' }) bytes"
}

# Patch: inject INCLUDE env var as -I flags.
# Only proceed if the installed wrapper is sane-sized (clean restore succeeded).
$cwInstalled = if (Test-Path $compileWrapper) { [System.IO.File]::ReadAllText($compileWrapper) } else { $null }
$cwInstalledSize = if ($cwInstalled) { $cwInstalled.Length } else { 0 }
Write-Host "==> compile wrapper size after restore: $cwInstalledSize chars"

if ($cwInstalled -and $cwInstalledSize -lt 200000) {
    if ($cwInstalled -notmatch [regex]::Escape($cwMarker)) {
        # Injection text - strip \r so the file has LF-only line endings throughout.
        # IMPORTANT: do NOT use -replace to insert this into the file.  The replacement
        # string in .NET regex gives $_ special meaning (= entire input string), so any
        # variable like $_arm32_inc_args silently embeds the whole 53 KB wrapper.
        # Use IndexOf/Substring instead - safe with any content.
        $cwInject = @'
# ARM32: inject INCLUDE env var as -I flags after the compiler name.
# The compile wrapper is called as: compile <compiler> [flags...]
# $1 is the compiler (cl.exe); -I flags must go after it, not before,
# or the case statement that dispatches to func_cl_wrapper won't match.
if [ -n "${INCLUDE:-}" ]; then
    _arm32_prog="$1"; shift
    _arm32_inc_args=""
    _arm32_IFS_save="$IFS"
    IFS=";"
    for _arm32_inc_dir in ${INCLUDE}; do
        if [ -n "$_arm32_inc_dir" ]; then
            _arm32_inc_dir=$(printf '%s' "$_arm32_inc_dir" | tr '\\' '/')
            _arm32_inc_args="$_arm32_inc_args -I$_arm32_inc_dir"
        fi
    done
    IFS="$_arm32_IFS_save"
    set -- "$_arm32_prog" $_arm32_inc_args "$@"
    unset _arm32_prog _arm32_inc_args _arm32_inc_dir _arm32_IFS_save
fi

'@ -replace '\r', ''

        # Normalize the restored file to LF-only as well.
        $cwInstalled = $cwInstalled -replace '\r', ''

        # Insert injection after the first newline (end of shebang line).
        # IndexOf/Substring never interprets $ in content - safe for sh variables.
        $firstNL = $cwInstalled.IndexOf("`n")
        if ($firstNL -lt 0) {
            Write-Warning "compile wrapper has no newline - cannot patch"
        } else {
            $cwPatched = $cwInstalled.Substring(0, $firstNL + 1) + $cwInject + $cwInstalled.Substring($firstNL + 1)
            # Write without BOM - #!/bin/sh breaks if EF BB BF precedes the shebang.
            [System.IO.File]::WriteAllText($compileWrapper, $cwPatched, [System.Text.UTF8Encoding]::new($false))
            Write-Host "==> Patched compile wrapper: INCLUDE -> -I flags for ARM32 cl.exe"
        }
    } else {
        Write-Host "==> compile wrapper already has ARM32 patch."
    }
} elseif ($cwInstalledSize -ge 200000) {
    Write-Warning "compile wrapper is still $cwInstalledSize chars after restore - patch NOT applied, ICU configure WILL fail!"
} else {
    Write-Warning "compile wrapper not found at $compileWrapper"
}

# ── Patch brotli portfile ─────────────────────────────────────────────────────
# brotli: the CLI tool (brotli.exe) unconditionally links against the Unix math
# library "m" regardless of any cmake option.  brotli 1.2.0 has no cmake option
# to disable this; the BROTLI_BUILD_PROGRAMS injection above has no effect.
# The dummy m.lib + CMAKE_EXE_LINKER_FLAGS_INIT /LIBPATH approach above is the
# definitive fix.  Clean the brotli buildtree so the new chainload (with
# CMAKE_EXE_LINKER_FLAGS_INIT) is picked up by a fresh cmake configure.
$brotliBuild = "$vcpkgDir\buildtrees\brotli"
if (Test-Path $brotliBuild) {
    Remove-Item $brotliBuild -Recurse -Force
    Write-Host "==> Cleaned brotli buildtree (fresh configure for /LIBPATH dummy-libs)"
}

# ── Strip pthreads/pthread from ports that don't need it on ARM32 Windows ─────
# pthreads4w's nmake build hardcodes __PTW32_ARCHx86 and calls `rc` without a
# full path, so it can't be cross-compiled to ARM32 via our chainload.
# All affected libraries (pixman, cairo) have Win32 thread/mutex fallbacks that
# are correct for our ARM32 MSVC target.  Remove the pthread/pthreads dependency
# from each port's vcpkg.json so vcpkg never tries to build pthreads4w.
function Remove-PthreadsDep {
    param([string]$JsonPath, [string]$PortName)
    if (!(Test-Path $JsonPath)) { return }
    $obj = Get-Content $JsonPath -Raw | ConvertFrom-Json
    $pthreadNames = @('pthread','pthreads','pthreads-windows','PThreads_windows','PThreads4W')
    $before = @($obj.dependencies).Count
    $obj.dependencies = @($obj.dependencies | Where-Object {
        $dep = $_
        if ($dep -is [string]) { $pthreadNames -notcontains $dep }
        elseif ($dep -is [System.Management.Automation.PSCustomObject]) {
            $pthreadNames -notcontains $dep.name
        } else { $true }
    })
    if (@($obj.dependencies).Count -lt $before) {
        $obj | ConvertTo-Json -Depth 10 | Set-Content $JsonPath -Encoding UTF8
        Write-Host "==> Patched $PortName vcpkg.json: removed pthread(s) dependency"
        # Clean stale package/buildtree state for this port
        foreach ($name in @($PortName, 'pthreads', 'pthread')) {
            foreach ($sub in @("$vcpkgDir\buildtrees\$name","$vcpkgDir\packages\${name}_$t")) {
                if (Test-Path $sub) { Remove-Item $sub -Recurse -Force }
            }
        }
    } else {
        Write-Host "==> $PortName vcpkg.json: no pthread dep found (already clean)."
    }
}

Remove-PthreadsDep "$vcpkgDir\ports\pixman\vcpkg.json" "pixman"
Remove-PthreadsDep "$vcpkgDir\ports\cairo\vcpkg.json"  "cairo"
# Pre-emptively patch harfbuzz and woff2 in case they also declare a pthread dep
Remove-PthreadsDep "$vcpkgDir\ports\harfbuzz\vcpkg.json" "harfbuzz"
Remove-PthreadsDep "$vcpkgDir\ports\woff2\vcpkg.json"    "woff2"

# ── Patch scripts/get_cmake_vars/CMakeLists.txt ──────────────────────────────
# vcpkg uses this cmake script to detect compiler/linker flags once per triplet.
# The result is cached in each package's buildtree as cmake-vars-<triplet>-rel.cmake
# and is used by vcpkg_configure_meson to generate the meson cross file.
#
# Problem: cmake 4.x does NOT propagate CMAKE_SHARED_LINKER_FLAGS_INIT (set in
# our chainload) to the cmake cache variable CMAKE_SHARED_LINKER_FLAGS when
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY, because a shared-library link
# test is never run and the INIT→cache propagation only happens during that test.
# The cmake-get-vars script reads CMAKE_SHARED_LINKER_FLAGS from the cmake cache
# (which is just "-machine:ARM" from MSVC platform detection) and writes it to
# VCPKG_DETECTED_CMAKE_SHARED_LINKER_FLAGS_RELEASE.  vcpkg_configure_meson then
# writes THAT into c_link_args — missing all /LIBPATH: entries.  Without ARM32
# SDK lib paths in c_link_args, meson's link.exe invocations fall back to the
# x64 LIB from vcpkg's host-tools environment → LNK4272 + LNK1120 wrong arch.
#
# Fix: patch get_cmake_vars/CMakeLists.txt to merge CMAKE_<X>_LINKER_FLAGS_INIT
# into the cmake cache variables AFTER project() runs.  The INIT variable IS
# available in cmake scope at that point (project() triggers platform detection
# which sets INIT but doesn't cache it); we just force it into the cache ourselves.
$getCmakeVarsFile = "$vcpkgDir\scripts\get_cmake_vars\CMakeLists.txt"
if (Test-Path $getCmakeVarsFile) {
    $gcv = Get-Content $getCmakeVarsFile -Raw
    $gcvMarker = '# ARM32 MSVC: merge LINKER_FLAGS_INIT into cache'
    if ($gcv -notmatch [regex]::Escape($gcvMarker)) {
        # Inject immediately after the project() line so the INIT vars are
        # already populated by cmake's MSVC ARM platform detection.
        $gcvInject = @'
# ARM32 MSVC: merge LINKER_FLAGS_INIT into cache
# cmake 4.x does not propagate CMAKE_<X>_LINKER_FLAGS_INIT to the cmake cache
# variable when CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY (no shared-library
# link test is run, so the INIT->cache propagation never fires).
# Force-merge them here so that cmake-vars detection reports the correct ARM32
# /LIBPATH: entries, which vcpkg_configure_meson writes into the meson cross
# file's c_link_args.
foreach(_gcv_lt SHARED EXE MODULE)
  if(CMAKE_${_gcv_lt}_LINKER_FLAGS_INIT)
    set(CMAKE_${_gcv_lt}_LINKER_FLAGS
        "${CMAKE_${_gcv_lt}_LINKER_FLAGS_INIT} ${CMAKE_${_gcv_lt}_LINKER_FLAGS}"
        CACHE STRING "" FORCE)
  endif()
endforeach()
unset(_gcv_lt)
'@
        # Find the project() line and insert after it
        $gcv = $gcv -replace '(project\(get_cmake_vars[^\)]*\))', "`$1`n$gcvInject"
        Set-Content $getCmakeVarsFile $gcv -Encoding UTF8
        Write-Host "==> Patched get_cmake_vars/CMakeLists.txt: ARM32 linker flags INIT->cache merge"
    } else {
        Write-Host "==> get_cmake_vars/CMakeLists.txt already patched."
    }
}

# Clean meson-based buildtrees so they reconfigure with the updated cmake-vars patch.
# The cmake-vars-<triplet>-rel.cmake cache files (which lack ARM32 /LIBPATH:) are
# inside each package's buildtree; removing the buildtrees forces re-detection.
foreach ($bt in @(
        "$vcpkgDir\buildtrees\pixman", "$vcpkgDir\packages\pixman_$t",
        "$vcpkgDir\buildtrees\cairo",  "$vcpkgDir\packages\cairo_$t")) {
    if (Test-Path $bt) { Remove-Item $bt -Recurse -Force }
}

# ── Write supplementary meson cross-file for cmake compiler-test fix ─────────
# meson's cmake sub-invocations use CMakeMesonTempToolchainFile.cmake generated
# from the [cmake] section of ALL cross-files meson was given.  The main vcpkg
# cross-file lacks CMAKE_TRY_COMPILE_TARGET_TYPE so cmake tries to LINK a test
# ARM exe on the x64 host -> LNK4272/LNK1120.
#
# Fix: pass an extra cross-file via VCPKG_MESON_CROSS_FILE (set in the triplet).
# It appends to arg_OPTIONS_RELEASE after the vcpkg-generated cross file, so
# meson's last-definition-wins rule applies our c_winlibs (with advapi32) over
# vcpkg's. Also carries CMAKE_TRY_COMPILE_TARGET_TYPE so cmake runs compile-only.
$arm32CmakeFix = "$vcpkgDir\arm32-cmake-fix.ini"
$arm32CmakeFixContent = "[cmake]`r`nCMAKE_TRY_COMPILE_TARGET_TYPE = 'STATIC_LIBRARY'`r`n" +
    "CMAKE_OSX_ARCHITECTURES = 'arm'`r`n" +
    "[built-in options]`r`n" +
    "c_winlibs = ['kernel32.lib', 'user32.lib', 'advapi32.lib', 'ws2_32.lib']`r`n" +
    "cpp_winlibs = ['kernel32.lib', 'user32.lib', 'advapi32.lib', 'ws2_32.lib']`r`n"
[System.IO.File]::WriteAllText($arm32CmakeFix, $arm32CmakeFixContent, [System.Text.UTF8Encoding]::new($false))
Write-Host "==> Wrote supplementary meson cross-file: $arm32CmakeFix"

# ── Patch meson.template.in: inject CMAKE_TRY_COMPILE_TARGET_TYPE ────────────
# meson runs cmake sub-invocations (compiler detection, find_package) using a
# CMakeMesonToolchainFile.cmake generated from the cross-file [cmake] section.
# This toolchain does NOT include our vcpkg chainload, so
# CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY (from chainload) is absent.
# cmake links a test ARM32 exe on the x64 host → LNK4272/LNK1120 (x64 libs).
#
# Fix: patch the meson template so every generated cross-file includes
# CMAKE_TRY_COMPILE_TARGET_TYPE = 'STATIC_LIBRARY' in the [cmake] section.
# cmake then uses a static-library target for compiler tests (no link step).
#
# Previous attempt patched vcpkg_configure_meson.cmake to use cmake
# string(REGEX REPLACE) on the generated file — that silently failed because
# cmake's regex engine treats \r as literal 'r', not CR, so the CRLF-terminated
# [cmake] section header was never matched.
$mesonTemplate = "$vcpkgDir\scripts\buildsystems\meson\meson.template.in"
$mesonTemplateMarker = 'CMAKE_TRY_COMPILE_TARGET_TYPE'
if (Test-Path $mesonTemplate) {
    Push-Location $vcpkgDir
    & $git checkout -- "scripts/buildsystems/meson/meson.template.in" 2>&1 | Out-Null
    Pop-Location
    $mt = [System.IO.File]::ReadAllText($mesonTemplate)
    if ($mt -notmatch [regex]::Escape($mesonTemplateMarker)) {
        # Insert after the [cmake] section header (handles both CRLF and LF).
        # The replacement captures the [cmake] header + its line ending and
        # appends our variable on the next line.
        $mt = $mt -replace '(\[cmake\]\r?\n)', "`$1CMAKE_TRY_COMPILE_TARGET_TYPE = 'STATIC_LIBRARY'`r`n"
        if ($mt -match [regex]::Escape($mesonTemplateMarker)) {
            [System.IO.File]::WriteAllText($mesonTemplate, $mt, [System.Text.UTF8Encoding]::new($false))
            Write-Host "==> Patched meson.template.in: CMAKE_TRY_COMPILE_TARGET_TYPE = STATIC_LIBRARY"
        } else {
            Write-Warning "meson.template.in: [cmake] section not found - CMAKE_TRY_COMPILE_TARGET_TYPE NOT injected"
        }
    } else {
        Write-Host "==> meson.template.in already has CMAKE_TRY_COMPILE_TARGET_TYPE."
    }
} else {
    Write-Warning "meson.template.in not found at $mesonTemplate - CMAKE_TRY_COMPILE_TARGET_TYPE NOT injected"
}
# ── Patch vcpkg_configure_meson.cmake: inject CMAKE_TRY_COMPILE_TARGET_TYPE ──
# meson's cmake sub-invocations use CMakeMesonTempToolchainFile.cmake generated
# from the cross-file [cmake] section.  This file has NO include(vcpkg.cmake)
# so CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY from the chainload never
# reaches cmake's __CMake_compiler_info__ step.  cmake then tries to link an
# ARM exe on the x64 host → LNK4272/LNK1120.
#
# Previous attempt patched meson.template.in (from which configure_file generates
# the cross-file) but the cross-file was not regenerated from our patched template
# (cause TBD — the build-logs copy of the template IS patched, but the cross-file
# consistently shows the upstream content).
#
# Reliable fix: patch vcpkg_configure_meson.cmake to post-process the generated
# cross-file immediately after configure_file() and before meson setup, injecting
# CMAKE_TRY_COMPILE_TARGET_TYPE = 'STATIC_LIBRARY' into the [cmake] section.
# cmake then uses STATIC_LIBRARY target type (compile-only, no link) for compiler
# tests → no LNK4272/LNK1120.
# vcpkg-tool-meson is an installed port; vcpkg loads cmake helper scripts from
# installed/x64-windows/share/vcpkg-tool-meson/ NOT scripts/cmake/.
# Both locations must be patched; the installed copy is what actually executes.
$vcmFile = "$vcpkgDir\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake"
if (-not (Test-Path $vcmFile)) {
    Write-Warning "vcpkg-tool-meson installed copy not found, falling back to scripts/cmake/"
    $vcmFile = "$vcpkgDir\scripts\cmake\vcpkg_configure_meson.cmake"
}
Write-Host "==> vcpkg_configure_meson.cmake target: $vcmFile"
# Dump the installed vcpkg-tool-meson share dir so we can inspect it
$vcmShareDir = "$vcpkgDir\installed\x64-windows\share\vcpkg-tool-meson"
if (Test-Path $vcmShareDir) {
    Get-ChildItem $vcmShareDir | ForEach-Object { Write-Host "==>   vcpkg-tool-meson share: $($_.Name)" }
    # Copy the entire share dir to build-logs for inspection (only if $logDst is defined)
    if ($logDst) {
        Get-ChildItem $vcmShareDir -File | ForEach-Object {
            Copy-Item $_.FullName "$logDst\vcpkg-tool-meson-$($_.Name)" -Force -ErrorAction SilentlyContinue
        }
    }
} else {
    Write-Warning "vcpkg-tool-meson share dir not found: $vcmShareDir"
}
$vcmCmakeMarker = '# ARM32: advapi32 winlibs cross-file post-processing'
if (Test-Path $vcmFile) {
    $vcm = [System.IO.File]::ReadAllText($vcmFile)
    Write-Host "==>   vcpkg_configure_meson.cmake size: $($vcm.Length) chars, already-patched: $($vcm -match [regex]::Escape($vcmCmakeMarker))"
    if ($vcm -notmatch [regex]::Escape($vcmCmakeMarker)) {
        # Single-quoted here-string: PowerShell does NOT expand $, \n, \r here.
        # cmake sees ${...} as cmake variable references, \n as LF in string literals.
        # Strategy: write a supplementary mini cross-file and append --cross-file to
        # arg_OPTIONS so meson merges it with the main cross-file.  This avoids any
        # string(REPLACE) on the generated cross-file (CRLF/LF fragility).
        $vcmInject = @'
        # ARM32: CMAKE_TRY_COMPILE_TARGET_TYPE cross-file injection
        # meson generates CMakeMesonTempToolchainFile.cmake from the [cmake] sections
        # of ALL cross-files it receives.  The main vcpkg cross-file lacks
        # CMAKE_TRY_COMPILE_TARGET_TYPE, so cmake tries to LINK a test ARM exe on
        # the x64 host -> LNK4272/LNK1120 (x64 kernel32.lib/MSVCRT.lib).
        # Fix: write a supplementary mini cross-file and append it to arg_OPTIONS so
        # meson merges its [cmake] section.  cmake then uses STATIC_LIBRARY target
        # type (compile-only, no link step) for all compiler tests.
        # Intentionally unconditional: STATIC_LIBRARY is correct for any cross-build
        # and harmless for native builds.  A conditional on VCPKG_TARGET_ARCHITECTURE
        # was previously tried but silently skipped (variable undefined in this scope).
        # ARM32: advapi32 winlibs cross-file post-processing
        message(STATUS "ARM-cmake-fix: VCPKG_TARGET_ARCHITECTURE=${VCPKG_TARGET_ARCHITECTURE} TARGET_TRIPLET=${TARGET_TRIPLET}")
        set(_z_arm32_xf "${CURRENT_BUILDTREES_DIR}/${TARGET_TRIPLET}-cmake-fix.ini")
        file(WRITE "${_z_arm32_xf}" "[cmake]\nCMAKE_TRY_COMPILE_TARGET_TYPE = 'STATIC_LIBRARY'\n")
        vcpkg_list(APPEND arg_OPTIONS "--cross-file" "${_z_arm32_xf}")
        message(STATUS "ARM-cmake-fix: Appended supplementary meson cross-file (CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY): ${_z_arm32_xf}")
'@
        # Find configure_file() call for meson.template.in and insert our block after it.
        # Use IndexOf to find the call, then find its closing ')' and insert after the line.
        $cfSearch = 'configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/meson.template.in"'
        $cfIdx = $vcm.IndexOf($cfSearch)
        if ($cfIdx -ge 0) {
            # Find closing ')' of configure_file(...) — no nested parens in this call
            $closeIdx = $vcm.IndexOf(')', $cfIdx + $cfSearch.Length)
            if ($closeIdx -ge 0) {
                $insertAt = $closeIdx + 1
                # Skip past the line ending (CR+LF or just LF) after the closing paren
                if ($insertAt -lt $vcm.Length -and $vcm[$insertAt] -eq [char]13) { $insertAt++ }
                if ($insertAt -lt $vcm.Length -and $vcm[$insertAt] -eq [char]10) { $insertAt++ }
                $vcm = $vcm.Substring(0, $insertAt) + "`r`n" + $vcmInject + $vcm.Substring($insertAt)
                if ($vcm -match [regex]::Escape($vcmCmakeMarker)) {
                    [System.IO.File]::WriteAllText($vcmFile, $vcm, [System.Text.UTF8Encoding]::new($false))
                    Write-Host "==> Patched vcpkg_configure_meson.cmake: ARM cross-file CMAKE_TRY_COMPILE_TARGET_TYPE injection"
                } else {
                    Write-Warning "vcpkg_configure_meson.cmake: marker not found after splice -- patch NOT applied"
                }
            } else {
                Write-Warning "vcpkg_configure_meson.cmake: configure_file closing ')' not found -- patch NOT applied"
            }
        } else {
            Write-Warning "vcpkg_configure_meson.cmake: configure_file() call not found -- patch NOT applied"
        }
    } else {
        Write-Host "==> vcpkg_configure_meson.cmake already has ARM32 cross-file patch."
    }
}

# ── Patch harfbuzz portfile: set LIB for meson cmake sub-invocations ─────────
# When meson runs cmake for dependency detection (freetype2, glib), cmake tests
# the C compiler by compiling AND linking a test exe.  vcpkg strips LIB from
# the cmake subprocess env; without ARM lib paths link.exe falls back to x64
# kernel32.lib/MSVCRT.lib → LNK4272/LNK1120.
# Fix: restore LIB to the ARM junction lib paths in the portfile, before
# vcpkg_configure_meson.  Same pattern as CPPFLAGS in the ICU portfile.
#
# Belt-and-suspenders with the meson.template.in patch above: if cmake uses
# STATIC_LIBRARY target type it won't link at all; if it does link, LIB is set.
$harfbuzzPortfile = "$vcpkgDir\ports\harfbuzz\portfile.cmake"
if (Test-Path $harfbuzzPortfile) {
    Push-Location $vcpkgDir
    & $git checkout -- "ports/harfbuzz/portfile.cmake" 2>&1 | Out-Null
    Pop-Location

    $hp = Get-Content $harfbuzzPortfile -Raw
    $hbMarker = '# ARM32: set LIB env var for meson cmake sub-invocations'
    if ($hp -notmatch [regex]::Escape($hbMarker)) {
        # Compute ARM lib paths from $env:LIB (already rewritten to junctions above)
        $arm32LibEnv = ($env:LIB -split ';' | Where-Object { $_ -ne '' } |
            ForEach-Object { $_ -replace '\\', '/' }) -join ';'

        $hbInject = "if(VCPKG_CROSSCOMPILING AND VCPKG_TARGET_ARCHITECTURE STREQUAL `"arm`") $hbMarker`n" +
            "    # vcpkg strips LIB; cmake's compiler test links against x64 CRT/SDK`n" +
            "    # without it.  Restore ARM junction lib paths so link.exe finds the`n" +
            "    # ARM kernel32.lib / MSVCRT.lib and the compiler test passes.`n" +
            "    set(ENV{LIB} `"$arm32LibEnv`")`n" +
            "    # ARM32: CMAKE_TRY_COMPILE_TARGET_TYPE supplementary cross-file`n" +
            "    # VCPKG_MESON_CONFIGURE_OPTIONS is read by vcpkg_configure_meson() to`n" +
            "    # append extra --cross-file args to meson.  The extra cross-file adds`n" +
            "    # CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY to the [cmake] section`n" +
            "    # so meson's cmake compiler test compiles only (no link -> no LNK4272).`n" +
            "    # The ini file is written by build-webkit-arm32.ps1 before vcpkg runs.`n" +
            "    set(VCPKG_MESON_CONFIGURE_OPTIONS --cross-file C:/vcpkg-webkit/arm32-cmake-fix.ini)`n" +
            "    message(STATUS `"ARM32: Set VCPKG_MESON_CONFIGURE_OPTIONS for cmake compiler test fix`")`n" +
            "    # ARM32: generate freetype2.pc if vcpkg freetype did not produce one.`n" +
            "    # vcpkg's cmake-based freetype port does not emit a .pc file on Windows.`n" +
            "    # Meson's cmake detection also fails (CMake architectures: [] on Windows`n" +
            "    # ARM cross-build because CMAKE_OSX_ARCHITECTURES is Apple-only).`n" +
            "    # Generating the .pc manually makes pkg-config detection succeed first,`n" +
            "    # which is enough -- meson only needs one method to succeed.`n" +
            "    set(_ft_pfx `"C:/vcpkg-webkit/installed/arm-windows-webkit`")`n" +
            "    set(_ft_pc `"`${_ft_pfx}/lib/pkgconfig/freetype2.pc`")`n" +
            "    file(WRITE `"`${_ft_pc}`"`n" +
            "        `"Name: FreeType 2\n`"`n" +
            "        `"Description: A free, high-quality, and portable font engine.\n`"`n" +
            "        `"Version: 26.1.20\n`"`n" +
            "        `"Libs: -L`${_ft_pfx}/lib -lfreetype\n`"`n" +
            "        `"Cflags: -I`${_ft_pfx}/include/freetype2 -I`${_ft_pfx}/include\n`"`n" +
            "    )`n" +
            "    message(STATUS `"ARM32: Wrote freetype2.pc (libtool ver 26.1.20)`")`n" +
            "    # ARM32 diagnostic: dump installed pkg-config .pc filenames to Z:`n" +
            "    file(GLOB _arm32_pc `"C:/vcpkg-webkit/installed/arm-windows-webkit/lib/pkgconfig/*.pc`")`n" +
            "    string(REPLACE `";`" `"\n`" _arm32_pc_str `"`${_arm32_pc}`")`n" +
            "    file(WRITE `"Z:/prism-browser/deps/build-logs/arm32-pkgconfig-files.txt`" `"`${_arm32_pc_str}\n`")`n" +
            "    # ARM32 diagnostic: find freetype headers`n" +
            "    file(GLOB_RECURSE _arm32_ft `"C:/vcpkg-webkit/installed/arm-windows-webkit/include/*ft2build*`")`n" +
            "    string(REPLACE `";`" `"\n`" _arm32_ft_str `"`${_arm32_ft}`")`n" +
            "    file(GLOB _arm32_inc `"C:/vcpkg-webkit/installed/arm-windows-webkit/include/*`")`n" +
            "    string(REPLACE `";`" `"\n`" _arm32_inc_str `"`${_arm32_inc}`")`n" +
            "    file(GLOB _arm32_lib `"C:/vcpkg-webkit/installed/arm-windows-webkit/lib/freetype*`")`n" +
            "    string(REPLACE `";`" `"\n`" _arm32_lib_str `"`${_arm32_lib}`")`n" +
            "    file(GLOB _arm32_pkg `"C:/vcpkg-webkit/packages/freetype_arm-windows-webkit/include/*`")`n" +
            "    string(REPLACE `";`" `"\n`" _arm32_pkg_str `"`${_arm32_pkg}`")`n" +
            "    file(GLOB _arm32_pkgft `"C:/vcpkg-webkit/packages/freetype_arm-windows-webkit/include/freetype2/*`")`n" +
            "    list(LENGTH _arm32_pkgft _arm32_pkgft_len)`n" +
            "    string(REPLACE `";`" `"\n`" _arm32_pkgft_str `"`${_arm32_pkgft}`")`n" +
            "    file(WRITE `"Z:/prism-browser/deps/build-logs/arm32-freetype-headers.txt`"`n" +
            "        `"installed/include/* :\n`${_arm32_inc_str}\n\ninstalled/lib/freetype* :\n`${_arm32_lib_str}\n\nft2build search:\n`${_arm32_ft_str}\n\npkgs/include/* :\n`${_arm32_pkg_str}\n\npkgs/include/freetype2/* count=`${_arm32_pkgft_len}:\n`${_arm32_pkgft_str}\n`")`n" +
            "endif()`n"

        $insertIdx = $hp.IndexOf('vcpkg_configure_meson(')
        if ($insertIdx -ge 0) {
            $hp = $hp.Substring(0, $insertIdx) + $hbInject + $hp.Substring($insertIdx)
            [System.IO.File]::WriteAllText($harfbuzzPortfile, $hp, [System.Text.UTF8Encoding]::new($false))
            Write-Host "==> Patched harfbuzz portfile: ARM32 LIB=$arm32LibEnv"
        } else {
            Write-Warning "vcpkg_configure_meson( not found in harfbuzz portfile - LIB not patched"
        }
    } else {
        Write-Host "==> Harfbuzz portfile already patched (LIB env var)."
    }
    # Clean harfbuzz buildtree so it reconfigures with the patched portfile.
    foreach ($sub in @("$vcpkgDir\buildtrees\harfbuzz", "$vcpkgDir\packages\harfbuzz_$t")) {
        if (Test-Path $sub) { Remove-Item $sub -Recurse -Force }
    }
}

# ── Patch cairo portfile: add ole32.lib to meson winlibs ─────────────────────
# Cairo's DWrite font backend uses COM (CoCreateInstance, CoInitialize) which
# lives in ole32.lib.  Cairo's meson.build omits ole32 from the dwrite link
# args, so csi-exec.exe and csi-replay.exe (cairo-script util tools) fail to
# link.  Adding ole32.lib to c_winlibs/cpp_winlibs makes it visible to all
# cairo targets via meson's built-in Windows library mechanism.
$cairoPortfile = "$vcpkgDir\ports\cairo\portfile.cmake"
if (Test-Path $cairoPortfile) {
    $cp = Get-Content $cairoPortfile -Raw
    $cairoMarker = '# ARM32: ole32.lib for COM (DWrite/WIC)'
    if ($cp -notmatch [regex]::Escape($cairoMarker)) {
        # Inject before vcpkg_configure_meson
        $injection = "$cairoMarker`r`nlist(APPEND OPTIONS`r`n    -Dc_winlibs=['kernel32.lib','user32.lib','ole32.lib']`r`n    -Dcpp_winlibs=['kernel32.lib','user32.lib','ole32.lib']`r`n)`r`n"
        $cp = $cp -replace '(vcpkg_configure_meson\()', "$injection`$1"
        Set-Content $cairoPortfile $cp -Encoding UTF8
        Write-Host "==> Patched cairo portfile: ole32.lib added to winlibs"
    } else {
        Write-Host "==> Cairo portfile already patched (ole32)."
    }
}

# ── Patch ICU portfile: bypass autoconf compiler-works test (ARM32 cross) ────
# ICU 78.2 uses autoconf 2.72.  In autoconf 2.72 the old trick of passing
# "VAR=VALUE" positional args to ./configure does NOT set shell-level cache
# variables — they become make-level overrides only.  So we cannot bypass
# autoconf tests that way with this version of ICU.
#
# Fix: use autoconf's --cache-file=FILE mechanism.  When --cache-file is given
# autoconf sources the file as a shell script before running any tests, so
# variables set in it ARE used by AC_CACHE_VAL.  The file path must be
# MSYS2-style (/c/...) and must not contain spaces.
#
# Two-pronged strategy:
#   1. Directory junctions above already rewrote $env:INCLUDE/$env:LIB to
#      spaceless paths, so $arm32IFlags/$arm32LibFlags are spaceless.  This
#      fixes CFLAGS/LDFLAGS word-splitting in all autoconf test commands.
#   2. We write an autoconf cache shell file with ac_cv_* pre-set, then
#      inject --cache-file=<MSYS2-path> into ICU's CONFIGURE_OPTIONS.
#      The portfile injects it into the vcpkg_make_configure call.
#
# Note: vcpkg auto-installs icu:x64-windows[tools] as a host dependency first
# (declared in icu/vcpkg.json).  That build copies icucross.mk to the tools
# dir, so --with-cross-build will resolve correctly for the ARM32 configure.

# Write the autoconf cache file (plain shell assignments, sourced by configure).
# Path must be inside $vcpkgDir (already on C:\) so the MSYS2 /c/... form is
# short and has no spaces.
$icuCacheFile     = "$vcpkgDir\arm32-icu-cache.sh"
$icuCacheFileFwd  = '/c/vcpkg-webkit/arm32-icu-cache.sh'   # MSYS2 absolute path

# Compute CPPFLAGS from the junction-backed INCLUDE directories.
# These are set at the top of this script; junctions are stable (hash-based names).
# Using forward-slash paths so cl.exe -E accepts them inside the MSYS2 shell.
$arm32CppFlags = ($env:INCLUDE -split ';' | Where-Object { $_ -ne '' } |
    ForEach-Object { "-I$($_ -replace '\\', '/')" }) -join ' '
Write-Host "==> ARM32 CPPFLAGS for cache file: $arm32CppFlags"

# The cache file is a shell script sourced by autoconf's ./configure BEFORE any
# AC_* test runs.  Setting CPPFLAGS here is the only reliable way to get -I flags
# into the preprocessor sanity check: vcpkg_make_configure (whichever cmake file
# defines it) overwrites ENV{CPPFLAGS} to empty before calling configure, and
# the sanity check is not inside AC_CACHE_VAL so cache variable overrides can't
# bypass it.  The cache sourcing runs first → our CPPFLAGS wins.
$icuCacheContent = @"
# autoconf cache pre-set for ICU ARM32 MSVC cross-compilation
# Sourced by ./configure via --cache-file=.
# Each variable matches an autoconf cache slot so the corresponding test is skipped.

# AC_PROG_CC / AC_PROG_CXX: compiler works
ac_cv_prog_cc_works=yes
ac_cv_prog_cxx_works=yes
ac_cv_prog_cc_cross=yes

# _AC_COMPILER_EXEEXT_CROSS: sets cross_compiling, probes exe extension.
ac_cv_c_cross=yes
ac_cv_exeext=.exe
ac_cv_objext=obj

# GNU compiler detection -- cl.exe is not GCC
ac_cv_c_compiler_gnu=no
ac_cv_cxx_compiler_gnu=no

# No Unix math library on Windows; everything is in the CRT
ac_cv_lib_m_floor=no

# C preprocessor: cache the detected CPP program.
ac_cv_prog_CPP='compile cl.exe -E'

# CPPFLAGS: junction-backed MSVC include dirs so cl.exe -E finds limits.h.
# AC_PROG_CPP runs _AC_PROG_PREPROC_WORKS_IFELSE unconditionally (not cached).
# Setting CPPFLAGS here (in the cache shell script) ensures it is available
# when configure expands: compile cl.exe -E `$CPPFLAGS conftest.c
CPPFLAGS="$arm32CppFlags"
export CPPFLAGS

# CRT linkage: /MD (dynamic) to match VCPKG_CRT_LINKAGE=dynamic.
# vcpkg_configure_make.cmake does NOT set CRT flags for autoconf builds;
# without this, cl.exe defaults to /MT (static CRT) which causes LNK2038
# mismatch when linking against WebKit DLLs compiled with /MD.
CFLAGS="-MD -O2"
CXXFLAGS="-MD -O2"
export CFLAGS CXXFLAGS
"@
# Write with LF line endings, no BOM (it's a shell script).
[System.IO.File]::WriteAllText($icuCacheFile, ($icuCacheContent -replace '\r', ''), [System.Text.UTF8Encoding]::new($false))
Write-Host "==> Wrote ICU autoconf cache file: $icuCacheFile"

$icuPortfile = "$vcpkgDir\ports\icu\portfile.cmake"
if (Test-Path $icuPortfile) {
    # Always restore the portfile from git before patching so we never accumulate
    # stale patches across re-runs of the build script.
    Push-Location $vcpkgDir
    & $git checkout -- "ports/icu/portfile.cmake" 2>&1 | Out-Null
    Pop-Location
    $ip = Get-Content $icuPortfile -Raw
    $icuMarker = '# ARM32: --cache-file bypass for cross-compile'
    if ($ip -notmatch [regex]::Escape($icuMarker)) {
        # Find the existing Windows cache-var block so we can inject after it.
        # The portfile has:  list(APPEND CONFIGURE_OPTIONS ac_cv_lib_m_floor=no)
        # We need to:
        #   a) remove that line (our cache file already sets ac_cv_lib_m_floor=no)
        #   b) append --cache-file=... when VCPKG_CROSSCOMPILING is true
        $m = [regex]::Match($ip, '([ \t]*)list\(APPEND CONFIGURE_OPTIONS ac_cv_lib_m_floor=no\)')
        if (-not $m.Success) {
            Write-Warning "ICU portfile: could not find ac_cv_lib_m_floor=no line - trying fallback injection."
            # Fallback: inject before vcpkg_make_configure
            $icuInject = "# ARM32: --cache-file bypass for cross-compile`n" +
                "if(VCPKG_CROSSCOMPILING)`n" +
                "    list(APPEND CONFIGURE_OPTIONS `"--cache-file=$icuCacheFileFwd`")`n" +
                "endif()`n"
            $ip = $ip -replace '(vcpkg_make_configure\()', "$icuInject`$1"
            Set-Content $icuPortfile $ip -Encoding UTF8
            Write-Host "==> Patched ICU portfile (fallback): --cache-file for ARM32 cross-compile"
        } else {
            $ind     = $m.Groups[1].Value   # leading whitespace of the matched line
            $oldSnip = $m.Value             # the ac_cv_lib_m_floor=no list() line
            # Replace the Windows-only list() call with a cross-compile block that:
            #   - always appends ac_cv_lib_m_floor=no (keeps native Windows builds working)
            #   - additionally injects --cache-file when cross-compiling (ARM32)
            $newSnip = $oldSnip + "`n" +
                $ind + "    if(VCPKG_CROSSCOMPILING) # ARM32: --cache-file bypass for cross-compile`n" +
                $ind + "        # autoconf 2.72 ignores positional VAR=VALUE cache overrides.`n" +
                $ind + "        # --cache-file is sourced as a shell script before any test runs,`n" +
                $ind + "        # so ac_cv_* assignments in it ARE honoured by AC_CACHE_VAL.`n" +
                $ind + "        # The cache file is written by build-webkit-arm32.ps1 at $icuCacheFileFwd`n" +
                $ind + "        list(APPEND CONFIGURE_OPTIONS `"--cache-file=$icuCacheFileFwd`")`n" +
                $ind + "    endif()"
            $ip = $ip.Replace($oldSnip, $newSnip)
            Set-Content $icuPortfile $ip -Encoding UTF8
            Write-Host "==> Patched ICU portfile: --cache-file=$icuCacheFileFwd for ARM32 cross-compile"
        }
        # Clean ARM32 ICU state so the fresh portfile is used.
        # Do NOT clean the x64-windows ICU package (needed for --with-cross-build).
        foreach ($sub in @("$vcpkgDir\buildtrees\icu",
                           "$vcpkgDir\packages\icu_$t")) {
            if (Test-Path $sub) { Remove-Item $sub -Recurse -Force }
        }
    } else {
        Write-Host "==> ICU portfile already patched."
        # Still clean the ARM32 ICU buildtree: the autoconf cache file is always
        # re-written above, so configure must re-run to pick up any new ac_cv_* entries.
        foreach ($sub in @("$vcpkgDir\buildtrees\icu",
                           "$vcpkgDir\packages\icu_$t")) {
            if (Test-Path $sub) { Remove-Item $sub -Recurse -Force }
        }
    }

    # ── Second portfile patch: hardcode junction -I paths into CPPFLAGS ─────────
    # autoconf's AC_PROG_CPP runs _AC_PROG_PREPROC_WORKS_IFELSE unconditionally
    # using "$CPP $CPPFLAGS conftest.c".  vcpkg_make_configure leaves CPPFLAGS
    # empty; cl.exe -E can't find limits.h.  Fix: set CPPFLAGS to the junction
    # -I paths in the portfile.  We patch vcpkg_configure_make.cmake separately
    # so it preserves non-empty CPPFLAGS instead of overwriting with "".
    #
    # Junction paths are stable (hash of path) so hardcoding is safe.
    # We ALWAYS rewrite this injection so paths stay current.
    $arm32CppFlags = ($env:INCLUDE -split ';' | Where-Object { $_ -ne '' } |
        ForEach-Object { "-I$($_ -replace '\\', '/')" }) -join ' '
    Write-Host "==> ARM32 CPPFLAGS: $arm32CppFlags"

    $ip2 = Get-Content $icuPortfile -Raw
    # Remove any previous CPPFLAGS injection block (handles both old and new marker).
    foreach ($oldMark in @('# ARM32: CPPFLAGS mirrors CFLAGS for preprocessor sanity check',
                           '# ARM32: CPPFLAGS hardcoded junction paths')) {
        $escaped = [regex]::Escape($oldMark)
        $ip2 = [regex]::Replace($ip2,
            "if\(VCPKG_CROSSCOMPILING\)[^\r\n]*$escaped[\s\S]*?endif\(\)\r?\n", '',
            [System.Text.RegularExpressions.RegexOptions]::None)
    }
    # Build new injection and insert before vcpkg_make_configure.
    $cppFlagsMarker = '# ARM32: CPPFLAGS hardcoded junction paths'
    $cppFlagsBlock  = "if(VCPKG_CROSSCOMPILING) $cppFlagsMarker`r`n" +
        "    # AC_PROG_CPP sanity check uses `$CPP `$CPPFLAGS; vcpkg_make_configure`r`n" +
        "    # leaves CPPFLAGS empty.  Hardcode junction -I paths so cl.exe -E`r`n" +
        "    # finds limits.h.  vcpkg_configure_make.cmake is patched to preserve`r`n" +
        "    # non-empty CPPFLAGS rather than overwriting with empty.`r`n" +
        "    set(ENV{CPPFLAGS} `"$arm32CppFlags`")`r`n" +
        "    # ARM32: force /MD (dynamic CRT) for CFLAGS/CXXFLAGS so ICU static libs`r`n" +
        "    # match WebKit's MD linkage.  Without this ICU defaults to /MT and causes`r`n" +
        "    # LNK2038 (RuntimeLibrary mismatch) when linking WTF.dll.`r`n" +
        "    set(ENV{CFLAGS} `"/MD `$ENV{CFLAGS}`")`r`n" +
        "    set(ENV{CXXFLAGS} `"/MD `$ENV{CXXFLAGS}`")`r`n" +
        "endif()`r`n"
    $insertIdx = $ip2.IndexOf('vcpkg_make_configure(')
    if ($insertIdx -ge 0) {
        $ip2 = $ip2.Substring(0, $insertIdx) + $cppFlagsBlock + $ip2.Substring($insertIdx)
        [System.IO.File]::WriteAllText($icuPortfile, $ip2, [System.Text.UTF8Encoding]::new($false))
        Write-Host "==> ICU portfile: CPPFLAGS set to junction paths for preprocessor check"
    } else {
        Write-Warning "vcpkg_make_configure( not found in portfile - CPPFLAGS not patched"
    }

    # ── Patch vcpkg_configure_make.cmake: preserve non-empty CPPFLAGS ───────────
    # Line 819: set(ENV{CPPFLAGS} "${CPPFLAGS_${current_buildtype}}")
    # This overwrites whatever the portfile set.  Change it to only write CPPFLAGS
    # if it is currently empty (portfile value wins when non-empty).
    $vcmFile   = "$vcpkgDir\scripts\cmake\vcpkg_configure_make.cmake"
    $vcmMarker = '# ARM32: preserve portfile CPPFLAGS'
    if ((Test-Path $vcmFile)) {
        $vcm = [System.IO.File]::ReadAllText($vcmFile)
        if ($vcm -notmatch [regex]::Escape($vcmMarker)) {
            $oldLine = 'set(ENV{CPPFLAGS} "${CPPFLAGS_${current_buildtype}}")'
            $newBlock = "if(`"`$ENV{CPPFLAGS}`" STREQUAL `"`") $vcmMarker`n" +
                        "            set(ENV{CPPFLAGS} `"`${CPPFLAGS_`${current_buildtype}}`")`n" +
                        "        else()`n" +
                        "            set(ENV{CPPFLAGS} `"`$ENV{CPPFLAGS} `${CPPFLAGS_`${current_buildtype}}`")`n" +
                        "        endif()"
            $vcm = $vcm.Replace($oldLine, $newBlock)
            [System.IO.File]::WriteAllText($vcmFile, $vcm, [System.Text.UTF8Encoding]::new($false))
            Write-Host "==> Patched vcpkg_configure_make.cmake: preserve portfile CPPFLAGS"
        } else {
            Write-Host "==> vcpkg_configure_make.cmake CPPFLAGS patch already present."
        }
    } else {
        Write-Warning "vcpkg_configure_make.cmake not found - CPPFLAGS may still be wiped"
    }
} else {
    Write-Warning "ICU portfile not found at $icuPortfile - ICU cross-compile may fail."
}

$packages = @(
    # Explicitly select only [core] on any package that ships CLI tools as a
    # default feature.  Those tools are ARM32 executables that require a full
    # CRT link step -- we only need the static libraries for WebKit, so there
    # is no reason to build them and no guarantee the linker flags are set
    # correctly in vcpkg's subprocess environment.
    "zlib:$t",
    "bzip2[core]:$t",
    "liblzma[core]:$t",
    "libpng:$t",
    "libjpeg-turbo[core]:$t",
    "tiff[core]:$t",
    "libwebp[core]:$t",
    "sqlite3[core]:$t",
    "openssl[core]:$t",
    "curl[core,ssl,http2]:$t",
    "libxml2[core]:$t",
    "libxslt[core]:$t",
    # ICU: re-added now that ARM32 LIB paths are baked into linker flags.
    # The vcpkg ICU port on Windows uses nmake (not autoconf/bash), so the earlier
    # autoconf cross-compile issue does not apply.  ICU is required for proper
    # Unicode/i18n support and for libpsl (Public Suffix List / cookie security).
    "icu[core]:$t",
    "freetype[core,bzip2,png,zlib]:$t",
    "brotli:$t",
    "woff2:$t",
    "pixman:$t",
    "cairo[core,freetype]:$t",
    "harfbuzz[core,freetype,icu]:$t",  # re-add icu dep now that ICU is back
    "libpsl[libicu]:$t"                # Windows default: ICU backend
)

Write-Host ""
Write-Host "==> Installing WebKit dependencies via vcpkg (triplet: $t)..."
Write-Host "    --allow-unsupported overrides arm32 deprecation guards."
Write-Host "    First run takes 30-60 minutes; subsequent runs use the cache."
Write-Host ""

# Patch vcpkg_configure_meson.cmake to always include advapi32.lib in c_winlibs.
# cmake's ARM32 cross-compile platform init only produces "kernel32.lib user32.lib"
# for CMAKE_C_STANDARD_LIBRARIES; our CACHE FORCE in the chainload is overridden by
# cmake's own platform files which run after the toolchain.  The only reliable fix is
# to inject advapi32 directly into the meson cross-file generation code, right after
# vcpkg splits VCPKG_DETECTED_CMAKE_C_STANDARD_LIBRARIES into a list.
# This affects ALL meson-built packages (harmless -- advapi32 is always available).
$vcpkgMesonCmake = "$vcpkgDir\scripts\cmake\vcpkg_configure_meson.cmake"
if (Test-Path $vcpkgMesonCmake) {
    $mc = [System.IO.File]::ReadAllText($vcpkgMesonCmake)
    if ($mc -notmatch "advapi32.*winlibs patch") {
        $inject = @'

    # ARM32 cross-build advapi32 winlibs patch: cmake platform init only gives
    # kernel32+user32 for ARM cross-compile; advapi32 is needed by ICU wintz.c.
    if(NOT "advapi32.lib" IN_LIST c_winlibs)
        list(APPEND c_winlibs "advapi32.lib")
    endif()
    if(NOT "advapi32.lib" IN_LIST cpp_winlibs)
        list(APPEND cpp_winlibs "advapi32.lib")
    endif()
'@
        # Insert after the separate_arguments(cpp_winlibs ...) line
        $mc = $mc -replace '(?m)(separate_arguments\(cpp_winlibs[^\r\n]*\))', "`$1$inject"
        [System.IO.File]::WriteAllText($vcpkgMesonCmake, $mc, [System.Text.UTF8Encoding]::new($false))
        Write-Host "==> Patched vcpkg_configure_meson.cmake: advapi32.lib added to c_winlibs/cpp_winlibs"
    } else {
        Write-Host "==> vcpkg_configure_meson.cmake already patched for advapi32"
    }
} else {
    Write-Host "==> WARNING: $vcpkgMesonCmake not found -- advapi32 winlibs patch skipped"
}

# Disable vcpkg binary cache for all installs.
# The arm-windows-webkit binary cache entries are corrupt (ABI hash matches but
# package files are empty/missing).  Setting VCPKG_BINARY_SOURCES=clear forces
# every package to be built from source, guaranteeing the installed tree is
# actually populated.  Future runs also build from source (safe -- this VM
# rebuilds from a known-good state each time).
$env:VCPKG_BINARY_SOURCES = "clear"
Write-Host "==> Binary cache disabled (VCPKG_BINARY_SOURCES=clear) to avoid corrupt cache."

# ── Check ICU CRT linkage ─────────────────────────────────────────────────────
# ICU is built via autoconf; vcpkg_configure_make.cmake does NOT set /MD.
# Without explicit CFLAGS=-MD in the cache file, cl.exe defaults to /MT, which
# causes LNK2038 when linking WTF.dll (compiled with /MD).
# Detection: search for the string "MT_StaticRelease" in icuuc.lib.
# If found, wipe ONLY the ICU installed files (not all deps) so only ICU rebuilds.
# NOTE: Do NOT delete depSentinels files here — that would trigger a full dep wipe.
$icuLibCheck = "$vcpkgDir\installed\$t\lib\icuuc.lib"
if (Test-Path $icuLibCheck) {
    Write-Host "==> Checking ICU CRT linkage in $icuLibCheck ..."
    try {
        $icuBytes = [System.IO.File]::ReadAllBytes($icuLibCheck)
        $icuAscii = [System.Text.Encoding]::ASCII.GetString($icuBytes)
        $hasMT = $icuAscii -match 'MT_StaticRelease'
        $hasMD = $icuAscii -match 'MD_DynamicRelease'
        Write-Host "==> ICU CRT linkage: MT=$hasMT MD=$hasMD"
        if ($hasMT -and -not $hasMD) {
            Write-Warning "==> ICU was built with /MT but WebKit needs /MD — wiping ICU files only."
            # Wipe ICU-specific installed files (headers + libs) without touching other deps.
            # This makes vcpkg see ICU as uninstalled and rebuild it, but leaves other
            # deps intact so the depSentinels check still passes for non-ICU packages.
            Remove-Item "$vcpkgDir\installed\$t\include\unicode" -Recurse -Force -ErrorAction SilentlyContinue
            Remove-Item "$vcpkgDir\installed\$t\lib\icuuc.lib" -Force -ErrorAction SilentlyContinue
            Remove-Item "$vcpkgDir\installed\$t\lib\icudt.lib" -Force -ErrorAction SilentlyContinue
            Remove-Item "$vcpkgDir\installed\$t\lib\icuin.lib" -Force -ErrorAction SilentlyContinue
            Remove-Item "$vcpkgDir\installed\$t\bin\icuuc*.dll" -Force -ErrorAction SilentlyContinue
            Remove-Item "$vcpkgDir\installed\$t\bin\icudt*.dll" -Force -ErrorAction SilentlyContinue
            Remove-Item "$vcpkgDir\installed\$t\bin\icuin*.dll" -Force -ErrorAction SilentlyContinue
            # Remove ICU list files from vcpkg info so vcpkg treats it as uninstalled.
            Get-ChildItem "$vcpkgDir\installed\vcpkg\info" -Filter "icu*$t.list" -ErrorAction SilentlyContinue |
                ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
            # Clear ICU status entries from vcpkg's status DB so install works correctly.
            $vcpkgStatusFile = "$vcpkgDir\installed\vcpkg\status"
            if (Test-Path $vcpkgStatusFile) {
                $statusContent = [System.IO.File]::ReadAllText($vcpkgStatusFile)
                # Split into blocks (separated by blank lines), remove ICU blocks.
                $statusBlocks = $statusContent -split "(?m)^$\r?\n" | Where-Object {
                    $_ -notmatch "^Package:\s*icu\b" -and $_ -notmatch "^Package:\s*icu:"
                }
                [System.IO.File]::WriteAllText($vcpkgStatusFile, ($statusBlocks -join "`r`n`r`n"), [System.Text.UTF8Encoding]::new($false))
            }
            # Replace the ICU uchar.h sentinel with a dummy so depSentinels still passes.
            # vcpkg will detect the missing lib/headers and rebuild ICU when it runs install.
            New-Item "$vcpkgDir\installed\$t\include\unicode" -ItemType Directory -Force | Out-Null
            [System.IO.File]::WriteAllText("$vcpkgDir\installed\$t\include\unicode\uchar.h", "/* placeholder - ICU being rebuilt with /MD */`n")
            Write-Host "==> Wiped ICU libs+info. depSentinels placeholder written. ICU will be rebuilt with /MD."
        }
    } catch {
        Write-Warning "==> Could not check ICU CRT linkage: $_"
    }
}

# Skip the entire wipe+rebuild if all deps are already installed.
# Check a handful of sentinel headers spanning the full dep list.
$depSentinels = @(
    "$vcpkgDir\installed\$t\include\zlib.h",
    "$vcpkgDir\installed\$t\include\unicode\uchar.h",      # icu
    "$vcpkgDir\installed\$t\include\harfbuzz\hb.h",
    "$vcpkgDir\installed\$t\include\cairo\cairo.h",
    "$vcpkgDir\installed\$t\include\libpsl.h"              # last dep
)
$depsOk = ($depSentinels | Where-Object { -not (Test-Path $_) }).Count -eq 0
if ($depsOk) {
    Write-Host "==> All deps already installed -- skipping wipe+rebuild."
    Write-Host "    To force a full rebuild: Remove-Item $vcpkgDir\installed\$t -Recurse -Force"
} else {
    Write-Host "==> Some deps missing — running wipe+rebuild..."
}
if (-not $depsOk) {

# Nuke the arm-windows-webkit installed tree and vcpkg status entries.
# vcpkg binary cache entries are corrupt (ABI hash matches but package files are
# missing).  vcpkg considers them installed so it skips rebuilding them.
# We can't reliably use "vcpkg remove" because list output has feature specifiers
# that confuse the parsing.  Instead, delete the files directly and clear the
# status DB entries so vcpkg treats everything as uninstalled.
Write-Host "==> Wiping $t installed tree and status entries for clean rebuild..."
# 1. Delete the entire arm-windows-webkit installed dir (headers, libs, share)
$armInstDir = "$vcpkgDir\installed\$t"
if (Test-Path $armInstDir) {
    Remove-Item $armInstDir -Recurse -Force -ErrorAction SilentlyContinue
    Write-Host "==>   Deleted $armInstDir"
}
# 2. Delete per-package .list files for this triplet
Get-ChildItem "$vcpkgDir\installed\vcpkg\info" -Filter "*_$t.list" -ErrorAction SilentlyContinue |
    ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
# 3. Rewrite the vcpkg status file, stripping all blocks for this triplet.
#    vcpkg status file uses blank-line-separated stanzas.
#    The "Architecture:" field holds the FULL TRIPLET name (e.g. "arm-windows-webkit"),
#    NOT a CPU arch string.  There is no separate "Triplet:" field in this vcpkg version.
$statusFile = "$vcpkgDir\installed\vcpkg\status"
if (Test-Path $statusFile) {
    $raw = [System.IO.File]::ReadAllText($statusFile)
    # Split on double newline (stanza separator), filter out arm-windows-webkit stanzas.
    # Match on "Architecture: arm-windows-webkit" (the full triplet, vcpkg's own field name).
    $stanzas = $raw -split "(\r?\n){2,}"
    $before  = $stanzas.Count
    $kept    = $stanzas | Where-Object { $_ -notmatch "Architecture:\s*$t" }
    $removed = $before - $kept.Count
    $newStatus = ($kept -join "`r`n`r`n").TrimEnd() + "`r`n"
    [System.IO.File]::WriteAllText($statusFile, $newStatus, [System.Text.UTF8Encoding]::new($false))
    Write-Host "==>   Stripped $removed $t stanzas from vcpkg status (Architecture: field)"
}
# 4. Delete cached cmake-get-vars output files from all buildtrees.
#    vcpkg reuses these files across runs without re-running cmake; if
#    vcpkg_configure_meson.cmake was patched the cache must be cleared so
#    the new code runs and the updated c_winlibs (e.g. advapi32) is written
#    into the meson cross-file for every package.
Get-ChildItem "$vcpkgDir\buildtrees" -Recurse `
    -Include "cmake-get-vars*$t*.cmake.log","cmake-get-vars*$t*.cmake" `
    -ErrorAction SilentlyContinue |
    ForEach-Object { Remove-Item $_.FullName -Force -ErrorAction SilentlyContinue }
Write-Host "==>   Cleared cmake-get-vars caches from all buildtrees (forces advapi32 patch to apply)"
Write-Host "==> Clean wipe done. Rebuilding all $t packages from source."

$logDst = "Z:\prism-browser\deps\build-logs"

foreach ($pkg in $packages) {
    Write-Host "  vcpkg install $pkg"
    & $vcpkg install $pkg $overlayFlag --allow-unsupported --recurse --no-print-usage
    # Quick disk-check: verify expected file is on disk after install.
    # Catches silent merge failures (build succeeded but files missing in installed/).
    $pkgBase = ($pkg -split "[:\[]")[0]
    $diskCheck = switch ($pkgBase) {
        "zlib"          { "$vcpkgDir\installed\$t\include\zlib.h" }
        "bzip2"         { "$vcpkgDir\installed\$t\include\bzlib.h" }
        "liblzma"       { "$vcpkgDir\installed\$t\include\lzma.h" }
        "libpng"        { "$vcpkgDir\installed\$t\include\png.h" }
        "libjpeg-turbo" { "$vcpkgDir\installed\$t\include\jpeglib.h" }
        "tiff"          { "$vcpkgDir\installed\$t\include\tiff.h" }
        "libwebp"       { "$vcpkgDir\installed\$t\include\webp\encode.h" }
        "libxml2"       { "$vcpkgDir\installed\$t\include\libxml2\libxml\tree.h" }
        "icu"           { "$vcpkgDir\installed\$t\include\unicode\uchar.h" }
        "freetype"      { "$vcpkgDir\installed\$t\include\freetype2\ft2build.h" }
        default         { $null }
    }
    if ($diskCheck) {
        if (Test-Path $diskCheck) {
            Write-Host "==> [disk-check PASS] $pkgBase : $diskCheck"
        } else {
            Write-Host "==> [disk-check FAIL] $pkgBase : $diskCheck NOT FOUND -- merge to installed/ failed!"
        }
    }
    # Post-ICU: patch icu-uc.pc to include sicudt in Libs: for static consumers.
    # ICU's pkg-config only lists sicuuc in Libs: but static binaries that link
    # against it (e.g. libpsl's psl tool) also need sicudt (the data library).
    # Without it the linker fails with LNK1120 unresolved externals.
    if ($pkg -like "icu*") {
        foreach ($pcName in @("icu-uc", "icu-i18n")) {
            $pcFile = "$vcpkgDir\installed\$t\lib\pkgconfig\$pcName.pc"
            if (Test-Path $pcFile) {
                $pc = [System.IO.File]::ReadAllText($pcFile)
                if ($pc -notmatch "icudt") {
                    # Determine lib prefix (sicuXX for static, icuXX for import)
                    # and version number from the existing Libs: line.
                    $libsLine = ($pc -split "`r?`n" | Where-Object { $_ -match "^Libs:" })[0]
                    if ($libsLine -match "-l(s?icu\w+?)(\d+)") {
                        $libPrefix  = if ($matches[1] -match "^s") { "sicu" } else { "icu" }
                        $libVersion = $matches[2]
                        $dtLib = "-l${libPrefix}dt${libVersion}"
                        $pc = $pc -replace "(?m)^(Libs:.+)", "`$1 $dtLib"
                        [System.IO.File]::WriteAllText($pcFile, $pc, [System.Text.UTF8Encoding]::new($false))
                        Write-Host "==> ICU patch: added $dtLib to $pcName.pc Libs:"
                    } else {
                        Write-Host "==> ICU patch: could not parse lib name from Libs: line: $libsLine"
                    }
                } else {
                    Write-Host "==> ICU patch: $pcName.pc already references icudt, skipping"
                }
            }
        }
    }
    # Post-freetype diagnostic + corrupt-state fix
    if ($pkg -like "freetype*") {
        $ftLib = Get-Item "$vcpkgDir\installed\$t\lib\freetype*" -ErrorAction SilentlyContinue
        if (-not $ftLib) {
            Write-Host "==> freetype: lib missing -- forcing remove+reinstall (no binary cache)"
            & $vcpkg remove "freetype:$t" --recurse 2>&1 | Out-Null
            Write-Host "==> remove exit code: $LASTEXITCODE. Now verifying removed:"
            $stillListed = (& $vcpkg list 2>&1 | Select-String "^freetype:$t")
            Write-Host "==> still listed: $stillListed"
            # Clean buildtrees and packages staging dir
            foreach ($d in @("$vcpkgDir\buildtrees\freetype", "$vcpkgDir\packages\freetype_$t")) {
                if (Test-Path $d) { Remove-Item $d -Recurse -Force -ErrorAction SilentlyContinue }
            }
            # Bypass binary cache
            $savedBinSrc = $env:VCPKG_BINARY_SOURCES
            $env:VCPKG_BINARY_SOURCES = "clear"
            Write-Host "==> reinstalling freetype from source (VCPKG_BINARY_SOURCES=clear)..."
            & $vcpkg install $pkg $overlayFlag --allow-unsupported --recurse --no-print-usage
            Write-Host "==> freetype reinstall exit code: $LASTEXITCODE"
            $env:VCPKG_BINARY_SOURCES = $savedBinSrc
        }
        # Always copy freetype build logs for inspection
        Get-ChildItem "$vcpkgDir\buildtrees\freetype" -Recurse -Include "*.log" -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Item $_.FullName "$logDst\freetype-$($_.Name)" -Force -ErrorAction SilentlyContinue }
        $ftInc = "$vcpkgDir\installed\$t\include"
        $ftInfo = "=== freetype post-install check ===`n"
        $ftInfo += "installed/include/ : $((Get-ChildItem $ftInc -ErrorAction SilentlyContinue | Select -Exp Name) -join ', ')`n"
        $ftInfo += "installed/lib/freetype* : $((Get-Item "$vcpkgDir\installed\$t\lib\freetype*" -ErrorAction SilentlyContinue | Select -Exp Name) -join ', ')`n"
        $ftInfo += "packages/include/ : $((Get-ChildItem "$vcpkgDir\packages\freetype_$t\include" -ErrorAction SilentlyContinue | Select -Exp Name) -join ', ')`n"
        [System.IO.File]::WriteAllText("$logDst\freetype-install-check.txt", $ftInfo, [System.Text.UTF8Encoding]::new($false))
        Write-Host "==> freetype install check written to $logDst\freetype-install-check.txt"
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "==> Build failed for $pkg - copying logs to $logDst ..."
        New-Item $logDst -ItemType Directory -Force | Out-Null
        # Copy the FAILING package's logs first with explicit prefix so they
        # can't be overwritten by other packages that share the same filename.
        $failPkg = ($pkg -split "[:\[]")[0]   # e.g. "libpsl"
        $failBuildtree = "$vcpkgDir\buildtrees\$failPkg"
        if (Test-Path $failBuildtree) {
            Get-ChildItem $failBuildtree -Recurse -Include "*.log" -ErrorAction SilentlyContinue |
                ForEach-Object { Copy-Item $_.FullName "$logDst\FAIL-$failPkg-$($_.Name)" -Force -ErrorAction SilentlyContinue }
        }
        # Also copy all buildtree logs (prefixed by immediate parent dir name, which
        # is the package name when the log lives directly inside buildtrees/<pkg>/).
        Get-ChildItem "$vcpkgDir\buildtrees" -Recurse -Include "*.log" -ErrorAction SilentlyContinue |
            ForEach-Object {
                # Walk up from the log file to the first dir directly under buildtrees/.
                $d = $_.Directory
                while ($d.Parent -and $d.Parent.Name -ne "buildtrees") { $d = $d.Parent }
                $prefix = $d.Name   # e.g. "libpsl", "pixman"
                Copy-Item $_.FullName "$logDst\$prefix-$($_.Name)" -Force -ErrorAction SilentlyContinue
            }
        # Copy meson-generated cmake toolchain files so we can inspect what meson passes to cmake.
        Get-ChildItem "$vcpkgDir\buildtrees" -Recurse -Include "CMakeMeson*Toolchain*.cmake","CMakeMeson*File.cmake" -ErrorAction SilentlyContinue |
            ForEach-Object { Copy-Item $_.FullName "$logDst\$($_.Directory.Name)_$($_.Name)" -Force -ErrorAction SilentlyContinue }
        # Also copy vcpkg cmake scripts that we may need to inspect / patch.
        foreach ($script in @(
                "$vcpkgDir\scripts\get_cmake_vars\CMakeLists.txt",
                "$vcpkgDir\installed\x64-windows\share\vcpkg-tool-meson\vcpkg_configure_meson.cmake",
                "$vcpkgDir\scripts\cmake\vcpkg_configure_meson.cmake",
                "$vcpkgDir\scripts\cmake\vcpkg_make.cmake",
                "$vcpkgDir\scripts\cmake\vcpkg_make_common.cmake",
                "$vcpkgDir\scripts\cmake\vcpkg_make_configure.cmake",
                "$vcpkgDir\scripts\cmake\vcpkg_configure_make.cmake",
                "$vcpkgDir\ports\icu\portfile.cmake",
                "$vcpkgDir\ports\harfbuzz\portfile.cmake",
                "$vcpkgDir\scripts\buildsystems\meson\meson.template.in",
                "$vcpkgDir\arm32-cmake-fix.ini",
                "$vcpkgDir\installed\x64-windows\share\vcpkg-make\wrappers\compile")) {
            if (Test-Path $script) {
                Copy-Item $script "$logDst\$(Split-Path $script -Leaf)" -Force -ErrorAction SilentlyContinue
            }
        }
        # Copy harfbuzz cross-file with explicit prefix so pixman's copy doesn't overwrite it.
        $hbXf = "$vcpkgDir\buildtrees\harfbuzz\meson-arm-windows-webkit-rel.log"
        if (Test-Path $hbXf) {
            Copy-Item $hbXf "$logDst\harfbuzz-meson-cross-file.log" -Force -ErrorAction SilentlyContinue
        }
        # Copy supplementary cmake-fix cross-file injected by vcpkg_configure_meson.cmake patch.
        $hbCmakeFix = "$vcpkgDir\buildtrees\harfbuzz\arm-windows-webkit-cmake-fix.ini"
        if (Test-Path $hbCmakeFix) {
            Copy-Item $hbCmakeFix "$logDst\harfbuzz-cmake-fix.ini" -Force -ErrorAction SilentlyContinue
        }
        # Copy global cmake-fix from build-script (arm32-cmake-fix.ini at vcpkg root).
        if (Test-Path $arm32CmakeFix) {
            Copy-Item $arm32CmakeFix "$logDst\arm32-cmake-fix.ini" -Force -ErrorAction SilentlyContinue
        }
        Write-Host "==> Logs + vcpkg scripts copied. Check /mnt/ssd-raid/vm-shared/prism-browser/deps/build-logs/ on the Linux side."
        throw "vcpkg install failed for: $pkg"
    }
}
} # end if (-not $depsOk)

# ── Targeted ICU rebuild if it was built with wrong CRT ───────────────────────
# ICU's autoconf build can default to /MT (static CRT) when cmake cross-compile
# flags aren't passed correctly.  WebKit DLLs use /MD; mixing causes LNK2038.
# The portfile patch above adds /MD to CFLAGS/CXXFLAGS to fix new builds.
# This block detects if the CURRENT installed ICU was built with /MT and if so,
# removes only ICU from the installed dir and reinstalls just ICU.
$icuMdMarkerFile = "$vcpkgDir\icu-md-crt-ok.txt"
if (-not (Test-Path $icuMdMarkerFile)) {
    $icuLib = "$vcpkgDir\installed\$t\lib\icuuc.lib"
    $icuNeedsRebuild = $false
    if (Test-Path $icuLib) {
        Write-Host "==> Checking ICU CRT linkage..."
        # Read the .lib and search for the MT_StaticRelease marker written by MSVC.
        $libBytes = [System.IO.File]::ReadAllBytes($icuLib)
        $needle = [System.Text.Encoding]::ASCII.GetBytes("MT_StaticRelease")
        $hit = $false
        for ($i = 0; $i -le $libBytes.Length - $needle.Length -and -not $hit; $i++) {
            $match = $true
            for ($j = 0; $j -lt $needle.Length -and $match; $j++) {
                if ($libBytes[$i + $j] -ne $needle[$j]) { $match = $false }
            }
            if ($match) { $hit = $true }
        }
        if ($hit) {
            Write-Host "==> ICU was built with /MT (static CRT) -- LNK2038 will occur. Rebuilding ICU with /MD..."
            $icuNeedsRebuild = $true
        } else {
            Write-Host "==> ICU CRT is correct (MD/dynamic). Writing marker."
            "ICU MD CRT OK at $(Get-Date)" | Set-Content $icuMdMarkerFile -Encoding UTF8
        }
    } else {
        Write-Host "==> ICU lib not found (will be installed). Assuming fresh build with /MD."
    }
    if ($icuNeedsRebuild) {
        # Remove only ICU's installed files; leave other packages untouched.
        foreach ($item in @("lib\icuuc.lib","lib\icuin.lib","lib\icudata.lib",
                             "include\unicode","share\icu","share\icu-78.2",
                             "lib\icuuc78.lib","lib\icudt78.lib","lib\icuin78.lib")) {
            $p = "$vcpkgDir\installed\$t\$item"
            if (Test-Path $p) { Remove-Item $p -Recurse -Force -ErrorAction SilentlyContinue }
        }
        # Remove ICU arm buildtrees and packages so vcpkg does a clean build.
        foreach ($sub in @("$vcpkgDir\buildtrees\icu","$vcpkgDir\packages\icu_$t")) {
            if (Test-Path $sub) { Remove-Item $sub -Recurse -Force -ErrorAction SilentlyContinue }
        }
        # Remove ICU ARM32 entry from vcpkg status so vcpkg considers it uninstalled.
        $statusFile = "$vcpkgDir\installed\vcpkg\status"
        if (Test-Path $statusFile) {
            $statusRaw = [System.IO.File]::ReadAllText($statusFile)
            $tEsc = [regex]::Escape($t)
            # Remove any stanza with both "Package: icu" and "Architecture: <triplet>"
            $statusRaw = [regex]::Replace($statusRaw,
                "(?m)^Package: icu\b[\s\S]*?Status: install ok installed\r?\n",
                {
                    param($m)
                    if ($m.Value -match "Architecture: $tEsc") { "" } else { $m.Value }
                })
            [System.IO.File]::WriteAllText($statusFile, $statusRaw, [System.Text.UTF8Encoding]::new($false))
            Write-Host "==> Removed ICU $t entry from vcpkg status."
        }
        # Reinstall ICU only (host x64 tools + ARM32 target).
        Write-Host "==> Running vcpkg install for ICU with corrected /MD CFLAGS..."
        & $vcpkg install "icu[tools]:x64-windows" "icu:$t" $overlayFlag --allow-unsupported --no-print-usage 2>&1
        if ($LASTEXITCODE -ne 0) { throw "ICU rebuild (targeted) failed" }
        "ICU MD CRT OK at $(Get-Date)" | Set-Content $icuMdMarkerFile -Encoding UTF8
        Write-Host "==> ICU rebuilt with /MD CRT. Marker written."
    }
}

$vcpkgInstalled = "$vcpkgDir\installed\$tripletName"
$vcpkgInc       = "$vcpkgInstalled\include"
$vcpkgLib       = "$vcpkgInstalled\lib"
$vcpkgToolchain = "$vcpkgDir\scripts\buildsystems\vcpkg.cmake"

# Generate a wrapper toolchain that sets VCPKG_OVERRIDE_FIND_PACKAGE_NAME BEFORE vcpkg.cmake
# loads. This prevents vcpkg's find_package macro from being named "find_package", which
# would cause infinite recursion when WebKitFindPackage.cmake also overrides find_package
# (WebKit's override renames vcpkg's to _find_package, which then recurses into itself).
$wrapperToolchain = "$overlayDir\webkit-toolchain-wrapper.cmake"
$vcpkgFwd = $vcpkgToolchain -replace '\\','/'
$wrapperContent = "# Generated by build-webkit-arm32.ps1 -- do not edit`n"
$wrapperContent += "# Disables vcpkg find_package override to prevent double-override recursion`n"
$wrapperContent += "# with WebKitFindPackage.cmake (which also overrides find_package).`n"
$wrapperContent += "set(VCPKG_OVERRIDE_FIND_PACKAGE_NAME z_vcpkg_webkit_noop CACHE STRING `"`" FORCE)`n"
$wrapperContent += "include(`"$vcpkgFwd`")`n"
[System.IO.File]::WriteAllText($wrapperToolchain, $wrapperContent, [System.Text.UTF8Encoding]::new($false))
Write-Host "==> Generated wrapper toolchain: $wrapperToolchain"

Write-Host ""
Write-Host "==> vcpkg deps installed: $vcpkgInstalled"

# ══════════════════════════════════════════════════════════════════════════════
# 2.  Clone / checkout WebKit (last WinCairo commit before removal)
# ══════════════════════════════════════════════════════════════════════════════

$git = @("C:\Program Files\Git\cmd\git.exe","C:\Program Files\Git\bin\git.exe") |
       Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$git) { $git = "git" }

& $git config --global --add safe.directory "%(prefix)///VBoxSvr/vm-shared/prism-browser/deps/src/webkit"
& $git config --global core.longpaths true

$needsCheckout = !(Test-Path "$srcDir\Source\cmake\OptionsWinCairo.cmake")

if (!(Test-Path "$srcDir\.git")) {
    Write-Host "==> Cloning WebKit (blobless)..."
    & $git clone --filter=blob:none https://github.com/WebKit/WebKit.git $srcDir
    if (!(Test-Path "$srcDir\Source\WebCore\CMakeLists.txt")) {
        throw "WebKit clone failed - Source\WebCore missing"
    }
    $needsCheckout = $true
}

if ($needsCheckout) {
    Write-Host "==> Checking out last WinCairo commit (before 2022-09-01)..."
    Push-Location $srcDir
    $targetCommit = & $git log --before="2022-09-01" -1 --format="%H"
    Write-Host "    commit: $targetCommit"
    & $git checkout -f $targetCommit
    if (!(Test-Path "$srcDir\Source\cmake\OptionsWinCairo.cmake")) {
        throw "OptionsWinCairo.cmake not found after checkout"
    }
    Pop-Location
} else {
    Write-Host "==> WebKit source already at WinCairo commit."
}

# ══════════════════════════════════════════════════════════════════════════════
# 3.  ARM32 CMake patches
# ══════════════════════════════════════════════════════════════════════════════

Patch-CmakeForArm32 $srcDir

# ══════════════════════════════════════════════════════════════════════════════
# 4.  Host build tools: Perl, Ruby, gperf
#     These run on the HOST (x64) and are NOT cross-compiled.
# ══════════════════════════════════════════════════════════════════════════════

# ── Perl ──────────────────────────────────────────────────────────────────────
$perlExe = @(
    "C:\Strawberry\perl\bin\perl.exe",
    "C:\Program Files\Git\usr\bin\perl.exe",
    "C:\Program Files (x86)\Git\usr\bin\perl.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$perlExe) { Write-Error "Perl not found. Install Strawberry Perl or Git for Windows."; exit 1 }
Write-Host "==> Perl:  $perlExe"
$env:PATH = (Split-Path $perlExe) + ";$env:PATH"

# ── Ruby ──────────────────────────────────────────────────────────────────────
$rubyExe = @(
    "C:\Ruby34-x64\bin\ruby.exe","C:\Ruby33-x64\bin\ruby.exe",
    "C:\Ruby32-x64\bin\ruby.exe","C:\Ruby31-x64\bin\ruby.exe",
    "C:\Ruby30-x64\bin\ruby.exe","C:\Ruby27-x64\bin\ruby.exe",
    "C:\Ruby34\bin\ruby.exe","C:\Ruby33\bin\ruby.exe","C:\Ruby\bin\ruby.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (!$rubyExe) {
    $rc = Get-Command "ruby.exe" -ErrorAction SilentlyContinue
    if ($rc) { $rubyExe = $rc.Source }
}
if (!$rubyExe) { Write-Error "Ruby not found. Install RubyInstaller from rubyinstaller.org."; exit 1 }
Write-Host "==> Ruby:  $rubyExe"
$env:PATH = (Split-Path $rubyExe) + ";$env:PATH"
$rubyRoot    = Split-Path $rubyExe -Parent | Split-Path -Parent
$rubyLibFile = Get-ChildItem "$rubyRoot\lib" -Filter "*ruby*static*.a" -ErrorAction SilentlyContinue |
               Select-Object -First 1
if (!$rubyLibFile) {
    $rubyLibFile = Get-ChildItem "$rubyRoot\lib" -Filter "*ruby*.a" -ErrorAction SilentlyContinue |
                   Select-Object -First 1
}
$rubyLibFlag = if ($rubyLibFile) { "-DRuby_LIBRARY=$($rubyLibFile.FullName -replace '\\','/')" } else { "" }

# ── gperf ─────────────────────────────────────────────────────────────────────
$gperfExe = Get-Command gperf -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (!$gperfExe) {
    $gperfExe = @(
        "C:\Program Files (x86)\GnuWin32\bin\gperf.exe",
        "C:\Program Files\Git\usr\bin\gperf.exe",
        "C:\msys64\usr\bin\gperf.exe",
        "C:\msys64\mingw64\bin\gperf.exe",
        "C:\tools\gperf\gperf.exe"
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (!$gperfExe) {
    Write-Host "  gperf not found - installing via winget..."
    & winget install --id GnuWin32.Gperf --silent --accept-source-agreements --accept-package-agreements | Out-Null
    $gperfExe = "C:\Program Files (x86)\GnuWin32\bin\gperf.exe"
}
if (!(Test-Path $gperfExe)) { Write-Error "gperf not available"; exit 1 }
Write-Host "==> gperf: $gperfExe"
$gperfBin = Split-Path $gperfExe -Parent
if ($env:PATH -notlike "*$gperfBin*") { $env:PATH = "$gperfBin;$env:PATH" }

# ══════════════════════════════════════════════════════════════════════════════
# 5.  CMake configure
# ══════════════════════════════════════════════════════════════════════════════

# Skip clean + cmake configure if build.ninja already exists (incremental rebuild).
$buildNinjaPath = "$bldDir\build.ninja"
$skipCleanConfigure = (Test-Path $buildNinjaPath)

if (!$skipCleanConfigure -and (Test-Path $bldDir)) {
    # Use cmd's rd /s /q which handles long paths and VirtualBox shares better
    # than PowerShell's Remove-Item which fails with "path format not supported"
    # on deep paths created by cmake/ninja under the VirtualBox shared folder.
    $bldDirNative = $bldDir -replace '/','\'
    & cmd /c "rd /s /q `"$bldDirNative`"" 2>&1 | Out-Null
    if (Test-Path $bldDir) { Remove-Item $bldDir -Recurse -Force -ErrorAction SilentlyContinue }
}
New-Item $bldDir -ItemType Directory -Force | Out-Null

Push-Location $bldDir
try {
    if ($skipCleanConfigure) {
        Write-Host ""
        Write-Host "==> build.ninja exists -- skipping clean and cmake configure (incremental build)."
    } else {
        Write-Host ""
        Write-Host "==> Configuring WebKit WinCairo (ARM32)..."
    & $cmake -G Ninja `
        "-DCMAKE_TOOLCHAIN_FILE=$($wrapperToolchain -replace '\\','/')" `
        "-DVCPKG_TARGET_TRIPLET=$tripletName" `
        "-DVCPKG_OVERLAY_TRIPLETS=$($overlayDir -replace '\\','/')" `
        "-DVCPKG_MANIFEST_INSTALL=OFF" `
        "-DVCPKG_INSTALLED_DIR=$((Split-Path $vcpkgInstalled) -replace '\\','/')" `
        -DCMAKE_FIND_ROOT_PATH_MODE_PACKAGE=BOTH `
        -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=BOTH `
        -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=BOTH `
        "-DCMAKE_INCLUDE_PATH:STRING=$($vcpkgInstalled -replace '\\','/')/include" `
        "-DCMAKE_LIBRARY_PATH:STRING=$($vcpkgInstalled -replace '\\','/')/lib;$($vcpkgInstalled -replace '\\','/')/bin" `
        "-DPERL_EXECUTABLE=$($perlExe -replace '\\','/')" `
        "-DRuby_EXECUTABLE=$($rubyExe -replace '\\','/')" `
        $rubyLibFlag `
        -DPORT=WinCairo `
        -DCMAKE_BUILD_TYPE=Release `
        -DCMAKE_SYSTEM_NAME=Windows `
        -DCMAKE_SYSTEM_PROCESSOR=ARM `
        "-DCMAKE_C_COMPILER=$($clExe -replace '\\','/')" `
        "-DCMAKE_CXX_COMPILER=$($clExe -replace '\\','/')" `
        "-DCMAKE_ASM_COMPILER=$($armasmExe -replace '\\','/')" `
        `
        -DENABLE_JIT=OFF `
        -DENABLE_DFG_JIT=OFF `
        -DENABLE_FTL_JIT=OFF `
        -DENABLE_SAMPLING_PROFILER=OFF `
        -DUSE_CAIRO=ON `
        -DUSE_CURL=ON `
        -DUSE_LIBXML2=ON `
        -DUSE_LIBXSLT=ON `
        -DUSE_SQLITE=ON `
        -DUSE_ICU=ON `
        -DUSE_HARFBUZZ=ON `
        -DUSE_FREETYPE=ON `
        -DUSE_OPENSSL=ON `
        -DUSE_WOFF2=ON `
        -DUSE_WEBP=ON `
        -DUSE_LIBPSL=ON `
        -DENABLE_XSLT=ON `
        -DENABLE_WEBGL=ON `
        -DENABLE_VIDEO=ON `
        -DENABLE_WEB_AUDIO=ON `
        -DENABLE_GEOLOCATION=ON `
        -DENABLE_NOTIFICATIONS=ON `
        -DENABLE_MEDIA_STREAM=OFF `
        -DENABLE_ENCRYPTED_MEDIA=OFF `
        -DENABLE_API_TESTS=OFF `
        -DENABLE_TOOLS=OFF `
        -DENABLE_MINIBROWSER=OFF `
        -DENABLE_WEBKIT_LEGACY=OFF `
        $srcDir

        if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }
    } # end else (!$skipCleanConfigure)

    # ── 6.  Build ─────────────────────────────────────────────────────────────
    # Pipe native ninja output through Write-Host so Start-Transcript captures it.
    # 2>&1 merges stderr (error lines) into stdout; ForEach-Object + Write-Host
    # routes each line through PowerShell's output stream where Transcript sees it.

    Write-Host ""; Write-Host "==> Building JavaScriptCore..."
    & $ninja JavaScriptCore 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "JavaScriptCore build failed" }

    Write-Host ""; Write-Host "==> Building WebCore..."
    # Background job: flush transcript to Z: share every 20 seconds so Linux can monitor.
    $flushJob = Start-Job -ScriptBlock {
        param($src, $dst)
        while ($true) {
            Start-Sleep -Seconds 20
            try { Copy-Item $src $dst -Force -ErrorAction SilentlyContinue } catch {}
        }
    } -ArgumentList $transcriptLog, $sharedLog
    try {
        & $ninja WebCore 2>&1 | ForEach-Object { Write-Host $_ }
        $webCoreFailed = $LASTEXITCODE -ne 0
    } finally {
        Stop-Job $flushJob -ErrorAction SilentlyContinue
        Remove-Job $flushJob -Force -ErrorAction SilentlyContinue
    }
    if ($webCoreFailed) { throw "WebCore build failed" }

    Write-Host ""; Write-Host "==> Building WebKit..."
    & $ninja WebKit 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "WebKit build failed" }

} finally { Pop-Location }

# ── 7.  Install ───────────────────────────────────────────────────────────────
New-Item "$outDir\lib"                    -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\JavaScriptCore" -ItemType Directory -Force | Out-Null
New-Item "$outDir\include\WebCore"        -ItemType Directory -Force | Out-Null

foreach ($target in @("JavaScriptCore","WebCore","WebKit")) {
    $l = Get-ChildItem "$bldDir" -Filter "${target}*.lib" -Recurse -ErrorAction SilentlyContinue |
         Select-Object -First 1
    if ($l) {
        Copy-Item $l.FullName "$outDir\lib\${target}.lib" -Force
        Write-Host "  $($l.Name) -> $outDir\lib\${target}.lib"
    } else { Write-Warning "${target}.lib not found in build output" }
}

$jsHeaders = "$srcDir\Source\JavaScriptCore\API"
$wkHeaders = "$srcDir\Source\WebKit\UIProcess\API\C"
if (Test-Path $jsHeaders) { Copy-Item "$jsHeaders\*.h" "$outDir\include\JavaScriptCore\" -Force }
if (Test-Path $wkHeaders) { Copy-Item "$wkHeaders\*.h" "$outDir\include\WebCore\" -Force }

Write-Host ""
Write-Host "Done. WebKit WinCairo ARM32 built with full feature set:"
Write-Host "  XSLT/WebP/WOFF2/OpenSSL/ICU/libpsl: ON   WebGL/Video/Audio/Geolocation: ON"
Write-Host "  $outDir\lib\JavaScriptCore.lib"
Write-Host "  $outDir\lib\WebCore.lib"
Write-Host "  $outDir\lib\WebKit.lib"

Stop-Transcript | Out-Null
try { Copy-Item $transcriptLog $sharedLog -Force -ErrorAction SilentlyContinue } catch {}

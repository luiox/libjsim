# build-sample.ps1 - E2E sample regeneration: Java trio + self-contained native DLL.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File build-sample.ps1
#
# Java side uses JAVA_HOME or javac on PATH (compile --release 8, falls back on JDK 8).
# Native side needs MSVC (cl/link): already on PATH, or located via vswhere and
# prepared by calling vcvars64 in a child cmd session.
#
# Output: ..\..\corpus\jni\jni-sample.jar (build product, NOT committed; consumed
# by target jsim-jni-e2e-test, which skips when it is absent). A Gradle
# equivalent lives in build.gradle for CI.

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$outJar = Join-Path $root "..\..\corpus\jni\jni-sample.jar"
$classes = Join-Path $root "classes"
New-Item -ItemType Directory -Force -Path $classes, (Split-Path $outJar -Parent) | Out-Null

# ---- Java: javac + jar ----
$javac = "javac"
$jarTool = "jar"
if ($env:JAVA_HOME) {
    $javac = Join-Path $env:JAVA_HOME "bin\javac.exe"
    $jarTool = Join-Path $env:JAVA_HOME "bin\jar.exe"
}

Push-Location $root
try {
    & $javac --release 8 -encoding UTF-8 -d $classes Decrypt.java Caller.java Main.java
    if ($LASTEXITCODE -ne 0) {
        # JDK 8 does not know --release; its default target is already v52.
        & $javac -encoding UTF-8 -d $classes Decrypt.java Caller.java Main.java
        if ($LASTEXITCODE -ne 0) { throw "javac failed" }
    }

    # ---- Native: cl + link (MSVC x64) ----
    $vcprep = ""
    Get-Command cl -ErrorAction SilentlyContinue | Out-Null
    if (-not $?) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (-not (Test-Path $vswhere)) { throw "cl not on PATH and vswhere not found; run from a VS dev prompt" }
        $vsPath = & $vswhere -latest -property installationPath
        if (-not $vsPath) { throw "vswhere found no Visual Studio installation" }
        $vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
        $vcprep = "call `"$vcvars`" >nul && "
    }

    $buildCmd = "$vcprep" + `
        "cl /nologo /c /GS- /Zl /O2 /Fodecrypt.obj decrypt_jni.c && " + `
        "link /nologo /DLL /NOENTRY /NODEFAULTLIB /MACHINE:X64 /BASE:0x10000 /OUT:decrypt.dll decrypt.obj"
    cmd /c $buildCmd
    if ($LASTEXITCODE -ne 0) { throw "native build failed" }

    # ---- Jar: classes + dll ----
    & $jarTool cf $outJar `
        -C $classes sample\Decrypt.class `
        -C $classes sample\Caller.class `
        -C $classes sample\Main.class `
        -C $root decrypt.dll
    if ($LASTEXITCODE -ne 0) { throw "jar failed" }
    Remove-Item (Join-Path $root "decrypt.obj") -ErrorAction SilentlyContinue
    Write-Host "[build-sample] OK: $outJar"
} finally {
    Pop-Location
}

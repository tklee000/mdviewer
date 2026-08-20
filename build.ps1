param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [string]$CefRoot = "",
    [switch]$Deploy
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
$workspace = Split-Path -Parent $MyInvocation.MyCommand.Path
$packagesDirectory = Join-Path $workspace "packages"

$cefVersion = "151.3.17+gf059e67+chromium-151.0.7922.138"
$cefArchiveName = "cef_binary_${cefVersion}_windows64_minimal.tar.bz2"
$cefArchiveSha1 = "B200B01DDD82C1BF575FCB28CC70430E7106D4D1"
$cefDirectoryName = "cef_binary_${cefVersion}_windows64_minimal"

if (!$CefRoot) {
    $CefRoot = Join-Path $packagesDirectory $cefDirectoryName
    $cefArchive = Join-Path $packagesDirectory $cefArchiveName
    New-Item -ItemType Directory -Force -Path $packagesDirectory | Out-Null
    if (!(Test-Path -LiteralPath $CefRoot)) {
        if (!(Test-Path -LiteralPath $cefArchive)) {
            $encodedArchiveName = $cefArchiveName.Replace("+", "%2B")
            Write-Host "Downloading CEF $cefVersion (Windows x64 minimal)..."
            Invoke-WebRequest -UseBasicParsing `
                "https://cef-builds.spotifycdn.com/$encodedArchiveName" `
                -OutFile $cefArchive
        }
        $actualSha1 = (Get-FileHash -Algorithm SHA1 -LiteralPath $cefArchive).Hash
        if ($actualSha1 -ne $cefArchiveSha1) {
            Remove-Item -LiteralPath $cefArchive -Force
            throw "CEF archive checksum mismatch. The invalid archive was removed."
        }
        Write-Host "Extracting CEF..."
        & tar.exe -xf $cefArchive -C $packagesDirectory
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $CefRoot)) {
            throw "CEF extraction failed."
        }
    }
}

$CefRoot = [IO.Path]::GetFullPath($CefRoot)
if (!(Test-Path -LiteralPath (Join-Path $CefRoot "cmake\FindCEF.cmake"))) {
    throw "The selected CEF root is not a valid extracted CEF distribution: $CefRoot"
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (!(Test-Path -LiteralPath $vswhere)) {
    throw "Visual Studio Installer (vswhere) was not found."
}

$visualStudio = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
$visualStudioVersion = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationVersion
if (!$visualStudio) {
    throw "Visual Studio with the Desktop development with C++ workload is required."
}

$toolDirectory = Join-Path $env:TEMP "MdViewer-build-tools"
$cmakeVersion = "3.31.8"
$cmakeDirectory = Join-Path $toolDirectory "cmake-$cmakeVersion-windows-x86_64"
$cmake = Join-Path $cmakeDirectory "bin\cmake.exe"
if (!(Test-Path -LiteralPath $cmake)) {
    New-Item -ItemType Directory -Force -Path $toolDirectory | Out-Null
    $cmakeArchive = Join-Path $toolDirectory "cmake-$cmakeVersion-windows-x86_64.zip"
    if (!(Test-Path -LiteralPath $cmakeArchive)) {
        Invoke-WebRequest -UseBasicParsing `
            "https://cmake.org/files/v3.31/cmake-$cmakeVersion-windows-x86_64.zip" `
            -OutFile $cmakeArchive
    }
    Expand-Archive -LiteralPath $cmakeArchive -DestinationPath $toolDirectory -Force
}

$generator = if ($visualStudioVersion.StartsWith("17.")) {
    "Visual Studio 17 2022"
} elseif ($visualStudioVersion.StartsWith("16.")) {
    "Visual Studio 16 2019"
} else {
    throw "Visual Studio $visualStudioVersion is not supported."
}

$buildDirectory = Join-Path $workspace "out\build"
$configurationOutput = [IO.Path]::GetFullPath((Join-Path $workspace "x64\$Configuration"))
$expectedOutputParent = [IO.Path]::GetFullPath((Join-Path $workspace "x64"))
if ([IO.Path]::GetDirectoryName($configurationOutput) -ne $expectedOutputParent) {
    throw "Resolved build output directory is outside x64."
}
if (Test-Path -LiteralPath $configurationOutput) {
    Remove-Item -LiteralPath $configurationOutput -Recurse -Force
}

Push-Location $workspace
try {
    & $cmake -S $workspace -B $buildDirectory -G $generator -A x64 `
        "-DCEF_ROOT=$CefRoot" "-DCEF_RUNTIME_LIBRARY_FLAG=/MT"
    if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed." }

    & $cmake --build $buildDirectory --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) { throw "Build failed." }
} finally {
    Pop-Location
}

if ($Deploy) {
    if ($Configuration -ne "Release") {
        throw "Deployment requires -Configuration Release."
    }
    $sourceDirectory = Join-Path $workspace "x64\Release"
    $deployDirectory = [IO.Path]::GetFullPath((Join-Path $workspace "deploy"))
    if ([IO.Path]::GetDirectoryName($deployDirectory) -ne [IO.Path]::GetFullPath($workspace)) {
        throw "Resolved deploy directory is outside the workspace."
    }
    if (Test-Path -LiteralPath $deployDirectory) {
        Remove-Item -LiteralPath $deployDirectory -Recurse -Force
    }
    New-Item -ItemType Directory -Path $deployDirectory | Out-Null
    Copy-Item -Path (Join-Path $sourceDirectory "*") `
        -Destination $deployDirectory -Recurse -Force
    Write-Host "Deploy complete: deploy\MdViewer.exe"
}

Write-Host ""
Write-Host "Build complete: x64\$Configuration\MdViewer.exe"

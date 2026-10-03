#Requires -Version 5.1
<#
.SYNOPSIS
    CI deploy step for ArtMoon Windows (Qt 6.8.3, msvc2022_64).

.DESCRIPTION
    Replicates FoggyBytes' manual build-release.ps1 recipe, adapted for a
    headless GitHub Actions runner:
      1. build-arch.bat release (its own windeployqt step fails on Qt 6.8.3 —
         the --no-quickcontrols2fluentwinui3styleimpl flag is unsupported there;
         this is EXPECTED and ignored as long as ArtMoon.exe was produced)
      2. windeployqt run separately with the Qt 6.8.3-compatible flag set
      3. libs/windows/lib/x64 DLLs + AntiHooking.dll + gamecontrollerdb.txt
         copied into the deploy folder
      4. unused Qt Quick Controls styles pruned
    The final folder (build/deploy-x64-release) is what ArtMoon.iss packages.
#>
$ErrorActionPreference = 'Stop'

$RepoRoot    = $PSScriptRoot | Split-Path
$QtBinPath   = $env:ARTMOON_QT_BIN
if (-not $QtBinPath) { $QtBinPath = 'C:\Qt\6.8.3\msvc2022_64\bin' }
$BuildArchBat = Join-Path $RepoRoot 'scripts\build-arch.bat'
$CompiledExe  = Join-Path $RepoRoot 'build\build-x64-release\app\release\ArtMoon.exe'
$DeployFolder = Join-Path $RepoRoot 'build\deploy-x64-release'
$LibsFolder   = Join-Path $RepoRoot 'libs\windows\lib\x64'
$QmlDir       = Join-Path $RepoRoot 'app\gui'

function Fail { param($msg) Write-Host "##vso[task.logissue type=error]$msg"; exit 1 }
function Ok   { param($msg) Write-Host "[OK] $msg" }

# ---------------------------------------------------------------------------
# Step 1 — PATH
# ---------------------------------------------------------------------------
$env:PATH = "$QtBinPath;$env:PATH"

# ---------------------------------------------------------------------------
# Step 2 — Build (build-arch.bat release). Known-wrong exit code tolerated.
# ---------------------------------------------------------------------------
Push-Location $RepoRoot
cmd /c "`"$BuildArchBat`" release" 2>&1 | Write-Host
$batExit = $LASTEXITCODE
Pop-Location

if (-not (Test-Path $CompiledExe)) {
    Fail "build-arch.bat failed AND ArtMoon.exe was not produced (bat exit: $batExit)"
}
Ok "ArtMoon.exe produced (bat exit $batExit tolerated — expected windeployqt flag issue on Qt 6.8.3)"

# ---------------------------------------------------------------------------
# Step 3 — windeployqt with Qt 6.8.3-compatible flags
# ---------------------------------------------------------------------------
$windeployArgs = @(
    '--release'
    '--qmldir', $QmlDir
    '--no-opengl-sw'
    '--no-compiler-runtime'
    '--no-sql'
    '--no-system-d3d-compiler'
    '--no-system-dxc-compiler'
    '--skip-plugin-types', 'qmltooling,generic'
    '--no-ffmpeg'
    '--no-quickcontrols2fusion'
    '--no-quickcontrols2imagine'
    '--no-quickcontrols2universal'
    '--dir', $DeployFolder
    $CompiledExe
)

& (Join-Path $QtBinPath 'windeployqt.exe') @windeployArgs
if ($LASTEXITCODE -ne 0) { Fail "windeployqt failed with exit $LASTEXITCODE" }
Ok "windeployqt complete"

# ---------------------------------------------------------------------------
# Step 4 — libs DLLs (SDL2, ssl, crypto, avcodec, opus, placebo, ...)
# ---------------------------------------------------------------------------
$libDlls = Get-ChildItem -Path $LibsFolder -Filter '*.dll' -ErrorAction SilentlyContinue
if ($libDlls.Count -eq 0) { Fail "No DLLs found in $LibsFolder" }
foreach ($dll in $libDlls) { Copy-Item $dll.FullName -Destination $DeployFolder -Force }
Ok "$($libDlls.Count) DLLs copied from libs"

# ---------------------------------------------------------------------------
# Step 5 — AntiHooking.dll + gamecontrollerdb.txt
# ---------------------------------------------------------------------------
$antiHook = Join-Path $RepoRoot 'build\build-x64-release\AntiHooking\release\AntiHooking.dll'
if (Test-Path $antiHook) { Copy-Item $antiHook -Destination $DeployFolder -Force; Ok "AntiHooking.dll copied" }
else { Fail "AntiHooking.dll missing at $antiHook" }

$gcDb = Join-Path $RepoRoot 'app\SDL_GameControllerDB\gamecontrollerdb.txt'
Copy-Item $gcDb -Destination $DeployFolder -Force
Ok "gamecontrollerdb.txt copied"

# ---------------------------------------------------------------------------
# Step 6 — ensure ArtMoon.exe itself is in the deploy folder
# ---------------------------------------------------------------------------
Copy-Item $CompiledExe -Destination $DeployFolder -Force

# ---------------------------------------------------------------------------
# Step 7 — prune unused Qt Quick Controls styles
# ---------------------------------------------------------------------------
foreach ($rel in @(
    'qml\QtQuick\Controls\Fusion',
    'qml\QtQuick\Controls\Imagine',
    'qml\QtQuick\Controls\Universal',
    'qml\QtQuick\Controls\Windows',
    'qml\QtQuick\Controls\FluentWinUI3',
    'qml\QtQuick\Controls\NativeStyle')) {
    $p = Join-Path $DeployFolder $rel
    if (Test-Path $p) { Remove-Item -Recurse -Force $p }
}
Ok "Unused Qt styles pruned"

# ---------------------------------------------------------------------------
# Step 8 — the privileged input service, compiled and its guards exercised
# ---------------------------------------------------------------------------
# ArtMoon cannot bind devices for itself: `bind` is refused without administrator
# rights, so this service does it on the user's behalf. It runs as LocalSystem and
# its input arrives from an unprivileged process, which is why the refusals matter
# far more here than the happy path — a guard that quietly stopped holding would be
# a privilege-escalation hole, not a cosmetic bug. `selftest` exercises them against
# the hostile list without needing the service installed or a device bound.
$serviceSrc = Join-Path $RepoRoot 'service\artmoon-input-service-win.cpp'
$serviceExe = Join-Path $DeployFolder 'artmoon-input-service.exe'
if (-not (Test-Path $serviceSrc)) { Fail "missing the input service source at $serviceSrc" }

$serviceArgs = @('/nologo', '/EHsc', '/W4', '/O2', $serviceSrc,
                 "/Fe:$serviceExe", "/Fo:$env:TEMP\artmoon-input-service.obj",
                 '/link', 'advapi32.lib')

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    & cl.exe @serviceArgs 2>&1 | Write-Host
} else {
    # The runner image normally has the tools on PATH already (build-arch.bat depends on
    # it). This is the fallback rather than the assumption: cl.exe without INCLUDE/LIB
    # set fails in a way that reads like a code error and is not one.
    $vsDev = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\2022\*\Common7\Tools\VsDevCmd.bat' -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $vsDev) { Fail "cl.exe is not on PATH and no VsDevCmd.bat was found" }
    $cl = "`"$($vsDev.FullName)`" -arch=amd64 -host_arch=amd64 >nul && cl.exe " +
          ($serviceArgs -join ' ')
    cmd /c $cl 2>&1 | Write-Host
}
if ($LASTEXITCODE -ne 0 -or -not (Test-Path $serviceExe)) { Fail "the input service did not compile" }
Ok "artmoon-input-service.exe compiled"

& $serviceExe selftest 2>&1 | Write-Host
if ($LASTEXITCODE -ne 0) { Fail "the input service's busid guards did not hold" }
Ok "input service guards hold"

$exe = Join-Path $DeployFolder 'ArtMoon.exe'
if (-not (Test-Path $exe)) { Fail "deploy folder missing ArtMoon.exe" }
$count = (Get-ChildItem -Recurse -File $DeployFolder).Count
Ok "Deploy folder ready: $DeployFolder ($count files)"

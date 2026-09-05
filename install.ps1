<#
.SYNOPSIS
    Install Crucible on Windows.

.DESCRIPTION
    The Windows half of install.sh. One command:

        irm https://raw.githubusercontent.com/mattsaund/Crucible/main/install.ps1 | iex

    It does what the shell installer does, in the same order and with the same
    decisions: find or install the build tools, fetch the source, configure,
    build, install into a user-writable prefix, and put that prefix on PATH.

    No GPU runtime is built here, on Windows or anywhere else. Crucible compiles
    one on demand from its settings screen, because a backend has to match the
    machine it will run on and a ten-minute compile does not belong in an
    installer.

.PARAMETER Gpu
    Which GPU SDK to make sure is present so that runtime can be built later:
    cuda, vulkan, cpu or auto. Metal is macOS only and is refused here.

.PARAMETER Prefix
    Where to install. Defaults to %LOCALAPPDATA%\Programs\Crucible.

.PARAMETER NoGui
    Build only the terminal program, skipping crucible-gui. The desktop app is
    built by default: it needs nothing extra on Windows, since OpenGL is part
    of the system, so unlike Linux it costs only build time.

.PARAMETER Gui
    Build the desktop application. On by default; the switch is kept so an
    explicit -Gui still means what it says.

.PARAMETER Uninstall
    Remove an installed Crucible instead of installing one.
#>
[CmdletBinding()]
param(
    [ValidateSet('auto', 'cuda', 'vulkan', 'cpu')]
    [string] $Gpu    = 'auto',
    [string] $Prefix = (Join-Path $env:LOCALAPPDATA 'Programs\Crucible'),
    [string] $Branch = 'main',
    [int]    $Jobs   = 0,
    [switch] $Gui,
    [switch] $NoGui,
    [switch] $NoDeps,
    [switch] $Yes,
    [switch] $Check,
    [switch] $Uninstall
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# The banner is braille and the notes carry arrows, and Windows PowerShell 5.1
# writes both as question marks unless the console is told otherwise. PowerShell
# 7 is already UTF-8; this costs nothing there. Wrapped because a host without a
# real console -- the ISE, an embedded runspace -- throws on the assignment, and
# a mangled banner is not worth failing an install over.
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }

$RepoUrl = 'https://github.com/mattsaund/Crucible.git'
$SrcDir  = Join-Path $env:LOCALAPPDATA 'crucible\src'

# ---------------------------------------------------------------------------
# Output
#
# The same three voices the shell installer uses: a step, a note, and a
# refusal. Colour through Write-Host because this is a console tool and the
# information is the point, not the object stream.
# ---------------------------------------------------------------------------
function Write-Banner {
    # packaging/flame.txt, verbatim. A single-quoted here-string, so nothing
    # in it is expanded or escaped.
    Write-Host @'

      ⠀⠀⠀⠀⠀⠀⢱⣆⠀⠀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⠈⣿⣷⡀⠀⠀⠀⠀
      ⠀⠀⠀⠀⠀⠀⢸⣿⣿⣷⣧⠀⠀⠀
      ⠀⠀⠀⠀⡀⢠⣿⡟⣿⣿⣿⡇⠀⠀
      ⠀⠀⠀⠀⣳⣼⣿⡏⢸⣿⣿⣿⢀⠀   Crucible
      ⠀⠀⠀⣰⣿⣿⡿⠁⢸⣿⣿⡟⣼⡆   a local forge: experts on demand, projects that cook
      ⢰⢀⣾⣿⣿⠟⠀⠀⣾⢿⣿⣿⣿⣿
      ⢸⣿⣿⣿⡏⠀⠀⠀⠃⠸⣿⣿⣿⡿
      ⢳⣿⣿⣿⠀⠀⠀⠀⠀⠀⢹⣿⡿⡁
      ⠀⠹⣿⣿⡄⠀⠀⠀⠀⠀⢠⣿⡞⠁
      ⠀⠀⠈⠛⢿⣄⠀⠀⠀⣠⠞⠋⠀⠀
      ⠀⠀⠀⠀⠀⠀⠉⠀⠀⠀⠀⠀⠀⠀

'@ -ForegroundColor DarkYellow
}

function Write-Step ($Message) { Write-Host "==> $Message" -ForegroundColor DarkYellow }
function Write-Note ($Message) { Write-Host "    $Message" -ForegroundColor DarkGray }
function Write-Ok   ($Message) { Write-Host "    OK $Message" -ForegroundColor Green }
function Write-Warn ($Message) { Write-Host "    !  $Message" -ForegroundColor Yellow }
function Stop-Install ($Message) {
    Write-Host ""
    Write-Host "error: $Message" -ForegroundColor Red
    exit 1
}

function Test-Command ($Name) {
    $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------------------
# Dependencies
#
# winget rather than chocolatey or scoop: it ships with Windows 10 1809 and
# later, so it is the one package manager that is already there. Where it is
# not, this says what to install by hand rather than installing a package
# manager to install a compiler.
# ---------------------------------------------------------------------------
function Install-Package ($Id, $What) {
    if (-not (Test-Command 'winget')) {
        Stop-Install "$What is missing and winget is not available. Install $What by hand, then run this again."
    }
    Write-Note "installing $What"
    winget install --id $Id --exact --silent --accept-source-agreements --accept-package-agreements | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Stop-Install "could not install $What"
    }
    # winget puts new tools on PATH for future sessions, not this one.
    $env:PATH = [Environment]::GetEnvironmentVariable('PATH', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('PATH', 'User')
}

function Find-VisualStudio {
    # vswhere is installed by every Visual Studio since 2017 and lives at a
    # fixed path, which is the whole reason it exists.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        return $null
    }
    $found = & $vswhere -latest -products * `
                        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                        -property installationPath 2>$null
    if ([string]::IsNullOrWhiteSpace($found)) { return $null }
    return $found.Trim()
}

function Initialize-BuildEnvironment {
    # The compiler needs its environment: INCLUDE, LIB and a cl.exe on PATH.
    # VsDevCmd sets them, and the only way to get them into this process is to
    # run it and read back what changed.
    $vs = Find-VisualStudio
    if ($null -eq $vs) { return $false }

    $devcmd = Join-Path $vs 'Common7\Tools\VsDevCmd.bat'
    if (-not (Test-Path $devcmd)) { return $false }

    Write-Note "using the toolchain from $vs"
    cmd /s /c "`"$devcmd`" -arch=amd64 -no_logo && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] -ErrorAction SilentlyContinue
        }
    }
    return (Test-Command 'cl')
}

function Resolve-Runtime {
    if ($Gpu -ne 'auto') { return $Gpu }

    # The same detection the shell installer does, by the same rule: name the
    # SDK that could actually be built for, not the card that is present.
    $nvidia = $false
    try {
        $nvidia = (Get-CimInstance Win32_VideoController -ErrorAction Stop |
                   Where-Object { $_.Name -match 'NVIDIA' }).Count -gt 0
    } catch { }

    if ($nvidia) { return 'cuda' }
    return 'vulkan'
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
function Remove-Crucible {
    $exe = Join-Path $Prefix 'bin\crucible.exe'
    if (Test-Path $exe) {
        Write-Step 'removing Crucible'
        # The binary knows what it put down, and asks before removing config,
        # models and history. Better than this script guessing.
        & $exe --uninstall @($(if ($Yes) { '--yes' }))
    } else {
        Write-Warn "no crucible.exe at $exe"
    }
    foreach ($dir in @($Prefix, $SrcDir)) {
        if (Test-Path $dir) {
            Write-Note "removing $dir"
            Remove-Item -Recurse -Force $dir -ErrorAction SilentlyContinue
        }
    }

    # The install put bin\ on the user PATH, so the uninstall takes it off
    # again. A PATH entry pointing at a directory that no longer exists is not
    # harmful, but it is litter, and it is litter this script created.
    $binDir   = Join-Path $Prefix 'bin'
    $userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
    if ($userPath -and $userPath -like "*$binDir*") {
        $kept = @($userPath -split ';' | Where-Object { $_ -and $_ -ne $binDir })
        [Environment]::SetEnvironmentVariable('PATH', ($kept -join ';'), 'User')
        Write-Note "removed $binDir from PATH"
    }

    # Say what survived rather than claiming success over the top of it.
    $left = @($Prefix, $SrcDir) | Where-Object { Test-Path $_ }
    if ($left) {
        foreach ($path in $left) { Write-Warn "still present: $path" }
        exit 1
    }
    Write-Ok 'done'
    exit 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
Write-Banner

if ($Uninstall) { Remove-Crucible }

$runtime = Resolve-Runtime
Write-Step "checking the machine"
Write-Note "prefix     : $Prefix"
Write-Note "GPU SDK    : $runtime"
# -Gui is accepted and redundant; -NoGui is the one that changes anything.
$BuildGui = -not $NoGui
Write-Note "desktop app: $(if ($BuildGui) { 'yes' } else { 'no (-NoGui)' })"

if ($Check) {
    Write-Note "cmake      : $(if (Test-Command 'cmake') { 'found' } else { 'would install' })"
    Write-Note "git        : $(if (Test-Command 'git')   { 'found' } else { 'would install' })"
    Write-Note "compiler   : $(if ($null -ne (Find-VisualStudio)) { 'found' } else { 'would install' })"
    Write-Ok 'nothing was changed'
    exit 0
}

if (-not $NoDeps) {
    Write-Step 'build tools'
    if (-not (Test-Command 'git'))   { Install-Package 'Git.Git' 'git' }
    if (-not (Test-Command 'cmake')) { Install-Package 'Kitware.CMake' 'cmake' }
    if ($null -eq (Find-VisualStudio)) {
        Write-Note 'installing the Visual Studio build tools -- this is a large download'
        Install-Package 'Microsoft.VisualStudio.2022.BuildTools' 'the Visual Studio build tools'
    }
}

if (-not (Initialize-BuildEnvironment)) {
    Stop-Install @'
no C++ compiler. Install the "Desktop development with C++" workload from the
Visual Studio Build Tools, then run this again:
  winget install --id Microsoft.VisualStudio.2022.BuildTools --exact
'@
}
Write-Ok 'toolchain ready'

Write-Step 'fetching the source'
if (Test-Path (Join-Path $SrcDir '.git')) {
    git -C $SrcDir fetch --depth 1 origin $Branch  | Out-Null
    git -C $SrcDir checkout -q FETCH_HEAD          | Out-Null
} else {
    New-Item -ItemType Directory -Force -Path (Split-Path $SrcDir) | Out-Null
    git clone --depth 1 --branch $Branch $RepoUrl $SrcDir | Out-Null
}
if ($LASTEXITCODE -ne 0) { Stop-Install "could not fetch $RepoUrl" }
Write-Ok "source in $SrcDir"

Write-Step 'building'
$buildDir = Join-Path $SrcDir 'build'
if ($Jobs -le 0) { $Jobs = [Environment]::ProcessorCount }

# Ninja when it is there and MSBuild when it is not. Ninja is several times
# faster on a build this size, and the Visual Studio installer ships it.
$generator = if (Test-Command 'ninja') { @('-G', 'Ninja') } else { @() }

& cmake -S $SrcDir -B $buildDir @generator `
    -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_INSTALL_PREFIX="$Prefix" `
    -DCRUCIBLE_BACKEND_DL=ON `
    -DCRUCIBLE_BUILD_GUI="$(if ($BuildGui) { 'ON' } else { 'OFF' })" 2>&1 |
    Tee-Object -Variable configureLog | Out-Null
if ($LASTEXITCODE -ne 0) {
    $configureLog | Select-String -Pattern 'CMake Error|error:' | Select-Object -First 20 |
        ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Stop-Install 'cmake configure failed'
}

& cmake --build $buildDir --config Release -j $Jobs 2>&1 |
    Tee-Object -Variable buildLog | Out-Null
if ($LASTEXITCODE -ne 0) {
    $buildLog | Select-String -Pattern 'error|FAILED' | Select-Object -First 20 |
        ForEach-Object { Write-Host $_ -ForegroundColor Red }
    Stop-Install 'the build failed'
}
Write-Ok 'built'

Write-Step 'installing'
# The component, for the same reason the shell installer passes it: llama.cpp
# and ggml carry their own install rules written for people installing them as
# a library, and a plain install would scatter their headers and import
# libraries through the prefix.
& cmake --install $buildDir --config Release --component crucible | Out-Null
if ($LASTEXITCODE -ne 0) { Stop-Install 'the install failed' }

$binDir = Join-Path $Prefix 'bin'
$userPath = [Environment]::GetEnvironmentVariable('PATH', 'User')
if ($userPath -notlike "*$binDir*") {
    Write-Note "adding $binDir to PATH"
    [Environment]::SetEnvironmentVariable('PATH', "$userPath;$binDir", 'User')
    Write-Warn 'open a new terminal for that to take effect'
}

Write-Ok "installed to $Prefix"
Write-Host ''
Write-Note 'cd into a project and run:  crucible'
if ($BuildGui) { Write-Note 'or open the desktop app:     crucible-gui' }
Write-Note 'no GPU runtime is installed yet -- type /runtimes inside Crucible to build one'
Write-Host ''

# build_Windows.ps1 - Build and package 6502caEd for Windows
# Run from the MSYS2 UCRT64 terminal:
#   powershell -ExecutionPolicy Bypass -File build_Windows.ps1
# Or from PowerShell directly.
#
# Options:
#   -Choice <1-5>   Skip the menu and run a specific option directly
#   -Yes            Skip confirmation prompts
#   -Help           Show usage

param(
    [string]$Choice = "",
    [switch]$Yes,
    [switch]$Help
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

# Use a safe build directory - the source path may contain special characters
# (spaces, !, etc.) that confuse gcc/ninja. Build in a fixed safe location.
# If the source path is clean, build inside it as normal.
if ($ScriptDir -match '[!&%^()+=\[\]{};,`~]') {
    $BuildDir = "C:\6502caEd-build\win-mingw64-release"
    Write-Host "  Note: Source path contains special characters."
    Write-Host "  Building in safe location: $BuildDir"
} else {
    $BuildDir = Join-Path $ScriptDir "out\build\win-mingw64-release"
}
$DistDir      = Join-Path $ScriptDir "out\dist"
$InstallerDir = Join-Path $ScriptDir "installers"

# ---------------------------------------------------------------------------
# Read versions.txt
# Rules: blank/comment lines ignored, all whitespace stripped, split on =
# ---------------------------------------------------------------------------
function Read-Version {
    param([string]$Key)
    $file = Join-Path $ScriptDir "versions.txt"
    if (-not (Test-Path $file)) { return "0.00" }
    foreach ($line in [System.IO.File]::ReadAllLines($file)) {
        $line = $line -replace '[\s\r]', ''
        if ($line -eq '' -or $line.StartsWith('#')) { continue }
        if ($line -match "^${Key}=(.+)$") { return $Matches[1] }
    }
    return "0.00"
}

$VER_CLI = Read-Version "cli"
$VER_TUI = Read-Version "tui"
$VER_GTK = Read-Version "gtk"

# ---------------------------------------------------------------------------
# Banner
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "+==============================================+"
Write-Host "|         6502caEd Windows Build Script        |"
Write-Host "+==============================================+"
Write-Host ""
Write-Host "  Versions from versions.txt:"
Write-Host "    CLI : $VER_CLI"
Write-Host "    TUI : $VER_TUI"
Write-Host "    GTK : $VER_GTK"
Write-Host ""

if ($Help) {
    Write-Host "Usage: powershell -File build_Windows.ps1 [-Choice 1-5] [-Yes] [-Help]"
    Write-Host ""
    Write-Host "  [1]  CLI assembler only"
    Write-Host "  [2]  TUI editor only"
    Write-Host "  [3]  GTK editor only"
    Write-Host "  [4]  CLI + TUI + GTK  (all targets, no installer)"
    Write-Host "  [5]  All targets + bundle DLLs + create Inno Setup installer"
    Write-Host "  [6]  Quit"
    exit 0
}

# ---------------------------------------------------------------------------
# Menu
# ---------------------------------------------------------------------------
Write-Host "  What would you like to build?"
Write-Host ""
Write-Host "    [1]  CLI assembler only"
Write-Host "    [2]  TUI editor only"
Write-Host "    [3]  GTK editor only"
Write-Host "    [4]  CLI + TUI + GTK  (all targets, no installer)"
Write-Host "    [5]  All targets + bundle DLLs + create Inno Setup installer"
Write-Host "    [6]  Quit"
Write-Host ""

if ($Choice -eq "") {
    $Choice = Read-Host "  Your choice [1-6] (default=4)"
    if ($Choice -eq "") { $Choice = "4" }
}

$BUILD_CLI = $false
$BUILD_TUI = $false
$BUILD_GTK = $false
$BUNDLE_DLLS = $false
$BUILD_INSTALLER = $false

switch ($Choice) {
    "1" { $BUILD_CLI = $true }
    "2" { $BUILD_TUI = $true }
    "3" { $BUILD_GTK = $true }
    "4" { $BUILD_CLI = $true; $BUILD_TUI = $true; $BUILD_GTK = $true }
    "5" { $BUILD_CLI = $true; $BUILD_TUI = $true; $BUILD_GTK = $true
          $BUNDLE_DLLS = $true; $BUILD_INSTALLER = $true }
    "6" { Write-Host "  Bye."; exit 0 }
    default { Write-Host "[!] Invalid choice."; exit 1 }
}

# ---------------------------------------------------------------------------
# Build plan summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "  +-----------------------------------------+"
Write-Host "  |  Build plan                             |"
Write-Host "  +-----------------------------------------+"
if ($BUILD_CLI) { Write-Host ("  |  CLI : v{0,-31}|" -f $VER_CLI) }
if ($BUILD_TUI) { Write-Host ("  |  TUI : v{0,-31}|" -f $VER_TUI) }
if ($BUILD_GTK) { Write-Host ("  |  GTK : v{0,-31}|" -f $VER_GTK) }
if ($BUNDLE_DLLS)     { Write-Host "  |  DLLs: bundle GTK dependencies          |" }
if ($BUILD_INSTALLER) { Write-Host "  |  ISS : create Inno Setup installer      |" }
Write-Host "  +-----------------------------------------+"
Write-Host ""

if (-not $Yes) {
    $confirm = Read-Host "  Proceed? [Y/n]"
    if ($confirm -match '^[Nn]') { Write-Host "  Aborted."; exit 0 }
}

# ---------------------------------------------------------------------------
# Helper: run a command and check exit code
# ---------------------------------------------------------------------------
function Invoke-Cmake {
    param([string]$Desc, [string[]]$CmakeArgs)
    Write-Host ""
    Write-Host "  >> $Desc"
    & $Cmake @CmakeArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[!] FAILED: $Desc (exit code $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

function Invoke-Cmd {
    param([string]$Desc, [string]$Cmd, [string[]]$CmdArgs)
    Write-Host ""
    Write-Host "  >> $Desc"
    & $Cmd @CmdArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[!] FAILED: $Desc (exit code $LASTEXITCODE)"
        exit $LASTEXITCODE
    }
}

# ---------------------------------------------------------------------------
# MSYS2 detection and installation
# ---------------------------------------------------------------------------
$MSYS2Root = $null
foreach ($candidate in @("C:\msys64", "C:\msys2", "$env:LOCALAPPDATA\msys64")) {
    if (Test-Path (Join-Path $candidate "usr\bin\bash.exe")) {
        $MSYS2Root = $candidate; break
    }
}

if ($null -eq $MSYS2Root) {
    Write-Host ""
    Write-Host "  MSYS2 is not installed. It is required to build 6502caEd."
    Write-Host "  Download size: ~100 MB. Install location: C:\msys64"
    Write-Host ""
    if (-not $Yes) {
        $r = Read-Host "  Download and install MSYS2 now? [Y/n]"
        if ($r -match '^[Nn]') { Write-Host "  Aborted."; exit 1 }
    }
    Write-Host "  >> Downloading MSYS2 installer..."
    $Installer = Join-Path $env:TEMP "msys2-installer.exe"
    $Url = "https://github.com/msys2/msys2-installer/releases/download/nightly-x86_64/msys2-x86_64-latest.exe"
    Invoke-WebRequest -Uri $Url -OutFile $Installer -UseBasicParsing
    Write-Host "  >> Running MSYS2 installer (follow the prompts)..."
    Start-Process -FilePath $Installer -ArgumentList @("install", "--root", "C:\msys64", "--confirm-command") -Wait
    Remove-Item $Installer -Force
    $MSYS2Root = "C:\msys64"
    if (-not (Test-Path (Join-Path $MSYS2Root "usr\bin\bash.exe"))) {
        Write-Host "[!] MSYS2 installation failed or was cancelled."
        exit 1
    }
    Write-Host "  >> MSYS2 installed at $MSYS2Root"
}

Write-Host "  >> MSYS2 found at $MSYS2Root"
$Bash   = Join-Path $MSYS2Root "usr\bin\bash.exe"
$Pacman = Join-Path $MSYS2Root "usr\bin\pacman.exe"

# ---------------------------------------------------------------------------
# Install required MSYS2 packages
# ---------------------------------------------------------------------------
function Install-Packages {
    param([string[]]$Packages)
    $list = $Packages -join " "
    Write-Host "  >> Installing: $list"
    & $Bash "-lc" "pacman -S --needed --noconfirm $list"
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[!] Package installation failed."
        exit $LASTEXITCODE
    }
}

Write-Host ""
Write-Host "  >> Checking required packages..."

# Always needed
$CorePkgs = @(
    "mingw-w64-ucrt-x86_64-gcc",
    "mingw-w64-ucrt-x86_64-cmake",
    "mingw-w64-ucrt-x86_64-ninja"
)
Install-Packages $CorePkgs

if ($BUILD_TUI) {
    Install-Packages @("mingw-w64-ucrt-x86_64-pdcurses")
}
if ($BUILD_GTK) {
    Install-Packages @(
        "mingw-w64-ucrt-x86_64-gtk4",
        "mingw-w64-ucrt-x86_64-gtksourceview5",
        "mingw-w64-ucrt-x86_64-pkgconf"
    )
}

$Cmake = Join-Path $MSYS2Root "ucrt64\bin\cmake.exe"
if (-not (Test-Path $Cmake)) {
    Write-Host "[!] cmake still not found after install. Check MSYS2 setup."
    exit 1
}

$NinjaM = (Join-Path $MSYS2Root "ucrt64\bin\ninja.exe") -replace '\\', '/'

$GccPath  = Join-Path $MSYS2Root "ucrt64\bin\gcc.exe"
if (-not (Test-Path $GccPath)) {
    # Fallback to cross-compiler name
    $GccPath = Join-Path $MSYS2Root "ucrt64\bin\x86_64-w64-mingw32-gcc.exe"
}

# Add MSYS2 bin directories to PATH so gcc can find its runtime DLLs
$env:PATH = "$MSYS2Root\ucrt64\bin;$MSYS2Root\usr\bin;$env:PATH"

# ---------------------------------------------------------------------------
# Configure - use -S/-B directly, no preset required
# ---------------------------------------------------------------------------
$ConfigArgs = @(
    "-S", $ScriptDir,
    "-B", $BuildDir,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_SYSTEM_NAME=Windows",
    "-DCMAKE_C_COMPILER=$GccPath",
    "-DCMAKE_MAKE_PROGRAM=$NinjaM",
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)
if ($BUILD_GTK) { $ConfigArgs += "-DED_BUILD_GTK=ON"  } else { $ConfigArgs += "-DED_BUILD_GTK=OFF" }
if ($BUILD_GTK) { $ConfigArgs += "-DED_BUILD_SIM=ON"  } else { $ConfigArgs += "-DED_BUILD_SIM=OFF" }
if ($BUILD_TUI) { $ConfigArgs += "-DED_BUILD_TUI=ON"  } else { $ConfigArgs += "-DED_BUILD_TUI=OFF" }

$CacheFile = Join-Path $BuildDir "CMakeCache.txt"
if (Test-Path $CacheFile) {
    $CacheContent = Get-Content $CacheFile -Raw -ErrorAction SilentlyContinue
    if ($CacheContent -and -not $CacheContent.Contains($ScriptDir.Replace('\','/'))) {
        Write-Host "  >> Removing stale CMake cache from different source path..."
        Remove-Item $CacheFile -Force
    }
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

Invoke-Cmake "Configuring" $ConfigArgs

# ---------------------------------------------------------------------------
# Build targets
# ---------------------------------------------------------------------------
if ($BUILD_CLI) {
    Invoke-Cmake "Building CLI assembler (v$VER_CLI)" @(
        "--build", $BuildDir, "--target", "6502ca-cli"
    )
}
if ($BUILD_TUI) {
    Invoke-Cmake "Building TUI editor (v$VER_TUI)" @(
        "--build", $BuildDir, "--target", "6502caEd"
    )
}
if ($BUILD_GTK) {
    Invoke-Cmake "Building GTK editor (v$VER_GTK)" @(
        "--build", $BuildDir, "--target", "6502caEdGtk"
    )
}

# ---------------------------------------------------------------------------
# Helper: find a built exe - CMake may put it in the subdirectory or root
# ---------------------------------------------------------------------------
function Find-BuildExe {
    param([string]$Name)
    # Probe with and without .exe in case CMAKE_EXECUTABLE_SUFFIX isn't set
    $bases = @($Name, ($Name -replace '\.exe$', ''))
    foreach ($base in $bases) {
        foreach ($sub in @("6502caEd", "")) {
            $p = if ($sub) { Join-Path $BuildDir "$sub\$base" } else { Join-Path $BuildDir $base }
            if (Test-Path $p) { return $p }
        }
    }
    Write-Host "[!] Cannot find $Name in build output. Searched:"
    foreach ($base in $bases) {
        foreach ($sub in @("6502caEd", "")) {
            $p = if ($sub) { Join-Path $BuildDir "$sub\$base" } else { Join-Path $BuildDir $base }
            Write-Host "      $p"
        }
    }
    exit 1
}

# ---------------------------------------------------------------------------
# Bundle DLLs for GTK editor
# ---------------------------------------------------------------------------
if ($BUNDLE_DLLS -and $BUILD_GTK) {
    Write-Host ""
    Write-Host "  >> Bundling GTK DLLs..."

    $GtkExe    = Find-BuildExe "6502caEdGtk.exe"
    $BundleDir = Join-Path $DistDir "gtk"
    New-Item -ItemType Directory -Force -Path $BundleDir | Out-Null

    Copy-Item $GtkExe $BundleDir -Force

    # Preserve the project's licence and dependency notices in the bundle.
    Copy-Item (Join-Path $ScriptDir "LICENSE") $BundleDir -Force
    Copy-Item (Join-Path $ScriptDir "6502caEd\license-readme.txt") $BundleDir -Force
    Copy-Item (Join-Path $ScriptDir "THIRD_PARTY_NOTICES.txt") $BundleDir -Force
    $NoticeSource = Join-Path $MSYS2Root "ucrt64\share\licenses"
    $NoticeDest = Join-Path $BundleDir "third-party-licenses"
    if (-not (Test-Path $NoticeSource)) {
        throw "MSYS2 licence directory is missing: $NoticeSource"
    }
    if (Test-Path $NoticeDest) { Remove-Item $NoticeDest -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $NoticeDest | Out-Null
    Copy-Item (Join-Path $NoticeSource "*") $NoticeDest -Recurse -Force
    $PackageList = & $Bash "-lc" "pacman -Q"
    if ($LASTEXITCODE -ne 0) { throw "Could not record installed package versions." }
    $PackageList | Out-File (Join-Path $NoticeDest "installed-packages.txt") -Encoding utf8

    Copy-Item (Join-Path $ScriptDir "6502caEd\resources\6502caEd.ico") $BundleDir -Force

    # Copy all DLLs from ucrt64/bin - simpler and more reliable than recursive ldd
    Write-Host "    Copying DLLs from ucrt64/bin..."
    $Ucrt64Bin = Join-Path $MSYS2Root "ucrt64\bin"
    Get-ChildItem -Path $Ucrt64Bin -Filter "*.dll" | ForEach-Object {
        Copy-Item $_.FullName $BundleDir -Force
    }
    Write-Host "    DLLs copied."

    # Copy GTK runtime data
    $Ucrt64    = Join-Path $MSYS2Root "ucrt64"
    $ShareDest = Join-Path $BundleDir "share"
    $LibDest   = Join-Path $BundleDir "lib"
    New-Item -ItemType Directory -Force -Path $ShareDest | Out-Null
    New-Item -ItemType Directory -Force -Path $LibDest   | Out-Null

    Write-Host "    Copying GTK icons..."
    Copy-Item (Join-Path $Ucrt64 "share\icons") $ShareDest -Recurse -Force

    Write-Host "    Copying GLib schemas..."
    Copy-Item (Join-Path $Ucrt64 "share\glib-2.0") $ShareDest -Recurse -Force
    # Compile GLib schemas so GTK settings work
    $GlibCompile = Join-Path $Ucrt64 "bin\glib-compile-schemas.exe"
    $SchemaDir = Join-Path $ShareDest "glib-2.0\schemas"
    if ((Test-Path $GlibCompile) -and (Test-Path $SchemaDir)) {
        Write-Host "    Compiling GLib schemas..."
        & $GlibCompile $SchemaDir
    }

    Write-Host "    Copying gdk-pixbuf loaders..."
    Copy-Item (Join-Path $Ucrt64 "lib\gdk-pixbuf-2.0") $LibDest -Recurse -Force
    # Update gdk-pixbuf loader cache
    $PixbufQuery = Join-Path $Ucrt64 "bin\gdk-pixbuf-query-loaders.exe"
    $LoaderCache = Join-Path $LibDest "gdk-pixbuf-2.0\2.10.0\loaders.cache"
    if (Test-Path $PixbufQuery) {
        Write-Host "    Generating pixbuf loader cache..."
        & $PixbufQuery "--update-cache" 2>$null
        # Copy the generated cache
        $SrcCache = Join-Path $Ucrt64 "lib\gdk-pixbuf-2.0\2.10.0\loaders.cache"
        if (Test-Path $SrcCache) { Copy-Item $SrcCache $LoaderCache -Force }
    }

    if ($BUILD_CLI) {
        Copy-Item (Find-BuildExe "6502ca-cli.exe") $BundleDir -Force
    }
    if ($BUILD_TUI) {
        Copy-Item (Find-BuildExe "6502caEd.exe") $BundleDir -Force
        $PdcDll = Join-Path $MSYS2Root "ucrt64\bin\libpdcurses_wincon.dll"
        if (Test-Path $PdcDll) { Copy-Item $PdcDll $BundleDir -Force }
    }

    Write-Host "  >> Bundle complete: $BundleDir"
}

# ---------------------------------------------------------------------------
# Inno Setup installer
# ---------------------------------------------------------------------------
if ($BUILD_INSTALLER) {
    Write-Host ""
    Write-Host "  >> Building Inno Setup installer..."

    # Look for Inno Setup in common locations
    $InnoSetup = $null
    $InnoPaths = @(
        "C:\Program Files (x86)\Inno Setup 6\ISCC.exe",
        "C:\Program Files\Inno Setup 6\ISCC.exe"
    )
    foreach ($p in $InnoPaths) {
        if (Test-Path $p) { $InnoSetup = $p; break }
    }

    if ($null -eq $InnoSetup) {
        Write-Host "[!] Inno Setup not found. Install from https://jrsoftware.org/isinfo.php"
        Write-Host "    Skipping installer creation."
    } else {
        # Generate .iss script on the fly
        $IssFile = Join-Path $env:TEMP "6502caEd_setup.iss"
        $BundleDir = (Join-Path $DistDir "gtk") -replace "\\", "\"
        $InstDir   = $InstallerDir -replace "\\", "\"
        $IssContent = @"
[Setup]
AppId={{B8A2C3D4-E5F6-7890-ABCD-EF1234567890}
AppName=6502caEd
AppVersion=$VER_GTK
AppPublisher=6502caEd
SetupIconFile=$BundleDir\6502caEd.ico
UninstallDisplayIcon={app}\6502caEd.ico
DefaultDirName={autopf}\6502caEd
DefaultGroupName=6502caEd
DisableProgramGroupPage=yes
OutputDir=$InstDir
OutputBaseFilename=6502caEd-$VER_GTK-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesInstallIn64BitMode=x64compatible

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "$BundleDir\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\6502caEd";                          FileName: "{app}\6502caEdGtk.exe"; IconFilename: "{app}\6502caEd.ico"
Name: "{group}\{cm:UninstallProgram,6502caEd}";    FileName: "{uninstallexe}"; IconFilename: "{app}\6502caEd.ico"
Name: "{autodesktop}\6502caEd";                    FileName: "{app}\6502caEdGtk.exe"; IconFilename: "{app}\6502caEd.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\6502caEdGtk.exe"; Description: "{cm:LaunchProgram,6502caEd}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}"
"@
        $IssContent | Out-File -FilePath $IssFile -Encoding utf8
        New-Item -ItemType Directory -Force -Path $InstallerDir | Out-Null
        Invoke-Cmd "Running Inno Setup compiler" $InnoSetup @(
            "/O$InstDir",
            $IssFile
        )
        Remove-Item $IssFile -Force -ErrorAction SilentlyContinue
        Write-Host "  >> Installer written to: $InstallerDir"
    }
}

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "+==============================================+"
Write-Host "|                  Complete!                   |"
Write-Host "+==============================================+"
Write-Host ""
if ($BUILD_CLI) { Write-Host "  CLI : $(Find-BuildExe '6502ca-cli.exe')" }
if ($BUILD_TUI) { Write-Host "  TUI : $(Find-BuildExe '6502caEd.exe')" }
if ($BUILD_GTK) { Write-Host "  GTK : $(Find-BuildExe '6502caEdGtk.exe')" }
if ($BUNDLE_DLLS)     { Write-Host "  Bundle : $DistDir\gtk\" }
if ($BUILD_INSTALLER) { Write-Host "  Installer : $InstallerDir\" }
Write-Host ""

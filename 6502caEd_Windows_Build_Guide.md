# 6502caEd - Windows Build Guide

**MJD 2026**

---

## Before You Start

All three targets build and run on Windows:

| Target | Binary | Notes |
|---|---|---|
| CLI assembler | `6502ca-cli.exe` | Pure C, no dependencies |
| TUI editor | `6502caEd.exe` | PDCurses; run from Windows Terminal or cmd.exe |
| GTK editor | `6502caEdGtk.exe` | GTK4 via MSYS2; DLLs must be bundled to run standalone |

**Mouse support in the TUI editor:** works correctly in Windows Terminal and
cmd.exe. Does not work in MSYS2 mintty — use mintty for building only.

**Source path:** avoid special characters (!, &, %, spaces) in the path to the
source folder. Use a simple path such as C:\6502caEd or C:\a\6502caEd.

---

## Option A - Automated Build (Recommended)

The project includes a PowerShell script that handles everything automatically,
including installing MSYS2, all required packages, and Inno Setup if missing.

Open PowerShell and run:

    cd C:\6502caEd
    powershell -ExecutionPolicy Bypass -File build_Windows.ps1

The script presents a menu:

    [1]  CLI assembler only
    [2]  TUI editor only
    [3]  GTK editor only
    [4]  CLI + TUI + GTK  (all targets, no installer)
    [5]  All targets + bundle DLLs + create Inno Setup installer
    [6]  Quit

Choose option 4 for a standard build, or option 5 to produce a distributable
installer. The script installs MSYS2, packages, and Inno Setup automatically.

Version numbers are read from versions.txt in the project root.

---

## Option B - Manual Build

Follow these steps if you prefer to build manually or need to troubleshoot.

### Step 1 - Install MSYS2

1. Go to https://www.msys2.org and download the installer.
2. Run it and accept all defaults. Install to C:\msys64.
3. When the installer finishes it opens an MSYS2 UCRT64 terminal.
   Use that terminal for all steps below.

### Step 2 - Install the Build Tools

In the MSYS2 UCRT64 terminal, run these commands. Press Enter if asked to
confirm a selection.

Always required:

    pacman -S --needed mingw-w64-ucrt-x86_64-gcc
    pacman -S --needed mingw-w64-ucrt-x86_64-cmake
    pacman -S --needed mingw-w64-ucrt-x86_64-ninja

TUI editor - also required:

    pacman -S --needed mingw-w64-ucrt-x86_64-pdcurses

GTK editor - also required:

    pacman -S --needed mingw-w64-ucrt-x86_64-gtk4
    pacman -S --needed mingw-w64-ucrt-x86_64-gtksourceview5
    pacman -S --needed mingw-w64-ucrt-x86_64-pkgconf

### Step 3 - Get the Source Code

Unzip the source to a path with no special characters, for example C:\6502caEd.
The layout must be:

    C:\6502caEd\                    <- root (contains build_Windows.ps1)
        CMakeLists.txt              <- root CMakeLists
        CMakePresets.json
        versions.txt
        build_Windows.ps1
        6502caEd\                   <- inner folder (contains second CMakeLists)
            CMakeLists.txt          <- inner CMakeLists (defines targets)
            6502caEd\               <- actual source files
                core\
                cli\
                editor\
                gui\

Note the three levels of 6502caEd nesting. The two CMakeLists.txt files are
at different levels and are not the same file.

### Step 4 - Add MSYS2 to PATH

The build_Windows.ps1 script handles this automatically. For manual builds
from PowerShell, add MSYS2 to your session PATH first:

    $env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"

This ensures cmake can find gcc and its runtime DLLs.

### Step 5 - Configure

Run cmake with -S pointing at the root source folder and -B pointing at the
build output folder:

    cmake `
      -S C:\6502caEd `
      -B C:\6502caEd\out\build\win-mingw64-release `
      -G Ninja `
      -DCMAKE_BUILD_TYPE=Release `
      -DCMAKE_SYSTEM_NAME=Windows `
      -DCMAKE_C_COMPILER=C:\msys64\ucrt64\bin\x86_64-w64-mingw32-gcc.exe `
      -DCMAKE_MAKE_PROGRAM=C:\msys64\ucrt64\bin\ninja.exe `
      -DED_BUILD_TUI=ON `
      -DED_BUILD_GTK=ON

You will see a warning about CMAKE_CXX_COMPILER - this is harmless.

### Step 6 - Build the Targets

Replace BUILD_DIR with C:\6502caEd\out\build\win-mingw64-release throughout.

CLI assembler:

    cmake --build BUILD_DIR --target 6502ca-cli

TUI editor:

    cmake --build BUILD_DIR --target 6502caEd

GTK editor:

    cmake --build BUILD_DIR --target 6502caEdGtk

Output binaries are in BUILD_DIR\6502caEd\:

    6502ca-cli.exe
    6502caEd.exe
    6502caEdGtk.exe

### Step 7 - Test

CLI:

    BUILD_DIR\6502caEd\6502ca-cli.exe --help

TUI (run from Windows Terminal or cmd.exe for mouse support):

    BUILD_DIR\6502caEd\6502caEd.exe

GTK (from the MSYS2 terminal so DLLs are on PATH):

    BUILD_DIR\6502caEd\6502caEdGtk.exe

---

## Versions

Version numbers for all three targets are stored in versions.txt:

    # 6502caEd version numbers
    cli = 1.00
    tui = 1.00
    gtk = 1.00

Edit this file before building. Blank lines and lines starting with # are
ignored. All whitespace is stripped before parsing. Both CMake and the build
scripts read this file automatically.

---

## Appendix A - Bundling DLLs for the GTK Editor

The GTK editor depends on DLLs in MSYS2. To run it from Explorer or distribute
it, copy all DLLs from ucrt64\bin alongside the exe:

From PowerShell:

    $dest = "C:\6502caEd\out\dist\gtk"
    New-Item -ItemType Directory -Force -Path $dest
    Copy-Item BUILD_DIR\6502caEd\6502caEdGtk.exe $dest
    Copy-Item BUILD_DIR\6502caEd\6502ca-cli.exe $dest
    Copy-Item BUILD_DIR\6502caEd\6502caEd.exe $dest
    Get-ChildItem C:\msys64\ucrt64\bin\*.dll | Copy-Item -Destination $dest
    Copy-Item C:\msys64\ucrt64\share\icons "$dest\share" -Recurse
    Copy-Item C:\msys64\ucrt64\share\glib-2.0 "$dest\share" -Recurse
    Copy-Item C:\msys64\ucrt64\lib\gdk-pixbuf-2.0 "$dest\lib" -Recurse

The dist\gtk\ folder is then self-contained and can be moved or distributed.
The build_Windows.ps1 script option 5 does this automatically.

---

## Appendix B - Creating an Installer with Inno Setup

### Install Inno Setup

Option 1 - winget (recommended, Windows 10 1709+):

    winget install -e --id JRSoftware.InnoSetup

Option 2 - download from https://jrsoftware.org/isdl.php and run the installer.

### Generate the Installer

The build_Windows.ps1 script option 5 generates the Inno Setup script (.iss)
automatically and runs the compiler. No separate .iss file is needed.

To run the Inno Setup compiler manually, create a file called 6502caEd.iss with
the following content (adjust paths as needed):

    [Setup]
    AppName=6502caEd
    AppVersion=1.00
    AppPublisher=MJD
    DefaultDirName={autopf}\6502caEd
    DefaultGroupName=6502caEd
    OutputDir=C:\6502caEd\installers
    OutputBaseFilename=6502caEd-1.00-win64-setup
    Compression=lzma2
    SolidCompression=yes
    ArchitecturesInstallIn64BitMode=x64compatible
    PrivilegesRequired=lowest
    UninstallDisplayIcon={app}\6502caEdGtk.exe

    [Types]
    Name: full;    Description: All components
    Name: gtk;     Description: GTK editor only
    Name: tui;     Description: TUI editor only
    Name: cli;     Description: CLI assembler only
    Name: custom;  Description: Custom; Flags: iscustom

    [Components]
    Name: gtk; Description: GTK editor (6502caEdGtk.exe); Types: full gtk custom
    Name: tui; Description: TUI editor (6502caEd.exe);    Types: full tui custom
    Name: cli; Description: CLI assembler (6502ca-cli.exe); Types: full cli custom

    [Files]
    Source: dist\gtk\6502caEdGtk.exe; DestDir: {app}; Components: gtk; Flags: ignoreversion
    Source: dist\gtk\*.dll;           DestDir: {app}; Components: gtk; Flags: ignoreversion recursesubdirs
    Source: dist\gtk\lib\*;           DestDir: {app}\lib; Components: gtk; Flags: ignoreversion recursesubdirs
    Source: dist\gtk\share\*;         DestDir: {app}\share; Components: gtk; Flags: ignoreversion recursesubdirs
    Source: dist\gtk\6502caEd.exe;    DestDir: {app}; Components: tui; Flags: ignoreversion
    Source: dist\gtk\6502ca-cli.exe;  DestDir: {app}; Components: cli; Flags: ignoreversion

    [Icons]
    Name: {group}\6502caEd (GTK);    Filename: {app}\6502caEdGtk.exe;  Components: gtk
    Name: {group}\6502caEd (TUI);    Filename: {app}\6502caEd.exe;     Components: tui
    Name: {group}\6502ca Assembler;  Filename: {app}\6502ca-cli.exe;   Components: cli
    Name: {group}\Uninstall 6502caEd; Filename: {uninstallexe}

    [Run]
    Filename: {app}\6502caEdGtk.exe; Description: Launch GTK editor; Flags: nowait postinstall skipifsilent; Components: gtk

    [Code]
    function IsWine: Boolean;
    begin
      Result := GetProcAddress(GetModuleHandle('ntdll.dll'), 'wine_get_version') <> 0;
    end;

    function InitializeSetup: Boolean;
    begin
      if IsWine then begin
        MsgBox('This installer is for Windows only.' + #13#10 +
               'A native Linux version of 6502caEd is available - please use that instead.',
               mbCriticalError, MB_OK);
        Result := False;
      end else
        Result := True;
    end;

Then compile it:

    "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" 6502caEd.iss

The installer will be written to the OutputDir folder.

### What the Installer Does

- Offers component selection: GTK editor, TUI editor, CLI assembler, or any combination
- Installs to C:\Program Files\6502caEd by default (no UAC required - uses lowest privilege)
- Creates Start Menu shortcuts for each selected component
- Optionally launches the GTK editor after install
- Detects if running under Wine and refuses to install, directing users to the Linux version

---

## Appendix C - PDCurses Technical Notes

The TUI editor uses PDCurses on Windows instead of ncurses. The MSYS2 UCRT64
build uses wide character / UTF-8 mode (PDC_WIDE + PDC_FORCE_UTF8), which
shifts colour pair values to 44-bit positions in a 64-bit chtype. The source
code uses uint64_t and chtype throughout to handle this correctly.

The correct library is libpdcurses_wincon.a (Windows Console variant).
The other variants (wingui, vt, sdl) will not work with this project.

Mouse clicks work in Windows Terminal and cmd.exe. They do not work in mintty
because mintty uses a PTY rather than a true Windows Console, so PDCurses
cannot receive mouse events from it.

---

End of guide

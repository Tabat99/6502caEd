#!/usr/bin/env bash

#  6502caEd Installer Builder
#  Part of 6502caEd cross compiler suite
#  MJD 2026

# Installer.sh — build and package 6502caEd
#
# Options:
#    [1]  GTK editor   — AppImage  (graphical, no install needed)"
#    [2]  TUI editor   — .deb      (terminal, installs system-wide)"
#    [3]  GTK + TUI    — AppImage + .deb"
#    [4]  GTK editor   — .deb      (graphical, installs system-wide)"
#    [5]  GTK + TUI    — .deb + .deb"
#    [6]  CLI assembler — .deb     (command-line only)"
#    [7]  All          — AppImage + .deb (GTK) + .deb (TUI) + .deb (CLI)  [default]"
#    [8]  Quit"
#
# Usage: ./Installer.sh [--yes] [--version X.Y.Z]

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALLER_DIR="${SRC_DIR}/installers"
BUILD_DIR="${SRC_DIR}/out/build/linux-gcc-release-installer"
LOG="${INSTALLER_DIR}/installer-build.log"
AUTO_YES=0
CHOICE=""

# ---------------------------------------------------------------------------
# Read versions from versions.txt
# Rules: blank lines ignored, # comments ignored, all whitespace/CR stripped
# ---------------------------------------------------------------------------
read_ver() {
    local key="$1"
    local file="${SRC_DIR}/versions.txt"
    if [[ ! -f "$file" ]]; then echo "0.00"; return; fi
    while IFS= read -r line; do
        # Strip all spaces, tabs, carriage returns
        line="${line//[$' \t\r']/}"
        # Skip blank lines and comments
        [[ -z "$line" || "$line" == \#* ]] && continue
        # Match key=value
        if [[ "$line" == "${key}="* ]]; then
            echo "${line#*=}"
            return
        fi
    done < "$file"
    echo "0.00"
}

VER_CLI="$(read_ver cli)"
VER_TUI="$(read_ver tui)"
VER_GTK="$(read_ver gtk)"
# Use TUI version as the overall package version (or override with --version)
VERSION="${VER_TUI}"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --version) shift; VERSION="$1"; VER_CLI="$1"; VER_TUI="$1"; VER_GTK="$1" ;;
        --yes|-y)  AUTO_YES=1 ;;
        --choice)  shift; CHOICE="$1" ;;
        --help|-h)
            echo "Usage: $0 [--version X.Y.Z] [--yes] [--choice 1|2|3|4|5|6|7|8]"
            exit 0
            ;;
        *) echo "[!] Unknown argument: $1"; exit 1 ;;
    esac
    shift
done

# ---------------------------------------------------------------------------
# Banner
# ---------------------------------------------------------------------------
echo ""
echo "╔══════════════════════════════════════════════╗"
echo "║              6502caEd Installer              ║"
echo "╚══════════════════════════════════════════════╝"
echo ""
echo "  What would you like to build/install?"
echo ""
echo "    [1]  GTK editor    — AppImage  (graphical, no install needed)"
echo "    [2]  TUI editor    — .deb      (terminal, installs system-wide)"
echo "    [3]  GTK + TUI     — AppImage + .deb"
echo "    [4]  GTK editor    — .deb      (graphical, installs system-wide)"
echo "    [5]  GTK + TUI     — .deb + .deb"
echo "    [6]  CLI assembler — .deb     (command-line only)"
echo "    [7]  All           — GTK (AppImage + .deb) + TUI (.deb) + CLI (.deb)  [default]"
echo "    [8]  Quit"
echo ""

if [[ -z "${CHOICE}" ]]; then
    read -rp "  Your choice [1-8] (default=7): " CHOICE
    CHOICE="${CHOICE:-7}"
fi

case "${CHOICE}" in
    1) BUILD_GTK=1; BUILD_TUI=0; BUILD_GTK_DEB=0; BUILD_CLI_DEB=0 ;;
    2) BUILD_GTK=0; BUILD_TUI=1; BUILD_GTK_DEB=0; BUILD_CLI_DEB=0 ;;
    3) BUILD_GTK=1; BUILD_TUI=1; BUILD_GTK_DEB=0; BUILD_CLI_DEB=0 ;;
    4) BUILD_GTK=0; BUILD_TUI=0; BUILD_GTK_DEB=1; BUILD_CLI_DEB=0 ;;
    5) BUILD_GTK=0; BUILD_TUI=1; BUILD_GTK_DEB=1; BUILD_CLI_DEB=0 ;;
    6) BUILD_GTK=0; BUILD_TUI=0; BUILD_GTK_DEB=0; BUILD_CLI_DEB=1 ;;
    7) BUILD_GTK=1; BUILD_TUI=1; BUILD_GTK_DEB=1; BUILD_CLI_DEB=1 ;;
    8) echo "  Bye."; exit 0 ;;
    *) echo "[!] Invalid choice."; exit 1 ;;
esac

echo ""
echo "  ┌─────────────────────────────────────────┐"
echo "  │  Build plan                             │"
echo "  ├─────────────────────────────────────────┤"
[[ ${BUILD_GTK} -eq 1 ]] && \
printf  "  │  GTK  : %-31s│\n" "AppImage → installer/"
[[ ${BUILD_GTK_DEB} -eq 1 ]] && \
printf  "  │  GTK  : %-31s│\n" ".deb     → installer/"
[[ ${BUILD_TUI} -eq 1 ]] && \
printf  "  │  TUI  : %-31s│\n" ".deb     → installer/"
[[ ${BUILD_CLI_DEB} -eq 1 ]] && \
printf  "  │  CLI  : %-31s│\n" ".deb     → installer/"
printf  "  │  Ver  : %-31s│\n" "${VERSION}"
echo "  └─────────────────────────────────────────┘"
echo ""

if [[ ${AUTO_YES} -eq 0 ]]; then
    read -rp "  Proceed? [Y/n]: " _c
    case "${_c}" in [nN]*) echo "  Aborted."; exit 0 ;; esac
fi

echo ""
echo "Calling help builder script"
./build_help.sh

echo ""

# ---------------------------------------------------------------------------
# Setup
# ---------------------------------------------------------------------------
PKG_NAME="6502caEd"
PKG_ARCH="amd64"
DEB_NAME="${PKG_NAME}_${VERSION}_${PKG_ARCH}.deb"

mkdir -p "${INSTALLER_DIR}"
: > "${LOG}"

log()  { echo -e "[+] $*" | tee -a "${LOG}"; }
warn() { echo -e "[!] $*" | tee -a "${LOG}" >&2; }
die()  { echo -e "[X] $*" | tee -a "${LOG}" >&2; exit 1; }
trap 'die "FAILED — see: ${LOG}"' ERR

log "Version : ${VERSION}"
log "GTK     : $([ ${BUILD_GTK} -eq 1 ] && echo yes || echo no)"
log "TUI     : $([ ${BUILD_TUI} -eq 1 ] && echo yes || echo no)"

# ---------------------------------------------------------------------------
# Install build dependencies
# ---------------------------------------------------------------------------
MISSING_PKGS=()
need_pkg() { dpkg -s "$1" &>/dev/null || MISSING_PKGS+=("$1"); }

need_pkg build-essential
need_pkg cmake
need_pkg ninja-build
need_pkg pkg-config
need_pkg libncurses-dev

[[ ${BUILD_TUI} -eq 1 ]] && {
    need_pkg dpkg-dev
    need_pkg fakeroot
    need_pkg pandoc
}

[[ ${BUILD_GTK} -eq 1 ]] && {
    need_pkg libgtk-4-dev
    need_pkg libgtksourceview-5-dev
    need_pkg curl
    need_pkg patchelf
    need_pkg librsvg2-bin
    need_pkg desktop-file-utils
    need_pkg pandoc
}

if [[ ${#MISSING_PKGS[@]} -gt 0 ]]; then
    mapfile -t MISSING_PKGS < <(printf "%s\n" "${MISSING_PKGS[@]}" | sort -u)
    log "Installing missing deps: ${MISSING_PKGS[*]}"
    sudo apt-get update -q 2>&1 | tee -a "${LOG}"
    sudo apt-get install -y "${MISSING_PKGS[@]}" 2>&1 | tee -a "${LOG}"
fi

# ---------------------------------------------------------------------------
# Compile (both TUI and GTK share one build)
# ---------------------------------------------------------------------------
log "Configuring..."
rm -rf "${BUILD_DIR}" && mkdir -p "${BUILD_DIR}"

cmake -S "${SRC_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr \
    "-DED_BUILD_GTK=$([ ${BUILD_GTK} -eq 1 ] || [ ${BUILD_GTK_DEB} -eq 1 ] && echo ON || echo OFF)" \
    "-DED_BUILD_SIM=$([ ${BUILD_GTK} -eq 1 ] || [ ${BUILD_GTK_DEB} -eq 1 ] && echo ON || echo OFF)" \
    2>&1 | tee -a "${LOG}"

log "Compiling..."
cmake --build "${BUILD_DIR}" 2>&1 | tee -a "${LOG}"

# ---------------------------------------------------------------------------
# Find binaries
# ---------------------------------------------------------------------------
find_exe() {
    find "${BUILD_DIR}" -maxdepth 8 -type f -name "$1" -perm -111 \
        2>/dev/null | head -1 || true
}

TUI_BIN="$(find_exe 6502caEd)"
CLI_BIN="$(find_exe 6502ca-cli)"
GTK_BIN="$(find_exe 6502caEdGtk)"

[[ ${BUILD_TUI} -eq 1 ]] && { [[ -n "${TUI_BIN}" ]] || die "TUI binary not found"; log "TUI: ${TUI_BIN}"; }
[[ ${BUILD_GTK} -eq 1 || ${BUILD_GTK_DEB} -eq 1 ]] && { [[ -n "${GTK_BIN}" ]] || die "GTK binary not found"; log "GTK: ${GTK_BIN}"; }
[[ ${BUILD_CLI_DEB} -eq 1 ]] && { [[ -n "${CLI_BIN}" ]] || die "CLI binary not found"; log "CLI: ${CLI_BIN}"; }
[[ -n "${CLI_BIN}" ]] && log "CLI: ${CLI_BIN}"

# ===========================================================================
# TUI — build .deb
# ===========================================================================
if [[ ${BUILD_TUI} -eq 1 ]]; then
    log "--- Building TUI .deb ---"

    PKGROOT="${INSTALLER_DIR}/pkgroot"
    rm -rf "${PKGROOT}"

    # Binaries
    mkdir -p "${PKGROOT}/usr/bin"
    cp "${TUI_BIN}" "${PKGROOT}/usr/bin/6502caEd" && chmod 755 "${PKGROOT}/usr/bin/6502caEd"
    [[ -n "${CLI_BIN}" ]] && {
        cp "${CLI_BIN}" "${PKGROOT}/usr/bin/6502ca-cli" && chmod 755 "${PKGROOT}/usr/bin/6502ca-cli"
    }

    # Config / theme
    mkdir -p "${PKGROOT}/etc/6502caEd"
    for _f in 6502caEd.colours 6502caEd.theme; do
        [[ -f "${SRC_DIR}/${_f}" ]] && cp "${SRC_DIR}/${_f}" "${PKGROOT}/etc/6502caEd/"
    done

    # Docs
    mkdir -p "${PKGROOT}/usr/share/doc/6502caEd"
    [[ -f "${SRC_DIR}/ReadMe.txt" ]] && \
        cp "${SRC_DIR}/ReadMe.txt" "${PKGROOT}/usr/share/doc/6502caEd/README.txt"

    # License
    mkdir -p "${PKGROOT}/usr/share/6502caEd"
    [[ -f "${SRC_DIR}/6502caEd/license-readme.txt" ]] && \
        cp "${SRC_DIR}/6502caEd/license-readme.txt" "${PKGROOT}/usr/share/6502caEd/license-readme.txt"
    cp "${SRC_DIR}/THIRD_PARTY_NOTICES.txt" "${PKGROOT}/usr/share/6502caEd/THIRD_PARTY_NOTICES.txt"
    mkdir -p "${PKGROOT}/usr/share/doc/6502caEd"
    cp "${SRC_DIR}/LICENSE" "${PKGROOT}/usr/share/doc/6502caEd/copyright"

    # Help file
    mkdir -p "${PKGROOT}/usr/share/6502caEd/help"
    if [[ -f "${SRC_DIR}/help/help.md" ]]; then
        if command -v pandoc &>/dev/null; then
            pandoc "${SRC_DIR}/help/help.md" \
                --standalone \
                --embed-resources \
                --css "${SRC_DIR}/help/help.css" \
                -o "${PKGROOT}/usr/share/6502caEd/help/help.html" \
                2>&1 | tee -a "${LOG}"
            log "Help HTML generated for .deb"
        else
            warn "pandoc not found — help HTML not included in .deb"
        fi
        cp "${SRC_DIR}/help/help.md" "${PKGROOT}/usr/share/6502caEd/help/"
        [[ -f "${SRC_DIR}/help/help.css" ]] && \
            cp "${SRC_DIR}/help/help.css" "${PKGROOT}/usr/share/6502caEd/help/"
    else
        warn "help/help.md not found — skipping help install in .deb"
    fi

    # Icon
    mkdir -p "${PKGROOT}/usr/share/icons/hicolor/scalable/apps"
    mkdir -p "${PKGROOT}/usr/share/icons/hicolor/256x256/apps"
    _SVG="${PKGROOT}/usr/share/icons/hicolor/scalable/apps/6502caEd.svg"
    cat > "${_SVG}" << 'SVG'
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect x="16" y="16" width="224" height="224" rx="28" fill="#2e2e2e"/>
  <text x="128" y="108" text-anchor="middle" font-family="monospace" font-size="48" font-weight="700" fill="#4fc3f7">6502ca</text>
  <text x="128" y="162" text-anchor="middle" font-family="monospace" font-size="38" font-weight="500" fill="#ffffff">Editor</text>
</svg>
SVG
    if command -v rsvg-convert &>/dev/null; then
        rsvg-convert -w 256 -h 256 \n            -o "${PKGROOT}/usr/share/icons/hicolor/256x256/apps/6502caEd.png" \n            "${_SVG}" && log "TUI icon PNG generated"
    else
        warn "rsvg-convert not found — SVG icon only"
    fi

    # Desktop entry (application menu only — no desktop shortcut)
    mkdir -p "${PKGROOT}/usr/share/applications"
    cat > "${PKGROOT}/usr/share/applications/6502caEd.desktop" << 'DESK'
[Desktop Entry]
Version=1.0
Type=Application
Name=6502caEd
Comment=6502 Assembler Editor (TUI)
Exec=bash -c '/usr/bin/6502caEd %F; exec bash'
Terminal=true
Icon=6502caEd
Categories=Development;
MimeType=text/x-asm;text/plain;
StartupNotify=false
DESK

    # DEBIAN/control
    mkdir -p "${PKGROOT}/DEBIAN"
    INSTALLED_KB=$(du -sk "${PKGROOT}" | awk '{print $1}')

    cat > "${PKGROOT}/DEBIAN/control" << CTRL
Package: ${PKG_NAME}
Version: ${VERSION}
Architecture: ${PKG_ARCH}
Maintainer: MJD <mjd@example.com>
Installed-Size: ${INSTALLED_KB}
Depends: libc6, libncursesw6 | libncurses6
Section: devel
Priority: optional
Description: 6502 Assembler Editor (TUI)
 A 6502/65C02 assembler and editor for the terminal.
CTRL

    cat > "${PKGROOT}/DEBIAN/postinst" << 'POST'
#!/bin/sh
set -e
update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \n    gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
POST
    chmod 755 "${PKGROOT}/DEBIAN/postinst"

    cat > "${PKGROOT}/DEBIAN/postrm" << 'POST'
#!/bin/sh
set -e
update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \n    gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
POST
    chmod 755 "${PKGROOT}/DEBIAN/postrm"

    log "Building .deb..."
    fakeroot dpkg-deb --build "${PKGROOT}" "${INSTALLER_DIR}/${DEB_NAME}" \
        2>&1 | tee -a "${LOG}"
    rm -rf "${PKGROOT}"

    [[ -f "${INSTALLER_DIR}/${DEB_NAME}" ]] || die ".deb not produced"
    log "TUI .deb ready: installer/${DEB_NAME}"
fi

# ===========================================================================
# GTK — build .deb
# ===========================================================================
if [[ ${BUILD_GTK_DEB} -eq 1 ]]; then
    log "--- Building GTK .deb ---"

    GTK_DEB_NAME="${PKG_NAME}-gtk_${VERSION}_${PKG_ARCH}.deb"
    PKGROOT="${INSTALLER_DIR}/pkgroot-gtk"
    rm -rf "${PKGROOT}"

    # Binary
    mkdir -p "${PKGROOT}/usr/bin"
    cp "${GTK_BIN}" "${PKGROOT}/usr/bin/6502caEdGtk" && chmod 755 "${PKGROOT}/usr/bin/6502caEdGtk"

    # Icons
    mkdir -p "${PKGROOT}/usr/share/icons/hicolor/scalable/apps"
    mkdir -p "${PKGROOT}/usr/share/icons/hicolor/256x256/apps"
    # Use 6502caEdGtk as icon name to avoid conflict with TUI .deb
    _SVG="${PKGROOT}/usr/share/icons/hicolor/scalable/apps/6502caEdGtk.svg"
    cat > "${_SVG}" << 'SVG'
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect x="16" y="16" width="224" height="224" rx="28" fill="#2e2e2e"/>
  <text x="128" y="108" text-anchor="middle" font-family="monospace" font-size="48" font-weight="700" fill="#4fc3f7">6502ca</text>
  <text x="128" y="162" text-anchor="middle" font-family="monospace" font-size="38" font-weight="500" fill="#ffffff">Editor</text>
</svg>
SVG
    if command -v rsvg-convert &>/dev/null; then
        rsvg-convert -w 256 -h 256 \
            -o "${PKGROOT}/usr/share/icons/hicolor/256x256/apps/6502caEdGtk.png" \
            "${_SVG}" && log "GTK .deb icon PNG generated"
    else
        warn "rsvg-convert not found — SVG icon only in GTK .deb"
    fi

    # Desktop entry
    mkdir -p "${PKGROOT}/usr/share/applications"
    cat > "${PKGROOT}/usr/share/applications/6502caEdGtk.desktop" << 'DESK'
[Desktop Entry]
Version=1.0
Type=Application
Name=6502caEd (GTK)
Comment=6502 Assembler Editor (GTK)
Exec=6502caEdGtk %F
Terminal=false
Icon=6502caEdGtk
Categories=Development;
MimeType=text/x-asm;text/plain;
StartupNotify=true
DESK

    # License
    mkdir -p "${PKGROOT}/usr/share/6502caEd"
    [[ -f "${SRC_DIR}/6502caEd/license-readme.txt" ]] && \
        cp "${SRC_DIR}/6502caEd/license-readme.txt" "${PKGROOT}/usr/share/6502caEd/license-readme.txt"
    cp "${SRC_DIR}/THIRD_PARTY_NOTICES.txt" "${PKGROOT}/usr/share/6502caEd/THIRD_PARTY_NOTICES.txt"
    mkdir -p "${PKGROOT}/usr/share/doc/6502caEd"
    cp "${SRC_DIR}/LICENSE" "${PKGROOT}/usr/share/doc/6502caEd/copyright"

    # Docs
    mkdir -p "${PKGROOT}/usr/share/doc/6502caEd-gtk"
    [[ -f "${SRC_DIR}/ReadMe.txt" ]] && \
        cp "${SRC_DIR}/ReadMe.txt" "${PKGROOT}/usr/share/doc/6502caEd-gtk/README.txt"

    # DEBIAN/control
    mkdir -p "${PKGROOT}/DEBIAN"
    INSTALLED_KB=$(du -sk "${PKGROOT}" | awk '{print $1}')
    cat > "${PKGROOT}/DEBIAN/control" << CTRL
Package: ${PKG_NAME}-gtk
Version: ${VERSION}
Architecture: ${PKG_ARCH}
Maintainer: MJD <mjd@example.com>
Installed-Size: ${INSTALLED_KB}
Depends: libc6, libgtk-4-1, libgtksourceview-5-0
Recommends: 6502caEd
Section: devel
Priority: optional
Description: 6502 Assembler Editor (GTK)
 A 6502/65C02 assembler and editor with a graphical GTK4 interface.
CTRL

    cat > "${PKGROOT}/DEBIAN/postinst" << 'POST'
#!/bin/sh
set -e
update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
POST
    chmod 755 "${PKGROOT}/DEBIAN/postinst"

    cat > "${PKGROOT}/DEBIAN/postrm" << 'POST'
#!/bin/sh
set -e
update-desktop-database -q /usr/share/applications || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && \
    gtk-update-icon-cache -q -t /usr/share/icons/hicolor || true
POST
    chmod 755 "${PKGROOT}/DEBIAN/postrm"

    log "Building GTK .deb..."
    fakeroot dpkg-deb --build "${PKGROOT}" "${INSTALLER_DIR}/${GTK_DEB_NAME}" \
        2>&1 | tee -a "${LOG}"
    rm -rf "${PKGROOT}"

    [[ -f "${INSTALLER_DIR}/${GTK_DEB_NAME}" ]] || die "GTK .deb not produced"
    log "GTK .deb ready: installer/${GTK_DEB_NAME}"
fi

# ===========================================================================
# CLI — build .deb
# ===========================================================================
if [[ ${BUILD_CLI_DEB} -eq 1 ]]; then
    log "--- Building CLI .deb ---"

    CLI_DEB_NAME="${PKG_NAME}-cli_${VERSION}_${PKG_ARCH}.deb"
    PKGROOT="${INSTALLER_DIR}/pkgroot-cli"
    rm -rf "${PKGROOT}"

    # Binary
    mkdir -p "${PKGROOT}/usr/bin"
    cp "${CLI_BIN}" "${PKGROOT}/usr/bin/6502ca-cli" && chmod 755 "${PKGROOT}/usr/bin/6502ca-cli"

    # License
    mkdir -p "${PKGROOT}/usr/share/6502caEd"
    [[ -f "${SRC_DIR}/6502caEd/license-readme.txt" ]] && \
        cp "${SRC_DIR}/6502caEd/license-readme.txt" "${PKGROOT}/usr/share/6502caEd/license-readme.txt"
    cp "${SRC_DIR}/THIRD_PARTY_NOTICES.txt" "${PKGROOT}/usr/share/6502caEd/THIRD_PARTY_NOTICES.txt"
    mkdir -p "${PKGROOT}/usr/share/doc/6502caEd"
    cp "${SRC_DIR}/LICENSE" "${PKGROOT}/usr/share/doc/6502caEd/copyright"

    # Docs
    mkdir -p "${PKGROOT}/usr/share/doc/6502caEd-cli"
    [[ -f "${SRC_DIR}/ReadMe.txt" ]] && \
        cp "${SRC_DIR}/ReadMe.txt" "${PKGROOT}/usr/share/doc/6502caEd-cli/README.txt"

    # Man page stub
    mkdir -p "${PKGROOT}/usr/share/man/man1"
    gzip -c - > "${PKGROOT}/usr/share/man/man1/6502ca-cli.1.gz" << 'MAN'
.TH 6502CA-CLI 1 "" "6502caEd" "User Commands"
.SH NAME
6502ca-cli \- 6502/65C02/65816 command-line assembler
.SH SYNOPSIS
.B 6502ca-cli
[IOPTIONSR] Isource.asmR
.SH DESCRIPTION
Command-line assembler for 6502, 65C02 and 65816 processors.
.SH OPTIONS
Run B6502ca-cli --helpR for full option list.
MAN

    # DEBIAN/control
    mkdir -p "${PKGROOT}/DEBIAN"
    INSTALLED_KB=$(du -sk "${PKGROOT}" | awk '{print $1}')
    cat > "${PKGROOT}/DEBIAN/control" << CTRL
Package: ${PKG_NAME}-cli
Version: ${VERSION}
Architecture: ${PKG_ARCH}
Maintainer: MJD <mjd@example.com>
Installed-Size: ${INSTALLED_KB}
Depends: libc6
Section: devel
Priority: optional
Description: 6502 Assembler CLI
 Command-line assembler for 6502, 65C02 and 65816 processors.
CTRL

    log "Building CLI .deb..."
    fakeroot dpkg-deb --build "${PKGROOT}" "${INSTALLER_DIR}/${CLI_DEB_NAME}" \
        2>&1 | tee -a "${LOG}"
    rm -rf "${PKGROOT}"

    [[ -f "${INSTALLER_DIR}/${CLI_DEB_NAME}" ]] || die "CLI .deb not produced"
    log "CLI .deb ready: installer/${CLI_DEB_NAME}"
fi

# ===========================================================================
# GTK — build AppImage (delegate to AppBuilder.sh logic)
# ===========================================================================
if [[ ${BUILD_GTK} -eq 1 ]]; then
    log "--- Building GTK AppImage ---"

    APPIMAGE_DIR="${SRC_DIR}/AppImage"
    APPDIR="${APPIMAGE_DIR}/6502caEdGtk.AppDir"
    APPIMAGE_OUT="${APPIMAGE_DIR}/6502caEdGtk-x86_64.AppImage"

    mkdir -p "${APPIMAGE_DIR}"

    # Stage AppDir
    rm -rf "${APPDIR}"
    mkdir -p "${APPDIR}/usr/bin"
    cp "${GTK_BIN}" "${APPDIR}/usr/bin/6502caEdGtk"
    chmod 755 "${APPDIR}/usr/bin/6502caEdGtk"

    mkdir -p "${APPDIR}/usr/share/applications"
    cat > "${APPDIR}/usr/share/applications/6502caEd.desktop" << 'DESK'
[Desktop Entry]
Type=Application
Name=6502ca Editor
Comment=6502/65C02 assembler editor (GTK)
Exec=6502caEdGtk
Icon=6502caEd
Categories=Development;
Terminal=false
StartupNotify=true
DESK

    mkdir -p "${APPDIR}/usr/share/icons/hicolor/scalable/apps"
    mkdir -p "${APPDIR}/usr/share/icons/hicolor/256x256/apps"

    SVG_ICON="${APPDIR}/usr/share/icons/hicolor/scalable/apps/6502caEd.svg"
    cat > "${SVG_ICON}" << 'SVG'
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <rect x="16" y="16" width="224" height="224" rx="28" fill="#2e2e2e"/>
  <text x="128" y="108" text-anchor="middle" font-family="monospace" font-size="48" font-weight="700" fill="#4fc3f7">6502ca</text>
  <text x="128" y="162" text-anchor="middle" font-family="monospace" font-size="38" font-weight="500" fill="#ffffff">Editor</text>
</svg>
SVG

    PNG_ICON="${APPDIR}/usr/share/icons/hicolor/256x256/apps/6502caEd.png"
    rsvg-convert -w 256 -h 256 -o "${PNG_ICON}" "${SVG_ICON}"

    cp "${PNG_ICON}" "${APPDIR}/.DirIcon"
    ln -sf "usr/bin/6502caEdGtk"                          "${APPDIR}/AppRun"
    ln -sf "usr/share/applications/6502caEd.desktop"      "${APPDIR}/6502caEd.desktop"
    ln -sf "usr/share/icons/hicolor/256x256/apps/6502caEd.png" "${APPDIR}/6502caEd.png"

    desktop-file-validate "${APPDIR}/usr/share/applications/6502caEd.desktop" || true

    # License
    mkdir -p "${APPDIR}/usr/share/6502caEd"
    [[ -f "${SRC_DIR}/6502caEd/license-readme.txt" ]] && \
        cp "${SRC_DIR}/6502caEd/license-readme.txt" "${APPDIR}/usr/share/6502caEd/license-readme.txt"
    cp "${SRC_DIR}/THIRD_PARTY_NOTICES.txt" "${APPDIR}/usr/share/6502caEd/THIRD_PARTY_NOTICES.txt"

    # Help file
    mkdir -p "${APPDIR}/usr/share/6502caEd/help"
    if [[ -f "${SRC_DIR}/help/help.md" ]]; then
        if command -v pandoc &>/dev/null; then
            pandoc "${SRC_DIR}/help/help.md" \
                --standalone \
                --embed-resources \
                --css "${SRC_DIR}/help/help.css" \
                -o "${APPDIR}/usr/share/6502caEd/help/help.html" \
                2>&1 | tee -a "${LOG}"
            log "Help HTML generated for AppImage"
        else
            warn "pandoc not found — help not included"
        fi
        cp "${SRC_DIR}/help/help.md" "${APPDIR}/usr/share/6502caEd/help/"
        [[ -f "${SRC_DIR}/help/help.css" ]] && \
            cp "${SRC_DIR}/help/help.css" "${APPDIR}/usr/share/6502caEd/help/"
    fi

    # Download + extract linuxdeploy
    LINUXDEPLOY="${APPIMAGE_DIR}/linuxdeploy-x86_64.AppImage"
    LINUXDEPLOY_EXTRACT="${APPIMAGE_DIR}/linuxdeploy-extract"

    if [[ ! -f "${LINUXDEPLOY}" ]] || \
       [[ $(stat -c%s "${LINUXDEPLOY}" 2>/dev/null || echo 0) -lt 1048576 ]]; then
        log "Downloading linuxdeploy..."
        curl -fL --retry 3 -o "${LINUXDEPLOY}" \
            https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
            2>&1 | tee -a "${LOG}"
        chmod +x "${LINUXDEPLOY}"
    fi

    log "Extracting linuxdeploy..."
    cd "${APPIMAGE_DIR}"
    rm -rf "${LINUXDEPLOY_EXTRACT}" squashfs-root
    APPIMAGE_EXTRACT_AND_RUN=1 "${LINUXDEPLOY}" --appimage-extract \
        2>&1 | tee -a "${LOG}"
    mv squashfs-root "${LINUXDEPLOY_EXTRACT}"

    log "Running linuxdeploy..."
    export OUTPUT="${APPIMAGE_OUT}"
    export APPIMAGE_EXTRACT_AND_RUN=1
    "${LINUXDEPLOY_EXTRACT}/AppRun" \
        --appdir "${APPDIR}" \
        --desktop-file "${APPDIR}/usr/share/applications/6502caEd.desktop" \
        --icon-file "${PNG_ICON}" \
        --output appimage \
        2>&1 | tee -a "${LOG}"
    cd "${SRC_DIR}"

    # Copy AppImage to installer/ as well
    _FOUND="$(find "${APPIMAGE_DIR}" -maxdepth 1 -name '*.AppImage' \
        ! -name 'linuxdeploy*' 2>/dev/null | head -1 || true)"
    [[ -n "${_FOUND}" ]] || die "AppImage not produced"
    cp "${_FOUND}" "${INSTALLER_DIR}/"
    log "GTK AppImage ready: installer/$(basename "${_FOUND}")"
fi

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
log ""
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
log " Build complete"
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
[[ ${BUILD_TUI} -eq 1 ]] && {
    log " TUI .deb    : installer/${DEB_NAME}"
    log "   Install   : sudo dpkg -i installer/${DEB_NAME}"
    log "   Uninstall : sudo apt remove ${PKG_NAME}"
}
[[ ${BUILD_GTK_DEB} -eq 1 ]] && {
    log " GTK .deb    : installer/${GTK_DEB_NAME}"
    log "   Install   : sudo dpkg -i installer/${GTK_DEB_NAME}"
    log "   Uninstall : sudo apt remove ${PKG_NAME}-gtk"
}
[[ ${BUILD_CLI_DEB} -eq 1 ]] && {
    log " CLI .deb    : installer/${CLI_DEB_NAME}"
    log "   Install   : sudo dpkg -i installer/${CLI_DEB_NAME}"
    log "   Uninstall : sudo apt remove ${PKG_NAME}-cli"
}
[[ ${BUILD_GTK} -eq 1 ]] && {
    log " GTK AppImage: installer/6502caEdGtk-x86_64.AppImage"
    log "   Run       : ./installer/6502caEdGtk-x86_64.AppImage"
}
log "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
log ""

# ---------------------------------------------------------------------------
# Cleanup — remove build artefacts, keep only installer/ and its contents
# ---------------------------------------------------------------------------
log "Cleaning up build artefacts..."

# Remove the cmake build tree
rm -rf "${BUILD_DIR}"
log "  Removed: out/build/linux-gcc-release-installer/"

# Remove the entire AppImage working directory (AppDir, linuxdeploy, staging)
if [[ ${BUILD_GTK} -eq 1 ]]; then
    rm -rf "${APPIMAGE_DIR}"
    log "  Removed: AppImage/"
fi

# Remove the cmake out/ tree entirely if it is now empty
find "${SRC_DIR}/out" -maxdepth 0 -empty -exec rm -rf {} + 2>/dev/null || true

log "Cleanup complete — installer/ contains all deliverables."
log ""

xdg-open ./installer

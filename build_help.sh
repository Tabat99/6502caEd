#!/usr/bin/env bash
# build_help.sh — Generate help/help.html from help/help.md
#
# Usage: ./build_help.sh [--help]
#
# Checks for pandoc, installs it if missing, then converts
# help/help.md + help/help.css into a standalone help/help.html.

set -euo pipefail

# ── Colour output ────────────────────────────────────────────────────────────

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[0;33m'
BLU='\033[0;34m'
NC='\033[0m'  # no colour

log()  { echo -e "${GRN}[+]${NC} $*"; }
warn() { echo -e "${YLW}[!]${NC} $*"; }
die()  { echo -e "${RED}[X]${NC} $*" >&2; exit 1; }
info() { echo -e "${BLU}[i]${NC} $*"; }

# ── Help / usage ─────────────────────────────────────────────────────────────

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    echo ""
    echo "  build_help.sh — Generate 6502caEd help HTML from Markdown"
    echo ""
    echo "  Usage: ./build_help.sh [--help]"
    echo ""
    echo "  Requires: pandoc (installed automatically if missing)"
    echo "  Input:    help/help.md"
    echo "  Input:    help/help.css  (optional, embedded into HTML)"
    echo "  Output:   help/help.html"
    echo ""
    exit 0
fi

# ── Locate project root ───────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC_DIR="${SCRIPT_DIR}"

HELP_DIR="${SRC_DIR}/help"
HELP_MD="${HELP_DIR}/help.md"
HELP_CSS="${HELP_DIR}/help.css"
HELP_HTML="${HELP_DIR}/help.html"

# ── Sanity checks ─────────────────────────────────────────────────────────────

info "Project root : ${SRC_DIR}"
info "Help source  : ${HELP_MD}"
info "Output       : ${HELP_HTML}"
echo ""

# Check help directory exists
if [[ ! -d "${HELP_DIR}" ]]; then
    die "Help directory not found: ${HELP_DIR}
    
    Expected directory layout:
      ${SRC_DIR}/
        help/
          help.md
          help.css
    
    Create the directory and add help.md before running this script."
fi

# Check help.md exists
if [[ ! -f "${HELP_MD}" ]]; then
    die "Help Markdown source not found: ${HELP_MD}
    
    Create help/help.md before running this script."
fi

# Check help.md is not empty
if [[ ! -s "${HELP_MD}" ]]; then
    die "Help source file is empty: ${HELP_MD}
    
    Add content to help/help.md before running this script."
fi

# Warn if CSS is missing (not fatal — HTML will still be generated)
if [[ ! -f "${HELP_CSS}" ]]; then
    warn "CSS file not found: ${HELP_CSS}"
    warn "HTML will be generated without custom styling."
    warn "Add help/help.css to apply the 6502caEd dark theme."
    HAS_CSS=0
else
    log "CSS file found: ${HELP_CSS}"
    HAS_CSS=1
fi

# ── Check / install pandoc ────────────────────────────────────────────────────

echo ""
if command -v pandoc &>/dev/null; then
    PANDOC_VER="$(pandoc --version | head -1)"
    log "pandoc is already installed: ${PANDOC_VER}"
else
    warn "pandoc is not installed."

    # Check we can use apt-get
    if ! command -v apt-get &>/dev/null; then
        die "apt-get not found. Please install pandoc manually:
        
    Ubuntu/Debian:  sudo apt install pandoc
    Fedora:         sudo dnf install pandoc
    Arch:           sudo pacman -S pandoc
    Manual:         https://pandoc.org/installing.html
        
    Then re-run this script."
    fi

    # Check sudo is available
    if ! command -v sudo &>/dev/null; then
        die "sudo not found. Please install pandoc as root:
        
    apt-get install -y pandoc
        
    Then re-run this script."
    fi

    echo ""
    log "Installing pandoc via apt-get..."
    echo ""

    if ! sudo apt-get update -q 2>&1; then
        die "apt-get update failed. Check your internet connection and apt sources."
    fi

    if ! sudo apt-get install -y pandoc 2>&1; then
        die "Failed to install pandoc.
        
    Try manually: sudo apt-get install pandoc
    Or visit:     https://pandoc.org/installing.html"
    fi

    # Verify it actually installed
    if ! command -v pandoc &>/dev/null; then
        die "pandoc installation appeared to succeed but 'pandoc' command is still not found.
        
    Try opening a new terminal and running this script again."
    fi

    PANDOC_VER="$(pandoc --version | head -1)"
    log "pandoc installed successfully: ${PANDOC_VER}"
fi

# ── Generate HTML ─────────────────────────────────────────────────────────────

echo ""
log "Generating ${HELP_HTML} ..."

# Build pandoc arguments
PANDOC_ARGS=(
    "${HELP_MD}"
    --standalone
    --toc
    --toc-depth=2
    --from markdown
    --to html5
)

if [[ ${HAS_CSS} -eq 1 ]]; then
    # Embed the CSS inline so the HTML file is self-contained
    # (works correctly whether opened from AppImage, deb, or dev)
    PANDOC_ARGS+=(--embed-resources --css "${HELP_CSS}")
fi

PANDOC_ARGS+=(-o "${HELP_HTML}")

# Run pandoc
if ! pandoc "${PANDOC_ARGS[@]}" 2>&1; then
    # Clean up any partial output
    rm -f "${HELP_HTML}"
    die "pandoc failed to generate ${HELP_HTML}.
    
    Check that help/help.md contains valid Markdown."
fi

# Verify output was actually created
if [[ ! -f "${HELP_HTML}" ]]; then
    die "pandoc reported success but ${HELP_HTML} was not created.
    
    This is unexpected. Check disk space and directory permissions."
fi

# Verify output is not empty
if [[ ! -s "${HELP_HTML}" ]]; then
    rm -f "${HELP_HTML}"
    die "pandoc created an empty file: ${HELP_HTML}
    
    Check that help/help.md is not empty."
fi

HTML_SIZE="$(du -sh "${HELP_HTML}" | cut -f1)"

# ── Done ──────────────────────────────────────────────────────────────────────

echo ""
echo "  ┌─────────────────────────────────────────────────┐"
echo "  │  Help file generated successfully               │"
echo "  ├─────────────────────────────────────────────────┤"
printf "  │  Output : %-38s│\n" "help/help.html"
printf "  │  Size   : %-38s│\n" "${HTML_SIZE}"
printf "  │  CSS    : %-38s│\n" "$([ ${HAS_CSS} -eq 1 ] && echo "embedded" || echo "not included")"
echo "  └─────────────────────────────────────────────────┘"
echo ""
log "Open in browser: xdg-open ${HELP_HTML}"
xdg-open "${HELP_HTML}" &
echo ""
# read -rp "  Press any key to continue..." -n1
echo ""

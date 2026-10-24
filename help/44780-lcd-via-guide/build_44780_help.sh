#!/usr/bin/env bash
# build_44780_help.sh — Generate guide/44780_lcd_via.html from guide/44780_lcd_via.md
#
# This is a standalone companion-help builder for the HD44780 LCD simulator
# guide packaged alongside 6502caEd. It intentionally does not build or alter
# the main 6502caEd product help.

set -euo pipefail

RED='\033[0;31m'
GRN='\033[0;32m'
YLW='\033[0;33m'
BLU='\033[0;34m'
NC='\033[0m'

log()  { echo -e "${GRN}[+]${NC} $*"; }
warn() { echo -e "${YLW}[!]${NC} $*"; }
die()  { echo -e "${RED}[X]${NC} $*" >&2; exit 1; }
info() { echo -e "${BLU}[i]${NC} $*"; }

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    echo ""
    echo "  build_44780_help.sh — Generate standalone HD44780 LCD VIA guide HTML"
    echo ""
    echo "  Usage: ./build_44780_help.sh [--help]"
    echo ""
    echo "  Requires: pandoc"
    echo "  Input:    guide/44780_lcd_via.md"
    echo "  Input:    guide/help.css"
    echo "  Output:   guide/44780_lcd_via.html"
    echo ""
    echo "  This script is separate from the main 6502caEd help builder."
    echo ""
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUIDE_DIR="${SCRIPT_DIR}/guide"
GUIDE_MD="${GUIDE_DIR}/44780_lcd_via.md"
GUIDE_CSS="${GUIDE_DIR}/help.css"
GUIDE_HTML="${GUIDE_DIR}/44780_lcd_via.html"

info "Guide root  : ${SCRIPT_DIR}"
info "Guide source: ${GUIDE_MD}"
info "Output      : ${GUIDE_HTML}"
echo ""

[[ -d "${GUIDE_DIR}" ]] || die "Guide directory not found: ${GUIDE_DIR}"
[[ -s "${GUIDE_MD}" ]] || die "Guide Markdown source missing or empty: ${GUIDE_MD}"

if [[ -f "${GUIDE_CSS}" ]]; then
    log "CSS file found: ${GUIDE_CSS}"
    HAS_CSS=1
else
    warn "CSS file not found: ${GUIDE_CSS}"
    warn "HTML will be generated without custom styling."
    HAS_CSS=0
fi

echo ""
if command -v pandoc &>/dev/null; then
    log "pandoc found: $(pandoc --version | head -1)"
else
    die "pandoc is not installed. Install it, then re-run this script."
fi

echo ""
log "Generating ${GUIDE_HTML} ..."

PANDOC_ARGS=(
    "${GUIDE_MD}"
    --standalone
    --toc
    --toc-depth=2
    --from markdown
    --to html5
    --metadata title="HD44780 LCD Simulator VIA Guide"
)

if [[ ${HAS_CSS} -eq 1 ]]; then
    PANDOC_ARGS+=(--embed-resources --css "${GUIDE_CSS}")
fi

PANDOC_ARGS+=(-o "${GUIDE_HTML}")

if ! pandoc "${PANDOC_ARGS[@]}" 2>&1; then
    rm -f "${GUIDE_HTML}"
    die "pandoc failed to generate ${GUIDE_HTML}."
fi

[[ -s "${GUIDE_HTML}" ]] || die "pandoc did not create a usable HTML file."
HTML_SIZE="$(du -sh "${GUIDE_HTML}" | cut -f1)"

echo ""
echo "  ┌─────────────────────────────────────────────────┐"
echo "  │  HD44780 VIA guide generated successfully       │"
echo "  ├─────────────────────────────────────────────────┤"
printf "  │  Output : %-38s│\n" "guide/44780_lcd_via.html"
printf "  │  Size   : %-38s│\n" "${HTML_SIZE}"
printf "  │  CSS    : %-38s│\n" "$([ ${HAS_CSS} -eq 1 ] && echo "embedded" || echo "not included")"
echo "  └─────────────────────────────────────────────────┘"
echo ""

if command -v xdg-open &>/dev/null && [[ "${NO_OPEN:-0}" != "1" ]]; then
    log "Opening in browser: ${GUIDE_HTML}"
    xdg-open "${GUIDE_HTML}" >/dev/null 2>&1 || true
fi

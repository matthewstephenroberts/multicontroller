#!/bin/bash

# Generate PDF from user manual HTML using Chrome
# Usage: ./scripts/generate-pdf.sh [input.html] [output.pdf]
# Defaults to: docs/assets/user-manual.html → docs/assets/user-manual.pdf

set -e

INPUT_FILE="${1:-docs/assets/user-manual.html}"
OUTPUT_FILE="${2:-docs/assets/user-manual.pdf}"

# Resolve to absolute paths
INPUT_ABS="$(cd "$(dirname "$INPUT_FILE")" && pwd)/$(basename "$INPUT_FILE")"
OUTPUT_ABS="$(cd "$(dirname "$OUTPUT_FILE")" && pwd)/$(basename "$OUTPUT_FILE")"

# Check if input exists
if [ ! -f "$INPUT_FILE" ]; then
    echo "❌ Input file not found: $INPUT_FILE"
    exit 1
fi

echo "📄 Generating PDF from $INPUT_FILE..."

# Detect which browser is available
BROWSER=""
if command -v open &> /dev/null; then
    # macOS - check for Chrome or Edge
    if [ -d "/Applications/Google Chrome.app" ]; then
        BROWSER="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    elif [ -d "/Applications/Microsoft Edge.app" ]; then
        BROWSER="/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"
    fi
fi

if [ -z "$BROWSER" ]; then
    echo "❌ No supported browser found (Chrome or Edge required)"
    echo ""
    echo "📖 Alternative methods to generate the PDF:"
    echo ""
    echo "1️⃣  Using Chrome directly (macOS):"
    echo "   /Applications/Google\\ Chrome.app/Contents/MacOS/Google\\ Chrome \\"
    echo "     --headless --disable-gpu --print-to-pdf=\"$OUTPUT_ABS\" \"file://$INPUT_ABS\""
    echo ""
    echo "2️⃣  Install wkhtmltopdf:"
    echo "   brew install wkhtmltopdf"
    echo "   wkhtmltopdf $INPUT_FILE $OUTPUT_FILE"
    echo ""
    echo "3️⃣  Install Python packages:"
    echo "   pip3 install weasyprint"
    echo "   python3 scripts/generate-pdf.py"
    echo ""
    echo "4️⃣  Manual: Open in Chrome and print to PDF"
    echo "   File → Print → Save as PDF"
    exit 1
fi

# Use Chrome's headless mode to generate PDF.
# --no-pdf-header-footer: Chrome's default print header/footer would otherwise stamp every page
#   with the local file:// path (leaking the machine's username/directory layout) plus a
#   date/page-number line — the manual already has its own footer.pagefoot with real content.
# --run-all-compositor-stages-before-draw + --virtual-time-budget: give web fonts and the
#   embedded screenshots time to finish loading/decoding before the page is rasterized; without
#   this, a slow load can print blank image boxes or fall back to unstyled system fonts.
"$BROWSER" \
    --headless \
    --disable-gpu \
    --no-pdf-header-footer \
    --run-all-compositor-stages-before-draw \
    --virtual-time-budget=15000 \
    --print-to-pdf="$OUTPUT_ABS" \
    "file://$INPUT_ABS" \
    2>/dev/null

if [ -f "$OUTPUT_ABS" ]; then
    SIZE=$(stat -f%z "$OUTPUT_ABS" 2>/dev/null || stat -c%s "$OUTPUT_ABS" 2>/dev/null)
    SIZE_KB=$((SIZE / 1024))
    echo "✅ PDF generated successfully: $OUTPUT_FILE"
    echo "📦 File size: ${SIZE_KB} KB"
else
    echo "❌ PDF generation failed"
    exit 1
fi

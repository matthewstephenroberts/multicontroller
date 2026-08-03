# PDF Generation Guide

The user manual can be generated as a PDF from the HTML source with a single command. Three methods are provided, from simplest to most flexible.

## Quick start — generate PDF now

```bash
npm run pdf
```

This generates `docs/assets/user-manual.pdf` from `docs/assets/user-manual.html`.

**That's it!** No dependencies to install — uses Chrome/Edge if available on your system.

## Three generation methods

### 1. **Bash script (recommended, macOS/Linux)**

Fastest and easiest on macOS/Linux. Uses Chrome or Edge if installed (no extra dependencies).

```bash
bash scripts/generate-pdf.sh [input] [output]
```

Or via npm:
```bash
npm run pdf
```

**Works on:**
- macOS with Chrome or Microsoft Edge
- Linux with Chrome or Edge

### 2. **Python script (cross-platform)**

Works on any system with Python 3.8+ and WeasyPrint installed.

```bash
python3 scripts/generate-pdf.py [input] [output]
```

**Setup:**
```bash
# Option A: WeasyPrint (requires system libraries)
pip3 install weasyprint

# Option B: wkhtmltopdf (simpler)
brew install wkhtmltopdf          # macOS
sudo apt install wkhtmltopdf      # Linux
```

### 3. **Node.js script (alternative)**

Provides an NPM-based workflow using Puppeteer.

```bash
node scripts/generate-pdf.js [input] [output]
```

**Setup:**
```bash
npm install
npm run pdf
```

## Specify custom input/output

```bash
bash scripts/generate-pdf.sh docs/assets/user-manual.html dist/manual.pdf
python3 scripts/generate-pdf.py docs/assets/user-manual.html dist/manual.pdf
node scripts/generate-pdf.js docs/assets/user-manual.html dist/manual.pdf
```

## Watch for changes (auto-regenerate)

Regenerate the PDF every time you save the HTML:

```bash
npm run pdf:watch
```

Requires `nodemon` installed (`npm install --save-dev nodemon`)

## What's included

The PDF is generated with:
- **A4 page size** (210 × 297 mm)
- **18mm margins** (top/bottom), **16mm** (left/right)
- **Print-friendly styling** with exact color reproduction
- **Responsive tables and diagrams** that scale for printing
- **Cover page** with Brix mascot and app theme colors
- **Table of contents** and internal navigation

## Troubleshooting

**"Puppeteer failed to download"**
```bash
# Force puppeteer to use system Chromium instead
PUPPETEER_SKIP_CHROMIUM_DOWNLOAD=true npm run pdf
```

**PDF is blank or rendering incorrectly**
- Ensure the HTML file path is correct
- Check that fonts are loading (CSS @import statements)
- Try opening the HTML in a browser first to verify it renders

**File size is large (>10 MB)**
- This is normal for the first run (includes Puppeteer data)
- Subsequent PDFs are smaller
- If the PDF itself is large, consider removing large images or reducing print quality

## Editing the manual

The user manual is maintained as an HTML file (`docs/assets/user-manual.html`). To edit:

1. Open `docs/assets/user-manual.html` in your editor
2. Make changes to content, styling, or structure
3. Run `npm run pdf` to generate the updated PDF
4. View the PDF to verify changes render correctly

### Styling notes
- The manual uses CSS `@page` rules for print styling
- Colors and fonts are defined in `:root` CSS variables
- The cover page is a full-page SVG with background gradients
- Responsive design uses flexbox and grid for tables

### Adding sections
Each major section follows this structure:
```html
<h1>Section number. Section title</h1>
<p>Introduction paragraph...</p>

<h2>Subsection heading</h2>
<p>Content...</p>

<table>
  <!-- Use <table> for structured data -->
</table>

<div class="callout">
  <!-- Use callout for tips, warnings, notes -->
</div>

<ol class="step-list">
  <!-- Use step-list for numbered procedures -->
  <li>Step one</li>
  <li>Step two</li>
</ol>
```

## Automation

To automatically regenerate the PDF when you save the HTML:

```bash
npm run pdf:watch
```

This requires `nodemon` to be installed. To add it:
```bash
npm install --save-dev nodemon
```

Then update the `npm run pdf:watch` script in your workflow.

## CI/CD Integration

To include PDF generation in your GitHub Actions workflow, add:

```yaml
- name: Generate user manual PDF
  run: |
    npm install
    npm run pdf

    
- name: Upload PDF as artifact
  uses: actions/upload-artifact@v3
  with:
    name: user-manual
    path: docs/assets/user-manual.pdf
```

## Theme colors used

The manual uses these colors from the MultiController app:
- **Primary accent:** `#4ea8ff` (blue)
- **Secondary accent:** `#38d39f` (teal/green)
- **Warning:** `#ffb454` (orange)
- **Danger:** `#ff6b6b` (red)
- **LEGO colours:** red, yellow, blue, green, orange, etc.

To match your app's theme, edit the `:root` CSS variables in the `<style>` block of the HTML.

## Notes

- The PDF is optimized for **A4 paper** (210 × 297 mm)
- Page breaks are automatic based on content height
- The cover page doesn't have margins (full bleed)
- All subsequent pages have 18/16mm margins
- Fonts are embedded in the CSS (`@import url()`)
- SVG graphics render with full colour support

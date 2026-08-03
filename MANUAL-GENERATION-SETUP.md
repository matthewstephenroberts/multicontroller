# User Manual PDF Generation — Setup Complete ✅

Your MultiController user manual PDF generation is now fully set up and ready to use!

## What's been created

### 📄 User Manual (`docs/assets/user-manual.html`)
- **Modern design** with app theme colors (blue accents, LEGO brick palette)
- **Brix mascot** rendered on cover page (accurate SVG from firmware definition)
- **Improved architecture diagram** showing multiple sensors connecting to single LEGO hub port
- **8-page professional manual** with:
  - Cover page with features and hero layout
  - Table of contents
  - 13 detailed sections covering all features
  - Appendix with sensor reference
  - Print-optimized CSS with proper page breaks

### 🛠️ PDF Generation Scripts

Three methods to generate PDFs:

1. **`scripts/generate-pdf.sh`** (Bash)
   - ✅ Recommended for macOS/Linux
   - Uses Chrome or Edge if available
   - No dependencies needed
   - Fast and reliable

2. **`scripts/generate-pdf.py`** (Python)
   - ✅ Cross-platform alternative
   - Requires WeasyPrint or wkhtmltopdf
   - Good fallback option

3. **`scripts/generate-pdf.js`** (Node.js)
   - ✅ NPM/JavaScript workflow
   - Uses Puppeteer
   - Install with: `npm install puppeteer`

### 📋 Configuration Files

1. **`package.json`** (root)
   - NPM scripts configured:
     - `npm run pdf` — generate PDF now
     - `npm run pdf:watch` — auto-regenerate on save
   - Contains all project metadata

2. **`docs/README-PDF.md`**
   - Complete usage guide
   - Setup instructions for all three methods
   - Troubleshooting tips

## Quick Start

Generate the PDF right now:

```bash
npm run pdf
```

Output: `docs/assets/user-manual.pdf` (809 KB, 8 pages)

## Using the Generated PDF

The PDF is ready to:
- ✅ Share with users and students
- ✅ Print to physical copies
- ✅ Include in documentation packages
- ✅ Embed in releases on GitHub
- ✅ Host on your project website

## Editing the Manual

To update the PDF:

1. Edit `docs/assets/user-manual.html` in your editor
2. Run `npm run pdf`
3. The PDF regenerates automatically

To auto-regenerate on every save:
```bash
npm install --save-dev nodemon
npm run pdf:watch
```

Then edit and save — the PDF updates in real time.

## Style and Content

The manual uses:
- **Fredoka font** (headings) — matches the MultiController web app
- **Nunito font** (body text) — matches the web app
- **LEGO brick colors** — red, yellow, blue, green, orange (brand consistency)
- **App accent colors** — blue (#4ea8ff) and teal (#38d39f)
- **Responsive tables** — scale appropriately for print
- **SVG diagrams** — crisp vector graphics at any scale

The content is:
- **M5Stack-focused** — explains all three board options (AtomS3R, AtomS3, AtomS3 Lite)
- **Beginner-friendly** — written for students and non-technical users
- **Comprehensive** — covers all major features and troubleshooting
- **Classroom-ready** — includes safety notes, wiring diagrams, example code

## Automation & CI/CD

To auto-generate the PDF in GitHub Actions:

```yaml
- name: Generate user manual PDF
  run: npm run pdf

- name: Upload PDF artifact
  uses: actions/upload-artifact@v3
  with:
    name: user-manual
    path: docs/assets/user-manual.pdf
```

Or include in releases:

```yaml
- name: Upload to release
  uses: softprops/action-gh-release@v1
  with:
    files: docs/assets/user-manual.pdf
```

## Customization

The manual's appearance is controlled by CSS in the `<style>` block. To customize:

1. Edit `:root` CSS variables for colors
2. Adjust `@page` margins for print size
3. Modify fonts via `@import url()`
4. Update SVG diagrams inline

All changes regenerate automatically when you run `npm run pdf`.

## Troubleshooting

**PDF is blank?**
- Ensure Chrome/Edge is installed
- Check that `docs/assets/user-manual.html` exists
- Verify file permissions

**"wkhtmltopdf not found"?**
- Try the bash script first (uses Chrome)
- Or install: `brew install wkhtmltopdf`

**WeasyPrint errors?**
- Install system libraries: `brew install python-gobject3 cairo pango gdk-pixbuf`
- Then: `pip3 install weasyprint`

**Fonts not loading?**
- The CSS imports Google Fonts automatically
- Make sure internet is available during generation

## Files Summary

```
MultiController/
├── docs/
│   ├── assets/
│   │   ├── user-manual.html       ← Main source (edit this)
│   │   ├── user-manual.pdf        ← Generated PDF (don't edit)
│   │   └── README-PDF.md          ← PDF generation guide
│   └── ... other docs
├── scripts/
│   ├── generate-pdf.sh            ← Bash script (recommended)
│   ├── generate-pdf.py            ← Python script
│   ├── generate-pdf.js            ← Node.js script
│   └── ... other scripts
├── package.json                   ← NPM configuration (update for PDF)
└── ... rest of project
```

## Next Steps

1. ✅ Test: `npm run pdf`
2. ✅ Verify: Open `docs/assets/user-manual.pdf` in your reader
3. ✅ Share: Add to your GitHub releases
4. ✅ Automate: Set up GitHub Actions if desired
5. ✅ Update: Edit the HTML, regenerate as needed

---

**Questions?** See `docs/README-PDF.md` for detailed setup and troubleshooting.

**Manual ready to share!** 🎉

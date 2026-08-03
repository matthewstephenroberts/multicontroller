#!/usr/bin/env node

/**
 * Generate PDF from user manual HTML
 * Usage: node scripts/generate-pdf.js [input.html] [output.pdf]
 * Defaults to: docs/assets/user-manual.html → docs/assets/user-manual.pdf
 */

import puppeteer from 'puppeteer';
import path from 'path';
import { fileURLToPath } from 'url';
import fs from 'fs';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const projectRoot = path.join(__dirname, '..');

async function generatePDF() {
  const inputFile = process.argv[2] || 'docs/assets/user-manual.html';
  const outputFile = process.argv[3] || 'docs/assets/user-manual.pdf';

  const inputPath = path.join(projectRoot, inputFile);
  const outputPath = path.join(projectRoot, outputFile);

  // Verify input exists
  if (!fs.existsSync(inputPath)) {
    console.error(`❌ Input file not found: ${inputPath}`);
    process.exit(1);
  }

  let browser;
  try {
    console.log(`📄 Generating PDF from ${inputFile}...`);

    browser = await puppeteer.launch({
      headless: 'new',
      args: ['--no-sandbox', '--disable-setuid-sandbox']
    });

    const page = await browser.newPage();

    // Set viewport for consistent rendering
    await page.setViewport({ width: 1280, height: 960 });

    // Navigate to local file
    const fileUrl = `file://${inputPath}`;
    await page.goto(fileUrl, { waitUntil: 'networkidle0' });

    // Generate PDF with A4 settings matching the HTML
    await page.pdf({
      path: outputPath,
      format: 'A4',
      margin: {
        top: '18mm',
        bottom: '18mm',
        left: '16mm',
        right: '16mm'
      },
      printBackground: true,
      preferCSSPageSize: true,
      timeout: 60000
    });

    console.log(`✅ PDF generated successfully: ${outputFile}`);
    console.log(`📦 File size: ${(fs.statSync(outputPath).size / 1024).toFixed(1)} KB`);

  } catch (error) {
    console.error(`❌ Error generating PDF:`, error.message);
    process.exit(1);
  } finally {
    if (browser) {
      await browser.close();
    }
  }
}

generatePDF();

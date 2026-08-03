#!/usr/bin/env python3

"""
Generate PDF from user manual HTML using wkhtmltopdf or weasyprint.
Usage: python3 scripts/generate-pdf.py [input.html] [output.pdf]
Defaults to: docs/assets/user-manual.html → docs/assets/user-manual.pdf
"""

import sys
import os
from pathlib import Path

def generate_with_weasyprint():
    """Generate PDF using WeasyPrint (Python library)"""
    try:
        from weasyprint import HTML, CSS
        import shutil

        input_file = sys.argv[1] if len(sys.argv) > 1 else 'docs/assets/user-manual.html'
        output_file = sys.argv[2] if len(sys.argv) > 2 else 'docs/assets/user-manual.pdf'

        input_path = Path(input_file)
        output_path = Path(output_file)

        if not input_path.exists():
            print(f"❌ Input file not found: {input_path}")
            sys.exit(1)

        print(f"📄 Generating PDF from {input_file} using WeasyPrint...")

        # Convert to absolute file:// URL
        file_url = input_path.resolve().as_uri()

        # Generate PDF
        HTML(string=open(input_path).read(), base_url=file_url).write_pdf(str(output_path))

        size_mb = output_path.stat().st_size / (1024 * 1024)
        print(f"✅ PDF generated successfully: {output_file}")
        print(f"📦 File size: {size_mb:.1f} MB" if size_mb > 1 else f"📦 File size: {output_path.stat().st_size / 1024:.1f} KB")

    except ImportError:
        print("❌ WeasyPrint not installed. Install with: pip3 install weasyprint")
        sys.exit(1)
    except Exception as e:
        print(f"❌ Error generating PDF: {e}")
        sys.exit(1)

def generate_with_wkhtmltopdf():
    """Generate PDF using wkhtmltopdf (command-line tool)"""
    try:
        import subprocess
        import shutil

        # Check if wkhtmltopdf is installed
        if not shutil.which('wkhtmltopdf'):
            print("❌ wkhtmltopdf not installed.")
            print("   Install with: brew install --cask wkhtmltopdf (macOS)")
            print("   Or: apt-get install wkhtmltopdf (Linux)")
            return False

        input_file = sys.argv[1] if len(sys.argv) > 1 else 'docs/assets/user-manual.html'
        output_file = sys.argv[2] if len(sys.argv) > 2 else 'docs/assets/user-manual.pdf'

        input_path = Path(input_file)
        output_path = Path(output_file)

        if not input_path.exists():
            print(f"❌ Input file not found: {input_path}")
            sys.exit(1)

        print(f"📄 Generating PDF from {input_file} using wkhtmltopdf...")

        subprocess.run([
            'wkhtmltopdf',
            '--dpi', '150',
            '--quiet',
            '--enable-local-file-access',
            str(input_path),
            str(output_path)
        ], check=True)

        size_kb = output_path.stat().st_size / 1024
        print(f"✅ PDF generated successfully: {output_file}")
        print(f"📦 File size: {size_kb:.1f} KB")
        return True

    except subprocess.CalledProcessError as e:
        print(f"❌ wkhtmltopdf error: {e}")
        return False
    except Exception as e:
        print(f"❌ Error generating PDF: {e}")
        return False

if __name__ == '__main__':
    # Try wkhtmltopdf first (faster), fall back to WeasyPrint
    if not generate_with_wkhtmltopdf():
        print("\n💡 Falling back to WeasyPrint...")
        generate_with_weasyprint()

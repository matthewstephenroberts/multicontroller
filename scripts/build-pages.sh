#!/bin/bash
# Build web app for GitHub Pages deployment
# Output: /pages/app/ folder for the built React application
# Usage: ./scripts/build-pages.sh

set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WEB_DIR="$PROJECT_ROOT/web"
PAGES_APP_DIR="$PROJECT_ROOT/pages/app"

echo "🔨 Building web app for GitHub Pages..."
echo ""
echo "Input:  $WEB_DIR"
echo "Output: $PAGES_APP_DIR"
echo ""

# Check if web directory exists
if [ ! -d "$WEB_DIR" ]; then
    echo "❌ Web directory not found: $WEB_DIR"
    exit 1
fi

# Create pages/app directory if it doesn't exist
mkdir -p "$PAGES_APP_DIR"

# Navigate to web directory
cd "$WEB_DIR"

# Install dependencies if needed
if [ ! -d "node_modules" ]; then
    echo "📦 Installing dependencies..."
    npm install
fi

# Build the React app
echo "🏗️  Building React app..."
npm run build

# Copy built files to pages/app
echo "📂 Copying built files to $PAGES_APP_DIR..."
rm -rf "$PAGES_APP_DIR"/*
cp -r dist/* "$PAGES_APP_DIR/"

echo ""
echo "✅ Build complete!"
echo "📁 Output location: $PAGES_APP_DIR"
echo ""
echo "Next steps:"
echo "1. Add /pages/app to git: git add pages/app/"
echo "2. Commit: git commit -m 'Build: Web app for GitHub Pages'"
echo "3. Push: git push origin main"
echo "4. Enable GitHub Pages in repo settings (source: main branch, folder: /docs)"
echo ""
echo "Your site will be available at:"
echo "https://matthewstephenroberts.github.io/multicontroller/"
echo "With app at: /pages/app/"

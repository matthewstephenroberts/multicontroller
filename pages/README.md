# GitHub Pages - MultiController

This folder contains the GitHub Pages site for the MultiController project.

## Structure

```
pages/
├── index.html              # Landing page with links to the app
├── .nojekyll              # Tells GitHub Pages not to use Jekyll processing
├── app/                   # Built React web application (auto-generated)
│   ├── index.html
│   ├── assets/
│   └── ...
└── README.md              # This file
```

## Building and Deploying

### Build the web app for GitHub Pages:

```bash
./scripts/build-pages.sh
```

This will:
1. Install dependencies (if needed)
2. Build the React app with production optimizations
3. Copy the built files to `/pages/app/`
4. Output the final size and deployment info

### Deploy to GitHub Pages:

```bash
git add pages/
git commit -m "Build: Web app for GitHub Pages"
git push origin main
```

GitHub will automatically deploy from the `/pages` folder.

## GitHub Pages Configuration

Your repository is configured to serve from the `/pages` folder on the `main` branch.

**To verify/update settings:**
1. Go to Repository → Settings → Pages
2. Ensure:
   - Source: Deploy from a branch
   - Branch: main
   - Folder: / (root - since we're using /pages as the root)

**Site URL:** `https://matthewstephenroberts.github.io/multicontroller/`

## What's Included

- **Landing Page** (`index.html`) — Introduction and links
- **Web App** (`/app/`) — Full React application with Bluetooth support
- **Documentation Links** — Hackster.io article and GitHub

## Development Workflow

1. Make changes to the web app in `/web/`
2. Test locally: `cd web && npm run dev`
3. Build for production: `./scripts/build-pages.sh`
4. Commit and push: `git add pages/ && git commit -m "Build: Web app update" && git push`

## Offline Use

The web app works offline once loaded. Bluetooth connectivity requires:
- Chrome or Edge browser
- HTTPS connection (GitHub Pages provides this)
- AtomS3 device with MultiController firmware

## Notes

- The `.nojekyll` file tells GitHub Pages to skip Jekyll processing (required for React apps)
- Build artifacts are committed to git for faster page loads
- The `/pages/app/` folder is regenerated with each build — don't edit files there directly

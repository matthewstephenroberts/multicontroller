// afterPack.js — electron-builder hook: properly ad-hoc deep-sign the macOS bundle.
//
// Without a signing certificate configured, electron-builder can leave the app with only the
// raw linker-signed stub from the downloaded Electron binary (Identifier=Electron,
// "Info.plist=not bound", "Sealed Resources=none"). macOS TCC attributes privacy permissions
// (Bluetooth!) by bundle ID + code signature — with an unsealed bundle it can't validate the
// binary against com.multicontroller.app or its NSBluetoothAlwaysUsageDescription, so
// CoreBluetooth gets silently denied even when System Settings shows the toggle as on. A
// proper adhoc deep-sign ("-" identity, no cert needed) seals the bundle and binds the
// Info.plist so TCC attribution works.
const { execFileSync } = require("child_process");
const fs = require("fs");
const path = require("path");

module.exports = async function afterPack(context) {
  if (context.electronPlatformName !== "darwin") return;

  const appName = `${context.packager.appInfo.productFilename}.app`;
  const appPath = path.join(context.appOutDir, appName);

  // codesign refuses to seal a bundle containing Finder metadata ("resource fork, Finder
  // information, or similar detritus not allowed"). `xattr -cr` alone is not enough: it
  // dereferences symlinks, so metadata sitting ON a symlink itself (Electron.app's Frameworks
  // are full of Versions/Current links) survives and still trips codesign. Rebuilding the
  // bundle through ditto with --norsrc/--noextattr/--noqtn is the reliable way to produce a
  // guaranteed-clean copy: it walks everything (preserving symlinks as symlinks) and simply
  // never writes resource forks, xattrs, or quarantine flags into the destination.
  console.log(`  • afterPack: rebuilding ${appName} via ditto to strip all Finder metadata`);
  const cleanPath = `${appPath}.clean`;
  fs.rmSync(cleanPath, { recursive: true, force: true });
  execFileSync("ditto", ["--norsrc", "--noextattr", "--noqtn", appPath, cleanPath], { stdio: "inherit" });
  fs.rmSync(appPath, { recursive: true, force: true });
  fs.renameSync(cleanPath, appPath);
  execFileSync("find", [appPath, "-name", ".DS_Store", "-delete"], { stdio: "inherit" });

  console.log(`  • afterPack: adhoc deep-signing ${appName} (seals bundle for TCC/Bluetooth)`);
  execFileSync("codesign", ["--force", "--deep", "--sign", "-", appPath], { stdio: "inherit" });
  execFileSync("codesign", ["--verify", "--deep", appPath], { stdio: "inherit" });
};

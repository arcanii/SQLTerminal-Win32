# Releasing SQLTerminal (Win32)

Auto-update uses **WinSparkle** reading an **EdDSA-signed appcast hosted on
GitHub**, mirroring the macOS Sparkle setup. The Windows build shares the macOS
app's Ed25519 key pair — the public key in `src/platform/Updater.cpp`
(`win_sparkle_set_eddsa_public_key`) is the same string as the macOS
`SUPublicEDKey` — so **the same private key signs Windows packages**, and the feed
lives at the same kind of URL:

- Feed: `https://raw.githubusercontent.com/arcanii/SQLTerminal-Win32/main/appcast.xml`
- Installers: GitHub Release assets.

The running app reports its version as `APP_VERSION.BUILD` (e.g. `0.1.0.42`, from
`SQLT_VERSION_FULL`); WinSparkle offers an update when the appcast's
`sparkle:version` is higher.

## One-time setup
- Have the **Ed25519 private key** used for the macOS Sparkle releases.
- Get a signing tool that emits a base64 Ed25519 signature:
  - macOS: Sparkle's `sign_update` (already used for the Mac app), or
  - Windows: `sign_update.exe` from the WinSparkle release zip
    (<https://github.com/vslavik/winsparkle/releases>).
- (Optional but recommended) an Authenticode code-signing cert — see *Not yet
  covered* below.

## Per release
1. **Bump the version** if the marketing version changes, in all four places:
   `APP_VERSION` in `CMakeLists.txt` (drives the About box + update checks), `MyVer`
   in `packaging/installer.iss` (installer name/version), and the file-properties
   version in `packaging/SQLTerminal.rc` (`FILEVERSION`/`PRODUCTVERSION` + the
   `FileVersion`/`ProductVersion` strings) and `packaging/app.manifest`
   (`assemblyIdentity version`). The build number (git commit count) is automatic.
2. **Commit** everything first — the build number is baked at CMake configure
   time, so the version stamp only matches `HEAD` after a build that follows the
   commit (see the `build-stamp-after-commit` note).
3. **Build**:
   ```cmd
   scripts\build-and-test.cmd
   scripts\build-installer.cmd
   ```
   → `build\installer\SQLTerminal-<ver>-setup.exe`
4. **Sign the installer** with the private key and copy the printed
   `sparkle:edSignature` value:
   - macOS: `./bin/sign_update build/installer/SQLTerminal-<ver>-setup.exe`
   - Windows: `sign_update.exe build\installer\SQLTerminal-<ver>-setup.exe`
5. **Generate the appcast** (writes `appcast.xml` at the repo root):
   ```cmd
   pwsh scripts\make-appcast.ps1 -Version 0.1.0.<build> ^
        -SetupExe build\installer\SQLTerminal-<ver>-setup.exe -Signature <sig>
   ```
   (`-Version` must match `SQLT_VERSION_FULL` of that build, i.e.
   `APP_VERSION.BUILD`. Pass `-Tag` / `-Notes` to override the defaults.)
6. **Publish on GitHub**: create a Release tagged `v0.1.0.<build>` and upload the
   setup `.exe` as an asset (the appcast enclosure URL points there).
7. **Commit & push `appcast.xml`** on `main` — `raw.githubusercontent.com` serves
   it at the feed URL.

Installed apps pick it up via **Help → Check for Updates** and WinSparkle's
periodic check.

## Not yet covered
- **Authenticode signing** of the exe + installer is not set up. Without it,
  SmartScreen may warn on first download/run. (The EdDSA signature above is the
  *update-integrity* signature WinSparkle verifies; it is separate from
  Authenticode.)
- Keep the Ed25519 **private key out of the repo**.

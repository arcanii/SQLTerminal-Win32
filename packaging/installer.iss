; SPDX-License-Identifier: GPL-3.0-or-later
; Inno Setup script for SQLTerminal (Win32). Build with:
;   scripts\build-installer.cmd   (after a normal build)
; Produces build\installer\SQLTerminal-<ver>-setup.exe.

#define MyApp "SQLTerminal"
#define MyVer "0.1.0"

[Setup]
AppId={{8F3A1B62-9C44-4E0A-B7D1-2E6F5A9C0D11}}
AppName={#MyApp}
AppVersion={#MyVer}
AppPublisher=SQLTerminal
DefaultDirName={autopf}\{#MyApp}
DefaultGroupName={#MyApp}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\SQLTerminal.exe
OutputDir=..\build\installer
OutputBaseFilename=SQLTerminal-{#MyVer}-setup
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Files]
Source: "..\build\SQLTerminal.exe"; DestDir: "{app}"; Flags: ignoreversion
; Runtime DLLs (libpq + OpenSSL + WinSparkle) copied next to the exe by the build.
Source: "..\build\*.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\SQLTerminal"; Filename: "{app}\SQLTerminal.exe"
Name: "{group}\Uninstall SQLTerminal"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\SQLTerminal.exe"; Description: "Launch SQLTerminal"; \
  Flags: nowait postinstall skipifsilent

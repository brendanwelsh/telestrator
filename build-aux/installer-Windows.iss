; Telestrator for OBS Studio - Windows installer (Inno Setup 6).
;
; Driven from .github/scripts/Package-Windows.ps1, which reads buildspec.json and
; passes the values below via ISCC /D defines. The #ifndef fallbacks let you also
; run `iscc build-aux\installer-Windows.iss` by hand after a local build+install.
;
; The plugin installs into the per-machine OBS plugins folder
;   C:\ProgramData\obs-studio\plugins\<PluginId>\
; which OBS scans regardless of where OBS Studio itself is installed - so no
; registry probing for the OBS path is needed.

#ifndef PluginId
  #define PluginId "telestrator"
#endif
#ifndef AppName
  #define AppName "Telestrator"
#endif
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef AppPublisher
  #define AppPublisher "Brendan Welsh"
#endif
#ifndef AppURL
  #define AppURL "https://github.com/brendanwelsh/obs-telestrator"
#endif
; Folder holding the installed plugin tree (<PluginId>\bin, <PluginId>\data).
#ifndef SourceDir
  #define SourceDir "..\release\RelWithDebInfo\" + PluginId
#endif
#ifndef OutputDir
  #define OutputDir "..\release"
#endif
#ifndef LicenseFile
  #define LicenseFile "..\LICENSE"
#endif

[Setup]
; AppId is fixed forever so upgrades replace in place and uninstall is clean.
AppId={{9F2B1E7A-3C4D-4A6B-9E10-2D5F8C1A7B34}
AppName={#AppName} for OBS Studio
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppURL}
AppSupportURL={#AppURL}
AppUpdatesURL={#AppURL}
VersionInfoVersion={#AppVersion}
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginId}
DisableDirPage=yes
DisableProgramGroupPage=yes
UsePreviousAppDir=no
LicenseFile={#LicenseFile}
OutputDir={#OutputDir}
OutputBaseFilename={#PluginId}-{#AppVersion}-windows-x64-Installer
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
WizardStyle=modern
UninstallDisplayName={#AppName} for OBS Studio
UninstallDisplayIcon={app}\bin\64bit\{#PluginId}.dll

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Messages]
WelcomeLabel2=This will install [name] into your OBS Studio plugins folder.%n%nPlease close OBS Studio before continuing.

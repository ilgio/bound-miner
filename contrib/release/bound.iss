; Milestone 18: BOUND's Windows installer, bound-<version>-win64-setup.exe, made with Inno Setup 6
; (https://jrsoftware.org/isinfo.php) from the folder package-windows.ps1 installs and checks:
;
;   ISCC.exe /DAppVersion=1.1.0 /DPackageDir=C:\...\dist\bound-1.1.0 /DOutputDir=C:\...\dist bound.iss
;
; It installs into Program Files (or, for someone without admin rights, their own programs folder),
; adds BOUND to the Start menu (and the testnet beside it), optionally to the desktop, and an
; uninstaller. A newer version installs over an older one. Uninstalling removes the programs only:
; the chain and the wallet stay in %APPDATA%\BOUND.

#ifndef AppVersion
  #error Pass /DAppVersion=<version>
#endif
#ifndef PackageDir
  #error Pass /DPackageDir=<the installed package folder, an absolute path>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
; The same id for every version, so a new one replaces the old.
AppId={{B0C1D5A2-6E3F-4F7A-9D21-5B0A7E3C9F11}
AppName=BOUND
AppVersion={#AppVersion}
AppVerName=BOUND {#AppVersion}
AppPublisher=BOUND
AppPublisherURL=https://github.com/Sonic-Bound/Bound
AppSupportURL=https://github.com/Sonic-Bound/Bound/issues
AppUpdatesURL=https://github.com/Sonic-Bound/Bound/releases
AppCopyright=Copyright (c) 2026 BOUND. MIT license.
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\BOUND
DefaultGroupName=BOUND
DisableProgramGroupPage=yes
DisableWelcomePage=no
; Anyone can install: for all users with admin rights, for themselves without.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
LicenseFile={#PackageDir}\LICENSE
SetupIconFile=..\..\src\qt\res\bound.ico
UninstallDisplayIcon={app}\bound-qt.exe
UninstallDisplayName=BOUND {#AppVersion}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Asks to close a running BOUND (whose node holds the files) before replacing it.
CloseApplications=yes
RestartApplications=no
OutputDir={#OutputDir}
OutputBaseFilename=bound-{#AppVersion}-win64-setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\BOUND"; Filename: "{app}\bound-qt.exe"; Comment: "The BOUND wallet: a full node with a wallet"
Name: "{autoprograms}\BOUND (testnet)"; Filename: "{app}\bound-qt.exe"; Parameters: "--testnet"; Comment: "The BOUND wallet on the testnet, whose coins have no value"
Name: "{autodesktop}\BOUND"; Filename: "{app}\bound-qt.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\bound-qt.exe"; Description: "{cm:LaunchProgram,BOUND}"; Flags: nowait postinstall skipifsilent

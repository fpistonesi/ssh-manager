; Inno Setup 6 script for SSH Manager. Normally run by package.ps1, which
; passes AppVersion, SourceDir (the deployed folder), OutputDir and IconFile.
;
; Installs per user (no administrator rights) in %LOCALAPPDATA%\Programs.
; Uninstalling keeps the archive and the settings (%LOCALAPPDATA%\sshmanager
; and HKCU\Software\sshmanager).

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef SourceDir
  #error SourceDir is required (the folder prepared by package.ps1)
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F1B7C2E-4D3A-4B8E-9C61-2A7E5D9F0B13}
AppName=SSH Manager
AppVersion={#AppVersion}
AppVerName=SSH Manager {#AppVersion}
DefaultDirName={autopf}\SSH Manager
DefaultGroupName=SSH Manager
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=SSHManager-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\sshmanager.exe
#ifdef IconFile
SetupIconFile={#IconFile}
#endif
LicenseFile={#SourceDir}\LICENSE.txt
CloseApplications=yes

[Languages]
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\SSH Manager"; Filename: "{app}\sshmanager.exe"
Name: "{group}\{cm:UninstallProgram,SSH Manager}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\SSH Manager"; Filename: "{app}\sshmanager.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\sshmanager.exe"; Description: "{cm:LaunchProgram,SSH Manager}"; Flags: nowait postinstall skipifsilent

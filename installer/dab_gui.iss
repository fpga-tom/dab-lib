#define AppName "DAB SDR GUI"
#define AppExeName "dab_gui.exe"

#ifndef AppVersion
  #define AppVersion "0.1.4"
#endif

#ifndef SourceDir
  #define SourceDir "..\\dist\\install\\Release"
#endif

#ifndef OutputDir
  #define OutputDir "..\\dist\\installer"
#endif

[Setup]
AppId={{F4BC4B7A-1F11-4D85-9F18-8C4C5705A2C8}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=dab-lib
DefaultDirName={autopf64}\{#AppName}
DefaultGroupName={#AppName}
AllowNoIcons=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=dab-sdr-gui-setup-{#AppVersion}
Compression=lzma
SolidCompression=yes
WizardStyle=modern
DisableDirPage=no
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#AppExeName}
LicenseFile={#SourceDir}\LICENSE

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent

#define AppName "Klip"
#define AppVersion "0.1.0"
#define AppPublisher "Klip"
#define AppExeName "klip_core.exe"
#define BuildRoot "..\build-vs\Release"

[Setup]
AppId={{8A44B102-4E2D-4701-B01E-0B43D5C6C5A8}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
UninstallDisplayIcon={app}\{#AppExeName}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
OutputDir=.
OutputBaseFilename=ClippingSetup
PrivilegesRequired=admin

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"

[Dirs]
Name: "{app}\clips"

[Files]
Source: "{#BuildRoot}\{#AppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\avcodec-62.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\avdevice-62.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\avfilter-11.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\avformat-62.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\avutil-60.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\swresample-6.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildRoot}\swscale-9.dll"; DestDir: "{app}"; Flags: ignoreversion
; Optional if you choose to bundle the VC++ runtime installer:
; Source: "vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{group}\Clips Folder"; Filename: "{app}\clips"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch {#AppName}"; Flags: nowait postinstall skipifsilent
; Optional VC++ runtime install:
; Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: waituntilterminated

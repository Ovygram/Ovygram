[Setup]
AppId={{{#AppGuid}}
AppName=Ovygram
AppVersion={#AppVersion}
AppPublisher=Ovygram
AppPublisherURL=https://ovygram.opik.net
AppSupportURL=https://github.com/Ovygram/Ovygram
DefaultDirName={userappdata}\Ovygram
DefaultGroupName=Ovygram
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
SetupIconFile={#SourcePath}..\Resources\art\icon256.ico
UninstallDisplayIcon={app}\Ovygram.exe
WizardStyle=modern
SolidCompression=yes
CloseApplications=yes
OutputBaseFilename=Ovygram-setup

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Files]
Source: "{#PayloadDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Invalidate update data from the previous executable while preserving tdata.
Type: files; Name: "{app}\ovygram_build.json"
Type: files; Name: "{app}\tupdates\ovygram_ready.json"
Type: files; Name: "{app}\tupdates\ovygram_ready.zip"
Type: files; Name: "{app}\tupdates\ovygram_download.part"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Icons]
Name: "{group}\Ovygram"; Filename: "{app}\Ovygram.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\Ovygram"; Filename: "{app}\Ovygram.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Ovygram.exe"; Description: "{cm:LaunchProgram,Ovygram}"; Flags: nowait postinstall skipifsilent

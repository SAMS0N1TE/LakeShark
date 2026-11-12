#define AppVersion "2.21.4"

[Setup]
AppId={{A5C91551-8E2E-48F2-AC5E-1D24866D8681}
AppName=LakeShark
AppVersion={#AppVersion}
AppVerName=LakeShark {#AppVersion}
AppPublisher=Terminal Bay
VersionInfoVersion=2.21.4.0
DefaultDirName={localappdata}\Programs\LakeShark
DefaultGroupName=LakeShark
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\..\build_installer
OutputBaseFilename=LakeShark-Setup-{#AppVersion}
SetupIconFile=..\..\build_app\LakeShark.ico
UninstallDisplayIcon={app}\LakeShark.exe
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
DisableProgramGroupPage=yes
CloseApplications=yes

[Tasks]
Name: "desktopicon"; Description: "Create desktop shortcuts"; GroupDescription: "Shortcuts:"
Name: "compiler"; Description: "Download the app compiler now (about 706 MB)"; Flags: unchecked

[Files]
Source: "..\..\build_app\dist\LakeShark.exe"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
Type: files; Name: "{group}\LakeShark.lnk"
Type: files; Name: "{autodesktop}\LakeShark.lnk"

[Icons]
Name: "{group}\LakeShark Studio"; Filename: "{app}\LakeShark.exe"; Parameters: "--studio"; Comment: "Write apps for LakeShark boards"
Name: "{group}\LakeShark Desktop"; Filename: "{app}\LakeShark.exe"; Comment: "Every LakeShark app in windows, run by the board"
Name: "{autodesktop}\LakeShark Studio"; Filename: "{app}\LakeShark.exe"; Parameters: "--studio"; Tasks: desktopicon
Name: "{autodesktop}\LakeShark Desktop"; Filename: "{app}\LakeShark.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\LakeShark.exe"; Parameters: "--get-compiler"; Description: "Downloading the app compiler"; StatusMsg: "Downloading and verifying the app compiler (about 706 MB)..."; Tasks: compiler; Flags: runhidden waituntilterminated
Filename: "{app}\LakeShark.exe"; Parameters: "--studio"; Description: "Open LakeShark Studio"; Flags: postinstall nowait skipifsilent
Filename: "{app}\LakeShark.exe"; Parameters: "--studio"; Flags: nowait skipifnotsilent; Check: RelaunchStudio
Filename: "{app}\LakeShark.exe"; Flags: nowait skipifnotsilent; Check: RelaunchDesktop

[Code]
function RelaunchStudio: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|}') = 'studio';
end;

function RelaunchDesktop: Boolean;
begin
  Result := ExpandConstant('{param:RELAUNCH|}') = 'desktop';
end;

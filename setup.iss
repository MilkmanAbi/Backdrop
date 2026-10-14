; Backdrop installer (Inno Setup 6.7.3). Per-user, no admin: installs to %LOCALAPPDATA%\Programs\Backdrop.
; Replaces an earlier install under the old name (VidWall), keeping the chosen wallpaper.
#define AppVer "1.2.0"

[Setup]
AppId={{B3D7A1C4-2E9F-4B6A-8C51-7A0E6F2D9B13}
AppName=Backdrop
AppVersion={#AppVer}
AppVerName=Backdrop {#AppVer}
AppPublisher=Backdrop
DefaultDirName={localappdata}\Programs\Backdrop
DisableDirPage=yes
DisableProgramGroupPage=yes
DisableReadyPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir=dist
OutputBaseFilename=Backdrop-Setup-{#AppVer}
SetupIconFile=backdrop.ico
UninstallDisplayIcon={app}\backdrop.exe
UninstallDisplayName=Backdrop
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
CloseApplications=no
RestartApplications=no
VersionInfoVersion={#AppVer}
VersionInfoDescription=Backdrop setup

[Files]
Source: "backdrop.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "THIRD-PARTY-NOTICES.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\Backdrop"; Filename: "{app}\backdrop.exe"; Comment: "Video wallpaper and colour filters (opens the tray menu)"
Name: "{userprograms}\Backdrop colour filters"; Filename: "{app}\backdrop.exe"; Parameters: "/filters"; Comment: "Pick a colour filter and its strength"

[Run]
Filename: "{app}\backdrop.exe"; Parameters: "/install"; StatusMsg: "Setting Backdrop to start when you sign in..."; Flags: runhidden waituntilterminated
; start it as the signed-in user, never elevated: an elevated tray app can't receive the taskbar's clicks
Filename: "{app}\backdrop.exe"; Description: "Start Backdrop now"; Flags: postinstall nowait; Check: LaunchDirect
Filename: "{sys}\schtasks.exe"; Parameters: "/run /tn ""Backdrop ({username})"""; Description: "Start Backdrop now"; Flags: postinstall runhidden; Check: LaunchViaTask

[UninstallRun]
Filename: "{app}\backdrop.exe"; Parameters: "/uninstall"; RunOnceId: "BackdropCleanup"; Flags: runhidden waituntilterminated

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\Backdrop"
Type: dirifempty; Name: "{app}"

[Code]
const
  OldUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6C3F8E2A-5B1D-4C7E-9A42-1F0D3B7E8C55}_is1';

// a running copy holds the exe open: ask it to quit before files get replaced
procedure CloseByClass(Cls: String);
var
  I: Integer;
begin
  if FindWindowByClassName(Cls) <> 0 then begin
    PostMessage(FindWindowByClassName(Cls), $0010, 0, 0);
    for I := 1 to 50 do begin
      Sleep(100);
      if FindWindowByClassName(Cls) = 0 then Break;
    end;
    Sleep(500);
  end;
end;

procedure CopyDword(Name: String);
var
  V: Cardinal;
begin
  if RegQueryDWordValue(HKCU, 'Software\VidWall', Name, V) then RegWriteDWordValue(HKCU, 'Software\Backdrop', Name, V);
end;

// VidWall was this app's old name: carry the wallpaper choice over, then remove the old install
procedure MigrateFromVidWall();
var
  S: String;
  Code: Integer;
begin
  if RegKeyExists(HKCU, 'Software\VidWall') and not RegKeyExists(HKCU, 'Software\Backdrop') then begin
    if RegQueryStringValue(HKCU, 'Software\VidWall', 'Video', S) then RegWriteStringValue(HKCU, 'Software\Backdrop', 'Video', S);
    CopyDword('Fit');
    CopyDword('Enabled');
    CopyDword('Asked');
  end;
  if RegQueryStringValue(HKCU, OldUninstallKey, 'UninstallString', S) then
    Exec(RemoveQuotes(S), '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

// /nolaunch=1 (used by the one-line PowerShell installer, which starts Backdrop itself)
function WantLaunch(): Boolean;
begin
  Result := ExpandConstant('{param:nolaunch|0}') <> '1';
end;

function LaunchDirect(): Boolean;
begin
  Result := WantLaunch() and not IsAdmin();
end;

// setup was started elevated (e.g. from an admin PowerShell): launch through the logon task, which runs un-elevated
function LaunchViaTask(): Boolean;
begin
  Result := WantLaunch() and IsAdmin();
end;

function InitializeSetup(): Boolean;
begin
  CloseByClass('BackdropMain');
  Result := True;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then MigrateFromVidWall();
end;

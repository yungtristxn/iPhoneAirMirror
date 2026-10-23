; Generated constants/file entries come from Package-Installer.py after exhaustive
; portable-package verification. Compile through that script, not this file alone.
#if Ver < EncodeVer(6, 7, 1)
  #error Inno Setup 6.7.1 or newer is required.
#endif
#include "package-config.iss"

[Setup]
AppId={#InstallerAppId}
AppName=iPhone Air Mirror
AppVersion={#AppVersion}
AppVerName=iPhone Air Mirror {#AppVersion}
DefaultDirName={#InstallDirectory}
DefaultGroupName=iPhone Air Mirror
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0
DisableDirPage=yes
DisableProgramGroupPage=yes
AllowNetworkDrive=no
AllowUNCPath=no
AllowRootDirectory=no
CloseApplications=no
RestartApplications=no
SetupMutex=iPhoneAirMirror.Setup
UninstallFilesDir={app}\uninstall
UninstallDisplayIcon={app}\iPhoneAirMirror.exe
UninstallDisplayName=iPhone Air Mirror
UninstallRestartComputer=no
LicenseFile={#PayloadDirectory}\LICENSE.md
SetupIconFile={#PayloadDirectory}\source\assets\app-icon.ico
OutputDir={#CompilerOutputDirectory}
OutputBaseFilename={#CompilerOutputName}
VersionInfoVersion={#NumericVersion}
VersionInfoTextVersion={#AppVersion}
VersionInfoProductName=iPhone Air Mirror
VersionInfoProductTextVersion={#AppVersion}
Compression=lzma2/fast
SolidCompression=yes
WizardStyle=modern
#if TestOnly
CreateUninstallRegKey=no
UsePreviousAppDir=no
UsePreviousGroup=no
#endif

#if !TestOnly
[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked

[Icons]
Name: "{group}\iPhone Air Mirror"; Filename: "{app}\iPhoneAirMirror.exe"; Parameters: "--data-dir ""{localappdata}\iPhone Air Mirror"""; WorkingDir: "{app}"
Name: "{autodesktop}\iPhone Air Mirror"; Filename: "{app}\iPhoneAirMirror.exe"; Parameters: "--data-dir ""{localappdata}\iPhone Air Mirror"""; WorkingDir: "{app}"; Tasks: desktopicon
#endif

[Files]
#include "package-files.iss"

[Code]
function SetErrorMode(Mode: LongWord): LongWord;
  external 'SetErrorMode@kernel32.dll stdcall';
function FileAttributes(const Filename: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';

function SafeAncestors(Path: String): Boolean;
var
  Attributes: LongWord;
  ErrorCode: Integer;
  Parent: String;
begin
  Result := True;
  Path := RemoveBackslashUnlessRoot(ExpandFileName(Path));
  while Path <> '' do begin
    Attributes := FileAttributes(Path);
    if Attributes = $FFFFFFFF then begin
      ErrorCode := DLLGetLastError;
      if (ErrorCode <> 2) and (ErrorCode <> 3) then begin
        Result := False;
        Exit;
      end;
    end else if (Attributes and $400) <> 0 then begin
      Result := False;
      Exit;
    end;
    Parent := ExtractFileDir(Path);
    if Parent = Path then Break;
    Path := Parent;
  end;
end;

function TestBoundaryValid: Boolean;
begin
#if TestOnly
  Result :=
    (CompareText(RemoveBackslashUnlessRoot(ExpandFileName(ExpandConstant('{app}'))),
                 '{#TestInstallDirectoryPascal}') = 0) and
    SafeAncestors(ExpandConstant('{app}')) and
    SafeAncestors('{#TestDataDirectoryPascal}');
#else
  Result := True;
#endif
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Marker: String;
  Attributes: LongWord;
  ErrorCode: Integer;
begin
  Result := '';
  if not TestBoundaryValid then begin
    Result := 'The test installer is restricted to its compiled workspace app/data paths. Directory overrides, inaccessible ancestors and directory links are refused.';
    Exit;
  end;
  if not SafeAncestors(ExpandConstant('{app}')) then begin
    Result := 'The installation path contains an inaccessible directory or directory link. No existing files were changed.';
    Exit;
  end;
  Marker := ExpandConstant('{app}\installation-owner.txt');
  Attributes := FileAttributes(ExpandConstant('{app}'));
  if Attributes = $FFFFFFFF then begin
    ErrorCode := DLLGetLastError;
    if (ErrorCode <> 2) and (ErrorCode <> 3) then begin
      Result := 'The destination cannot be inspected safely. No existing files were changed.';
      Exit;
    end;
  end else begin
    { Every existing destination needs our marker, even if empty or impossible
      to enumerate. A denied directory listing must never mean "empty". }
    try
      if not FileExists(Marker) then
        Result := 'The destination already exists without the iPhone Air Mirror ownership marker. Use a fresh installation location; no existing files were changed.'
      else if CompareText(GetSHA256OfFile(Marker), '{#OwnerMarkerSha256}') <> 0 then
        Result := 'The destination ownership marker does not match iPhone Air Mirror. No existing files were changed.';
    except
      Result := 'The existing installation ownership marker could not be verified. No existing files were changed.';
    end;
    if Result <> '' then Exit;
  end;
  if not FileExists(ExpandConstant('{sys}\MSVCP140.dll')) or
     not FileExists(ExpandConstant('{sys}\VCRUNTIME140.dll')) or
     not FileExists(ExpandConstant('{sys}\VCRUNTIME140_1.dll')) then
    Result := 'The Microsoft Visual C++ v14 x64 Redistributable is required. Install it from https://aka.ms/vc14/vc_redist.x64.exe, then run this installer again. This installer does not download or install system components.';
end;

function InitializeUninstall: Boolean;
begin
  Result := TestBoundaryValid and SafeAncestors(ExpandConstant('{app}'));
  if not Result then
    SuppressibleMsgBox('The installation path no longer matches its safe location. Uninstall was cancelled without deleting files.', mbError, MB_OK, IDOK);
end;

procedure ProvisionWorker;
var
  ResultCode: Integer;
  PreviousErrorMode: LongWord;
  Started: Boolean;
begin
  { A file callback runs before uninstall-log finalization. A raised exception
    aborts this installation while Inno can still roll back replaced files. }
  { Child processes inherit this mode. Suppress loader/critical-error dialogs
    so a broken or incomplete CRT produces a clear installation error instead
    of leaving unattended setup waiting behind a hidden Windows dialog. }
  PreviousErrorMode := SetErrorMode($8003);
  try
    Started := Exec(ExpandConstant('{app}\iPhoneAirMirror.exe'), '--provision-worker',
                    ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
  finally
    SetErrorMode(PreviousErrorMode);
  end;
  if not Started then
    RaiseException('Could not prepare the restricted codec worker: ' + SysErrorMessage(ResultCode) +
      '. Check folder permissions and install the Microsoft Visual C++ v14 x64 runtime from https://aka.ms/vc14/vc_redist.x64.exe. No receiver was launched.')
  else if ResultCode <> 0 then
    RaiseException('Restricted codec worker preparation failed (exit ' + IntToStr(ResultCode) +
      '). Check folder permissions and install the Microsoft Visual C++ v14 x64 runtime from https://aka.ms/vc14/vc_redist.x64.exe. No receiver was launched.');
end;

{ No [Run], [UninstallRun], [InstallDelete] or [UninstallDelete] entries:
  installation never launches reception or recursively deletes user data. }

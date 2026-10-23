; Generated constants/file entries come from Package-Installer.py after exhaustive
; portable-package verification. Compile through that script, not this file alone.
#if Ver != EncodeVer(6, 7, 1)
  #error The reviewed Inno Setup 6.7.1 compiler is required.
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
MinVersion=10.0.15063
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
Name: "{group}\iPhone Air Mirror"; Filename: "{app}\iPhoneAirMirror.exe"; Parameters: "--data-dir ""{localappdata}\iPhone Air Mirror"""; WorkingDir: "{app}"; Check: WorkerIsReady
Name: "{autodesktop}\iPhone Air Mirror"; Filename: "{app}\iPhoneAirMirror.exe"; Parameters: "--data-dir ""{localappdata}\iPhone Air Mirror"""; WorkingDir: "{app}"; Tasks: desktopicon; Check: WorkerIsReady
#endif

[Files]
#include "package-files.iss"

[Code]
type
  { Pinned Inno 6.7.1 uses a 32-bit Setup engine even for x64 install mode. }
  TStartupInfo32 = record
    cb, Reserved, Desktop, Title, X, Y, XSize, YSize: LongWord;
    XCountChars, YCountChars, FillAttribute, Flags: LongWord;
    ShowWindow, Reserved2Size: Word;
    Reserved2, StdInput, StdOutput, StdError: LongWord;
  end;
  TProcessInfo32 = record
    Process, Thread, ProcessId, ThreadId: LongWord;
  end;

var
  WorkerPrepared: Boolean;
  WorkerFailure: String;

function SetErrorMode(Mode: LongWord): LongWord;
  external 'SetErrorMode@kernel32.dll stdcall';
function FileAttributes(const Filename: String): LongWord;
  external 'GetFileAttributesW@kernel32.dll stdcall';
function CreateWorkerProcess(ApplicationName, CommandLine: String;
  ProcessAttributes, ThreadAttributes: LongWord; InheritHandles: Integer;
  CreationFlags, Environment: LongWord; CurrentDirectory: String;
  var Startup: TStartupInfo32; var ProcessInfo: TProcessInfo32): Integer;
  external 'CreateProcessW@kernel32.dll stdcall';
function WaitForWorker(Handle, Milliseconds: LongWord): LongWord;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function WorkerExitCode(Handle: LongWord; var ExitCode: LongWord): Integer;
  external 'GetExitCodeProcess@kernel32.dll stdcall';
function StopWorker(Handle, ExitCode: LongWord): Integer;
  external 'TerminateProcess@kernel32.dll stdcall';
function CloseWorkerHandle(Handle: LongWord): Integer;
  external 'CloseHandle@kernel32.dll stdcall';

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

function ProvisionExecutable(const Filename: String): String;
var
  Startup: TStartupInfo32;
  ProcessInfo: TProcessInfo32;
  CommandLine: String;
  ResultCode, WaitResult: LongWord;
  PreviousErrorMode: LongWord;
begin
  Result := '';
  Startup.cb := 68;
  Startup.Reserved := 0; Startup.Desktop := 0; Startup.Title := 0;
  Startup.X := 0; Startup.Y := 0; Startup.XSize := 0; Startup.YSize := 0;
  Startup.XCountChars := 0; Startup.YCountChars := 0; Startup.FillAttribute := 0;
  Startup.Flags := $81; { STARTF_USESHOWWINDOW | STARTF_FORCEOFFFEEDBACK }
  Startup.ShowWindow := 0; Startup.Reserved2Size := 0; Startup.Reserved2 := 0;
  Startup.StdInput := 0; Startup.StdOutput := 0; Startup.StdError := 0;
  ProcessInfo.Process := 0; ProcessInfo.Thread := 0;
  ProcessInfo.ProcessId := 0; ProcessInfo.ThreadId := 0;
  CommandLine := '"' + Filename + '" --provision-worker';
  { Inno Exec uses CREATE_DEFAULT_ERROR_MODE. Call CreateProcess directly so
    this error mode is actually inherited and missing DLLs cannot show UI. }
  PreviousErrorMode := SetErrorMode($8003);
  try
    try
      if CreateWorkerProcess(Filename, CommandLine, 0, 0, 0, $08000000, 0,
                             ExtractFileDir(Filename), Startup, ProcessInfo) = 0 then
        Result := 'Could not prepare the restricted codec worker: ' + SysErrorMessage(DLLGetLastError)
      else begin
        CloseWorkerHandle(ProcessInfo.Thread);
        try
          WaitResult := WaitForWorker(ProcessInfo.Process, 15000);
          if WaitResult <> 0 then begin
            StopWorker(ProcessInfo.Process, 1460);
            WaitForWorker(ProcessInfo.Process, 5000);
            Result := 'Restricted codec worker preparation exceeded its bounded wait or could not be monitored';
          end else if WorkerExitCode(ProcessInfo.Process, ResultCode) = 0 then
            Result := 'Could not read the codec worker preparation result'
          else if ResultCode <> 0 then
            Result := Format('Restricted codec worker preparation failed (exit 0x%.8x)', [ResultCode]);
        finally
          CloseWorkerHandle(ProcessInfo.Process);
        end;
      end;
    except
      Result := 'Could not prepare the restricted codec worker: ' + GetExceptionMessage;
    end;
  finally
    SetErrorMode(PreviousErrorMode);
  end;
  if Result <> '' then
    Result := Result + '. Check folder permissions and the Microsoft Visual C++ v14 x64 runtime (https://aka.ms/vc14/vc_redist.x64.exe). Windows N also requires Microsoft''s Media Feature Pack for Media Foundation. No receiver was launched.';
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Marker: String;
  Attributes: LongWord;
  ErrorCode: Integer;
begin
  Result := '';
  WorkerPrepared := False;
  WorkerFailure := 'Required codec worker preparation did not complete.';
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
  if Result <> '' then Exit;
  { PrepareToInstall failures stop setup before application files are changed.
    Probe the exact packaged executable bytes in the installer's temporary
    directory; the installed executable still needs its own separate ACL grant. }
  try
    ExtractTemporaryFile('iPhoneAirMirror-preflight.exe');
    Result := ProvisionExecutable(ExpandConstant('{tmp}\iPhoneAirMirror-preflight.exe'));
  except
    Result := 'The packaged worker prerequisite could not be checked: ' + GetExceptionMessage;
  end;
end;

function InitializeUninstall: Boolean;
begin
  Result := TestBoundaryValid and SafeAncestors(ExpandConstant('{app}'));
  if not Result then
    SuppressibleMsgBox('The installation path no longer matches its safe location. Uninstall was cancelled without deleting files.', mbError, MB_OK, IDOK);
end;

procedure ProvisionWorker;
begin
  { Inno deliberately swallows file-callback exceptions. Record failure instead
    and make the final status/exit code fail closed; do not claim rollback. }
  try
    WorkerFailure := ProvisionExecutable(ExpandConstant('{app}\iPhoneAirMirror.exe'));
    WorkerPrepared := WorkerFailure = '';
  except
    WorkerPrepared := False;
    WorkerFailure := 'Installed worker preparation failed: ' + GetExceptionMessage;
  end;
  if not WorkerPrepared then begin
    Log(WorkerFailure);
    SuppressibleMsgBox(WorkerFailure + #13#10 +
      'Application files were copied but the app is not ready. Correct the permissions/runtime and rerun setup, or uninstall. Existing user data is retained.',
      mbError, MB_OK, IDOK);
  end;
end;

function WorkerIsReady: Boolean;
begin
  Result := WorkerPrepared;
end;

function GetCustomSetupExitCode: Integer;
begin
  if WorkerPrepared then Result := 0 else Result := 20;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if (CurPageID = wpFinished) and not WorkerPrepared then begin
    WizardForm.FinishedHeadingLabel.Caption := 'iPhone Air Mirror needs repair';
    WizardForm.FinishedLabel.Caption := WorkerFailure + #13#10#13#10 +
      'Application files were copied, but required worker preparation failed. Rerun setup after fixing the problem, or uninstall. User data is retained.';
    WizardForm.AdjustLabelHeight(WizardForm.FinishedLabel);
  end;
end;

{ No [Run], [UninstallRun], [InstallDelete] or [UninstallDelete] entries:
  installation never launches reception or recursively deletes user data. }

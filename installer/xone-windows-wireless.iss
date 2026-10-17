; SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AppVersion
  #define AppVersion "0.1.0-alpha.6"
#endif
#ifndef StageDir
  #error StageDir must point to a verified public release stage
#endif
[Setup]
#ifdef InstallerSmokeTest
AppId={{40E8C37A-1A80-41D6-A4BB-0578AD2B7A19}
AppName=xone-windows-wireless installer smoke test
DefaultDirName={localappdata}\Programs\xone-windows-wireless-smoke
DefaultGroupName=xone-windows-wireless-smoke
OutputBaseFilename=xone-installer-smoke
#else
AppId={{40E8C37A-1A80-41D6-A4BB-0578AD2B7A18}
AppName=xone Wireless
DefaultDirName={localappdata}\Programs\xone-windows-wireless
DefaultGroupName=xone-windows-wireless
OutputBaseFilename=xone-windows-wireless-{#AppVersion}-Setup-x64
#endif
AppVersion={#AppVersion}
AppVerName=xone Wireless {#AppVersion}
AppPublisher=Ktoto59
AppPublisherURL=https://github.com/Ktoto59/xone-windows-wireless
AppSupportURL=https://github.com/Ktoto59/xone-windows-wireless/issues
AppUpdatesURL=https://github.com/Ktoto59/xone-windows-wireless/releases
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableDirPage=yes
DisableReadyPage=no
OutputDir=..\dist
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
SetupIconFile=..\assets\branding\app.ico
UninstallDisplayIcon={app}\outputs\xone-background.exe
CloseApplications=no
RestartApplications=no
SetupLogging=no
UsePreviousTasks=yes

[Tasks]
Name: "autostart"; Description: "Start automatically when I sign in to Windows"; GroupDescription: "Startup:"
Name: "startnow"; Description: "Start xone Wireless after installation"; GroupDescription: "Startup:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Start xone Wireless"; Filename: "{app}\outputs\xone-background.exe"; WorkingDir: "{app}"
Name: "{group}\Stop xone Wireless"; Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass -File ""{app}\scripts\Setup-Action.ps1"" -Action Stop"; WorkingDir: "{app}"; IconFilename: "{app}\outputs\xone-background.exe"
Name: "{group}\Uninstall xone-windows-wireless"; Filename: "{uninstallexe}"; IconFilename: "{app}\outputs\xone-background.exe"; IconIndex: 0

[Code]
function NeedRestart(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\work\setup-restart-required.json'));
end;
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Keep the Xbox Wireless Adapter connected.' + NewLine + NewLine +
    'Setup prepares the adapter and required Windows components, then applies the startup options below.' + NewLine +
    'Windows asks for administrator approval for adapter setup. The first USBip installation can briefly restart USB hubs.' + NewLine + NewLine +
    MemoTasksInfo + NewLine + NewLine +
    'Do not update headset/controller firmware through this bridge. Stop it and use a physical USB data cable.' + NewLine + NewLine +
    'Use the Start menu shortcuts to start or stop the bridge later. No control window or tray icon is required.';
end;
function StartupChoice: String;
begin
  if WizardIsTaskSelected('autostart') then Result := 'on' else Result := 'off';
end;
function MaintenanceResultPath(Action: String): String;
begin
  Result := ExpandConstant('{app}\work\setup-') + Action + '.json';
end;

function MaintenanceFailure(Action, Message: String; ResultCode: Integer): String;
var
  Lines: TArrayOfString;
  I: Integer;
begin
  Result := Message + #13#10 + 'Exit code: ' + IntToStr(ResultCode);
  if LoadStringsFromFile(MaintenanceResultPath(Action) + '.error.txt', Lines) then begin
    for I := 0 to GetArrayLength(Lines) - 1 do begin
      if I >= 8 then Break;
      Result := Result + #13#10 + Copy(Lines[I], 1, 350);
    end;
  end;
  Result := Result + #13#10 + #13#10 + 'Local details: ' + MaintenanceResultPath(Action);
end;

function RunMaintenance(Action: String; Elevated: Boolean; var ResultCode: Integer): Boolean;
var
  ProgramPath, Parameters: String;
begin
#ifdef InstallerSmokeTest
  { Separate compiled test product: never executes setup/driver/startup hooks. }
  if Action = 'InitializePreferences' then begin
    RegWriteStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{40E8C37A-1A80-41D6-A4BB-0578AD2B7A19}_is1', 'SmokeStartup', StartupChoice);
    RegWriteDWordValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{40E8C37A-1A80-41D6-A4BB-0578AD2B7A19}_is1', 'SmokeStartRequested', 0);
  end;
  if Action = 'Start' then
    RegWriteDWordValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{40E8C37A-1A80-41D6-A4BB-0578AD2B7A19}_is1', 'SmokeStartRequested', 1);
  ResultCode := 0;
  Result := True;
  Exit;
#endif
  ResultCode := 0;
  ProgramPath := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  DeleteFile(MaintenanceResultPath(Action) + '.error.txt');
  Parameters := '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\scripts\Setup-Action.ps1') + '" -Action ' + Action + ' -ResultPath "' + MaintenanceResultPath(Action) + '"';
  if Action = 'InitializePreferences' then Parameters := Parameters + ' -Startup ' + StartupChoice;
  if Elevated then
    Result := ShellExec('runas', ProgramPath, Parameters, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode)
  else
    Result := Exec(ProgramPath, Parameters, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;
procedure ApplyMaintenance(Action, Status, Failure: String; Elevated: Boolean);
var
  ResultCode: Integer;
begin
  WizardForm.StatusLabel.Caption := Status;
  WizardForm.FilenameLabel.Caption := '';
  if not RunMaintenance(Action, Elevated, ResultCode) then begin
    if Elevated then
      RaiseException(Failure + #13#10 + 'Windows approval may have been cancelled. Run Setup again to retry.')
    else RaiseException(MaintenanceFailure(Action, Failure, ResultCode));
  end;
  if ResultCode <> 0 then RaiseException(MaintenanceFailure(Action, Failure, ResultCode));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    ApplyMaintenance('InitializePreferences', 'Saving installation preferences...', 'Could not save installation preferences.', False);
    ApplyMaintenance('AutomaticSetup', 'Preparing adapter and Windows components...', 'Adapter setup did not complete.', True);
    ApplyMaintenance('ConfigureStartup', 'Applying Windows sign-in settings...', 'Could not apply startup settings.', False);
    if WizardIsTaskSelected('startnow') and not NeedRestart then
      ApplyMaintenance('Start', 'Starting xone Wireless...', 'Could not start the bridge.', False);
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then begin
    if NeedRestart and not WizardIsTaskSelected('autostart') then
      WizardForm.FinishedLabel.Caption := 'Restart Windows to complete adapter setup. Then use Start xone Wireless from the Start menu. Automatic startup is off.'
    else if not NeedRestart and not WizardIsTaskSelected('startnow') then
      WizardForm.FinishedLabel.Caption := 'Setup is complete. Use Start xone Wireless from the Start menu when you are ready to connect your devices.';
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\scripts\Setup-Action.ps1')) then begin
    if not RunMaintenance('Upgrade', False, ResultCode) then
      Result := 'Could not stop the previous bridge. Close it and retry.'
    else if ResultCode <> 0 then
      Result := 'The previous bridge could not be stopped. Keep its recovery data and retry after it exits.';
  end;
end;

function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  Result := RunMaintenance('Remove', FileExists(ExpandConstant('{app}\work\binding-state.json')), ResultCode);
  if Result then Result := ResultCode = 0;
  if not Result then
    MsgBox('Uninstall was stopped because the bridge or stock-driver restoration could not be verified. Connect the adapter to its original USB port and retry. Your files and recovery backup have been kept.', mbError, MB_OK)
  else if not UninstallSilent then
    MsgBox('The bridge is stopped and any stock binding owned by this installation is restored. Reconnect the adapter. Private work/config files and shared USBip are kept; remove them separately only when no longer needed.', mbInformation, MB_OK);
end;

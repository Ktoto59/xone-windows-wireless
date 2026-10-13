; SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AppVersion
  #define AppVersion "0.1.0-alpha.4"
#endif
#ifndef StageDir
  #error StageDir must point to a verified public release stage
#endif
[Setup]
AppId={{40E8C37A-1A80-41D6-A4BB-0578AD2B7A18}
AppName=xone-windows-wireless
AppVersion={#AppVersion}
AppVerName=xone-windows-wireless {#AppVersion}
AppPublisher=Ktoto59
AppPublisherURL=https://github.com/Ktoto59/xone-windows-wireless
AppSupportURL=https://github.com/Ktoto59/xone-windows-wireless/issues
AppUpdatesURL=https://github.com/Ktoto59/xone-windows-wireless/releases
DefaultDirName={localappdata}\Programs\xone-windows-wireless
DefaultGroupName=xone-windows-wireless
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.22000
DisableProgramGroupPage=yes
DisableWelcomePage=yes
DisableDirPage=yes
DisableReadyPage=no
OutputDir=..\dist
OutputBaseFilename=xone-windows-wireless-{#AppVersion}-Setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\outputs\xone-background.exe
CloseApplications=no
RestartApplications=no
SetupLogging=no

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Uninstall xone-windows-wireless"; Filename: "{uninstallexe}"

[Code]
function NeedRestart(): Boolean;
begin
  Result := FileExists(ExpandConstant('{app}\work\setup-restart-required.json'));
end;
function UpdateReadyMemo(Space, NewLine, MemoUserInfoInfo, MemoDirInfo,
  MemoTypeInfo, MemoComponentsInfo, MemoGroupInfo, MemoTasksInfo: String): String;
begin
  Result := 'Keep the Xbox Wireless Adapter connected.' + NewLine + NewLine +
    'Setup prepares WinUSB, verified firmware and the signed USBip dependency, then enables an invisible C++ host at sign-in.' + NewLine +
    'Windows asks for administrator approval. USB hubs may briefly restart during the first USBip installation.' + NewLine + NewLine +
    'No control window, device-selection dialog, overlay or Python runtime is installed.';
end;
function RunMaintenance(Action: String; Elevated: Boolean; var ResultCode: Integer): Boolean;
var
  ProgramPath, Parameters: String;
begin
  ProgramPath := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
  Parameters := '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\scripts\Setup-Action.ps1') + '" -Action ' + Action;
  if Elevated then
    Result := ShellExec('runas', ProgramPath, Parameters, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode)
  else
    Result := Exec(ProgramPath, Parameters, ExpandConstant('{app}'), SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then begin
    if not RunMaintenance('InitializePreferences', False, ResultCode) then
      RaiseException('Could not save installation preferences.');
    if ResultCode <> 0 then
      RaiseException('Could not save installation preferences.');
    if not RunMaintenance('AutomaticSetup', True, ResultCode) then
      RaiseException('Windows did not approve adapter setup. Run Setup again with the adapter connected.');
    if ResultCode <> 0 then
      RaiseException('Adapter setup did not complete. Keep the adapter connected and run Setup again.');
    if not RunMaintenance('AutomaticEnable', False, ResultCode) then
      RaiseException('Could not enable the background host. Run Setup again.');
    if ResultCode <> 0 then
      RaiseException('Background startup did not complete. Run Setup again.');
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

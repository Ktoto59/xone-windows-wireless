; SPDX-License-Identifier: GPL-2.0-or-later
#ifndef AppVersion
  #define AppVersion "0.1.0-alpha.2"
#endif
#ifndef StageDir
  #error StageDir must point to a verified public release stage
#endif
#ifndef PythonDir
  #error PythonDir must point to the verified embeddable runtime
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
LicenseFile=..\LICENSE
InfoBeforeFile=preview-notice.txt
OutputDir=..\dist
OutputBaseFilename=xone-windows-wireless-{#AppVersion}-Setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\outputs\xone-control.exe
CloseApplications=no
RestartApplications=no
SetupLogging=yes

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"
Name: "automatic"; Description: "Start the background bridge automatically at sign-in"; GroupDescription: "Background operation:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PythonDir}\*"; DestDir: "{app}\runtime\python"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\xone-windows-wireless"; Filename: "{app}\outputs\xone-control.exe"; WorkingDir: "{app}"
Name: "{group}\Uninstall xone-windows-wireless"; Filename: "{uninstallexe}"
Name: "{autodesktop}\xone-windows-wireless"; Filename: "{app}\outputs\xone-control.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{sys}\WindowsPowerShell\v1.0\powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\scripts\Set-AutomaticBridge.ps1"" -Mode Enable"; WorkingDir: "{app}"; Tasks: automatic; Flags: runhidden
Filename: "{app}\outputs\xone-control.exe"; Description: "Open the first-run setup and control panel"; Flags: nowait postinstall skipifsilent

[Code]
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

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Result := '';
  if FileExists(ExpandConstant('{app}\scripts\Setup-Action.ps1')) then begin
    if not RunMaintenance('Upgrade', False, ResultCode) then
      Result := 'Could not stop the previous bridge. Close it and retry.'
    else if ResultCode <> 0 then
      Result := 'The previous bridge could not be stopped. Keep its recovery data and inspect the local work logs.';
  end;
end;

function InitializeUninstall(): Boolean;
var
  ResultCode: Integer;
begin
  Result := RunMaintenance('Remove', FileExists(ExpandConstant('{app}\work\binding-state.json')), ResultCode);
  if Result then Result := ResultCode = 0;
  if not Result then
    MsgBox('Uninstall was stopped because the bridge or stock-driver restoration could not be verified. Connect the adapter to its original USB port, close the control panel, and use Restore stock driver before retrying. Your files and recovery backup have been kept.', mbError, MB_OK)
  else if not UninstallSilent then
    MsgBox('The bridge is stopped and any stock binding owned by this installation is restored. Reconnect the adapter. Private work/config files and shared USBip are kept; remove them separately only when no longer needed.', mbInformation, MB_OK);
end;

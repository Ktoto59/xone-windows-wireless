# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([switch]$Advanced,[string]$RenderPreview='',[switch]$PreviewWelcome)
$ErrorActionPreference='Stop'
if($Advanced){& (Join-Path $PSScriptRoot 'Control-Panel.ps1');return}
$script:appRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$script:appOperation=$null;$script:appAction='';$script:appResult='';$script:appAdapter=$null;$script:appPrepared=$false;$script:appAdvanced=$false;$script:appLock=$null
if(-not $RenderPreview){
    [void][IO.Directory]::CreateDirectory((Join-Path $appRoot 'work'))
    try{$script:appLock=[IO.FileStream]::new((Join-Path $appRoot 'work/control-panel.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
    catch{[void][Windows.Forms.MessageBox]::Show('The application is already open.','xone-windows-wireless');return}
    if(-not ('Xone.AdapterHealth' -as [type])){Add-Type -Path (Join-Path $appRoot 'src/adapter_health.cs')}
}
function App-Error([string]$Text){[void][Windows.Forms.MessageBox]::Show($Text,'xone-windows-wireless',[Windows.Forms.MessageBoxButtons]::OK,[Windows.Forms.MessageBoxIcon]::Error)}
function App-Action([string]$Action,[string]$Profile=''){
    if($script:appOperation){return}
    $script:appResult=Join-Path $appRoot ('work/app-result-'+[guid]::NewGuid().ToString('N')+'.json')
    $arguments='-NoProfile -ExecutionPolicy Bypass -File '+(Quote-XonePath (Join-Path $PSScriptRoot 'Setup-Action.ps1'))+' -Action '+$Action+' -ResultPath '+(Quote-XonePath $appResult)
    if($Action -in @('Configure','Enroll')){
        if(-not $script:appAdapter){throw 'Connect one Xbox Wireless Adapter first.'}
        $arguments+=' -InstanceId '+(Quote-XonePath $script:appAdapter.InstanceId)
    }
    if($Profile){$arguments+=' -Profile '+(Quote-XonePath $Profile)}
    $parameters=@{FilePath=(Join-Path $env:windir 'System32/WindowsPowerShell/v1.0/powershell.exe');ArgumentList=$arguments;WorkingDirectory=$appRoot;WindowStyle='Hidden';PassThru=$true}
    if($Action -eq 'Configure'){$parameters.Verb='RunAs'}
    $script:appOperation=Start-Process @parameters;$script:appAction=$Action
    $primary.Enabled=$false;$settings.Enabled=$false;$startup.Enabled=$false
    $status.Text=switch($Action){'Configure'{'Preparing your adapter...'};'Enroll'{'Waiting for your device...'};default{'Updating background operation...'}}
    $detail.Text=if($Action -eq 'Enroll'){'Turn on pairing on the selected device, then press the adapter pairing button.'}elseif($Action -eq 'Configure'){'Approve Windows UAC. Required dependencies and the adapter driver are prepared automatically.'}else{'This window can be closed after the operation finishes.'}
}
function App-StartBackground{
    App-Action $(if($startup.Checked){'AutomaticEnable'}else{'Start'})
}
function App-AddDevice{
    $dialog=[Windows.Forms.Form]::new();$dialog.Text='Add an Xbox device';$dialog.Font=$form.Font;$dialog.ClientSize=[Drawing.Size]::new(520,220);$dialog.StartPosition='CenterParent';$dialog.FormBorderStyle='FixedDialog';$dialog.MaximizeBox=$false;$dialog.MinimizeBox=$false
    $instructions=[Windows.Forms.Label]::new();$instructions.Text="Choose your model. Turn off other Xbox devices while adding this one.`r`nThen put this device in pairing mode and press the adapter pairing button.";$instructions.SetBounds(20,18,480,65);$dialog.Controls.Add($instructions)
    $models=[Windows.Forms.ComboBox]::new();$models.DropDownStyle='DropDownList';$models.SetBounds(20,94,480,30)
    [void]$models.Items.Add('Xbox Series controller');[void]$models.Items.Add('Xbox Wireless Headset 2021 (green ring)');[void]$models.Items.Add('Xbox Wireless Headset 2024 (no green ring)');$models.SelectedIndex=0;$dialog.Controls.Add($models)
    $connect=[Windows.Forms.Button]::new();$connect.Text='Connect';$connect.SetBounds(345,156,155,36);$connect.DialogResult='OK';$dialog.AcceptButton=$connect;$dialog.Controls.Add($connect)
    try{if($dialog.ShowDialog($form) -eq 'OK'){$profiles=@('controller-0b12-usb.json','headset-0b17-usb.json','headset-0b32-usb.json');App-Action 'Enroll' $profiles[$models.SelectedIndex]}}finally{$dialog.Dispose()}
}
function App-Refresh{
    if($RenderPreview -or $script:appOperation){return}
    $script:appAdapter=[Xone.AdapterHealth]::Find()
    $config=Read-XoneJson (Join-Path $appRoot 'config/devices.local.json')
    $script:appPrepared=$script:appAdapter -and $appAdapter.Service -eq 'WinUSB' -and $config -and [IO.File]::Exists((Join-Path $appRoot 'work/firmware/FW_ACC_00U.bin'))
    $maintenance=Read-XoneJson (Join-Path $appRoot 'work/maintenance-status.json')
    $state=Read-XoneJson (Join-Path $appRoot 'work/usb-gip-running.json')
    $list.Items.Clear();$connected=0
    foreach($entry in @($config.Devices)){
        if(-not $entry){continue}
        $name=switch($entry.Profile){'controller-0b12-usb.json'{'Xbox Series controller'};'headset-0b17-usb.json'{'Xbox Wireless Headset 2021'};'headset-0b32-usb.json'{'Xbox Wireless Headset 2024'};default{'Xbox device'}}
        $peer=Read-XoneJson (Join-Path $appRoot ('work/usb-gip-'+$entry.Name+'-radio-status.json'))
        $ready=$state -and $state.Running -and $peer -and $peer.Ready
        if($ready){$connected++}
        $item=[Windows.Forms.ListViewItem]::new($name);[void]$item.SubItems.Add($(if($ready){'Connected'}else{'Off / waiting'}));[void]$list.Items.Add($item)
    }
    $primary.Text=if($appPrepared){'Add device'}else{'Set up adapter'};$primary.Enabled=[bool]$appAdapter
    $status.Text=if(-not $appAdapter){'Connect your Xbox Wireless Adapter'}elseif(-not $appPrepared){'Let us set up your adapter'}elseif($connected){"$connected device(s) connected"}else{'Ready for your Xbox devices'}
    $detail.Text=if(-not $appAdapter){'Plug the adapter into USB. Setup will become available automatically.'}elseif(-not $appPrepared){'Setup prepares the required signed drivers and enables background operation.'}elseif(-not $list.Items.Count){'Add your controller or headset once. Reconnects are handled in the background.'}else{'Turn on a saved device. You can close this window; the bridge keeps running.'}
    if($maintenance -and $maintenance.Error){$detail.Text=$maintenance.Error}
    if($state -and $state.Error){$detail.Text=$state.Error}
}
$form=[Windows.Forms.Form]::new();$form.Text='xone-windows-wireless';$form.Font=[Drawing.Font]::new('Segoe UI',11);$form.ClientSize=[Drawing.Size]::new(650,480);$form.MinimumSize=$form.Size;$form.StartPosition='CenterScreen';$form.BackColor=[Drawing.Color]::White
$title=[Windows.Forms.Label]::new();$title.Text='Xbox Wireless';$title.Font=[Drawing.Font]::new('Segoe UI',23,[Drawing.FontStyle]::Bold);$title.SetBounds(28,22,590,48);$form.Controls.Add($title)
$status=[Windows.Forms.Label]::new();$status.Text='Connect your Xbox Wireless Adapter';$status.Font=[Drawing.Font]::new('Segoe UI',14);$status.SetBounds(30,91,590,34);$form.Controls.Add($status)
$detail=[Windows.Forms.Label]::new();$detail.Text='Plug the adapter into USB to get started.';$detail.ForeColor=[Drawing.Color]::DimGray;$detail.SetBounds(30,132,590,54);$form.Controls.Add($detail)
$primary=[Windows.Forms.Button]::new();$primary.Text='Set up adapter';$primary.SetBounds(30,197,590,44);$primary.Add_Click({try{if($script:appPrepared){App-AddDevice}else{App-Action 'Configure'}}catch{App-Error $_.Exception.Message}});$form.Controls.Add($primary)
$list=[Windows.Forms.ListView]::new();$list.View='Details';$list.FullRowSelect=$true;$list.HeaderStyle='Nonclickable';$list.SetBounds(30,261,590,110);[void]$list.Columns.Add('Your devices',405);[void]$list.Columns.Add('Status',155);$form.Controls.Add($list)
$startup=[Windows.Forms.CheckBox]::new();$startup.Text='Start automatically with Windows';$startup.SetBounds(30,389,405,28)
$preferences=Read-XoneJson (Join-Path $appRoot 'config/app.local.json');$startup.Checked=if($preferences){[bool]$preferences.AutoStart}else{$true};$form.Controls.Add($startup)
$settings=[Windows.Forms.Button]::new();$settings.Text='Advanced';$settings.SetBounds(482,386,138,33);$settings.Add_Click({if(-not $script:appOperation){$script:appAdvanced=$true;$form.Close()}});$form.Controls.Add($settings)
$footer=[Windows.Forms.Label]::new();$footer.Text='Preview | Device firmware updates are not supported.';$footer.ForeColor=[Drawing.Color]::DimGray;$footer.SetBounds(30,438,590,26);$form.Controls.Add($footer)
$startup.Add_CheckedChanged({
    if($RenderPreview -or $script:appOperation){return}
    try{Write-XoneJson (Join-Path $appRoot 'config/app.local.json') @{Version=1;AutoStart=$startup.Checked};if($script:appPrepared){App-Action $(if($startup.Checked){'AutomaticEnable'}else{'AutomaticDisable'})}}catch{App-Error $_.Exception.Message}
})
$timer=[Windows.Forms.Timer]::new();$timer.Interval=2000;$timer.Add_Tick({
    try{
        if($script:appOperation){
            $script:appOperation.Refresh()
            if($script:appOperation.HasExited){
                $result=Read-XoneJson $script:appResult;$action=$script:appAction;$script:appOperation=$null
                $primary.Enabled=$true;$settings.Enabled=$true;$startup.Enabled=$true
                if(-not $result -or -not $result.Success){App-Error $(if($result.Error){$result.Error}else{'Windows approval was cancelled or setup did not finish.'});App-Refresh}
                elseif($action -in @('Configure','Enroll')){
                    Write-XoneJson (Join-Path $appRoot 'config/app.local.json') @{Version=1;AutoStart=$startup.Checked}
                    App-StartBackground
                    if($result.ReconnectRequired){[void][Windows.Forms.MessageBox]::Show('Unplug and reconnect the adapter once, then add your devices.','xone-windows-wireless')}
                }elseif($action -eq 'AutomaticDisable'){App-Action 'Start'}else{App-Refresh}
            }
        }else{App-Refresh}
    }catch{$detail.Text=$_.Exception.Message}
})
$form.Add_FormClosing({param($sender,$eventArgs)if($script:appOperation){$eventArgs.Cancel=$true;App-Error 'Wait for setup or device pairing to finish.'}})
try{
    if($RenderPreview){
        if(-not $PreviewWelcome){$status.Text='2 devices connected';$detail.Text='Turn on a saved device. You can close this window; the bridge keeps running.';$primary.Text='Add device';foreach($name in @('Xbox Series controller','Xbox Wireless Headset 2024')){$item=[Windows.Forms.ListViewItem]::new($name);[void]$item.SubItems.Add('Connected');[void]$list.Items.Add($item)}}
        $form.StartPosition='Manual';$form.Location=[Drawing.Point]::new(-30000,-30000);$form.Show();[Windows.Forms.Application]::DoEvents();$bitmap=[Drawing.Bitmap]::new($form.Width,$form.Height)
        try{$form.DrawToBitmap($bitmap,[Drawing.Rectangle]::new(0,0,$bitmap.Width,$bitmap.Height));$bitmap.Save([IO.Path]::GetFullPath($RenderPreview),[Drawing.Imaging.ImageFormat]::Png)}finally{$bitmap.Dispose()};$form.Close()
    }else{
        $form.Add_Shown({try{App-Refresh;$active=Read-XoneJson (Join-Path $appRoot 'work/maintenance-status.json');if($script:appPrepared -and -not ($active -and (Test-XoneProcess $active.Process))){App-StartBackground}}catch{$detail.Text=$_.Exception.Message}})
        $timer.Start();[void]$form.ShowDialog()
    }
}finally{$timer.Stop();$timer.Dispose();$form.Dispose();if($script:appLock){$script:appLock.Dispose()}}
if($script:appAdvanced){Start-Process -FilePath (Join-Path $appRoot 'outputs/xone-control.exe') -ArgumentList '--advanced' -WindowStyle Hidden}

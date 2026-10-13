# SPDX-License-Identifier: GPL-2.0-or-later
[CmdletBinding()]
param([string]$RenderPreview='',[ValidateRange(0,2)][int]$PreviewTab=0)
$ErrorActionPreference='Stop'
$script:taskRoot=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'Setup-Helpers.ps1')
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$script:taskOperation=$null;$script:taskResultPath='';$script:taskDiscoveryPath='';$script:taskAction=''
$script:taskEntries=@();$script:taskUiButtons=@();$script:taskLastStatus=[datetime]::MinValue
$script:taskLock=$null
if(-not $RenderPreview){
    [void][IO.Directory]::CreateDirectory((Join-Path $taskRoot 'work'))
    try{$script:taskLock=[IO.FileStream]::new((Join-Path $taskRoot 'work/control-panel.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)}
    catch{[void][Windows.Forms.MessageBox]::Show('The control panel is already open.','xone-windows-wireless');return}
}
function Show-XoneError([string]$Message){[void][Windows.Forms.MessageBox]::Show($Message,'xone-windows-wireless',[Windows.Forms.MessageBoxButtons]::OK,[Windows.Forms.MessageBoxIcon]::Error)}
function Confirm-Xone([string]$Message){return [Windows.Forms.MessageBox]::Show($Message,'xone-windows-wireless',[Windows.Forms.MessageBoxButtons]::OKCancel,[Windows.Forms.MessageBoxIcon]::Information) -eq [Windows.Forms.DialogResult]::OK}
function Add-XoneLabel($Parent,[string]$Text,[int]$X,[int]$Y,[int]$Width,[int]$Height){
    $label=[Windows.Forms.Label]::new();$label.Text=$Text;$label.SetBounds($X,$Y,$Width,$Height);$Parent.Controls.Add($label);return $label
}
function Add-XoneButton($Parent,[string]$Text,[int]$X,[int]$Y,[int]$Width,[scriptblock]$Handler){
    $button=[Windows.Forms.Button]::new();$button.Text=$Text;$button.SetBounds($X,$Y,$Width,34)
    $button.Add_Click({try{& $Handler}catch{Show-XoneError $_.Exception.Message}}.GetNewClosure())
    $Parent.Controls.Add($button);$script:taskUiButtons+=@($button);return $button
}
function Start-XoneUiAction([string]$Action,[switch]$Elevated){
    if($script:taskOperation){throw 'Wait for the current operation to finish.'}
    $script:taskResultPath=Join-Path $taskRoot ('work/setup-result-'+[guid]::NewGuid().ToString('N')+'.json')
    $script:taskAction=$Action
    $arguments='-NoProfile -ExecutionPolicy Bypass -File '+(Quote-XonePath (Join-Path $PSScriptRoot 'Setup-Action.ps1'))+' -Action '+$Action+' -ResultPath '+(Quote-XonePath $taskResultPath)
    if($Action -in @('Bind','Restore','Discover')){
        if($taskAdapterBox.SelectedIndex -lt 0){throw 'Connect and select one external 045E:02FE Xbox Wireless Adapter, then click Refresh.'}
        $arguments+=' -InstanceId '+(Quote-XonePath $taskAdapterBox.SelectedItem.InstanceId)
    }
    if($Action -eq 'Discover'){
        $script:taskDiscoveryPath=Join-Path $taskRoot ('work/discovery-'+[guid]::NewGuid().ToString('N')+'.log')
        $taskMacBox.Items.Clear();$arguments+=' -DiscoveryPath '+(Quote-XonePath $taskDiscoveryPath)
    }
    $parameters=@{FilePath=(Join-Path $env:windir 'System32/WindowsPowerShell/v1.0/powershell.exe');ArgumentList=$arguments;WindowStyle='Hidden';PassThru=$true;WorkingDirectory=$taskRoot}
    if($Elevated){$parameters.Verb='RunAs'}
    try{
        $script:taskOperation=Start-Process @parameters
        foreach($button in $taskUiButtons){$button.Enabled=$false}
        $taskFinishDiscovery.Enabled=$Action -eq 'Discover'
        $taskOperationLabel.Text=if($Elevated){'Waiting for Windows UAC / '+$Action+'...'}else{$Action+' in progress...'}
    }catch{$script:taskAction='';throw}
}
function Refresh-XoneEntries {
    $taskDeviceList.Items.Clear()
    foreach($entry in $script:taskEntries){[void]$taskDeviceList.Items.Add(($entry.Name+' | '+$entry.Mac+' | '+$entry.Profile))}
}
function Update-XoneUiStatus {
    if($RenderPreview){return}
    $status=Get-XoneSetupStatus $taskRoot
    $selected=if($taskAdapterBox.SelectedIndex -ge 0){$taskAdapterBox.SelectedItem.InstanceId}else{''}
    $taskAdapterBox.Items.Clear()
    foreach($adapter in $status.Adapters){[void]$taskAdapterBox.Items.Add($adapter)}
    $taskAdapterBox.DisplayMember='InstanceId'
    for($index=0;$index -lt $taskAdapterBox.Items.Count;$index++){
        if($taskAdapterBox.Items[$index].InstanceId -eq $selected){$taskAdapterBox.SelectedIndex=$index}
    }
    if($taskAdapterBox.SelectedIndex -lt 0 -and $taskAdapterBox.Items.Count -eq 1){$taskAdapterBox.SelectedIndex=0}
    $taskSetupStatus.Text=('Bundled runtime: '+$(if($status.Python){'ready'}else{'missing'})+"`r`nUSBip virtual controller: "+$(if($status.Usbip.Ready){'ready ('+$status.Usbip.Version+')'}else{'not ready'})+"`r`nAdapter firmware: "+$(if($status.Firmware){'verified'}else{'not imported'})+"`r`nAdapter driver: "+$(if($taskAdapterBox.SelectedIndex -ge 0){$taskAdapterBox.SelectedItem.Service}else{'adapter not selected'}))
    $state=Read-XoneJson (Join-Path $taskRoot 'work/usb-gip-running.json')
    $maintenance=Read-XoneJson (Join-Path $taskRoot 'work/maintenance-status.json')
    $taskBridgeStatus.Text=if($maintenance -and (Test-XoneProcess $maintenance.Process)){
        'Background bridge: '+$maintenance.Phase+$(if($maintenance.Error){' | '+$maintenance.Error}else{''})
    }elseif($state -and (Test-XoneProcess $state.Worker)){
        if($state.Error){'Bridge error: '+$state.Error}else{'Bridge is running. Power on your configured devices.'}
    }elseif($state -and $state.Error){'Bridge stopped: '+$state.Error}else{'Bridge is stopped.'}
    if($state -and $state.BridgeBackend){$taskBridgeStatus.Text+=' | Runtime: '+$state.BridgeBackend}
    $script:taskLastStatus=Get-Date
}

$taskForm=[Windows.Forms.Form]::new()
$taskManifest=Read-XoneJson (Join-Path $taskRoot 'release-manifest.json')
$taskForm.Text='xone-windows-wireless | '+$(if($taskManifest){$taskManifest.Version}else{'development'})
$taskForm.Font=[Drawing.Font]::new('Segoe UI',10)
$taskForm.ClientSize=[Drawing.Size]::new(790,610);$taskForm.MinimumSize=$taskForm.Size
$taskForm.StartPosition='CenterScreen';$taskForm.AutoScaleMode='Dpi'
$title=Add-XoneLabel $taskForm 'Xbox Wireless setup & control' 22 16 740 32
$title.Font=[Drawing.Font]::new('Segoe UI',16,[Drawing.FontStyle]::Bold)
[void](Add-XoneLabel $taskForm 'A research preview for the external Xbox Wireless Adapter (045E:02FE).' 24 53 740 26)
$tabs=[Windows.Forms.TabControl]::new();$tabs.SetBounds(20,90,750,465);$taskForm.Controls.Add($tabs)
$setup=[Windows.Forms.TabPage]::new('1. Adapter');$devices=[Windows.Forms.TabPage]::new('2. Devices');$bridge=[Windows.Forms.TabPage]::new('3. Start / stop')
$tabs.TabPages.AddRange(@($setup,$devices,$bridge))
[void](Add-XoneLabel $setup 'Prepare dependencies, then select WinUSB. Reconnect the adapter after switching.' 16 18 700 42)
$script:taskAdapterBox=[Windows.Forms.ComboBox]::new();$taskAdapterBox.SetBounds(16,65,540,30);$taskAdapterBox.DropDownStyle='DropDownList';$setup.Controls.Add($taskAdapterBox)
[void](Add-XoneButton $setup 'Refresh' 570 63 140 {Update-XoneUiStatus})
$script:taskSetupStatus=Add-XoneLabel $setup '' 16 110 694 115
[void](Add-XoneButton $setup 'Prepare dependencies' 16 236 220 {
    if(Confirm-Xone 'The wizard imports adapter firmware locally or downloads it from Microsoft. If USBip is missing, it downloads and installs the signed 0.9.8.1 release. USB hubs may restart briefly, interrupting USB devices. Windows may request administrator approval. No automatic PC restart will occur.') {Start-XoneUiAction 'Prepare'}
})
[void](Add-XoneButton $setup 'Use WinUSB' 252 236 210 {
    if(Confirm-Xone 'This backs up the stock driver and switches only the selected adapter to WinUSB. Your Xbox devices will then require this bridge. Reconnect the adapter afterwards. Windows will request administrator approval.') {Start-XoneUiAction 'Bind' -Elevated}
})
[void](Add-XoneButton $setup 'Restore stock driver' 478 236 232 {
    if(Confirm-Xone 'Stop this bridge and restore the stock driver saved by this installation. Reconnect the adapter afterwards. Windows will request administrator approval.') {Start-XoneUiAction 'Restore' -Elevated}
})
[void](Add-XoneLabel $setup "Firmware stays private on this PC. Secure Boot and test-signing are unchanged.`r`nIf WinUSB was selected by a different installation, keep its recovery backup.`r`nThis wizard only restores recovery state saved in this installation." 16 299 695 96)

[void](Add-XoneLabel $devices 'Turn off all other Xbox devices. Choose the model, click Search, then turn on or pair that device using its pairing button and the adapter button.' 16 18 700 50)
$script:taskModelBox=[Windows.Forms.ComboBox]::new();$taskModelBox.SetBounds(16,76,370,30);$taskModelBox.DropDownStyle='DropDownList'
[void]$taskModelBox.Items.Add('Xbox Series controller (0B12)');[void]$taskModelBox.Items.Add('Xbox Wireless Headset 2021 / green ring (0B17)');[void]$taskModelBox.Items.Add('Xbox Wireless Headset 2024 / no ring (0B32)');$taskModelBox.SelectedIndex=0;$devices.Controls.Add($taskModelBox)
[void](Add-XoneButton $devices 'Search for device' 403 73 145 {
    if(Confirm-Xone "Searching stops this installation's bridge for up to 90 seconds. Turn on only the model you selected. Press the pairing buttons if it does not connect. The local radio address will appear in the list.") {Start-XoneUiAction 'Discover'}
})
$script:taskFinishDiscovery=Add-XoneButton $devices 'Finish search' 563 73 145 {
    Set-Content -LiteralPath (Join-Path $taskRoot 'work/discovery.stop') -Value stop
};$taskFinishDiscovery.Enabled=$false
$script:taskMacBox=[Windows.Forms.ComboBox]::new();$taskMacBox.SetBounds(16,128,370,30);$taskMacBox.DropDownStyle='DropDownList';$devices.Controls.Add($taskMacBox)
[void](Add-XoneButton $devices 'Add selected device' 403 124 305 {
    if($taskMacBox.SelectedIndex -lt 0){throw 'Search and select an observed radio address first.'}
    $names=@('controller','older-headset','newer-headset');$profiles=@('controller-0b12-usb.json','headset-0b17-usb.json','headset-0b32-usb.json');$index=$taskModelBox.SelectedIndex
    $entry=[pscustomobject]@{Name=$names[$index];Mac=[string]$taskMacBox.SelectedItem;Profile=$profiles[$index]}
    $next=@($script:taskEntries | Where-Object {$_.Profile -ne $entry.Profile})+@($entry)
    if(@($next | Where-Object {$_.Mac -eq $entry.Mac}).Count -gt 1){throw 'That address is already assigned to another model. Search with only one device on.'}
    $script:taskEntries=$next;Refresh-XoneEntries
})
$script:taskDeviceList=[Windows.Forms.ListBox]::new();$taskDeviceList.SetBounds(16,178,692,125);$devices.Controls.Add($taskDeviceList)
[void](Add-XoneButton $devices 'Remove selected' 16 317 210 {
    if($taskDeviceList.SelectedIndex -lt 0){throw 'Select a configured device first.'}
    $index=$taskDeviceList.SelectedIndex;$script:taskEntries=@(for($i=0;$i -lt $script:taskEntries.Count;$i++){if($i -ne $index){$script:taskEntries[$i]}});Refresh-XoneEntries
})
[void](Add-XoneButton $devices 'Save devices' 242 317 466 {
    if($taskAdapterBox.SelectedIndex -lt 0){throw 'Select the adapter first.'}
    Stop-XoneManagedBridge $taskRoot
    Set-XoneSetupConfig $taskRoot $taskAdapterBox.SelectedItem.InstanceId $script:taskEntries
    $taskOperationLabel.Text='Devices saved locally. Open Start / stop to start the bridge.'
})
[void](Add-XoneLabel $devices 'One device per model is supported. Addresses and configuration stay on this PC.' 16 368 700 40)

$script:taskBridgeStatus=Add-XoneLabel $bridge 'Bridge is stopped.' 16 22 690 58
[void](Add-XoneButton $bridge 'Start bridge' 16 97 215 {Start-XoneUiAction 'Start'})
[void](Add-XoneButton $bridge 'Stop bridge' 248 97 215 {Start-XoneUiAction 'Stop'})
[void](Add-XoneButton $bridge 'Refresh status' 480 97 230 {Update-XoneUiStatus})
[void](Add-XoneButton $bridge 'Enable automatic mode' 16 143 330 {Start-XoneUiAction 'AutomaticEnable'})
[void](Add-XoneButton $bridge 'Disable automatic mode' 363 143 347 {Start-XoneUiAction 'AutomaticDisable'})
[void](Add-XoneLabel $bridge 'Bridge version:' 16 198 150 28)
$script:taskBackendBox=[Windows.Forms.ComboBox]::new();$taskBackendBox.SetBounds(164,195,370,30);$taskBackendBox.DropDownStyle='DropDownList'
[void]$taskBackendBox.Items.Add('C++ bridge (preview)');[void]$taskBackendBox.Items.Add('Python reference bridge');$taskBackendBox.SelectedIndex=1;$bridge.Controls.Add($taskBackendBox)
[void](Add-XoneButton $bridge 'Apply and restart' 550 193 160 {Start-XoneUiAction $(if($taskBackendBox.SelectedIndex -eq 0){'UseCpp'}else{'UsePython'})})
[void](Add-XoneLabel $bridge "Automatic mode starts at sign-in and recovers after USB loss or sleep.`r`nStop ends this session; Disable also removes the sign-in task.`r`nSelect the Xbox audio endpoint in Windows or your app.`r`nApplying a bridge version briefly interrupts audio; device pairing is preserved.`r`nPreview: reconnection and simultaneous devices need further validation.`r`nAccessories can show Connected. Firmware updates are unavailable." 16 235 690 128)
[void](Add-XoneButton $bridge 'Open local logs' 16 371 215 {Start-Process -FilePath explorer.exe -ArgumentList (Quote-XonePath (Join-Path $taskRoot 'work'))})
[void](Add-XoneButton $bridge 'Open documentation' 248 371 215 {Start-Process 'https://github.com/Ktoto59/xone-windows-wireless#installation-and-first-run'})
$script:taskOperationLabel=Add-XoneLabel $taskForm 'Ready. Use the three tabs to complete the first-time setup.' 23 566 740 32

$config=Read-XoneJson (Join-Path $taskRoot 'config/devices.local.json')
if($config){$script:taskEntries=@($config.Devices);Refresh-XoneEntries;if($config.BridgeBackend -eq 'cpp'){$taskBackendBox.SelectedIndex=0}}
$timer=[Windows.Forms.Timer]::new();$timer.Interval=500
$timer.Add_Tick({
    try{
        if($script:taskOperation){
            if($script:taskAction -eq 'Discover' -and (Test-Path -LiteralPath $script:taskDiscoveryPath)){
                $macs=@(Get-XoneDiscoveredMacs ([IO.File]::ReadAllText($script:taskDiscoveryPath)))
                foreach($mac in $macs){if(-not $taskMacBox.Items.Contains($mac)){[void]$taskMacBox.Items.Add($mac)}}
                if($taskMacBox.SelectedIndex -lt 0 -and $taskMacBox.Items.Count -eq 1){$taskMacBox.SelectedIndex=0}
            }
            $script:taskOperation.Refresh()
            if($script:taskOperation.HasExited){
                $result=Read-XoneJson $script:taskResultPath
                $action=$script:taskAction;$script:taskOperation=$null;$script:taskAction=''
                foreach($button in $taskUiButtons){$button.Enabled=$true};$taskFinishDiscovery.Enabled=$false
                if($result -and $result.Success){
                    $taskOperationLabel.Text=if($action -eq 'Bind' -or $action -eq 'Restore'){'Driver changed. Unplug/reconnect the adapter in the same USB port.'}elseif($action -eq 'Discover'){'Search finished. Select the address and model, add the device, then save.'}else{$action+' completed.'}
                }else{
                    $errorText=if($result -and $result.Error){$result.Error}else{'Operation ended without a result. Windows approval may have been cancelled.'}
                    $taskOperationLabel.Text='Operation failed.';Show-XoneError $errorText
                }
                Update-XoneUiStatus
            }
        }elseif(((Get-Date)-$script:taskLastStatus).TotalSeconds -gt 5){Update-XoneUiStatus}
    }catch{$taskOperationLabel.Text=$_.Exception.Message}
})
$taskForm.Add_FormClosing({param($sender,$eventArgs)
    if($script:taskOperation){$eventArgs.Cancel=$true;Show-XoneError 'Wait for the current operation to finish. For discovery, click Finish search.'}
})
try{
    if($RenderPreview){
        $tabs.SelectedIndex=$PreviewTab
        $taskForm.StartPosition='Manual';$taskForm.Location=[Drawing.Point]::new(-30000,-30000)
        $taskSetupStatus.Text="Bundled runtime: ready`r`nUSBip virtual controller: ready (0.9.8.1)`r`nAdapter firmware: verified`r`nAdapter driver: WinUSB"
        $taskForm.CreateControl();$taskForm.Show();[Windows.Forms.Application]::DoEvents()
        $bitmap=[Drawing.Bitmap]::new($taskForm.Width,$taskForm.Height)
        try{$taskForm.DrawToBitmap($bitmap,[Drawing.Rectangle]::new(0,0,$bitmap.Width,$bitmap.Height));$bitmap.Save([IO.Path]::GetFullPath($RenderPreview),[Drawing.Imaging.ImageFormat]::Png)}finally{$bitmap.Dispose()}
        $taskForm.Close()
    }else{
        $taskForm.Add_Shown({try{Update-XoneUiStatus}catch{Show-XoneError $_.Exception.Message}})
        $timer.Start();[void]$taskForm.ShowDialog()
    }
}finally{$timer.Stop();$timer.Dispose();$taskForm.Dispose();if($script:taskLock){$script:taskLock.Dispose()}}

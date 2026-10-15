# SPDX-License-Identifier: GPL-2.0-or-later
# Privileged, explicit preparation for a locally test-signed kernel driver.
[CmdletBinding()]
param([ValidateSet('Prepare','EnableTestSigning','RestoreTestSettings')][string]$Mode='Prepare')
$ErrorActionPreference='Stop'
$taskRoot=Split-Path -Parent $PSScriptRoot
$taskWork=Join-Path $taskRoot 'work/native-test'
$taskResultPath=Join-Path $taskWork 'admin-result.json'
$taskResult=[ordered]@{Success=$false;Mode=$Mode;Error=$null;SecureBoot=$null;BitLockerProtection=$null;TestSigningChanged=$false;RestartRequired=$false;AdminRoot=$null}
$taskBcd=Join-Path $env:windir 'System32/bcdedit.exe'
function Save-NativeJson([string]$Path,$Value){
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}
try{
    $taskIdentity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $taskPrincipal=[Security.Principal.WindowsPrincipal]::new($taskIdentity)
    if(-not $taskPrincipal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator privileges are required.'}
    $taskPreparation=Get-Content -LiteralPath (Join-Path $taskWork 'preparation.json') -Raw | ConvertFrom-Json
    $taskThumbprint=[string]$taskPreparation.CertificateThumbprint
    if($taskThumbprint -notmatch '^[0-9A-F]{40}$'){throw 'Invalid development certificate identity.'}
    $taskAdminRoot=Join-Path $env:ProgramData ('xone-native-test-'+$taskThumbprint.Substring(0,12))
    $taskResult.AdminRoot=$taskAdminRoot
    if(Test-Path -LiteralPath $taskAdminRoot){
        if((Get-Item -LiteralPath $taskAdminRoot).Attributes -band [IO.FileAttributes]::ReparsePoint){throw 'Refusing a redirected preparation directory.'}
    }else{
        [void][IO.Directory]::CreateDirectory($taskAdminRoot)
    }
    $taskAcl=[Security.AccessControl.DirectorySecurity]::new()
    $taskAcl.SetAccessRuleProtection($true,$false)
    $taskInheritance=[Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit'
    foreach($taskSid in @('S-1-5-18','S-1-5-32-544')){
        $taskAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($taskSid),'FullControl',$taskInheritance,'None','Allow'))
    }
    $taskAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($taskIdentity.User,'ReadAndExecute',$taskInheritance,'None','Allow'))
    Set-Acl -LiteralPath $taskAdminRoot -AclObject $taskAcl
    $taskBeforePath=Join-Path $taskAdminRoot 'before.json'
    $taskResult.SecureBoot=(Get-ItemProperty 'HKLM:/SYSTEM/CurrentControlSet/Control/SecureBoot/State').UEFISecureBootEnabled
    try{
        $taskEncryption=Get-CimInstance -Namespace root/CIMV2/Security/MicrosoftVolumeEncryption -ClassName Win32_EncryptableVolume -Filter ('DriveLetter="'+$env:SystemDrive+'"')
        $taskResult.BitLockerProtection=if($taskEncryption){[int]$taskEncryption.ProtectionStatus}else{'NotApplicable'}
    }catch{$taskResult.BitLockerProtection='Unavailable'}
    if(Test-Path -LiteralPath $taskBeforePath){
        $taskBefore=Get-Content -LiteralPath $taskBeforePath -Raw | ConvertFrom-Json
    }else{
        if($Mode -ne 'Prepare'){throw 'Run Prepare before changing or restoring test settings.'}
        $taskBcdText=@(& $taskBcd /enum 2>&1)
        if($LASTEXITCODE -ne 0){throw 'Cannot read the current boot configuration.'}
        [IO.File]::WriteAllLines((Join-Path $taskAdminRoot 'bcd-before.txt'),[string[]]$taskBcdText)
        & $taskBcd /export (Join-Path $taskAdminRoot 'bcd-before.backup')
        if($LASTEXITCODE -ne 0){throw 'Cannot save boot configuration backup.'}
        $taskTestSigningLine=@($taskBcdText | Where-Object {$_ -match '^\s*testsigning\s+'})
        $taskBefore=[pscustomobject]@{SecureBoot=$taskResult.SecureBoot;TestSigningLine=$taskTestSigningLine;TestSigningChanged=$false;CertificateThumbprint=$taskThumbprint;CertificateStores=@()}
        foreach($taskStore in @('Root','TrustedPublisher')){
            $taskBefore.CertificateStores+=,[pscustomobject]@{Store=$taskStore;AlreadyPresent=(Test-Path ('Cert:/LocalMachine/'+$taskStore+'/'+$taskThumbprint));AddedByThisPreparation=$false}
        }
        Save-NativeJson $taskBeforePath $taskBefore
    }
    if($taskBefore.CertificateThumbprint -ne $taskThumbprint){throw 'Preparation belongs to another development certificate.'}
    if($Mode -eq 'RestoreTestSettings'){
        if($taskBefore.TestSigningChanged){
            if($taskBefore.TestSigningLine.Count -eq 0){& $taskBcd /deletevalue testsigning}
            elseif(($taskBefore.TestSigningLine -join ' ') -match '\s+(Yes|\u0414\u0430)\s*$'){& $taskBcd /set testsigning on}
            else{& $taskBcd /set testsigning off}
            if($LASTEXITCODE -ne 0){throw 'Cannot restore the original test-signing setting.'}
            $taskBefore.TestSigningChanged=$false;$taskResult.RestartRequired=$true
        }
        foreach($taskStore in $taskBefore.CertificateStores){
            $taskCertificatePath='Cert:/LocalMachine/'+$taskStore.Store+'/'+$taskThumbprint
            if($taskStore.AddedByThisPreparation -and (Test-Path $taskCertificatePath)){
                Remove-Item -LiteralPath $taskCertificatePath
                $taskStore.AddedByThisPreparation=$false
            }
        }
        Save-NativeJson $taskBeforePath $taskBefore
    }elseif($Mode -eq 'Prepare'){
        $taskCertificateFile=Join-Path $taskWork 'xone-native-development.cer'
        $taskCertificate=[Security.Cryptography.X509Certificates.X509Certificate2]::new($taskCertificateFile)
        if($taskCertificate.Thumbprint -ne $taskThumbprint -or $taskCertificate.Subject -ne 'CN=xone-native local development'){throw 'Unexpected certificate.'}
        Copy-Item -LiteralPath $taskCertificateFile -Destination $taskAdminRoot
        foreach($taskStore in $taskBefore.CertificateStores){
            if(-not $taskStore.AlreadyPresent -and -not $taskStore.AddedByThisPreparation){
                Import-Certificate -FilePath $taskCertificateFile -CertStoreLocation ('Cert:/LocalMachine/'+$taskStore.Store) | Out-Null
                $taskStore.AddedByThisPreparation=$true
                Save-NativeJson $taskBeforePath $taskBefore
            }
        }
        foreach($taskFile in @('xone-native.sys','xone-native.inf','xone-native.cat')){
            Copy-Item -LiteralPath (Join-Path $taskRoot ('outputs/native/'+$taskFile)) -Destination $taskAdminRoot
        }
    }else{
        if($taskResult.SecureBoot -ne 0){throw 'Disable Secure Boot in UEFI before enabling TESTSIGNING.'}
        if($taskResult.BitLockerProtection -eq 1 -or $taskResult.BitLockerProtection -eq 'Unavailable'){
            throw 'Boot encryption protection must be reviewed before changing boot settings. No encryption settings were changed.'
        }
        # Record ownership before mutation so an interruption still leaves a rollback.
        $taskBefore.TestSigningChanged=$true;Save-NativeJson $taskBeforePath $taskBefore
        & $taskBcd /set testsigning on
        if($LASTEXITCODE -ne 0){throw 'Windows rejected TESTSIGNING.'}
        $taskResult.TestSigningChanged=$true;$taskResult.RestartRequired=$true
    }
    $taskResult.Success=$true
}catch{$taskResult.Error=$_.Exception.Message}
finally{Save-NativeJson $taskResultPath $taskResult}
if(-not $taskResult.Success){exit 1}

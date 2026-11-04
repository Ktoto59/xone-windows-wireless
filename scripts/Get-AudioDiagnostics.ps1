param(
    [string]$RuntimeRoot = (Join-Path $env:LOCALAPPDATA 'Programs\xone-windows-wireless'),
    [string]$EvidenceDirectory = (Join-Path $PSScriptRoot ('..\work\audio-observation-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))),
    [ValidateRange(2,10)][int]$Samples = 3,
    [ValidateRange(30,600)][int]$IntervalSeconds = 60
)
$ErrorActionPreference = 'Stop'
$hostBinary = Join-Path $RuntimeRoot 'outputs\xone-background.exe'
if (!(Test-Path -LiteralPath $hostBinary)) { throw "Runtime host missing: $hostBinary" }
New-Item -ItemType Directory -Force -Path $EvidenceDirectory | Out-Null
$counters = @('AudioRxPackets','AudioTxPackets','RenderExpiredRequests','RenderDroppedFrames',
    'RenderRephases','RenderPredictedPackets','RenderExpiredBeforeTx','RenderTxExpired','RenderTxFailed',
    'RenderPcmExpiredFrames','RenderSuccessAfterDeadline','TimerWakes','TimerLate1ms','TimerLate8ms',
    'UsbImports','UsbClosed','AudioResyncs','MailboxDroppedAudio','StaleAudioEvents')
$gauges = @('RenderPendingHighWater','RenderQueueHighWater','RenderPeriodNs','MaxMailboxDelayUs',
    'MaxRadioBatchUs','MaxSocketDispatchUs','MaxRadioAudioGapUs','MaxRenderLatenessUs',
    'MaxRenderPcmAgeUs','MaxTimerLatenessUs','MaxTxCompletionDispatchUs','MaxUsbReplyFlushUs')
$previous = $null
$intervals = @()
for ($sample = 1; $sample -le $Samples; $sample++) {
    # One RAM snapshot per interval. No device changes or payload capture.
    $text = & $hostBinary --status | Out-String
    $current = $text | ConvertFrom-Json
    if (!$current.Slots) { throw 'No connected runtime snapshot available' }
    $text | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $EvidenceDirectory "status-$sample.json")
    if ($previous) {
        for ($slot = 0; $slot -lt $current.Slots.Count; $slot++) {
            $a = $previous.Slots[$slot]; $b = $current.Slots[$slot]
            if (!$b.Connector) { continue }
            $same = $null -ne $a.Generation -and $a.Generation -eq $b.Generation -and $a.UsbImports -eq $b.UsbImports
            $delta = [ordered]@{}
            if ($same) { foreach ($key in $counters) {
                if ($null -ne $a.$key -and $null -ne $b.$key) { $delta[$key] = [long]$b.$key - [long]$a.$key }
            } }
            $values = [ordered]@{}
            foreach ($key in $gauges) { if ($null -ne $b.$key) { $values[$key] = $b.$key } }
            $radio = @($current.Radio.Controllers | Where-Object { $_.Wcid -eq $b.Wcid })
            $priorRadio = @($previous.Radio.Controllers | Where-Object { $_.Wcid -eq $a.Wcid })
            $txDelta = [ordered]@{}
            if ($same -and $radio.Count -eq 1 -and $priorRadio.Count -eq 1 -and $radio[0].SessionGeneration -eq $priorRadio[0].SessionGeneration) {
                foreach ($key in @('Packets','Over8ms','Expired','Failed')) { $txDelta[$key] = [long]$radio[0].UsbTx.$key - [long]$priorRadio[0].UsbTx.$key }
            }
            $entry = [ordered]@{Slot=$slot; Connector=$b.Connector; SameImport=$same; Ready=$b.Ready;
                Seconds=([long]$b.PublishedTickMs-[long]$a.PublishedTickMs)/1000;
                Fresh=($b.SnapshotSequence -ne $a.SnapshotSequence); Delta=$delta; HighWaterAndLast=$values;
                RadioTxDelta=$txDelta; RadioTxLast= $(if ($radio.Count -eq 1) {$radio[0].UsbTx} else {$null})}
            $intervals += $entry
            $entry | ConvertTo-Json -Depth 6
        }
    }
    $previous = $current
    if ($sample -lt $Samples) { Start-Sleep -Seconds $IntervalSeconds }
}
ConvertTo-Json -InputObject @($intervals) -Depth 8 | Set-Content -Encoding UTF8 -LiteralPath (Join-Path $EvidenceDirectory 'intervals.json')
Write-Output "Saved diagnostics to $EvidenceDirectory"

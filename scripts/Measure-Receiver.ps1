param([Parameter(Mandatory=$true)][int]$ReceiverId, [int]$Seconds=60, [int]$Interval=5)
$ErrorActionPreference='Stop'
if($Seconds -lt 1 -or $Seconds -gt 7200 -or $Interval -lt 1 -or $Interval -gt 30){throw 'Invalid measurement interval'}
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root ('local/measure-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.jsonl')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
$receiver = Get-Process -Id $ReceiverId
$receiverStartTicks = $receiver.StartTime.ToUniversalTime().Ticks
$watch=[Diagnostics.Stopwatch]::StartNew()
$previous=@{}
while($true){
    $main=Get-Process -Id $ReceiverId -ErrorAction SilentlyContinue
    if(-not $main -or $main.StartTime.ToUniversalTime().Ticks -ne $receiverStartTicks){break}
    $pids=@($ReceiverId)+@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$ReceiverId" | Where-Object { $_.Name -ieq 'iPhoneAirMirror.exe' -or $_.Name -ieq 'AirMirrorLite.exe' -or $_.Name -ieq 'airmirror-lite.exe' } | ForEach-Object { [int]$_.ProcessId })
    $rows=@();$current=@{}
    foreach($taskId in $pids){
        $p=Get-Process -Id $taskId -ErrorAction SilentlyContinue;if(-not $p){continue}
        $identity='{0}:{1}' -f $taskId,$p.StartTime.ToUniversalTime().Ticks
        $cpu=$p.TotalProcessorTime.TotalSeconds;$sampleAt=$watch.Elapsed.TotalSeconds;$percent=$null
        if($previous.ContainsKey($identity)){
            $before=$previous[$identity];$intervalSeconds=$sampleAt-$before.at
            if($intervalSeconds -gt 0){$percent=[Math]::Round(100*($cpu-$before.cpu)/$intervalSeconds,3)}
        }
        $rows+=@{pid=$taskId;role=$(if($taskId -eq $ReceiverId){'host'}else{'codec'});cpuPercentOfOneCore=$percent;workingSetBytes=$p.WorkingSet64;privateBytes=$p.PrivateMemorySize64;handles=$p.HandleCount;threads=$p.Threads.Count}
        $current[$identity]=@{cpu=$cpu;at=$sampleAt}
    }
    $previous=$current;$elapsed=$watch.Elapsed.TotalSeconds
    @{utc=[DateTime]::UtcNow.ToString('o');elapsedSeconds=[Math]::Round($elapsed,3);processes=$rows} | ConvertTo-Json -Depth 5 -Compress | Add-Content -LiteralPath $output -Encoding utf8
    if($elapsed -ge $Seconds){break}
    # Sample the endpoint even when work/sleep crosses it. Anchor cadence to the
    # stopwatch rather than accumulating the cost of each CIM/process query.
    $nextSample=[Math]::Min($Seconds,([Math]::Floor($elapsed/$Interval)+1)*$Interval)
    $sleepMs=[Math]::Max(0,[Math]::Ceiling(1000*($nextSample-$watch.Elapsed.TotalSeconds)))
    if($sleepMs -gt 0){Start-Sleep -Milliseconds $sleepMs}
}
Write-Output $output

param([Parameter(Mandatory=$true)][int]$ReceiverId, [int]$Seconds=60, [int]$Interval=5)
$ErrorActionPreference='Stop'
if($Seconds -lt 1 -or $Seconds -gt 7200 -or $Interval -lt 1 -or $Interval -gt 30){throw 'Invalid measurement interval'}
$root=Split-Path -Parent $PSScriptRoot
$output=Join-Path $root ('local/measure-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '.jsonl')
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $output) | Out-Null
$watch=[Diagnostics.Stopwatch]::StartNew()
$previous=@{};$lastTime=0.0
while($watch.Elapsed.TotalSeconds -le $Seconds){
    $main=Get-Process -Id $ReceiverId -ErrorAction SilentlyContinue
    if(-not $main){break}
    $pids=@($ReceiverId)+@(Get-CimInstance Win32_Process -Filter "ParentProcessId=$ReceiverId" | Where-Object { $_.Name -ieq 'AirMirrorLite.exe' -or $_.Name -ieq 'airmirror-lite.exe' } | ForEach-Object { [int]$_.ProcessId })
    $elapsed=$watch.Elapsed.TotalSeconds;$intervalSeconds=$elapsed-$lastTime;$rows=@()
    foreach($taskId in $pids){
        $p=Get-Process -Id $taskId -ErrorAction SilentlyContinue;if(-not $p){continue}
        $cpu=$p.TotalProcessorTime.TotalSeconds;$percent=$null
        if($previous.ContainsKey($taskId)-and $intervalSeconds -gt 0){$percent=[Math]::Round(100*($cpu-$previous[$taskId])/$intervalSeconds,3)}
        $rows+=@{pid=$taskId;role=$(if($taskId -eq $ReceiverId){'host'}else{'codec'});cpuPercentOfOneCore=$percent;workingSetBytes=$p.WorkingSet64;privateBytes=$p.PrivateMemorySize64;handles=$p.HandleCount;threads=$p.Threads.Count}
        $previous[$taskId]=$cpu
    }
    @{utc=[DateTime]::UtcNow.ToString('o');elapsedSeconds=[Math]::Round($elapsed,3);processes=$rows} | ConvertTo-Json -Depth 5 -Compress | Add-Content -LiteralPath $output -Encoding utf8
    $lastTime=$elapsed
    if($elapsed -ge $Seconds){break}
    Start-Sleep -Seconds ([Math]::Min($Interval,[Math]::Max(1,$Seconds-[int]$elapsed)))
}
Write-Output $output

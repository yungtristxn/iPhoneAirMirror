param([string]$Executable = '')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $Executable) { $Executable = Join-Path $root 'target/debug/airmirror-lite.exe' }
$data = Join-Path $root 'local/control-test'
New-Item -ItemType Directory -Force -Path $data | Out-Null
$process = Start-Process -FilePath $Executable -ArgumentList @('--no-discovery','--hidden','--bind','127.0.0.1','--port','17000','--seconds','12','--data-dir',('"' + $data + '"')) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $data 'stdout.txt') -RedirectStandardError (Join-Path $data 'stderr.txt')
function Send-Request([string]$Request) {
    $client = [Net.Sockets.TcpClient]::new()
    try { $client.Connect('127.0.0.1',17000); $stream=$client.GetStream(); $stream.ReadTimeout=2000
        $bytes=[Text.Encoding]::ASCII.GetBytes($Request);$stream.Write($bytes,0,$bytes.Length)
        $buffer=[byte[]]::new(8192);$count=$stream.Read($buffer,0,$buffer.Length)
        if ($count) { return [Text.Encoding]::ASCII.GetString($buffer,0,$count) } else { return '' }
    } finally { $client.Dispose() }
}
try {
    $ready=$false
    for($i=0;$i -lt 50;$i++) {
        if ($process.HasExited) { throw ('Receiver exited: ' + (Get-Content -Raw (Join-Path $data 'stderr.txt'))) }
        try { $response=Send-Request "OPTIONS * RTSP/1.0`r`nCSeq: 1`r`n`r`n"; if($response -match '200 OK'){$ready=$true;break} } catch {}
        Start-Sleep -Milliseconds 100
    }
    if(-not $ready){throw 'Listener did not become ready'}
    $response=Send-Request "GET /info RTSP/1.0`r`nCSeq: 2`r`n`r`n"
    if($response -notmatch '200 OK'){throw '/info failed'}
    $response=Send-Request "RECORD rtsp://localhost/1 RTSP/1.0`r`nCSeq: 3`r`n`r`n"
    if($response -notmatch '455'){throw 'RECORD before SETUP was not rejected'}
    # Hold one owner open; another connection must not replace it.
    $first=[Net.Sockets.TcpClient]::new();$second=[Net.Sockets.TcpClient]::new()
    try {
        $first.Connect('127.0.0.1',17000);$first.GetStream().ReadTimeout=2000
        $request=[Text.Encoding]::ASCII.GetBytes("POST /pair-setup RTSP/1.0`r`nCSeq: 10`r`nContent-Length: 32`r`n`r`n") + [byte[]]::new(32)
        $first.GetStream().Write($request,0,$request.Length);$buffer=[byte[]]::new(8192)
        $n=$first.GetStream().Read($buffer,0,$buffer.Length)
        if([Text.Encoding]::ASCII.GetString($buffer,0,$n) -notmatch '200 OK'){throw 'First owner was not admitted'}
        $second.Connect('127.0.0.1',17000);$second.GetStream().ReadTimeout=2000
        $second.GetStream().Write($request,0,$request.Length);$n=$second.GetStream().Read($buffer,0,$buffer.Length)
        if([Text.Encoding]::ASCII.GetString($buffer,0,$n) -notmatch '453'){throw 'Second sender replaced owner'}
    } finally { $first.Dispose();$second.Dispose() }
    foreach($header in @('Content-Length: 65537','Content-Length: -1',"Content-Length: 2`r`nContent-Length: 2",'Transfer-Encoding: chunked')) {
        try { $response=Send-Request "POST /fp-setup RTSP/1.0`r`nCSeq: 4`r`n$header`r`n`r`n";if($response -match '200 OK'){throw 'Malformed input accepted'} }
        catch [System.IO.IOException] {} # A rejected malformed connection may reset.
    }
    $response=Send-Request "OPTIONS * RTSP/1.0`r`nCSeq: 5`r`n`r`n"
    if($response -notmatch '200 OK'){throw 'Listener did not survive malformed input'}
    if(-not $process.WaitForExit(15000)){throw 'Receiver did not exit on deadline'}
    if($process.ExitCode -ne 0){throw ('Receiver exit failure: ' + $process.ExitCode)}
    $probe=[Net.Sockets.TcpClient]::new();try{$probe.Connect('127.0.0.1',17000);throw 'Listener remains after shutdown'}catch [Net.Sockets.SocketException]{}finally{$probe.Dispose()}
    'PASS: real loopback control requests, one-owner admission, malformed framing/state rejection, listener survives, timed shutdown removes listener.'
} finally { if(-not $process.HasExited){Stop-Process -Id $process.Id -ErrorAction SilentlyContinue} }

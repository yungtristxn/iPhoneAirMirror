param([Parameter(Mandatory=$true)][int]$ReceiverId)
$ErrorActionPreference='Stop'
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class MirrorWindowTest {
 [DllImport("user32.dll")] public static extern int GetClassName(IntPtr h,StringBuilder name,int size);
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect rect);
 [DllImport("user32.dll",SetLastError=true)] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint msg,IntPtr wp,IntPtr lp);
 [DllImport("user32.dll",EntryPoint="GetWindowLongPtrW")] public static extern IntPtr GetWindowLongPtr(IntPtr h,int index);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr value);
}
'@
$process=Get-Process -Id $ReceiverId
$h=$process.MainWindowHandle
if($h -eq [IntPtr]::Zero){throw 'Receiver window missing'}
$class=[Text.StringBuilder]::new(256);[void][MirrorWindowTest]::GetClassName($h,$class,256)
if($class.ToString() -ne 'AirMirrorLiteWindow'){throw ('Expected receiver window, found ' + $class.ToString() + ': ' + $process.MainWindowTitle)}
$oldDpi=[MirrorWindowTest]::SetThreadDpiAwarenessContext([IntPtr](-4))
$rect=[MirrorWindowTest+Rect]::new()
if(-not [MirrorWindowTest]::GetWindowRect($h,[ref]$rect)){throw 'Could not read window rectangle'}
$originalStyle=[MirrorWindowTest]::GetWindowLongPtr($h,-16).ToInt64()
try {
    foreach($size in @(@(800,500),@(600,900),@(1200,700))) {
        if(-not [MirrorWindowTest]::SetWindowPos($h,[IntPtr]::Zero,$rect.Left,$rect.Top,$size[0],$size[1],0x14)){throw ('Resize failed: Win32 error ' + [Runtime.InteropServices.Marshal]::GetLastWin32Error())}
        Start-Sleep -Milliseconds 800
    }
    [void][MirrorWindowTest]::PostMessage($h,0x100,[IntPtr]0x7a,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    $fullscreenStyle=[MirrorWindowTest]::GetWindowLongPtr($h,-16).ToInt64()
    if(($fullscreenStyle -band 0x00c00000) -ne 0){throw 'F11 did not remove window caption'}
    [void][MirrorWindowTest]::PostMessage($h,0x100,[IntPtr]0x7a,[IntPtr]::Zero)
    Start-Sleep -Milliseconds 800
    if([MirrorWindowTest]::GetWindowLongPtr($h,-16).ToInt64() -ne $originalStyle){throw 'F11 did not restore window style'}
    'PASS: three real window resizes and fullscreen/restore; inspect receiver counters separately for stream continuity.'
} finally {
    [void][MirrorWindowTest]::SetWindowPos($h,[IntPtr]::Zero,$rect.Left,$rect.Top,$rect.Right-$rect.Left,$rect.Bottom-$rect.Top,0x14)
    [void][MirrorWindowTest]::SetThreadDpiAwarenessContext($oldDpi)
}

AirMirrorLite - private Windows x64 test build

1. Extract the whole ZIP to a local folder you own, such as Documents.
   Run it from the extracted folder, not from inside the ZIP or a network share.
2. Double-click Start-AirMirrorLite.cmd. No administrator mode is required.
   It grants this executable's restricted codec worker read/execute permission,
   then opens the receiver. Repeating this step leaves a correct grant unchanged.
   This is needed after extraction because ZIP files do not preserve that grant.
3. Put the PC and iPhone on the same trusted private Wi-Fi/LAN. On the iPhone,
   open Control Center > Screen Mirroring > AirMirrorLite.
   The Windows network profile can stay Public, Private or Domain. If Windows
   asks for firewall access, allow AirMirrorLite on the connected network.
   Guest Wi-Fi or client isolation can prevent discovery. Run one receiver at a time.
4. Close the phone-shaped window to exit. The bottom-right grip resizes it;
   the side controls adjust volume, restore size, enter fullscreen and close.
   M mutes, 0 restores size, F11 toggles fullscreen and D disconnects the phone.

Requirements
- Windows 10/11 x64, a D3D11-capable GPU/driver with native H.264 decoding,
  Media Foundation, and an enabled default Windows audio output device.
- Current Microsoft Visual C++ v14 Redistributable, x64. Missing MSVCP140.dll,
  VCRUNTIME140.dll or VCRUNTIME140_1.dll indicates this prerequisite is absent.
  Use Microsoft's installer; the launcher does not download or install it:
  https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist/
  Direct official x64 download: https://aka.ms/vc14/vc_redist.x64.exe
- Windows N editions also need Microsoft's Media Feature Pack:
  https://support.microsoft.com/en-us/windows/experience/platform-variants/media-feature-pack-for-windows-n
- Higher resolution uses native HEVC support when available. Otherwise the app
  selects H.264/1080 automatically. HEVC support and behavior vary by PC.
  No Bonjour service, codec DLL download or developer tools are required.

This is the experimental FFmpeg AAC build with transport rotation and dynamic
display updates enabled. It is for private testing; checks on the developer's PC
do not establish compatibility, Photos behavior or audio sync on another machine.
The included local/display-aspect.txt contains only the generic 994 2160 display
hint used with the tested phone. A different phone ratio may need adjustment;
delete that text file for the standard widescreen hint, then restart/reconnect.
It is an advertised canvas preference, not a guaranteed mirroring resolution.

First launch creates this user's protected identity and bounded diagnostic log
under local beside the app, or under LOCALAPPDATA/AirMirrorLite if needed. No
sender identity, phone screen capture or previous user's logs are included.
Do not share a used local folder: it contains your receiver identity and logs.
If startup fails, keep the exact error text and check these prerequisites. Do
not disable Windows security protections. Included source and component notices
describe this build and its licenses. No automatic updater or service is added.

Preparation without opening a receiver: Start-AirMirrorLite.cmd --prepare-only
This runs no media test, plays no sound and opens no network listener.

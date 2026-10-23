iPhone Air Mirror - installed beta

Open iPhone Air Mirror from the Start menu. Installation itself does not launch
the receiver, play sound, change firewall rules or install system components.

Identity, preferences and bounded logs use:
  %LOCALAPPDATA%\iPhone Air Mirror
The Start menu and optional desktop shortcuts select this directory explicitly.
The generic display-aspect preset is created only if absent. Your data and
changed preset are retained by upgrades and uninstall. To remove private data,
close the receiver and remove that separate folder yourself when it is no
longer needed; the uninstaller never does this automatically.

The executable is identical to the corresponding portable build. BUILD.json
identifies it; installed-SHA256SUMS.txt covers installed application files.
source\build-info\portable-origin contains the portable checksum manifest and
README. The portable launcher is omitted here because installed shortcuts use a
separate data directory. Complete corresponding source and license notices are
included under source and licenses.

Requirements: Windows 10 version 1703 (build 15063) or later / Windows 11 x64,
the Microsoft Visual C++ v14 x64 runtime,
Media Foundation, compatible D3D11 hardware and an enabled audio output device.
The runtime can be obtained from Microsoft's official download:
https://aka.ms/vc14/vc_redist.x64.exe
Higher resolutions additionally require native HEVC support. No codec, audio
driver or Windows feature is installed by this installer.

Close the receiver before upgrading or uninstalling. The installer does not
force-close applications or restart them. Existing identity/preferences remain
available after reinstalling. Only installer-owned application files are removed.

This beta is not qualified as a stable release. See source\docs\STATE.md for
the separate build, synthetic, GPU and physical iPhone/audio evidence. Signatures
and hashes are recorded in the installer build receipt; do not interpret a hash
as a publisher signature. No automatic updater, startup item or service is added.

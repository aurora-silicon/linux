# J514S physical panel dimensions validation

Source fix: cc1765d1fe7e. Installed module and m1n1 bundle hashes are in deployment.json.
The bootloader and every byte outside the J514S DT remained unchanged; device-tree.diff
contains only the panel node with 302x196 mm dimensions.

Reboot 366814ff-4281-477d-babf-c3506c8d08d7 completed guarded native DCP startup. DRM GETCONNECTOR
and KWin independently reported 302x196 mm at 3024x1964, approximately 254 DPI.
Initially removing only the saved scale key retained 1x through KWin's existing-setup
path. After stopping the display manager, terminating eryk's session and waiting for
KWin to exit, the single-panel configuration was archived. No scale was supplied.
KWin 6.7.4 then selected 1.7x automatically (1779x1155 logical desktop). Only keyboard
and touchpad input devices were present, consistent with its 150-DPI internal-output
heuristic without a lid switch. Upstream source consulted:
https://invent.kde.org/plasma/kwin/-/blob/Plasma/6.7/src/outputconfigurationstore.cpp
Panel specification: https://support.apple.com/en-ie/117736

Guarded real Plasma/Konsole/screenshot validation completed, peak five-channel PMP
sample 69.50 C, with the existing 60-second watchdog and M3 thermal protections.
The screenshot was visually checked: normally rendered desktop, readable controls.
The temporary validation Konsole closed; normal persistent autologin remains active.
A subsequent native KMS status showed completed=257, failed=0.

Target backups: /root/m3-dcp-dpi-backup-20260921 (old module, boot bundle, config),
and /home/eryk/.config/kwinoutputconfig.json.before-panel-dpi-autodetect.
Raw boot evidence and screenshot are retained under build/m3-dcp-dpi in the workspace.
This validates dimensions and automatic scaling, not new refresh-rate or recording support.

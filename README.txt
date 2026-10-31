USB Mirror 0.4.1 - USB mirroring for Windows

1. Close other apps that are mirroring your phone before connecting here.
2. Run the single-file USBMirror.exe. It prepares its own runtime files.
3. Connect your iPhone with a data-capable USB cable, unlock it and accept Trust.
4. Select your phone and click Connect.
5. Capture the "USB Mirror Preview" window in your streaming program.
6. Capture USB Mirror's application audio, or the Windows output device it uses.

Single-file edition
You only need to keep the supplied USBMirror.exe. On launch it verifies and
extracts its DLLs, tools, documentation and license files under
%LOCALAPPDATA%\USBMirror\Runtime. These files remain cached for later launches
and virtual-camera registration. The app does not put files beside your EXE.
Settings remain in %LOCALAPPDATA%\USBMirror, as in earlier versions.

The source build also produces an unpacked developer folder. If you use that
edition, keep its adjacent DLLs and helpers together.

The preview has no phone-shaped frame or rounded-corner clipping. The control
panel can be minimized independently. Keep the preview window restored while
using window capture; minimizing capture windows can stop updates in Windows.

The dark control panel groups connection, capture, audio and optional camera
settings with flat controls and simple separators. Tools is at the top right.
The connection status shows whether the phone is ready, connected or waiting.
Frame statistics and the input meter
are hidden when Windows reports no physical iPhone; USB cleanup can take longer.
The capture window and Windows application-audio capture work as before.

Tools > USB mode offers Standard (upstream A/Demo) and Compatibility (upstream
B). Stop mirroring before switching. On the tested phone, Compatibility was
selected when audible playback was confirmed; that choice is already saved
in the current Windows profile. For a new profile, select Compatibility before
connecting to reproduce that setup. Standard remains the new-profile default.
The exact reason audio started has not been isolated. Compatibility produced
1206x2620 portrait video in this test; Standard produced 1320x2868.

Local size and FPS settings only affect presentation on Windows. They do not
set the game's FPS or force a higher incoming capture rate from the phone.

Audio
Game sound uses normal Windows audio playback. Use Windows audio routing to
choose an output device for USB Mirror. This usually requires audio to be
playing before Windows lists the app. In your streaming program, capture this
sound once to avoid doubled audio. Muting game audio or lowering its volume in
USB Mirror also affects what an application-audio capture can receive.

The iPhone audio input meter measures the incoming 16-bit USB audio before
local playback volume/mute. "Input: silence" means the incoming samples
are zero. Test a game or a video recorded with Camera to check the source;
phone volume controls may be unavailable during USB capture. A moving input
meter with no audible Windows output instead points
to playback routing, Windows volume/mute, or the selected listening device.
The diagnostic log is %LOCALAPPDATA%\USBMirror\Logs\capture.log. If that
folder is unavailable, the app uses %TEMP%\USBMirror-capture.log.

Reconnect
After a successful manual connection, your chosen phone and settings are saved
under %LOCALAPPDATA%\USBMirror. With automatic reconnect enabled, subsequent
launches wait for that same phone. Disconnect deliberately suspends reconnect
until you click Connect again. Driver/setup failures do not trigger an endless
USB reset loop. Other recoverable failures use a delay between retries.

Optional camera (Windows 11)
Window capture is the simplest route. If a streaming app needs camera input,
use Install camera, approve Windows' administrator prompt yourself, then Start
camera while USB mirroring is running. Choose "USB Mirror Camera" in the
streaming app. Camera output is video-only; capture game audio separately.
The camera canvas is 1920x1080, or 1080x1920 with Portrait selected. Source
aspect ratio is preserved with black padding. Change the orientation while
the camera is stopped. Camera output adds a pixel-copy/conversion path; it
does not replace the normal GPU preview.

Driver setup
Tools > Driver setup opens the included USBMirror.Driver.exe. It lists connected
and remembered iPhones/iPads and shows their capture-filter state.

Connect the phone, stop mirroring, select it in Driver setup, and choose
Install / repair filter or Remove filter. Confirm the selected operation and
approve Windows' administrator prompt yourself. Reconnect the USB cable after
the operation. Opening the tool or pressing Refresh changes no drivers.

The tool uses the existing signed libusb-win32 1.2.6 filter and exact-device
installer. Filter bindings are changed only for the selected USB parent; the
legacy helper can temporarily restart other devices using libusb0. Missing
shared files can be deployed; an existing different libusb0 version is left
unchanged. Operation folders and registry backups are saved under
%ProgramData%\USBMirror.Driver. Driver changes and USB Mirror capture exclude
each other. Apple Devices/Apple Mobile Device Support remains a prerequisite;
Get Apple Devices opens its Microsoft Store listing.

Installation/removal and rollback have not yet been live-tested in this build.
Your working installed driver was not changed during development.

Application icon
The icon is embedded in both executables and included as USBMirror.ico. It
contains 16, 20, 24, 32, 40, 48, 64, 128 and 256 pixel images.

What was removed
No WPF/.NET UI runtime, bundled Python, remote control, device-profile binder,
AirPlay/DLNA receiver, streaming encoder, built-in recording or updater runs
in this frontend. The original native capture DLL remains unmodified and
contains dormant upstream APIs. No wireless receiver is started.

Validation
Build-time checks load the real native capture/camera DLLs, verify their API,
read the driver environment and confirm no wireless receiver was started.
--probe <report.json> additionally reads connected-device metadata. Neither
test mode starts capture, changes USB configuration or installs a camera.
--demo opens the interface without loading capture or changing settings.

Live video, a clean stop/reconnect and audible playback have been confirmed
on the user's phone. Audio was confirmed with both Apple Music and a game/video.
Gameplay audio sync and capture in the streaming app
still need verification. It does not claim a higher source frame rate or
measured latency gain.
The underlying capture and USB cleanup behavior are from your working v1.8.3.

License: GPL-3.0-only. See LICENSE, SOURCE.md and licenses/.

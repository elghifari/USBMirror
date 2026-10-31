# Validation - USB Mirror 0.4.1

Release prepared on 2026-10-31 using the x64 MSVC 2019 toolchain and the user's known-working
v1.8.3 USB capture/runtime binaries. No changes were made to the
original application installation or to Windows protection settings.

## Passed

- The Git import stores the capture-engine snapshot and expanded driver
  source as 702 ordinary files under third_party/capture-engine. A clean
  export of the staged files built the native app, driver manager and
  single-file launcher without a source ZIP or the ignored upstream folder.
  All 67 existing checks passed (25 runtime, 25 driver and 17 bundle checks).
  Exported dependency contents match the original archives apart from
  declared text line-ending normalization. No source ZIPs, generated API
  copies, build outputs or historical release packages are staged.
- In 0.4.1, public README/help text and competing-app messages use USB Mirror
  branding. Case-insensitive scans found no upstream product-name references
  in README.md or README.txt. Diagnostic logging was verified at
  %LOCALAPPDATA%\USBMirror\Logs\capture.log.
- The camera setup helper was packaged under its new neutral filename and
  checked against the original SHA-256. All copied runtime components remain
  byte-identical. Required internal helper/API names and existing source and
  license notices were preserved.
- The single-file edition embeds all 20 runtime, helper and notice files.
  Initial preparation wrote 20 cache files; the next launch reused all 20.
  Two simultaneous launches repaired one deliberately changed cached README
  exactly once, and restored its original SHA-256. No files were created
  beside the single executable.
- All 17 bundle-format checks pass: unsafe paths/reserved Windows filenames,
  compression round-trip, integrity rejection and Windows argument quoting.
- Through the single-file launcher, the capture runtime passes its 25 checks.
  The cached driver manager passes its 25 checks. Neither test starts capture
  or changes drivers. The launcher UI fixture fits all 31 controls and its
  screenshot was visually inspected.
- Native executable compiles with `/W4`, with no compiler warnings emitted.
- 25 native/runtime assertions pass: PCM16 silence/full-scale/odd-byte handling,
  aspect-fit bounds, reconnect safety gates,
  retry backoff, runtime ABI 18, read-only environment query, stopped wireless
  receiver, real D3D11 preview creation and teardown, camera-support query,
  and embedded icon loading at 16 and 256 pixels.
- The separate native driver manager passes 25 non-mutating assertions:
  Apple USB-parent/PID/serial validation, interface/command-injection rejection,
  preserving other filters and their order, duplicate-filter rejection,
  malformed registry-value rejection, known SHA-256 vector, embedded payload
  hashes, icon loading, read-only discovery and cross-thread hardware exclusion.
  Its report explicitly records driverChangesPerformed=false.
- Windows reported a Valid signature for the exact bundled libusb0.sys.
- The new driver manager was opened and visually inspected. With the phone
  disconnected, install/remove were disabled. Refresh was exercised, and the
  app's Driver setup command launched the bundled USBMirror.Driver.exe path.
  The original manager was already running before this build; it was left
  untouched and is not used by the new menu command.
- Windows extracted the new icon from USBMirror.exe. The ICO contains nine
  sizes and the native executables report file version 0.4.1.
- In 0.3.1, the driver status uses complete sentences and identifies Windows
  as the actor requesting administrator approval. Its two-line text was
  rendered and visually checked without clipping. Both 25-assertion test
  suites passed again; driver operations were not invoked.
- The final read-only connected-device probe passes 24 assertions (the above
  plus native discovery). Earlier connected-state checks found the user's phone;
  the final check found no connected device.
- All 31 controls fit within the client area at the current DPI. Offline,
  streaming and unplugged fixtures were rendered and visually inspected.
  The fixtures start no native capture session and change no saved settings.
- The unplugged fixture intentionally retains old video/audio values with a
  native session still active; the UI hides those values and shows waiting.
- The themed FPS combo opened, accepted the Up key to select 30 FPS, and closed.
  The themed audio checkbox toggled through its native button behavior.
- The native volume slider accepted Home to reach zero, and its percentage
  label updated to 0%. Demo interactions left the saved real profile unchanged:
  60 FPS, Native size, audio enabled, volume 100%, Compatibility USB mode.
- The 0.2.1 follow-up repeated ten demo checks, all passing: FPS dropdown open
  and arrow selection, audio/reconnect/portrait checkbox toggles, volume Home
  and percentage update, empty idle footer, preview open and close-to-hide.
  Results: build/flat-ui-interactions.json.
- The flat UI was visually checked in offline, streaming and unplugged states.
  No rounded section cards or status capsule remain. Buttons, inputs and
  checkboxes have square corners; the header slogan and idle instructions
  are absent. Streaming still shows the resolution/FPS/decoder/audio metrics.
- Runtime hashes record the exact original components copied into the package.

## Live device checks from the 0.1.0 capture build

- The user confirmed that live video works on their iPhone 18 Pro Max / iOS 27.
- Landscape 2868x1320 and portrait 1320x2868 were observed, using hardware decode.
- A reconnect produced successive samples near 60 FPS (59.2, 59.9 and 60.0).
- Clean stop returned normal USB discovery, followed by successful reconnection.
- Native audio initialization and 48 kHz stereo PCM transport were observed.
- Initial tests were silent in both this frontend and the original app.
  Windows reported the app unmuted at full session volume on Realtek Speakers;
  both its Windows output meter and incoming PCM meter showed zero signal.
  The user identified Apple Music as the initial test source, then subsequently
  confirmed that audible playback worked with both Apple Music and a game/video.
  These checks do not establish a content restriction as the cause of silence.
- The alternate upstream B USB mode was exposed as Tools > USB mode >
  Compatibility and saved as USBMode=1. With that build running, the user
  subsequently confirmed audible sound from both tested source types, then
  unplugged the phone after success.
  That successful executable is backed up under build/before-ui-refresh, and
  the saved Compatibility mode has been retained for 0.2.0. No further Windows
  output/volume change or capture-engine modification was made.
  A controlled comparison has not established why audio started; the mode
  switch alone should not be treated as a proven fix. Sync was not measured.
- Compatibility mode produced 1206x2620 portrait video near 60 FPS on this
  phone, compared with 1320x2868 in Standard (upstream A/Demo) mode.
- Windows subsequently reported no present Apple USB device. The frontend now
  treats NoVideoFrames plus an absent physical device as recoverable, waiting
  for the same phone to return instead of permanently abandoning reconnection.

## Still awaiting verification

- Live driver install/remove and failure rollback. No driver changes, service
  changes, registry writes, UAC approvals or kernel installs were performed.
- A live phone session after the 0.2.0 UI refresh. The phone was unplugged by
  the user after successful capture; the UI checks use simulated states.
- Physical unplug updates through Windows device-change notifications. These
  request a read-only USB-presence query and do not force or reset capture.
- Sustained gameplay audio and synchronization.
- The improved absent-device recovery on a physical reconnect, repeated cable
  unplug/replug cycles and a sustained gameplay session.
- Capture of the clean window/audio in the chosen streaming app.
- Optional virtual-camera installation, publishing and application capture.

The read-only tests do not activate USB screen capture, change a USB
configuration, install a driver, or register the optional camera. The desktop
automation runtime could not start because Windows Controlled Folder Access
blocked its sandbox setup; the GUI layout check used the app's own test mode.

The single-file executable is approximately 2.3 MB. It prepares an
approximately 3.5 MiB cached folder that includes the native driver manager
and its verified, signed driver payload. It is smaller because it contains the small native
frontend and a short list of native dependencies. Its original capture engine
is unchanged, including its dormant wireless API exports. This build makes no
claim of higher source FPS, lower measured latency, or a fully stripped engine.

# Vendored capture engine

`capture-engine/` contains the iPhoneMirror v1.8.3 source snapshot from
[RayrenSX/iPhoneMirror](https://github.com/RayrenSX/iPhoneMirror/tree/v1.8.3).
Files are checked into Git directly so reviewers can browse code and see
future changes in diffs.

The original tagged archive was downloaded from
[GitHub codeload](https://codeload.github.com/RayrenSX/iPhoneMirror/zip/refs/tags/v1.8.3).
Its SHA-256 is:

```text
0E04279CA4A37D5B26CFF1F23D241855AD3CDD22018DEFEAA7CB29440A82C2B4
```

The snapshot's nested `libusb-win32-src-1.2.6.0.zip` has been replaced by its
217 source files in `capture-engine/third_party/libusb-win32/source/`.
That original nested archive has SHA-256:

```text
F3FAF094C9B3415EDE42EEB5032FEDA2E71945F13F0CA3DA58CA10DCB439BFEE
```

The initial import only relocated the snapshot and expanded that archive.
Git applies the declared text line-ending rules.
The original licenses, notices and prebuilt upstream assets are retained.
There are no source ZIPs in this tree.

The original upstream driver-manager project still names that ZIP as an
embedded resource. Recreate the archive from the expanded driver-source
directory before building that separate project. USB Mirror does not use
that project or need the archive.

USB Mirror 0.4.2 builds the capture engine from this tree. Its local changes are:

- `src/Core/src/CoreApi.cpp` passes all four preview rotations to the renderer.
  The original API discarded requests for 90 and 270 degrees.
- `CMakeLists.txt` accepts CMake 3.20 for the explicit build used by USB Mirror.
- `src/Core/src/Capture/WirelessReceiverHub.h` and `.cpp` use a `std::thread`
  and explicit stop source for the playback writer. This retains native I/O
  cancellation on MSVC 2019, whose `std::jthread` lacks `native_handle()`.
- `src/Core/src/Media/MediaFoundationDecoder.cpp` uses Microsoft's documented
  Display P3 primary value (13), which SDK 19041 does not name.

The build reads API headers, signed driver payloads and license notices here.
It still takes the v1.8.3 USB libraries, configuration helper and camera
components through `-RuntimeSource`. The kernel driver is not rebuilt.
Other upstream projects and assets remain here for source review and are not
added to the USB Mirror runtime package.

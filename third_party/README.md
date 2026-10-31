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

Apart from relocating the snapshot and expanding that archive, upstream
contents are unchanged. Git applies the declared text line-ending rules.
The original licenses, notices and prebuilt upstream assets are retained.
There are no source ZIPs in this tree.

The original upstream driver-manager project still names that ZIP as an
embedded resource. Recreate the archive from the expanded driver-source
directory before building that separate project. USB Mirror does not use
that project or need the archive.

USB Mirror reads API headers, signed driver payloads and license notices from
this directory. Its build still takes the known-working v1.8.3 capture runtime
through `-RuntimeSource`; it does not rebuild the capture engine or kernel
driver. The other upstream projects and assets remain here for source review
and are not added to the USB Mirror runtime package.

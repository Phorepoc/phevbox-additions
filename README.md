# phevbox-additions

The VirtualBox guest additions for Phorepoc OS, as a Catalogue package. Phorepoc runs as a VirtualBox guest on a PC (x86-64
CPU, x32 ABI); this is what lets the host and the guest share things the way VirtualBox's own Guest Additions do. GPL-3.0.

| Phase | What | State |
|---|---|---|
| 1 | the HGCM transport (`vbox_hgcm.*`) and the shared clipboard, text | this repository, `phevboxsvc.exe` |
| 2 | shared folders as a drive: an F32 file-system plug-in over the shared-folders service | designed, not built |
| 3 | drag and drop from host files into a `Received` folder | designed, not built |

## How it fits together

    host (VirtualBox)  HGCM services: VBoxSharedClipboard, VBoxSharedFolders, VBoxDragAndDropSvc
         |  VMMDev PCI device (request port, guest-physical request buffers)
    kernel (the PC image, not here)  logical device PheVmmDev: an arena of contiguous RAM, Submit(offset)
         |  rvmmdevchannel.h
    phevboxsvc.exe (this package)  HGCM connect / call / disconnect, then the clipboard protocol on it

The kernel side stays tiny and generic and is part of the PC image: it carries no GPL code and speaks no protocol. Everything
that is VirtualBox's protocol is here, so it installs and updates through Catalogue and is under this package's licence. A
kernel cannot load a driver from a package on Phorepoc (the PC kernel links its drivers in), which is why the channel is in the
image and the service is not.

### Shared clipboard (phase 1)

`phevboxsvc.cpp` connects to the host service `VBoxSharedClipboard` and uses its "legacy" protocol (no feature negotiation; every
VirtualBox 6.1 and later host accepts it): a call parked on the host (`GET_HOST_MSG`) returns when the host's clipboard changed
(the service reads the text with `DATA_READ` and puts it on the Symbian clipboard) or when the host wants the guest's text
(`DATA_WRITE`). A change on the Symbian clipboard (`CClipboard`, polled once a second) is announced with `REPORT_FORMATS`.
Text is Unicode text only: UTF-16, line ends converted between CR LF and the paragraph end of the Symbian text engine. Limit:
what fits the channel's data area (250 KB).

## Install and use

Install `phevbox-additions-x32.sis` from Catalogue (listing `phevbox-additions`) on a Phorepoc PC image that has the PheVmmDev
channel, start **Guest Additions** from the menu once, and set *Devices > Shared Clipboard* in the VirtualBox window.
The service needs the CommDD capability to open the channel: it installs where the installer trusts the package (Phorepoc's
developer mode, the default). Autostart at boot is not part of this version: see "Next".

## Building (CI: `.github/workflows/release.yml`)

The pdk's `package.yml` builds `package.conf` on every push to main and releases what Conventional Commits ask for:
`phe component build phevboxsvc` (`registry/components_phevbox.py`, the mmp in `os/src/phevboxsvc`), then `recipes/sis.sh`
(`recipes/pack.py` writes the SIS: the service with its capability word, the menu entry, the icon, the licences).
`phevboxtest` is the same service with a self-test, a boot-test component for an image build (`SSM_TEST=phevboxtest`).

## Licence

GPL-3.0-only, for everything we wrote here (`LICENSE`). `NOTICE` says which VirtualBox files the code is written to and under
which licence each stands (the VMMDev headers are MIT, the clipboard headers GPL-3.0-only OR CDDL-1.0: we take the GPL).
`rvmmdevchannel.h` is the image's own interface header, EPL-2.0 OR GPL-2.0-or-later, used under GPL-2.0-or-later.
The written source offer is `SOURCE-OFFER.txt`; the package installs it with the licences.

## Next

Phase 2: an F32 file-system plug-in (`phevbox.fsy`) that mounts the host's shared folders (service `VBoxSharedFolders`,
operations by name: map, create, open, read, write, list, remove) as a drive; the HGCM transport here moves into a library
the plug-in and the service share. Phase 3: the drag-and-drop service delivers dropped files (URI list) into
`\Received` on the shared transport. Autostart: a generic "start these at boot" list for installed packages is the missing
piece in the image; until then the menu entry starts the service.

## Status (2026-11-19)

Built by CI (v0.1.0). Checked on VirtualBox 7.2.18 with the PC image's channel: the service opens `PheVmmDev`, submits an HGCM
connect and the host completes it (the host's clipboard service then refuses the connect when the host cannot open its own
clipboard or its mode is off: the service retries every 10 s). Not yet exercised: the clipboard data paths (host text to the
Symbian clipboard and back) and the install of the SIS on a running image; both need an interactive desktop session on the host.

# phevbox-additions

The VirtualBox guest additions for Phorepoc OS, as a Catalogue package. Phorepoc runs as a VirtualBox guest on a PC (x86-64
CPU, x32 ABI); this is what lets the host and the guest share things the way VirtualBox's own Guest Additions do. GPL-3.0.

| Phase | What | State |
|---|---|---|
| 1 | the HGCM transport (`vbox_hgcm.*`) and the shared clipboard, text | `phevboxsvc.exe` |
| 2 | shared folders as a drive (an F32 file-system plug-in) | skipped for now (decision 2026-11-19) |
| 3 | drag and drop, host to guest: files land in `C:\Data\Received\` | `vbox_dnd.*`, same service |

## How it fits together

    host (VirtualBox)  HGCM services: VBoxSharedClipboard, VBoxDragAndDropSvc
         |  VMMDev PCI device (request port, guest-physical request buffers)
    kernel (the PC image, not here)  logical device PheVmmDev: an arena of contiguous RAM, Submit(offset)
         |  rvmmdevchannel.h
    phevboxsvc.exe (this package)  HGCM connect / call / disconnect; the clipboard and the drag and drop protocols on it
         |  \resource\phe\services\phevbox.txt  (the image starts it at boot: pheshellsel)

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
what fits the channel's data area (240 KB).

### Drag and drop, host to guest (phase 3)

`vbox_dnd.cpp` connects to `VBoxDragAndDropSvc` (protocol 3, the one every 6.1 and later host speaks). The host's drag enters the
VM window: the service says "copy is fine" (`ACK_OP`) as long as the host offers a URI list (files); on the drop it asks for
`text/uri-list`, and the host sends a header, the list, then every directory and file in chunks. Names are made safe (no `..`,
no drive, no characters F32 refuses) and written under `C:\Data\Received\`; a top-level name that exists already gets a number
("name (2).txt"), so a drop never replaces an earlier one. A global note tells the user how many files arrived. Text and other
non-file drops are not accepted; guest to host is not implemented.

## Install and use

Install `phevbox-additions-x32.sis` from Catalogue (listing `phevbox-additions`) on a Phorepoc PC image that has the PheVmmDev
channel and the services list (core's `pheshellsel`), and restart the device: the package installs
`\resource\phe\services\phevbox.txt` and the image starts the service at boot. (Without a restart, start **Guest Additions** from the
menu.) In the VirtualBox window set *Devices > Shared Clipboard* and *Devices > Drag and Drop > Host to Guest*, then drag files
from the host onto the VM window. The service needs the CommDD capability to open the channel: it installs where the installer
trusts the package (Phorepoc's developer mode, the default).

## Building (CI: `.github/workflows/release.yml`)

The pdk's `package.yml` builds `package.conf` on every push to main and releases what Conventional Commits ask for:
`phe component build phevboxsvc` (`registry/components_phevbox.py`, the mmp in `os/src/phevboxsvc`), then `recipes/sis.sh`
(`recipes/pack.py` writes the SIS: the service with its capability word, the services list, the menu entry, the icon, the licences).
`phevboxtest` is the same service with a self-test, a boot-test component for an image build (`ROM_TAIL=phevboxtest`; the ROM carries
its own services list entry). `tests/vbox_drop.ps1` drops host files on a running VM through the VirtualBox COM API.

## Licence

GPL-3.0-only, for everything we wrote here (`LICENSE`). `NOTICE` says which VirtualBox files the code is written to and under
which licence each stands (the VMMDev headers are MIT, the clipboard and drag and drop headers GPL-3.0-only OR CDDL-1.0: we
take the GPL). `rvmmdevchannel.h` is the image's own interface header, EPL-2.0 OR GPL-2.0-or-later, used under GPL-2.0-or-later.
The written source offer is `SOURCE-OFFER.txt`; the package installs it with the licences.

## Next

Shared folders as a drive (phase 2) when it is wanted: an F32 file-system plug-in over `VBoxSharedFolders`. Guest to host drag and
drop. HGCM completion by the device's interrupt instead of polling. Further clipboard formats.

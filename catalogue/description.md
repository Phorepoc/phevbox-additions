# Guest additions (VirtualBox)

The guest side of VirtualBox's host integration for a Phorepoc PC image running in VirtualBox. This first version shares
the **clipboard** (text) in both directions: copy on the host, paste in the device, and the other way round.

- Turn it on in VirtualBox: *Devices > Shared Clipboard > Bidirectional* (or one direction).
- Start **Guest Additions** from the menu once after installing; it keeps running until the device stops.
- Text only, as much as fits in 250 KB. Pictures and files are not shared yet; shared folders and dropping files from the
  host come next.
- Needs the PC image's kernel channel for guest-additions programs (the image says so in its release notes); on other
  hardware, or under another hypervisor, it finds no device and ends.

Free software, GPL-3.0. The licences and the written source offer are installed with the package in
`\resource\phevbox\licences\`.

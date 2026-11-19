/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: EPL-2.0 OR GPL-2.0-or-later */

// rvmmdevchannel.h -- OURS: the user-side API of the VirtualBox VMMDev request channel (xk_vmmchannel.cpp, built into ekern,
// device "PheVmmDev"). A thin, generic channel: it carries a client's requests to the guest-additions device and nothing more.
// Everything that speaks a protocol over it (HGCM, the shared clipboard ...) lives in a user-side program that installs from
// Catalogue; no protocol code is in the image.
//   GetInfo    -> the device's request port, and the channel's arena (below)
//   OpenArena  -> a shared chunk over ONE physically contiguous RAM block: the request buffers. A request is a VMMDev request
//                 (header + body, VMMDev.h) the client builds in the arena; the device reads it, and writes its answer into
//                 the same memory, by physical address
//   Submit     -> hand the request at byte offset iOffset of the arena to the device (an OUT to the request port). Returns
//                 when the device has taken it: a request the host completes later (an HGCM call that waits) is complete
//                 when the client sees its header's done flag (HGCM request header, fu32Flags bit 0) set in the arena
//   Idle       -> the client says no request is outstanding any more. A channel closed without it keeps its arena (the
//                 device may still write into it) and says so in the kernel log
// What Submit accepts: the VMMDev request types of HGCM only (connect, disconnect, call, cancel), 4-byte aligned, inside the
// arena; for a call, every page list the request carries must point into the arena (a guard against a client's own bugs: the client shares the arena, so it is trusted -- the channel needs the CommDD capability).
// Open never needs User::LoadLogicalDevice (the device is installed by the kernel extension).
#ifndef RVMMDEVCHANNEL_H
#define RVMMDEVCHANNEL_H

#include <e32cmn.h>
#ifndef __KERNEL_MODE__
#include <e32std.h>
#endif

_LIT(KPheVmmDevName, "PheVmmDev");
const TInt KPheVmmDevMajorVersion = 1;

struct TPheVmmDevInfo
	{
	TInt iVersion;				// the interface version of the device's guest-info (VMMDEV_VERSION)
	TUint32 iPort;				// the device's request I/O port (informational)
	TUint32 iArenaPhys;			// the arena's physical address (the host reads page lists and data from here)
	TInt iArenaSize;			// bytes
	};

enum TPheVmmDevControl { EPheVmmCtrlGetInfo, EPheVmmCtrlOpenArena, EPheVmmCtrlSubmit, EPheVmmCtrlIdle };

#ifndef __KERNEL_MODE__
class RPheVmmDev : public RBusLogicalChannel
	{
public:
	inline static TVersion VersionRequired() { return TVersion(KPheVmmDevMajorVersion, 0, 0); }
	inline TInt Open() { return DoCreate(KPheVmmDevName, VersionRequired(), KNullUnit, NULL, NULL, EOwnerProcess); }
	inline TInt GetInfo(TPheVmmDevInfo& aInfo) { TPckg<TPheVmmDevInfo> p(aInfo); return DoControl(EPheVmmCtrlGetInfo, &p); }
	inline TInt OpenArena(RChunk& aChunk) { TInt h = DoControl(EPheVmmCtrlOpenArena); return h < 0 ? h : aChunk.SetReturnedHandle(h); }
	inline TInt Submit(TInt aOffset) { return DoControl(EPheVmmCtrlSubmit, (TAny*)aOffset); }
	inline TInt Idle() { return DoControl(EPheVmmCtrlIdle); }
	};
#endif

#endif

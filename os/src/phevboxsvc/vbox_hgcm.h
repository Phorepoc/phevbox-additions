/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * vbox_hgcm.h -- the HGCM transport of the VirtualBox guest additions, on the PheVmmDev channel (rvmmdevchannel.h).
 *
 * HGCM is how a guest talks to the host's services (shared clipboard, shared folders, drag and drop). A request is a
 * VMMDev request built in the channel's arena (physically contiguous memory shared with the kernel and, by physical
 * address, with the host): connect, call (a function number and typed parameters: 32/64-bit values, and buffers as
 * physical page lists), disconnect. A call the host cannot answer yet (a parked "wait for a host message") completes
 * later: the host sets the done flag in the request's header. The structures and constants are the ones of VirtualBox's
 * include/VBox/VMMDev.h and VMMDevCoreTypes.h (MIT, "Copyright (C) 2006-2026 Oracle and/or its affiliates", see NOTICE);
 * this is our own code over that interface.
 */
#ifndef VBOX_HGCM_H
#define VBOX_HGCM_H

#include <e32base.h>
#include "rvmmdevchannel.h"

// VBox status codes this code looks at
const TInt KVbInfoHgcmAsync = 2900;			// VINF_HGCM_ASYNC_EXECUTE: the host completes the request later
const TInt KVbInfoBufferOverflow = 2;		// VINF_BUFFER_OVERFLOW: data call, the buffer was too small

// HGCM buffer directions (HGCMPageListInfo::flags)
const TUint32 KHgcmToHost = 1, KHgcmFromHost = 2;

// Request slots (4 KB each, at the start of the arena): 0 connect + the clipboard's parked call, 1 its commands, 2 drag and drop's parked call, 3 its commands; the data area follows
const TInt KHgcmSlots = 4;
const TInt KHgcmSlotBytes = 0x1000;
const TInt KHgcmDataOff = 0x4000;

class CVbHgcm : public CBase
	{
public:
	static CVbHgcm* NewL();
	~CVbHgcm();
	TInt DataBytes() const { return iInfo.iArenaSize - KHgcmDataOff; }
	TUint8* Data() const { return iBase + KHgcmDataOff; }

	// connect to a host service by name ("VBoxSharedClipboard"); waits for the host (a connect is quick)
	TInt Connect(const TDesC8& aService, TUint32& aClientId, TInt aSlot = 0);
	TInt Disconnect(TUint32 aClientId, TInt aSlot = 0);

	// a call: Begin, the Parm* calls, Submit; then Done()/Result() (poll), or Wait() for a call that completes soon
	void Begin(TInt aSlot, TUint32 aClient, TUint32 aFunction, TInt aParms);
	void ParmU32(TInt aSlot, TInt aIndex, TUint32 aValue);
	void ParmU64(TInt aSlot, TInt aIndex, TUint64 aValue);
	// a buffer in the data area ([aDataOff, aDataOff + aLength)), as a page list; aDirection = KHgcmToHost / KHgcmFromHost
	void ParmBuf(TInt aSlot, TInt aIndex, TInt aDataOff, TInt aLength, TUint32 aDirection);
	TUint32 ParmValue32(TInt aSlot, TInt aIndex) const;
	TUint64 ParmValue64(TInt aSlot, TInt aIndex) const;
	TInt Submit(TInt aSlot);					// KErrNone when the device took it (the host's VMMDev status is in iLastRc)
	TBool Done(TInt aSlot) const;				// the host has completed it
	TInt Result(TInt aSlot) const;				// the VBox status code of a completed call
	TInt Wait(TInt aSlot, TInt aMillis);		// poll until done: KErrNone, or KErrTimedOut (the request stays outstanding)
	TInt iLastRc;								// VMMDev status of the last Submit

private:
	CVbHgcm() {}
	void ConstructL();
	volatile TUint8* Req(TInt aSlot) const { return iBase + aSlot * KHgcmSlotBytes; }
	TInt DoSubmit(TInt aSlot, TInt aSize);

	RPheVmmDev iChannel;
	RChunk iArena;
	TUint8* iBase;
	TPheVmmDevInfo iInfo;
	TInt iSize[KHgcmSlots];
	};

#endif

/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * vbox_hgcm.cpp -- the HGCM transport: see vbox_hgcm.h. Layouts (all little endian, packed to 4 bytes; offsets from the
 * request's start), from VirtualBox's VMMDev.h / VMMDevCoreTypes.h:
 *
 *   header (24):  size, version (0x10001), type, rc, reserved, requestor
 *   HGCM header (+8): flags (bit 0 = done), result
 *   connect (168): header, flags, result, location type (2 = local host, existing), name[128], client id
 *   disconnect (36): header, flags, result, client id
 *   call32 (44 + 12 per parameter): header, flags, result, client id, function, parameter count, then parameters:
 *       type (1 = 32 bit, 2 = 64 bit, 10 = page list), value32 | value64 | size + offset of a page list;
 *       a page list (at that offset): flags (direction), offset in first page (16), page count (16), pages (64 each)
 */
#include <e32std.h>
#include <e32debug.h>
#include "vbox_hgcm.h"

enum { EReqHgcmConnect = 60, EReqHgcmDisconnect = 61, EReqHgcmCall32 = 62 };
const TUint32 KReqHeaderVersion = 0x10001;
const TUint32 KHgcmDone = 1;
const TInt KHdrFlags = 24, KHdrResult = 28, KConnLoc = 32, KConnName = 36, KConnClient = 164, KConnSize = 168;
const TInt KCallClient = 32, KCallFunction = 36, KCallParms = 40, KCallFirst = 44, KParmSize = 12;
const TUint32 KLocLocalHostExisting = 2;
const TInt KPageList = 10;

static inline void W32(volatile TUint8* aReq, TInt aOff, TUint32 aVal) { *(volatile TUint32*)(aReq + aOff) = aVal; }
static inline TUint32 R32(const volatile TUint8* aReq, TInt aOff) { return *(const volatile TUint32*)(aReq + aOff); }

CVbHgcm* CVbHgcm::NewL()
	{
	CVbHgcm* self = new (ELeave) CVbHgcm;
	CleanupStack::PushL(self);
	self->ConstructL();
	CleanupStack::Pop(self);
	return self;
	}

void CVbHgcm::ConstructL()
	{
	User::LeaveIfError(iChannel.Open());
	User::LeaveIfError(iChannel.GetInfo(iInfo));
	User::LeaveIfError(iChannel.OpenArena(iArena));
	iBase = iArena.Base();
	for (TInt i = 0; i < KHgcmSlots; ++i)
		iSize[i] = 0;
	iLastRc = 0;
	}

CVbHgcm::~CVbHgcm()
	{
	if (iChannel.Handle())
		iChannel.Idle();							// nothing is outstanding any more (the callers disconnect first)
	iArena.Close();
	iChannel.Close();
	}

// zero the slot, fill the HGCM header; the type and the total size are the caller's
static void InitReq(volatile TUint8* aReq, TUint32 aSize, TUint32 aType)
	{
	for (TInt i = 0; i < KHgcmSlotBytes; i += 4)
		W32(aReq, i, 0);
	W32(aReq, 0, aSize);
	W32(aReq, 4, KReqHeaderVersion);
	W32(aReq, 8, aType);
	W32(aReq, 12, (TUint32)-1);						// no status yet
	}

TInt CVbHgcm::DoSubmit(TInt aSlot, TInt aSize)
	{
	iSize[aSlot] = aSize;
	W32(Req(aSlot), 0, aSize);
	W32(Req(aSlot), KHdrFlags, 0);
	W32(Req(aSlot), 12, (TUint32)-1);
	TInt r = iChannel.Submit(aSlot * KHgcmSlotBytes);
	iLastRc = (TInt)R32(Req(aSlot), 12);
	if (r != KErrNone)
		return r;
	return iLastRc < 0 ? KErrGeneral : KErrNone;
	}

TInt CVbHgcm::Wait(TInt aSlot, TInt aMillis)
	{
	for (TInt t = 0; t <= aMillis; t += 5)
		{
		if (Done(aSlot))
			return KErrNone;
		User::After(5000);
		}
	return Done(aSlot) ? KErrNone : KErrTimedOut;
	}

TBool CVbHgcm::Done(TInt aSlot) const
	{
	// the host sets the flag when it completes the request (the status of the Submit says "completes later", 2900)
	return (R32(Req(aSlot), KHdrFlags) & KHgcmDone) != 0;
	}

TInt CVbHgcm::Result(TInt aSlot) const
	{
	return (TInt)R32(Req(aSlot), KHdrResult);
	}

TInt CVbHgcm::Connect(const TDesC8& aService, TUint32& aClientId)
	{
	volatile TUint8* req = Req(0);
	InitReq(req, KConnSize, EReqHgcmConnect);
	W32(req, KConnLoc, KLocLocalHostExisting);
	for (TInt i = 0; i < aService.Length() && i < 127; ++i)
		req[KConnName + i] = aService[i];
	TInt r = DoSubmit(0, KConnSize);
	if (r == KErrNone)
		r = Wait(0, 2000);
	if (r != KErrNone)
		return r;
	const TInt res = Result(0);
	RDebug::Printf("[phevbox] hgcm connect: vmmdev rc %d, result %d, client %u", iLastRc, res, R32(req, KConnClient));
	if (res < 0)
		return KErrNotFound;
	aClientId = R32(req, KConnClient);
	return KErrNone;
	}

TInt CVbHgcm::Disconnect(TUint32 aClientId)
	{
	volatile TUint8* req = Req(0);
	InitReq(req, 36, EReqHgcmDisconnect);
	W32(req, 32, aClientId);
	TInt r = DoSubmit(0, 36);
	if (r == KErrNone)
		r = Wait(0, 2000);
	return r;
	}

void CVbHgcm::Begin(TInt aSlot, TUint32 aClient, TUint32 aFunction, TInt aParms)
	{
	volatile TUint8* req = Req(aSlot);
	InitReq(req, KCallFirst + aParms * KParmSize, EReqHgcmCall32);
	W32(req, KCallClient, aClient);
	W32(req, KCallFunction, aFunction);
	W32(req, KCallParms, aParms);
	iSize[aSlot] = KCallFirst + aParms * KParmSize;
	}

void CVbHgcm::ParmU32(TInt aSlot, TInt aIndex, TUint32 aValue)
	{
	volatile TUint8* req = Req(aSlot);
	const TInt p = KCallFirst + aIndex * KParmSize;
	W32(req, p, 1);
	W32(req, p + 4, aValue);
	W32(req, p + 8, 0);
	}

void CVbHgcm::ParmU64(TInt aSlot, TInt aIndex, TUint64 aValue)
	{
	volatile TUint8* req = Req(aSlot);
	const TInt p = KCallFirst + aIndex * KParmSize;
	W32(req, p, 2);
	W32(req, p + 4, (TUint32)aValue);
	W32(req, p + 8, (TUint32)(aValue >> 32));
	}

TUint32 CVbHgcm::ParmValue32(TInt aSlot, TInt aIndex) const
	{
	return R32(Req(aSlot), KCallFirst + aIndex * KParmSize + 4);
	}

void CVbHgcm::ParmBuf(TInt aSlot, TInt aIndex, TInt aDataOff, TInt aLength, TUint32 aDirection)
	{
	volatile TUint8* req = Req(aSlot);
	const TInt p = KCallFirst + aIndex * KParmSize;
	const TInt parms = (TInt)R32(req, KCallParms);
	const TInt infoOff = KCallFirst + parms * KParmSize;		// the page list follows the parameters
	const TUint32 pos = (TUint32)(KHgcmDataOff + aDataOff);		// within the arena
	const TUint32 first = pos & 0xFFF;
	const TUint32 pages = (first + (TUint32)aLength + 0xFFF) >> 12;
	const TUint32 firstPhys = (iInfo.iArenaPhys + pos) & ~0xFFFu;
	W32(req, p, KPageList);
	W32(req, p + 4, (TUint32)aLength);
	W32(req, p + 8, (TUint32)infoOff);
	W32(req, infoOff, aDirection);
	W32(req, infoOff + 4, first | (pages << 16));				// offset in the first page (16 bits), page count (16 bits)
	for (TUint32 k = 0; k < pages; ++k)
		{
		W32(req, infoOff + 8 + k * 8, firstPhys + (k << 12));
		W32(req, infoOff + 12 + k * 8, 0);
		}
	iSize[aSlot] = infoOff + 8 + pages * 8;
	W32(req, 0, iSize[aSlot]);
	}

TInt CVbHgcm::Submit(TInt aSlot)
	{
	return DoSubmit(aSlot, iSize[aSlot]);
	}

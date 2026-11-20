/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * vbox_dnd.cpp -- drag and drop, host to guest, over the host service "VBoxDragAndDropSvc". The message sequence and parameter
 * layouts are those of VirtualBox's guest side (include/VBox/HostServices/DragAndDropSvc.h, GPL-3.0-only OR CDDL-1.0, taken under
 * the GPL; src/VBox/Additions/common/VBoxGuest/lib/VBoxGuestR3LibDragAndDrop.cpp read for the order of calls), protocol version 3:
 *
 *   connect, CONNECT (10: context, protocol 3, flags), REPORT_FEATURES (12: two 64-bit masks)
 *   a parked call GET_NEXT_HOST_MSG (300: msg out, parameter count out, block in = 1) says which message the host has; the message is
 *   then fetched by calling the function with that number:
 *     ENTER 200 / MOVE 201 / DROPPED 203  8 parameters (context, screen, x, y, default action, allowed actions, formats, size)
 *       -> we answer ACK_OP 400 (context, action: 1 = copy) so the host shows the drop as possible
 *     DROPPED -> we ask for the data: REQ_DATA 401 (context, format "text/uri-list", size)
 *     LEAVE 202, CANCEL 204: one parameter (context)
 *     DATA_HDR 210 (12 parameters: sizes, object count, format ...), then DATA 205 (the URI list, the "meta" data), then one message per
 *     object, each fetched with its own function: DIR 207 (name, mode), FILE_HDR 209 (name, flags, mode, size), FILE_DATA 208 (chunk)
 *   progress to the host: PROGRESS 402 (context, status 2 = complete / 4 = error, percent, rc)
 * Buffers are page lists into the transport's data area (vbox_hgcm.h). Names are UTF-8, relative to the drop; they are made safe
 * (no "..", no drive or root, no characters F32 refuses) and written under C:\Data\Received\. A top-level name that exists gets
 * " (2)", " (3)" ... so a drop never overwrites an earlier one. Files only: a drop of text or anything else is not accepted.
 */
#include <e32std.h>
#include <e32debug.h>
#include <charconv.h>
#include <utf.h>
#include <aknglobalnote.h>
#include "vbox_dnd.h"

_LIT8(KDndService, "VBoxDragAndDropSvc");
_LIT8(KUriList, "text/uri-list");
_LIT(KReceived, "C:\\Data\\Received\\");

enum { EFnConnect = 10, EFnReportFeatures = 12, EFnGetNext = 300, EFnAckOp = 400, EFnReqData = 401, EFnProgress = 402 };
enum { EHgEnter = 200, EHgMove = 201, EHgLeave = 202, EHgDropped = 203, EHgCancel = 204, EHgData = 205, EHgDir = 207, EHgFileData = 208,
		EHgFileHdr = 209, EHgDataHdr = 210 };
const TUint32 KActionCopy = 1;
const TUint32 KProgressComplete = 2, KProgressError = 4;
const TInt KRetryTicks = 400;					// 10 s
const TInt KVbErrCancelled = -70;					// VERR_CANCELLED: the host ended the operation

// data area layout (one call at a time)
const TInt KOffName = 0, KNameMax = 2048;
const TInt KOffFmt = 2048, KFmtMax = 2048;
const TInt KOffSum = 4096, KSumMax = 256;
const TInt KOffData = 8192;
const TInt KChunk = 65536;						// DND_DEFAULT_CHUNK_SIZE

static void Log(const char* aMsg, TInt a = 0, TInt b = 0)
	{
	RDebug::Printf("[phevbox] dnd: %s %d %d", aMsg, a, b);
	}

CDndService* CDndService::NewL(CVbHgcm* aHgcm, RFs& aFs)
	{
	CDndService* s = new (ELeave) CDndService(aHgcm, aFs);
	s->iRetryTicks = 0;
	return s;
	}

CDndService::~CDndService()
	{
	if (iFileOpen)
		iFile.Close();
	if (iClient)
		iHgcm->Disconnect(iClient, 3);
	iRootFrom.ResetAndDestroy();
	iRootTo.ResetAndDestroy();
	}

TInt CDndService::Sync(TInt)
	{
	TInt r = iHgcm->Submit(3);
	if (r == KErrNone)
		r = iHgcm->Wait(3, 30000);
	if (r != KErrNone)
		return r;
	const TInt res = iHgcm->Result(3);
	return res < 0 ? res : KErrNone;
	}

TBool CDndService::TryConnect()
	{
	TUint32 id = 0;
	TInt r = iHgcm->Connect(KDndService, id, 3);
	Log("connect", r, (TInt)id);
	if (r != KErrNone)
		return EFalse;
	iClient = id;
	iHgcm->Begin(3, iClient, EFnConnect, 3);
	iHgcm->ParmU32(3, 0, 0);
	iHgcm->ParmU32(3, 1, 3);
	iHgcm->ParmU32(3, 2, 0);
	r = Sync(3);
	Log("CONNECT (protocol 3)", r, (TInt)iHgcm->ParmValue32(3, 1));
	if (r != KErrNone)
		{
		iHgcm->Disconnect(iClient, 3);
		iClient = 0;
		return EFalse;
		}
	iHgcm->Begin(3, iClient, EFnReportFeatures, 2);
	iHgcm->ParmU64(3, 0, 0);
	iHgcm->ParmU64(3, 1, (TUint64)1 << 63);
	r = Sync(3);
	Log("REPORT_FEATURES (an older host may refuse)", r);
	iWaiting = EFalse;
	iAccept = EFalse;
	return ETrue;
	}

void CDndService::ArmWait()
	{
	iHgcm->Begin(2, iClient, EFnGetNext, 3);
	iHgcm->ParmU32(2, 0, 0);
	iHgcm->ParmU32(2, 1, 0);
	iHgcm->ParmU32(2, 2, 1);
	if (iHgcm->Submit(2) == KErrNone)
		iWaiting = ETrue;
	else
		Log("wait for host message: submit failed", iHgcm->iLastRc);
	}

void CDndService::Tick()
	{
	if (!iClient)
		{
		if (iRetryTicks > 0)
			{
			--iRetryTicks;
			return;
			}
		if (!TryConnect())
			iRetryTicks = KRetryTicks;
		return;
		}
	if (iWaiting && iHgcm->Done(2))
		{
		iWaiting = EFalse;
		const TInt res = iHgcm->Result(2);
		if (res == KVbErrCancelled)
			{
			// the host gave up the operation (it could not read the files, the user left the window): we stay connected and wait again
			Log("host cancelled the operation", res, iAsked);
			if (iAsked)
				Notice(0, KErrCancel);
			iAsked = EFalse;
			iAccept = EFalse;
			ArmWait();
			return;
			}
		if (res < 0)
			{
			Log("host message call failed (service gone?)", res);
			iHgcm->Disconnect(iClient, 3);
			iClient = 0;
			iRetryTicks = KRetryTicks;
			return;
			}
		OnMessage(iHgcm->ParmValue32(2, 0));
		}
	if (!iWaiting && iClient)
		ArmWait();
	}

TInt CDndService::NextMsg(TUint32& aMsg)
	{
	iHgcm->Begin(3, iClient, EFnGetNext, 3);
	iHgcm->ParmU32(3, 0, 0);
	iHgcm->ParmU32(3, 1, 0);
	iHgcm->ParmU32(3, 2, 1);
	TInt r = Sync(3);
	if (r == KErrNone)
		aMsg = iHgcm->ParmValue32(3, 0);
	return r;
	}

// ENTER / MOVE / DROPPED: the position, the actions, the formats the host offers
TInt CDndService::RecvAction(TUint32 aFn, TUint32& aAllActions)
	{
	iHgcm->Begin(3, iClient, aFn, 8);
	for (TInt i = 0; i < 6; ++i)
		iHgcm->ParmU32(3, i, 0);
	iHgcm->ParmBuf(3, 6, KOffFmt, KFmtMax, KHgcmFromHost);
	iHgcm->ParmU32(3, 7, 0);
	TInt r = Sync(3);
	if (r != KErrNone)
		return r;
	aAllActions = iHgcm->ParmValue32(3, 5);
	if (aFn == EHgEnter)
		{
		// the formats: "\r\n" or NUL separated MIME types
		TPtrC8 f(iHgcm->Data() + KOffFmt, KFmtMax);
		iAccept = f.Find(KUriList) != KErrNotFound;
		Log("host offers; files:", iAccept, (TInt)aAllActions);
		}
	return KErrNone;
	}

void CDndService::AckOp(TUint32 aAction)
	{
	iHgcm->Begin(3, iClient, EFnAckOp, 2);
	iHgcm->ParmU32(3, 0, 0);
	iHgcm->ParmU32(3, 1, aAction);
	Sync(3);
	}

void CDndService::ReqData()
	{
	TUint8* d = iHgcm->Data() + KOffFmt;
	const TInt n = KUriList().Length() + 1;
	Mem::Copy(d, KUriList().Ptr(), n - 1);
	d[n - 1] = 0;
	iHgcm->Begin(3, iClient, EFnReqData, 3);
	iHgcm->ParmU32(3, 0, 0);
	iHgcm->ParmBuf(3, 1, KOffFmt, n, KHgcmToHost);
	iHgcm->ParmU32(3, 2, n);
	const TInt r = Sync(3);
	Log("asked the host for the files", r);
	iAsked = (r == KErrNone);
	}

void CDndService::Progress(TUint32 aStatus, TInt aRc)
	{
	iHgcm->Begin(3, iClient, EFnProgress, 4);
	iHgcm->ParmU32(3, 0, 0);
	iHgcm->ParmU32(3, 1, aStatus);
	iHgcm->ParmU32(3, 2, 100);
	iHgcm->ParmU32(3, 3, (TUint32)aRc);
	Sync(3);
	}

void CDndService::OnMessage(TUint32 aMsg)
	{
	Log("host message", (TInt)aMsg);
	TUint32 all = 0;
	switch (aMsg)
		{
	case EHgEnter:
	case EHgMove:
		if (RecvAction(aMsg, all) == KErrNone)
			AckOp(iAccept ? KActionCopy : 0);
		break;
	case EHgDropped:
		if (RecvAction(aMsg, all) == KErrNone && iAccept)
			ReqData();
		break;
	case EHgLeave:
	case EHgCancel:
		iHgcm->Begin(3, iClient, aMsg, 1);
		iHgcm->ParmU32(3, 0, 0);
		Sync(3);
		iAccept = EFalse;
		break;
	case EHgDataHdr:
		ReceiveDrop();
		break;
	default:
		Log("message not handled", (TInt)aMsg);
		break;
		}
	}

// ---- the files -----------------------------------------------------------------------------------------------------

// the top-level name aRoot of this drop -> the name it gets under C:\Data\Received\ (a new one when it exists already)
TInt CDndService::MapRoot(const TDesC& aRoot, TDes& aTo)
	{
	for (TInt i = 0; i < iRootFrom.Count(); ++i)
		if (iRootFrom[i]->Compare(aRoot) == 0)
			{
			aTo = *iRootTo[i];
			return KErrNone;
			}
	TFileName cand;
	for (TInt n = 1; n < 1000; ++n)
		{
		cand = aRoot;
		if (n > 1)
			{
			TBuf<16> suffix;
			suffix.Format(_L(" (%d)"), n);
			const TInt dot = aRoot.LocateReverse('.');
			if (dot > 0 && cand.Length() + suffix.Length() <= cand.MaxLength())
				cand.Insert(dot, suffix);
			else if (cand.Length() + suffix.Length() <= cand.MaxLength())
				cand.Append(suffix);
			}
		TFileName full(KReceived);
		full.Append(cand);
		TEntry e;
		if (iFs.Entry(full, e) != KErrNone)
			break;
		}
	HBufC* a = aRoot.Alloc();
	HBufC* b = cand.Alloc();
	if (!a || !b || iRootFrom.Append(a) != KErrNone || iRootTo.Append(b) != KErrNone)
		{
		delete a;
		delete b;
		return KErrNoMemory;
		}
	aTo = cand;
	return KErrNone;
	}

// the name the last call put at KOffName (UTF-8, '/' or '\' separated, relative) -> aOut = the full safe path under Received.
// aEntries = the number of path components
TBool CDndService::MakePath(TDes& aOut, TUint32& aEntries)
	{
	TPtrC8 raw(iHgcm->Data() + KOffName);
	const TInt len = raw.Locate(0) >= 0 ? raw.Locate(0) : KNameMax;
	TBuf8<KMaxFileName> u8;
	u8.Copy(raw.Left(Min(len, u8.MaxLength())));
	TFileName name;
	if (CnvUtfConverter::ConvertToUnicodeFromUtf8(name, u8) < 0 || name.Length() == 0)
		return EFalse;
	aOut = KReceived;
	aEntries = 0;
	TBool first = ETrue;
	TInt pos = 0;
	while (pos <= name.Length())
		{
		TInt sep = name.Mid(pos).Locate('/');
		TInt sep2 = name.Mid(pos).Locate('\\');
		if (sep < 0 || (sep2 >= 0 && sep2 < sep))
			sep = sep2;
		TPtrC comp = sep < 0 ? name.Mid(pos) : name.Mid(pos, sep);
		pos += (sep < 0 ? comp.Length() + 1 : sep + 1);
		if (comp.Length() == 0 || comp == _L("."))
			continue;
		if (comp == _L(".."))
			return EFalse;
		TFileName c(comp);
		for (TInt i = 0; i < c.Length(); ++i)
			{
			const TText ch = c[i];
			if (ch < 32 || ch == ':' || ch == '*' || ch == '?' || ch == '"' || ch == '<' || ch == '>' || ch == '|')
				c[i] = '_';
			}
		if (first)
			{
			TFileName mapped;
			if (MapRoot(c, mapped) != KErrNone)
				return EFalse;
			c = mapped;
			first = EFalse;
			}
		if (aOut.Length() + c.Length() + 1 > aOut.MaxLength())
			return EFalse;
		aOut.Append(c);
		aOut.Append('\\');
		++aEntries;
		}
	if (aEntries == 0)
		return EFalse;
	aOut.SetLength(aOut.Length() - 1);			// no trailing backslash: it is a file, or a directory (the caller adds it)
	return ETrue;
	}

// the call that delivers a name (DIR: parameters 4, name 1; FILE_HDR: 6, name 1); aNameParm is the buffer's index
TInt CDndService::RecvName(TUint32 aFn, TInt aParms, TInt aNameParm)
	{
	iHgcm->Begin(3, iClient, aFn, aParms);
	iHgcm->ParmU32(3, 0, 0);
	iHgcm->ParmBuf(3, aNameParm, KOffName, KNameMax, KHgcmFromHost);
	iHgcm->ParmU32(3, aNameParm + 1, KNameMax);
	for (TInt i = aNameParm + 2; i < aParms; ++i)
		{
		if (aFn == EHgFileHdr && i == 5)
			iHgcm->ParmU64(3, i, 0);
		else
			iHgcm->ParmU32(3, i, 0);
		}
	return Sync(3);
	}

void CDndService::Notice(TInt aFiles, TInt aErr)
	{
	TBuf<96> text;
	if (aErr == KErrNone)
		text.Format(_L("%d file(s) received from the host: C:\\Data\\Received"), aFiles);
	else if (aErr == KErrCancel)
		text = _L("The host could not send the file");
	else
		text.Format(_L("Drop from the host failed (%d)"), aErr);
	CAknGlobalNote* note = NULL;
	TRAPD(r, note = CAknGlobalNote::NewL());
	if (r == KErrNone)
		{
		TRAP(r, note->ShowNoteL(aErr == KErrNone ? EAknGlobalConfirmationNote : EAknGlobalErrorNote, text));
		delete note;
		}
	Log("notice", r, aFiles);
	}

void CDndService::ReceiveDrop()
	{
	iAsked = EFalse;
	iRootFrom.ResetAndDestroy();
	iRootTo.ResetAndDestroy();
	iFs.MkDirAll(KReceived);
	// the header
	iHgcm->Begin(3, iClient, EHgDataHdr, 12);
	iHgcm->ParmU32(3, 0, 0);
	iHgcm->ParmU32(3, 1, 0);
	iHgcm->ParmU32(3, 2, 0);
	iHgcm->ParmU64(3, 3, 0);
	iHgcm->ParmU32(3, 4, 0);
	iHgcm->ParmBuf(3, 5, KOffFmt, KFmtMax, KHgcmFromHost);
	iHgcm->ParmU32(3, 6, KFmtMax);
	iHgcm->ParmU64(3, 7, 0);
	iHgcm->ParmU32(3, 8, 0);
	iHgcm->ParmU32(3, 9, 0);
	iHgcm->ParmBuf(3, 10, KOffSum, KSumMax, KHgcmFromHost);
	iHgcm->ParmU32(3, 11, KSumMax);
	TInt r = Sync(3);
	if (r != KErrNone)
		{
		Log("data header failed", r);
		Progress(KProgressError, r);
		Notice(0, r);
		return;
		}
	const TUint64 total = iHgcm->ParmValue64(3, 3);
	const TUint32 meta = iHgcm->ParmValue32(3, 4);
	TUint32 objects = (TUint32)iHgcm->ParmValue64(3, 7);
	Log("data header: total bytes, meta bytes", (TInt)total, (TInt)meta);
	Log("objects", (TInt)objects);
	// the meta data (the URI list: not needed, the objects carry their names)
	const TInt chunk = KChunk;									// the protocol's default chunk size: the host sends up to this much per call, and a smaller buffer is an overflow (VERR_BUFFER_OVERFLOW, the drop is aborted)
	if (iHgcm->DataBytes() - KOffData < KChunk)
		{
		Progress(KProgressError, KErrNoMemory);
		Notice(0, KErrNoMemory);
		return;
		}
	TUint32 gotMeta = 0;
	while (gotMeta < meta && r == KErrNone)
		{
		iHgcm->Begin(3, iClient, EHgData, 5);
		iHgcm->ParmU32(3, 0, 0);
		iHgcm->ParmBuf(3, 1, KOffData, (TInt)Min((TUint32)chunk, meta - gotMeta), KHgcmFromHost);
		iHgcm->ParmU32(3, 2, 0);
		iHgcm->ParmBuf(3, 3, KOffSum, KSumMax, KHgcmFromHost);
		iHgcm->ParmU32(3, 4, KSumMax);
		r = Sync(3);
		const TUint32 cb = iHgcm->ParmValue32(3, 2);
		if (r == KErrNone && cb == 0)
			r = KErrCorrupt;
		gotMeta += cb;
		}
	TInt files = 0;
	TBool cancelled = EFalse;
	iFileOpen = EFalse;
	TFileName path;
	while (r == KErrNone && objects > 0)
		{
		TUint32 msg = 0;
		r = NextMsg(msg);
		if (r != KErrNone)
			break;
		TUint32 entries = 0;
		switch (msg)
			{
		case EHgDir:
			r = RecvName(EHgDir, 4, 1);
			if (r == KErrNone && MakePath(path, entries))
				{
				path.Append('\\');
				const TInt m = iFs.MkDirAll(path);
				if (m != KErrNone && m != KErrAlreadyExists)
					r = m;
				--objects;
				}
			else if (r == KErrNone)
				r = KErrBadName;
			break;
		case EHgFileHdr:
			r = RecvName(EHgFileHdr, 6, 1);
			if (r == KErrNone && MakePath(path, entries))
				{
				iFileLeft = (TInt64)iHgcm->ParmValue64(3, 5);
				TParse p;
				p.Set(path, NULL, NULL);
				iFs.MkDirAll(p.DriveAndPath());
				r = iFile.Replace(iFs, path, EFileWrite | EFileShareExclusive);
				if (r == KErrNone)
					{
					iFileOpen = ETrue;
					if (iFileLeft == 0)
						{
						iFile.Close();
						iFileOpen = EFalse;
						--objects;
						++files;
						}
					}
				}
			else if (r == KErrNone)
				r = KErrBadName;
			break;
		case EHgFileData:
			{
			iHgcm->Begin(3, iClient, EHgFileData, 5);
			iHgcm->ParmU32(3, 0, 0);
			iHgcm->ParmBuf(3, 1, KOffData, chunk, KHgcmFromHost);
			iHgcm->ParmU32(3, 2, 0);
			iHgcm->ParmBuf(3, 3, KOffSum, KSumMax, KHgcmFromHost);
			iHgcm->ParmU32(3, 4, KSumMax);
			r = Sync(3);
			if (r == KErrNone && iFileOpen)
				{
				const TInt cb = (TInt)iHgcm->ParmValue32(3, 2);
				TPtrC8 d(iHgcm->Data() + KOffData, Min(cb, chunk));
				r = iFile.Write(d);
				iFileLeft -= d.Length();
				if (r == KErrNone && iFileLeft <= 0)
					{
					iFile.Close();
					iFileOpen = EFalse;
					--objects;
					++files;
					}
				}
			else if (r == KErrNone)
				r = KErrNotReady;
			break;
			}
		case EHgCancel:
			iHgcm->Begin(3, iClient, EHgCancel, 1);
			iHgcm->ParmU32(3, 0, 0);
			Sync(3);
			cancelled = ETrue;
			r = KErrCancel;
			break;
		default:
			Log("unexpected message in a drop", (TInt)msg);
			r = KErrNotSupported;
			break;
			}
		}
	if (iFileOpen)
		{
		iFile.Close();
		iFileOpen = EFalse;
		iFs.Delete(path);							// the file that was cut short
		}
	if (r == KVbErrCancelled)
		r = KErrCancel;								// the host ended it
	Log("drop done: files, status", files, r);
	if (r == KErrNone)
		{
		Progress(KProgressComplete, 0);
		Notice(files, KErrNone);
		}
	else if (r == KErrCancel && !cancelled)
		Notice(files, KErrCancel);
	else if (!cancelled)
		{
		Progress(KProgressError, r);
		Notice(files, r);
		}
	iAccept = EFalse;
	}

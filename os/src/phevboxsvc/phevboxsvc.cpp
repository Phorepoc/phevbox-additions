/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * phevboxsvc -- the VirtualBox guest additions' user-side service for Phorepoc: HGCM over the PheVmmDev channel
 * (vbox_hgcm.cpp) and, on it, the shared clipboard (text).
 *
 * The clipboard protocol is VirtualBox's "legacy" one (VBox/HostServices/VBoxClipboardSvc.h, GPL-3.0-only OR CDDL-1.0; no
 * feature negotiation, no transfers): connect to "VBoxSharedClipboard"; a parked call (function 1) waits for a host message:
 *   3  the host's clipboard changed (formats mask)  -> we read the text (function 3) and put it in the Symbian clipboard
 *   2  the host wants our data (a format)           -> we write the Symbian clipboard's text (function 4)
 *   1  quit / 4 cancelled                           -> connect again
 * and when the Symbian clipboard changes we tell the host its formats (function 2); the host asks for the data when someone
 * pastes. Text is Unicode text (bit 0): UTF-16, NUL terminated, CR LF for line ends; Symbian's clipboard (CClipboard,
 * \System\Data\Clpboard.cbd) holds CPlainText, whose paragraph end is U+2029 and line break U+2028.
 * The Symbian clipboard has no change notification: it is read once a second (a file's time stamp is 2 s granular on FAT,
 * so the text itself is compared).
 *
 * One thread, one timer (25 ms while the parked call is outstanding: the host completes it by writing into shared memory).
 * The text is limited to what the channel's data area holds.
 */
#include <e32base.h>
#include <e32std.h>
#include <e32debug.h>
#include <f32file.h>
#include <baclipb.h>
#include <txtetext.h>
#include "vbox_hgcm.h"
#include "vbox_dnd.h"

_LIT8(KClipService, "VBoxSharedClipboard");
_LIT(KSingleton, "PheVboxSvcSingleton");

// VBoxClipboardSvc.h / SharedClipboard.h
const TUint32 KFnGetHostMsgOld = 1, KFnReportFormats = 2, KFnDataRead = 3, KFnDataWrite = 4;
const TUint32 KMsgQuit = 1, KMsgReadData = 2, KMsgFormatsReport = 3, KMsgCancelled = 4;
const TUint32 KFmtUnicodeText = 1;

const TInt KTickMs = 25;
const TInt KRetryMs = 10000;			// between connect attempts while the host refuses
const TInt KClipPollTicks = 40;			// 1 s

#ifdef PHEVBOX_SELFTEST
_LIT(KSelfText, "phevbox: guest to host clipboard, from the Symbian clipboard");
#endif

static void Log(const char* aMsg, TInt a = 0, TInt b = 0)
	{
	RDebug::Printf("[phevbox] %s %d %d", aMsg, a, b);
	}

// ---- the Symbian clipboard ---------------------------------------------------------------------------------------

// the text on the Symbian clipboard, "" when there is none (leaves on a store error)
static HBufC* ReadClipTextL(RFs& aFs)
	{
	CClipboard* clip = NULL;
	TRAPD(r, clip = CClipboard::NewForReadingL(aFs));
	if (r != KErrNone || !clip)
		return HBufC::NewL(0);
	CleanupStack::PushL(clip);
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	TInt n = 0;
	TRAP(r, n = text->PasteFromStoreL(clip->Store(), clip->StreamDictionary(), 0));
	HBufC* out = HBufC::NewL(r == KErrNone ? text->DocumentLength() : 0);
	if (r == KErrNone && text->DocumentLength() > 0)
		{
		TPtr p = out->Des();
		text->Extract(p, 0, text->DocumentLength());
		}
	CleanupStack::PopAndDestroy(2);
	(void)n;
	return out;
	}

static void WriteClipTextL(RFs& aFs, const TDesC& aText)
	{
	CPlainText* text = CPlainText::NewL();
	CleanupStack::PushL(text);
	if (aText.Length())
		text->InsertL(0, aText);
	CClipboard* clip = CClipboard::NewForWritingLC(aFs);
	text->CopyToStoreL(clip->Store(), clip->StreamDictionary(), 0, text->DocumentLength());
	clip->CommitL();
	CleanupStack::PopAndDestroy(2);
	}

// ---- the service -------------------------------------------------------------------------------------------------

class CClipService : public CTimer
	{
public:
	static CClipService* NewL()
		{
		CClipService* s = new (ELeave) CClipService;
		CleanupStack::PushL(s);
		s->ConstructL();
		CleanupStack::Pop(s);
		return s;
		}
	~CClipService()
		{
		Cancel();
		if (iHgcm && iClient)
			iHgcm->Disconnect(iClient);
		delete iDnd;
		delete iHgcm;
		delete iLast;
		iFs.Close();
		}
	void StartL()
		{
		iHgcm = CVbHgcm::NewL();
		User::LeaveIfError(iFs.Connect());
		TRAPD(dr, iDnd = CDndService::NewL(iHgcm, iFs));
		Log("drag and drop service", dr);
		TryConnectL();
		}
	// connect to the clipboard service; if the host refuses (clipboard mode off, the host cannot open its clipboard, no service) try again
	// every 10 s: the mode can be switched on while the device runs
	void TryConnectL()
		{
		iClient = 0;
		TInt r = iHgcm->Connect(KClipService, iClient);
		Log("shared clipboard connect", r, (TInt)iClient);
		if (r != KErrNone)
			{
			iClient = 0;
			iRetryTicks = KRetryMs / KTickMs;
			After(KTickMs * 1000);
			return;
			}
		delete iLast;
		iLast = ReadClipTextL(iFs);						// what is there now is not news
		iWaiting = EFalse;
		After(KTickMs * 1000);
		}
private:
	CClipService() : CTimer(EPriorityStandard) { CActiveScheduler::Add(this); }
	void ConstructL() { CTimer::ConstructL(); }
	void RunL();
	void ArmWait();
	void OnMessage(TUint32 aMsg, TUint32 aFormats);
	void GetHostText();
	void SendGuestText();
	void CheckGuestClipboard();

	CVbHgcm* iHgcm;
	TUint32 iClient;
	RFs iFs;
	TBool iWaiting;
	TInt iTicks;
	TInt iRetryTicks;
	CDndService* iDnd;
	HBufC* iLast;										// the clipboard text as last seen / set / reported
	TBool iDenied;
#ifdef PHEVBOX_SELFTEST
	TBool iSelfDone;
	TInt iSelfTicks;
#endif
	};

void CClipService::ArmWait()
	{
	iHgcm->Begin(0, iClient, KFnGetHostMsgOld, 2);
	iHgcm->ParmU32(0, 0, 0);
	iHgcm->ParmU32(0, 1, 0);
	TInt r = iHgcm->Submit(0);
	if (r != KErrNone)
		Log("wait for host message: submit failed", r, iHgcm->iLastRc);
	else
		iWaiting = ETrue;
	}

void CClipService::RunL()
	{
	if (iStatus.Int() != KErrNone)
		return;											// cancelled
	if (iDnd)
		iDnd->Tick();									// drag and drop has its own connection and its own retries
	if (!iClient)
		{
		if (iRetryTicks > 0)
			--iRetryTicks;
		else
			{
			TRAPD(cr, TryConnectL());
			if (cr != KErrNone)
				{
				iRetryTicks = KRetryMs / KTickMs;
				After(KTickMs * 1000);
				}
			}
		return;
		}
	if (iWaiting && iHgcm->Done(0))
		{
		iWaiting = EFalse;
		const TInt res = iHgcm->Result(0);
		if (res >= 0)
			OnMessage(iHgcm->ParmValue32(0, 0), iHgcm->ParmValue32(0, 1));
		else
			Log("host message call failed", res);
		}
	if (!iWaiting)
		ArmWait();
	if (++iTicks >= KClipPollTicks)
		{
		iTicks = 0;
		CheckGuestClipboard();
		}
	After(KTickMs * 1000);
	}

void CClipService::OnMessage(TUint32 aMsg, TUint32 aFormats)
	{
	Log("host message", (TInt)aMsg, (TInt)aFormats);
	switch (aMsg)
		{
	case KMsgFormatsReport:
		if (aFormats & KFmtUnicodeText)
			GetHostText();
		break;
	case KMsgReadData:
		if (aFormats & KFmtUnicodeText)
			SendGuestText();
		break;
	default:
		break;											// quit, cancelled: the next wait tells
		}
	}

// the host's clipboard text -> the Symbian clipboard
void CClipService::GetHostText()
	{
	const TInt max = iHgcm->DataBytes();
	iHgcm->Begin(1, iClient, KFnDataRead, 3);
	iHgcm->ParmU32(1, 0, KFmtUnicodeText);
	iHgcm->ParmBuf(1, 1, 0, max, KHgcmFromHost);
	iHgcm->ParmU32(1, 2, 0);
	TInt r = iHgcm->Submit(1);
	if (r == KErrNone)
		r = iHgcm->Wait(1, 3000);
	if (r != KErrNone)
		{
		Log("read host data: failed", r, iHgcm->iLastRc);
		return;
		}
	const TInt res = iHgcm->Result(1);
	const TInt cb = (TInt)iHgcm->ParmValue32(1, 2);
	if (res < 0 || cb <= 0 || cb > max)
		{
		Log("read host data: status / size", res, cb);
		return;
		}
	// UTF-16, NUL terminated; CR LF and lone LF -> paragraph end
	const TUint16* src = (const TUint16*)iHgcm->Data();
	TInt n = cb / 2;
	while (n > 0 && src[n - 1] == 0)
		--n;
	HBufC* t = HBufC::New(n);
	if (!t)
		return;
	TPtr tp = t->Des();
	for (TInt i = 0; i < n; ++i)
		{
		const TUint16 c = src[i];
		if (c == 0x0D && i + 1 < n && src[i + 1] == 0x0A)
			continue;
		tp.Append(c == 0x0A || c == 0x0D ? (TUint16)CEditableText::EParagraphDelimiter : c);
		}
	TRAPD(wr, WriteClipTextL(iFs, *t));
	Log("host text -> Symbian clipboard: chars, status", t->Length(), wr);
	if (wr == KErrNone)
		{
		delete iLast;
		iLast = t;										// not a guest change: do not report it back
		}
	else
		delete t;
	}

// the Symbian clipboard's text -> the host (it asked for it)
void CClipService::SendGuestText()
	{
	HBufC* t = NULL;
	TRAPD(r, t = ReadClipTextL(iFs));
	if (r != KErrNone || !t)
		{
		Log("guest text: clipboard read failed", r);
		return;
		}
	const TInt max = iHgcm->DataBytes() / 2 - 2;
	TUint16* dst = (TUint16*)iHgcm->Data();
	TInt n = 0;
	for (TInt i = 0; i < t->Length() && n < max; ++i)
		{
		const TUint16 c = (*t)[i];
		if (c == CEditableText::EParagraphDelimiter || c == CEditableText::ELineBreak || c == 0x0A)
			{
			dst[n++] = 0x0D;
			if (n < max)
				dst[n++] = 0x0A;
			}
		else
			dst[n++] = c;
		}
	dst[n++] = 0;
	delete t;
	iHgcm->Begin(1, iClient, KFnDataWrite, 2);
	iHgcm->ParmU32(1, 0, KFmtUnicodeText);
	iHgcm->ParmBuf(1, 1, 0, n * 2, KHgcmToHost);
	r = iHgcm->Submit(1);
	if (r == KErrNone)
		r = iHgcm->Wait(1, 3000);
	Log("guest text -> host: chars, status", n - 1, r == KErrNone ? iHgcm->Result(1) : r);
	}

// a change on the Symbian clipboard -> tell the host (it fetches the text when someone pastes)
void CClipService::CheckGuestClipboard()
	{
	HBufC* now = NULL;
	TRAPD(r, now = ReadClipTextL(iFs));
	if (r == KErrNone && now)
		{
		if (iLast->Compare(*now) != 0)
			{
			delete iLast;
			iLast = now;
			now = NULL;
			iHgcm->Begin(1, iClient, KFnReportFormats, 1);
			iHgcm->ParmU32(1, 0, iLast->Length() ? KFmtUnicodeText : 0);
			TInt s = iHgcm->Submit(1);
			if (s == KErrNone)
				s = iHgcm->Wait(1, 3000);
			const TInt res = s == KErrNone ? iHgcm->Result(1) : s;
			Log("guest clipboard changed, formats reported: chars, status", iLast->Length(), res);
			if (res < 0 && !iDenied)
				{
				iDenied = ETrue;						// the host's clipboard mode (Devices > Shared Clipboard) is off or one way
				Log("the host does not take the guest's clipboard (mode?)");
				}
			}
		}
	delete now;
#ifdef PHEVBOX_SELFTEST
	// the test build: ten seconds in, put a known text on the Symbian clipboard; the next poll reports it
	if (!iSelfDone && ++iSelfTicks >= 10)
		{
		iSelfDone = ETrue;
		TRAPD(w, WriteClipTextL(iFs, KSelfText));
		Log("selftest: text set on the Symbian clipboard", w);
		}
#endif
	}

static TInt Run()
	{
	RSemaphore single;
	if (single.CreateGlobal(KSingleton, 0) != KErrNone)
		{
		Log("already running");
		return KErrNone;
		}
	CTrapCleanup* cleanup = CTrapCleanup::New();
	CActiveScheduler* sched = new CActiveScheduler;
	if (!cleanup || !sched)
		return KErrNoMemory;
	CActiveScheduler::Install(sched);
	CClipService* svc = NULL;
	TRAPD(r, svc = CClipService::NewL());
	if (r == KErrNone)
		TRAP(r, svc->StartL());
	Log("started", r);
	if (r == KErrNone)
		CActiveScheduler::Start();
	delete svc;
	delete sched;
	delete cleanup;
	single.Close();
	return r;
	}

TInt E32Main()
	{
	return Run();
	}

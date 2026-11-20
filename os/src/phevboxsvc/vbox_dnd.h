/*
 * Copyright The Phorepoc Authors
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * vbox_dnd.h -- drag and drop from the host to the device (VirtualBox's host service "VBoxDragAndDropSvc"), on the HGCM
 * transport (vbox_hgcm.h). Host to guest only: files dropped on the VM window arrive in C:\Data\Received\ and a note says so.
 */
#ifndef VBOX_DND_H
#define VBOX_DND_H

#include <e32base.h>
#include <f32file.h>
#include "vbox_hgcm.h"

class CDndService : public CBase
	{
public:
	static CDndService* NewL(CVbHgcm* aHgcm, RFs& aFs);
	~CDndService();
	void Tick();					// every 25 ms from the service's timer: connect (again), the parked wait, a message
private:
	CDndService(CVbHgcm* aHgcm, RFs& aFs) : iHgcm(aHgcm), iFs(aFs) {}
	TBool TryConnect();
	void ArmWait();
	void OnMessage(TUint32 aMsg);
	TInt Sync(TInt aParms);			// slot 3: submit the call built there and wait for it
	TInt NextMsg(TUint32& aMsg);	// a blocking "next host message" on slot 3
	TInt RecvAction(TUint32 aFn, TUint32& aAllActions);
	void AckOp(TUint32 aAction);
	void ReqData();
	void Progress(TUint32 aStatus, TInt aRc);
	void ReceiveDrop();
	TInt RecvName(TUint32 aFn, TInt aParms, TInt aNameParm);
	TBool MakePath(TDes& aOut, TUint32& aEntries);
	TInt MapRoot(const TDesC& aRoot, TDes& aTo);
	void Notice(TInt aFiles, TInt aErr);

	CVbHgcm* iHgcm;
	RFs& iFs;
	TUint32 iClient;
	TBool iWaiting;
	TBool iAccept;					// the host offers a URI list (files)
	TBool iAsked;					// we asked for the data and the host has not started it
	TInt iRetryTicks;
	RFile iFile;
	TBool iFileOpen;
	TInt64 iFileLeft;
	RPointerArray<HBufC> iRootFrom, iRootTo;	// top-level names of this drop and the names they were given here
	TFileName iName;
	};

#endif

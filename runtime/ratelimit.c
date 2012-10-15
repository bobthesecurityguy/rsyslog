/* ratelimit.c
 * support for rate-limiting sources, including "last message
 * repeated n times" processing.
 *
 * Copyright 2012 Rainer Gerhards and Adiscon GmbH.
 *
 * This file is part of the rsyslog runtime library.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *       http://www.apache.org/licenses/LICENSE-2.0
 *       -or-
 *       see COPYING.ASL20 in the source distribution
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "rsyslog.h"
#include "errmsg.h"
#include "ratelimit.h"
#include "datetime.h"
#include "parser.h"
#include "unicode-helper.h"
#include "msg.h"
#include "dirty.h"

/* definitions for objects we access */
DEFobjStaticHelpers
DEFobjCurrIf(errmsg)
DEFobjCurrIf(glbl)
DEFobjCurrIf(datetime)
DEFobjCurrIf(parser)

/* static data */

/* generate a "repeated n times" message */
static inline msg_t *
ratelimitGenRepMsg(ratelimit_t *ratelimit)
{
	msg_t *repMsg;
	size_t lenRepMsg;
	uchar szRepMsg[1024];

	if(ratelimit->nsupp == 1) { /* we simply use the original message! */
		repMsg = MsgAddRef(ratelimit->pMsg);
	} else {/* we need to duplicate, original message may still be in use in other
		 * parts of the system!  */
		if((repMsg = MsgDup(ratelimit->pMsg)) == NULL) {
			DBGPRINTF("Message duplication failed, dropping repeat message.\n");
			goto done;
		}
		lenRepMsg = snprintf((char*)szRepMsg, sizeof(szRepMsg),
					" message repeated %d times: [%.800s]",
					ratelimit->nsupp, getMSG(ratelimit->pMsg));
		MsgReplaceMSG(repMsg, szRepMsg, lenRepMsg);
	}

done:	return repMsg;
}

/* ratelimit a message, that means:
 * - handle "last message repeated n times" logic
 * - handle actual (discarding) rate-limiting
 * This function returns RS_RET_OK, if the caller shall process
 * the message regularly and RS_RET_DISCARD if the caller must
 * discard the message. The caller should also discard the message
 * if another return status occurs. This places some burden on the
 * caller logic, but provides best performance. Demanding this
 * cooperative mode can enable a faulty caller to thrash up part
 * of the system, but we accept that risk (a faulty caller can
 * always do all sorts of evil, so...)
 * If *ppRepMsg != NULL on return, the caller must enqueue that
 * message before the original message.
 */
rsRetVal
ratelimitMsg(ratelimit_t *ratelimit, msg_t *pMsg, msg_t **ppRepMsg)
{
	rsRetVal localRet;
	int bNeedUnlockMutex = 0;
	DEFiRet;

	if((pMsg->msgFlags & NEEDS_PARSING) != 0) {
		if((localRet = parser.ParseMsg(pMsg)) != RS_RET_OK)  {
			DBGPRINTF("Message discarded, parsing error %d\n", localRet);
			ABORT_FINALIZE(RS_RET_DISCARDMSG);
		}
	}

	if(ratelimit->bThreadSafe) {
		pthread_mutex_lock(&ratelimit->mut);
		bNeedUnlockMutex = 1;
	}

	*ppRepMsg = NULL;
	/* suppress duplicate messages */
	if( ratelimit->pMsg != NULL &&
	    getMSGLen(pMsg) == getMSGLen(ratelimit->pMsg) &&
	    !ustrcmp(getMSG(pMsg), getMSG(ratelimit->pMsg)) &&
	    !strcmp(getHOSTNAME(pMsg), getHOSTNAME(ratelimit->pMsg)) &&
	    !strcmp(getPROCID(pMsg, LOCK_MUTEX), getPROCID(ratelimit->pMsg, LOCK_MUTEX)) &&
	    !strcmp(getAPPNAME(pMsg, LOCK_MUTEX), getAPPNAME(ratelimit->pMsg, LOCK_MUTEX))) {
		ratelimit->nsupp++;
		DBGPRINTF("msg repeated %d times\n", ratelimit->nsupp);
		/* use current message, so we have the new timestamp
		 * (means we need to discard previous one) */
		msgDestruct(&ratelimit->pMsg);
		ratelimit->pMsg = pMsg;
		ABORT_FINALIZE(RS_RET_DISCARDMSG);
	} else {/* new message, do "repeat processing" & save it */
		if(ratelimit->pMsg != NULL) {
			if(ratelimit->nsupp > 0) {
				*ppRepMsg = ratelimitGenRepMsg(ratelimit);
				ratelimit->nsupp = 0;
			}
			msgDestruct(&ratelimit->pMsg);
		}
		ratelimit->pMsg = MsgAddRef(pMsg);
	}

finalize_it:
	if(bNeedUnlockMutex)
		pthread_mutex_unlock(&ratelimit->mut);
	RETiRet;
}


/* add a message to a ratelimiter/multisubmit structure.
 * ratelimiting is automatically handled according to the ratelimit
 * settings.
 * if pMultiSub == NULL, a single-message enqueue happens (under reconsideration)
 */
rsRetVal
ratelimitAddMsg(ratelimit_t *ratelimit, multi_submit_t *pMultiSub, msg_t *pMsg)
{
	rsRetVal localRet;
	msg_t *repMsg;
	DEFiRet;

	if(pMultiSub == NULL) {
dbgprintf("DDDDD: multiSubmitAddMsg, not checking ratelimiter for single submit!\n");
#warning missing multisub Implementation?
		CHKiRet(submitMsg(pMsg));
	} else {
		localRet = ratelimitMsg(ratelimit, pMsg, &repMsg);
		if(repMsg != NULL) {
			pMultiSub->ppMsgs[pMultiSub->nElem++] = repMsg;
			if(pMultiSub->nElem == pMultiSub->maxElem)
				CHKiRet(multiSubmitMsg2(pMultiSub));
		}
		if(localRet == RS_RET_OK) {
			pMultiSub->ppMsgs[pMultiSub->nElem++] = pMsg;
			if(pMultiSub->nElem == pMultiSub->maxElem)
				CHKiRet(multiSubmitMsg2(pMultiSub));
		}
	}

finalize_it:
	RETiRet;
}

rsRetVal
ratelimitNew(ratelimit_t **ppThis)
{
	ratelimit_t *pThis;
	DEFiRet;

	CHKmalloc(pThis = calloc(1, sizeof(ratelimit_t)));
	*ppThis = pThis;
finalize_it:
	RETiRet;
}

/* enable thread-safe operations mode. This make sure that
 * a single ratelimiter can be called from multiple threads. As
 * this causes some overhead and is not always required, it needs
 * to be explicitely enabled. This operation cannot be undone
 * (think: why should one do that???)
 */
void
ratelimitSetThreadSafe(ratelimit_t *ratelimit)
{
	ratelimit->bThreadSafe = 1;
	pthread_mutex_init(&ratelimit->mut, NULL);
}

void
ratelimitDestruct(ratelimit_t *ratelimit)
{
	msg_t *pMsg;
	if(ratelimit->pMsg != NULL) {
		if(ratelimit->nsupp > 0) {
			pMsg = ratelimitGenRepMsg(ratelimit);
			if(pMsg != NULL)
				submitMsg2(pMsg);
		}
		msgDestruct(&ratelimit->pMsg);
	}
	if(ratelimit->bThreadSafe)
		pthread_mutex_destroy(&ratelimit->mut);
	free(ratelimit);
}

void
ratelimitModExit(void)
{
	objRelease(datetime, CORE_COMPONENT);
	objRelease(glbl, CORE_COMPONENT);
	objRelease(errmsg, CORE_COMPONENT);
	objRelease(parser, CORE_COMPONENT);
}

rsRetVal
ratelimitModInit(void)
{
	DEFiRet;
	CHKiRet(objGetObjInterface(&obj));
	CHKiRet(objUse(glbl, CORE_COMPONENT));
	CHKiRet(objUse(datetime, CORE_COMPONENT));
	CHKiRet(objUse(errmsg, CORE_COMPONENT));
	CHKiRet(objUse(parser, CORE_COMPONENT));
finalize_it:
	RETiRet;
}


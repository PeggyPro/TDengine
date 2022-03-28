/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http:www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "bmInt.h"

static void bmSendErrorRsp(SMgmtWrapper *pWrapper, SNodeMsg *pMsg, int32_t code) {
  SRpcMsg rpcRsp = {.handle = pMsg->rpcMsg.handle, .ahandle = pMsg->rpcMsg.ahandle, .code = code};
  dndSendRsp(pWrapper, &rpcRsp);

  dTrace("msg:%p, is freed", pMsg);
  rpcFreeCont(pMsg->rpcMsg.pCont);
  taosFreeQitem(pMsg);
}

static void bmSendErrorRsps(SMgmtWrapper *pWrapper, STaosQall *qall, int32_t numOfMsgs, int32_t code) {
  for (int32_t i = 0; i < numOfMsgs; ++i) {
    SNodeMsg *pMsg = NULL;
    taosGetQitem(qall, (void **)&pMsg);
    bmSendErrorRsp(pWrapper, pMsg, code);
  }
}

static void bmProcessQueue(SQueueInfo *pInfo, STaosQall *qall, int32_t numOfMsgs) {
  SBnodeMgmt   *pMgmt = pInfo->ahandle;
  SMgmtWrapper *pWrapper = pMgmt->pWrapper;

  SArray *pArray = taosArrayInit(numOfMsgs, sizeof(SNodeMsg *));
  if (pArray == NULL) {
    bmSendErrorRsps(pWrapper, qall, numOfMsgs, TSDB_CODE_OUT_OF_MEMORY);
    return;
  }

  for (int32_t i = 0; i < numOfMsgs; ++i) {
    SNodeMsg *pMsg = NULL;
    taosGetQitem(qall, (void **)&pMsg);
    dTrace("msg:%p, will be processed in bnode queue", pMsg);
    if (taosArrayPush(pArray, &pMsg) == NULL) {
      bmSendErrorRsp(pWrapper, pMsg, TSDB_CODE_OUT_OF_MEMORY);
    }
  }

  bndProcessWMsgs(pMgmt->pBnode, pArray);

  for (size_t i = 0; i < numOfMsgs; i++) {
    SNodeMsg *pMsg = *(SNodeMsg **)taosArrayGet(pArray, i);
    dTrace("msg:%p, is freed", pMsg);
    rpcFreeCont(pMsg->rpcMsg.pCont);
    taosFreeQitem(pMsg);
  }
  taosArrayDestroy(pArray);
}

int32_t bmProcessWriteMsg(SMgmtWrapper *pWrapper, SNodeMsg *pMsg) {
  SBnodeMgmt   *pMgmt = pWrapper->pMgmt;
  SMultiWorker *pWorker = &pMgmt->writeWorker;

  dTrace("msg:%p, put into worker:%s", pMsg, pWorker->name);
  taosWriteQitem(pWorker->queue, pMsg);
  return 0;
}

int32_t bmStartWorker(SBnodeMgmt *pMgmt) {
  SMultiWorkerCfg cfg = {.max = 1, .name = "bnode-write", .fp = (FItems)bmProcessQueue, .param = pMgmt};
  if (tMultiWorkerInit(&pMgmt->writeWorker, &cfg) != 0) {
    dError("failed to start bnode write worker since %s", terrstr());
    return -1;
  }

  dDebug("bnode workers are initialized");
  return 0;
}

void bmStopWorker(SBnodeMgmt *pMgmt) {
  tMultiWorkerCleanup(&pMgmt->writeWorker);
  dDebug("bnode workers are closed");
}

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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "vnodeQuery.h"
#include "vnd.h"

static int32_t vnodeGetTableList(SVnode *pVnode, SRpcMsg *pMsg);
static int     vnodeGetTableMeta(SVnode *pVnode, SRpcMsg *pMsg);

int vnodeQueryOpen(SVnode *pVnode) {
  return qWorkerInit(NODE_TYPE_VNODE, pVnode->vgId, NULL, (void **)&pVnode->pQuery, pVnode,
                     (putReqToQueryQFp)vnodePutReqToVQueryQ, (sendReqToDnodeFp)vnodeSendReqToDnode);
}

int vnodeProcessQueryMsg(SVnode *pVnode, SRpcMsg *pMsg) {
  vTrace("message in query queue is processing");

  switch (pMsg->msgType) {
    case TDMT_VND_QUERY:
      return qWorkerProcessQueryMsg(pVnode->pTsdb, pVnode->pQuery, pMsg);
    case TDMT_VND_QUERY_CONTINUE:
      return qWorkerProcessCQueryMsg(pVnode->pTsdb, pVnode->pQuery, pMsg);
    default:
      vError("unknown msg type:%d in query queue", pMsg->msgType);
      return TSDB_CODE_VND_APP_ERROR;
  }
}

int vnodeProcessFetchMsg(SVnode *pVnode, SRpcMsg *pMsg) {
  vTrace("message in fetch queue is processing");
  switch (pMsg->msgType) {
    case TDMT_VND_FETCH:
      return qWorkerProcessFetchMsg(pVnode, pVnode->pQuery, pMsg);
    case TDMT_VND_FETCH_RSP:
      return qWorkerProcessFetchRsp(pVnode, pVnode->pQuery, pMsg);
    case TDMT_VND_RES_READY:
      return qWorkerProcessReadyMsg(pVnode, pVnode->pQuery, pMsg);
    case TDMT_VND_TASKS_STATUS:
      return qWorkerProcessStatusMsg(pVnode, pVnode->pQuery, pMsg);
    case TDMT_VND_CANCEL_TASK:
      return qWorkerProcessCancelMsg(pVnode, pVnode->pQuery, pMsg);
    case TDMT_VND_DROP_TASK:
      return qWorkerProcessDropMsg(pVnode, pVnode->pQuery, pMsg);
    case TDMT_VND_SHOW_TABLES:
      return qWorkerProcessShowMsg(pVnode, pVnode->pQuery, pMsg);
    case TDMT_VND_SHOW_TABLES_FETCH:
      return vnodeGetTableList(pVnode, pMsg);
      //      return qWorkerProcessShowFetchMsg(pVnode->pMeta, pVnode->pQuery, pMsg);
    case TDMT_VND_TABLE_META:
      return vnodeGetTableMeta(pVnode, pMsg);
    case TDMT_VND_CONSUME:
      return tqProcessConsumeReq(pVnode->pTq, pMsg);
    default:
      vError("unknown msg type:%d in fetch queue", pMsg->msgType);
      return TSDB_CODE_VND_APP_ERROR;
  }
}

static int vnodeGetTableMeta(SVnode *pVnode, SRpcMsg *pMsg) {
  STableInfoReq * pReq = (STableInfoReq *)(pMsg->pCont);
  STbCfg *        pTbCfg = NULL;
  STbCfg *        pStbCfg = NULL;
  tb_uid_t        uid;
  int32_t         nCols;
  int32_t         nTagCols;
  SSchemaWrapper *pSW = NULL;
  STableMetaRsp  *pTbMetaMsg = NULL;
  SSchema *       pTagSchema;
  SRpcMsg         rpcMsg;
  int             msgLen = 0;
  int32_t         code = TSDB_CODE_VND_APP_ERROR;

  pTbCfg = metaGetTbInfoByName(pVnode->pMeta, pReq->tableFname, &uid);
  if (pTbCfg == NULL) {
    code = TSDB_CODE_VND_TB_NOT_EXIST;
    goto _exit;
  }

  if (pTbCfg->type == META_CHILD_TABLE) {
    pStbCfg = metaGetTbInfoByUid(pVnode->pMeta, pTbCfg->ctbCfg.suid);
    if (pStbCfg == NULL) {
      goto _exit;
    }

    pSW = metaGetTableSchema(pVnode->pMeta, pTbCfg->ctbCfg.suid, 0, true);
  } else {
    pSW = metaGetTableSchema(pVnode->pMeta, uid, 0, true);
  }

  nCols = pSW->nCols;
  if (pTbCfg->type == META_SUPER_TABLE) {
    nTagCols = pTbCfg->stbCfg.nTagCols;
    pTagSchema = pTbCfg->stbCfg.pTagSchema;
  } else if (pTbCfg->type == META_CHILD_TABLE) {
    nTagCols = pStbCfg->stbCfg.nTagCols;
    pTagSchema = pStbCfg->stbCfg.pTagSchema;
  } else {
    nTagCols = 0;
    pTagSchema = NULL;
  }

  msgLen = sizeof(STableMetaRsp) + sizeof(SSchema) * (nCols + nTagCols);
  pTbMetaMsg = (STableMetaRsp *)rpcMallocCont(msgLen);
  if (pTbMetaMsg == NULL) {
    goto _exit;
  }

  memcpy(pTbMetaMsg->dbFname, pReq->dbFname, sizeof(pTbMetaMsg->dbFname));
  strcpy(pTbMetaMsg->tbFname, pTbCfg->name);
  if (pTbCfg->type == META_CHILD_TABLE) {
    strcpy(pTbMetaMsg->stbFname, pStbCfg->name);
    pTbMetaMsg->suid = htobe64(pTbCfg->ctbCfg.suid);
  } else if (pTbCfg->type == META_SUPER_TABLE) {
    strcpy(pTbMetaMsg->stbFname, pTbCfg->name);
    pTbMetaMsg->suid = htobe64(uid);
  }
  pTbMetaMsg->numOfTags = htonl(nTagCols);
  pTbMetaMsg->numOfColumns = htonl(nCols);
  pTbMetaMsg->tableType = pTbCfg->type;
  pTbMetaMsg->tuid = htobe64(uid);
  pTbMetaMsg->vgId = htonl(pVnode->vgId);

  memcpy(pTbMetaMsg->pSchema, pSW->pSchema, sizeof(SSchema) * pSW->nCols);
  if (nTagCols) {
    memcpy(POINTER_SHIFT(pTbMetaMsg->pSchema, sizeof(SSchema) * pSW->nCols), pTagSchema, sizeof(SSchema) * nTagCols);
  }

  for (int i = 0; i < nCols + nTagCols; i++) {
    SSchema *pSch = pTbMetaMsg->pSchema + i;
    pSch->colId = htonl(pSch->colId);
    pSch->bytes = htonl(pSch->bytes);
  }

  code = 0;

_exit:

  if (pSW != NULL) {
    tfree(pSW->pSchema);
    tfree(pSW);
  }

  if (pTbCfg) {
    tfree(pTbCfg->name);
    if (pTbCfg->type == META_SUPER_TABLE) {
      free(pTbCfg->stbCfg.pTagSchema);
    } else if (pTbCfg->type == META_SUPER_TABLE) {
      kvRowFree(pTbCfg->ctbCfg.pTag);
    }

    tfree(pTbCfg);
  }

  rpcMsg.handle = pMsg->handle;
  rpcMsg.ahandle = pMsg->ahandle;
  rpcMsg.pCont = pTbMetaMsg;
  rpcMsg.contLen = msgLen;
  rpcMsg.code = code;

  rpcSendResponse(&rpcMsg);

  return 0;
}

static void freeItemHelper(void *pItem) {
  char *p = *(char **)pItem;
  free(p);
}

/**
 * @param pVnode
 * @param pMsg
 * @param pRsp
 */
static int32_t vnodeGetTableList(SVnode *pVnode, SRpcMsg *pMsg) {
  SMTbCursor *pCur = metaOpenTbCursor(pVnode->pMeta);
  SArray *    pArray = taosArrayInit(10, POINTER_BYTES);

  char *  name = NULL;
  int32_t totalLen = 0;
  int32_t numOfTables = 0;
  while ((name = metaTbCursorNext(pCur)) != NULL) {
    if (numOfTables < 10000) {  // TODO: temp get tables of vnode, and should del when show tables commad ok.
      taosArrayPush(pArray, &name);
      totalLen += strlen(name);
    } else {
      tfree(name);
    }

    numOfTables++;
  }

  // TODO: temp debug, and should del when show tables command ok
  vInfo("====vgId:%d, numOfTables: %d", pVnode->vgId, numOfTables);
  if (numOfTables > 10000) {
    numOfTables = 10000;
  }

  metaCloseTbCursor(pCur);

  int32_t rowLen =
      (TSDB_TABLE_NAME_LEN + VARSTR_HEADER_SIZE) + 8 + 2 + (TSDB_TABLE_NAME_LEN + VARSTR_HEADER_SIZE) + 8 + 4;
  // int32_t numOfTables = (int32_t)taosArrayGetSize(pArray);

  int32_t payloadLen = rowLen * numOfTables;
  //  SVShowTablesFetchReq *pFetchReq = pMsg->pCont;

  SVShowTablesFetchRsp *pFetchRsp = (SVShowTablesFetchRsp *)rpcMallocCont(sizeof(SVShowTablesFetchRsp) + payloadLen);
  memset(pFetchRsp, 0, sizeof(SVShowTablesFetchRsp) + payloadLen);

  char *p = pFetchRsp->data;
  for (int32_t i = 0; i < numOfTables; ++i) {
    char *n = taosArrayGetP(pArray, i);
    STR_TO_VARSTR(p, n);

    p += (TSDB_TABLE_NAME_LEN + VARSTR_HEADER_SIZE);
    // free(n);
  }

  pFetchRsp->numOfRows = htonl(numOfTables);
  pFetchRsp->precision = 0;

  SRpcMsg rpcMsg = {
      .handle = pMsg->handle,
      .ahandle = pMsg->ahandle,
      .pCont = pFetchRsp,
      .contLen = sizeof(SVShowTablesFetchRsp) + payloadLen,
      .code = 0,
  };

  rpcSendResponse(&rpcMsg);

  taosArrayDestroyEx(pArray, freeItemHelper);
  return 0;
}

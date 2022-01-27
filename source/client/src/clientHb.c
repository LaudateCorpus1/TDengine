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

#include "clientInt.h"
#include "trpc.h"
#include "catalog.h"
#include "clientLog.h"

static SClientHbMgr clientHbMgr = {0};

static int32_t hbCreateThread();
static void    hbStopThread();

static int32_t hbMqHbRspHandle(struct SAppHbMgr *pAppHbMgr, SClientHbRsp* pRsp) {
  return 0;
}

static int32_t hbProcessDBInfoRsp(void *value, int32_t valueLen, struct SCatalog *pCatalog) {
  int32_t msgLen = 0;
  int32_t code = 0;
  
  while (msgLen < valueLen) {
    SUseDbRsp *rsp = (SUseDbRsp *)((char *)value + msgLen);

    rsp->vgVersion = ntohl(rsp->vgVersion);
    rsp->vgNum = ntohl(rsp->vgNum);
    rsp->uid = be64toh(rsp->uid);

    tscDebug("hb db rsp, db:%s, vgVersion:%d, uid:%"PRIx64, rsp->db, rsp->vgVersion, rsp->uid);
    
    if (rsp->vgVersion < 0) {
      SDbVgVersion dbInfo;
      strcpy(dbInfo.dbName, rsp->db);
      dbInfo.dbId = rsp->uid;
      dbInfo.vgVersion = rsp->vgVersion;
      
      code = catalogRemoveDBVgroup(pCatalog, &dbInfo);
    } else {
      SDBVgroupInfo vgInfo = {0};
      vgInfo.dbId = rsp->uid;
      vgInfo.vgVersion = rsp->vgVersion;
      vgInfo.hashMethod = rsp->hashMethod;
      vgInfo.vgInfo = taosHashInit(rsp->vgNum, taosGetDefaultHashFunction(TSDB_DATA_TYPE_INT), true, HASH_ENTRY_LOCK);
      if (NULL == vgInfo.vgInfo) {
        tscError("hash init[%d] failed", rsp->vgNum);
        return TSDB_CODE_TSC_OUT_OF_MEMORY;
      }

      for (int32_t i = 0; i < rsp->vgNum; ++i) {
        rsp->vgroupInfo[i].vgId = ntohl(rsp->vgroupInfo[i].vgId);
        rsp->vgroupInfo[i].hashBegin = ntohl(rsp->vgroupInfo[i].hashBegin);
        rsp->vgroupInfo[i].hashEnd = ntohl(rsp->vgroupInfo[i].hashEnd);

        for (int32_t n = 0; n < rsp->vgroupInfo[i].epset.numOfEps; ++n) {
          rsp->vgroupInfo[i].epset.eps[n].port = ntohs(rsp->vgroupInfo[i].epset.eps[n].port);
        }

        if (0 != taosHashPut(vgInfo.vgInfo, &rsp->vgroupInfo[i].vgId, sizeof(rsp->vgroupInfo[i].vgId), &rsp->vgroupInfo[i], sizeof(rsp->vgroupInfo[i]))) {
          tscError("hash push failed, errno:%d", errno);
          taosHashCleanup(vgInfo.vgInfo);
          return TSDB_CODE_TSC_OUT_OF_MEMORY;
        }
      }  
      
      code = catalogUpdateDBVgroup(pCatalog, rsp->db, &vgInfo);
      if (code) {
        taosHashCleanup(vgInfo.vgInfo);
      }
    }

    if (code) {
      return code;
    }

    msgLen += sizeof(SUseDbRsp) + rsp->vgNum * sizeof(SVgroupInfo);
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t hbQueryHbRspHandle(struct SAppHbMgr *pAppHbMgr, SClientHbRsp* pRsp) {
  SHbConnInfo * info = taosHashGet(pAppHbMgr->connInfo, &pRsp->connKey, sizeof(SClientHbKey));
  if (NULL == info) {
    tscWarn("fail to get connInfo, may be dropped, connId:%d, type:%d", pRsp->connKey.connId, pRsp->connKey.hbType);
    return TSDB_CODE_SUCCESS;
  }

  int32_t kvNum = pRsp->info ? taosArrayGetSize(pRsp->info) : 0;

  tscDebug("hb got %d rsp kv", kvNum);
  
  for (int32_t i = 0; i < kvNum; ++i) {
    SKv *kv = taosArrayGet(pRsp->info, i);
    switch (kv->key) {
      case HEARTBEAT_KEY_DBINFO: {
        if (kv->valueLen <= 0 || NULL == kv->value) {
          tscError("invalid hb db info, len:%d, value:%p", kv->valueLen, kv->value);
          break;
        }

        int64_t *clusterId = (int64_t *)info->param;
        struct SCatalog *pCatalog = NULL;
        
        int32_t code = catalogGetHandle(*clusterId, &pCatalog);
        if (code != TSDB_CODE_SUCCESS) {
          tscWarn("catalogGetHandle failed, clusterId:%"PRIx64", error:%s", *clusterId, tstrerror(code));
          break;
        }

        hbProcessDBInfoRsp(kv->value, kv->valueLen, pCatalog);
        break;
      }
      case HEARTBEAT_KEY_STBINFO:

        break;
      default:
        tscError("invalid hb key type:%d", kv->key);
        break;
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t hbMqAsyncCallBack(void* param, const SDataBuf* pMsg, int32_t code) {
  static int32_t emptyRspNum = 0;
  if (code != 0) {
    tfree(param);
    return -1;
  }
  char *key = (char *)param;
  SClientHbBatchRsp pRsp = {0};
  tDeserializeSClientHbBatchRsp(pMsg->pData, &pRsp);
  
  int32_t rspNum = taosArrayGetSize(pRsp.rsps);

  SAppInstInfo** pInst = taosHashGet(appInfo.pInstMap, key, strlen(key));
  if (pInst == NULL || NULL == *pInst) {
    tscError("cluster not exist, key:%s", key);    
    tfree(param);
    tFreeClientHbBatchRsp(&pRsp);
    return -1;
  }

  tfree(param);

  if (rspNum) {
    tscDebug("hb got %d rsp, %d empty rsp prior", rspNum, atomic_val_compare_exchange_32(&emptyRspNum, emptyRspNum, 0));
  } else {
    atomic_add_fetch_32(&emptyRspNum, 1);
  }

  for (int32_t i = 0; i < rspNum; ++i) {
    SClientHbRsp* rsp = taosArrayGet(pRsp.rsps, i);
    code = (*clientHbMgr.rspHandle[rsp->connKey.hbType])((*pInst)->pAppHbMgr, rsp);
    if (code) {
      break;
    }
  }

  tFreeClientHbBatchRsp(&pRsp);
  
  return code;
}

int32_t hbGetExpiredDBInfo(SClientHbKey *connKey, struct SCatalog *pCatalog, SClientHbReq *req) {
  SDbVgVersion *dbs = NULL;
  uint32_t dbNum = 0;
  int32_t code = 0;

  code = catalogGetExpiredDBs(pCatalog, &dbs, &dbNum);
  if (TSDB_CODE_SUCCESS != code) {
    return code;
  }

  if (dbNum <= 0) {
    return TSDB_CODE_SUCCESS;
  }

  for (int32_t i = 0; i < dbNum; ++i) {
    SDbVgVersion *db = &dbs[i];
    db->dbId = htobe64(db->dbId);
    db->vgVersion = htonl(db->vgVersion);
  }

  SKv kv = {.key = HEARTBEAT_KEY_DBINFO, .valueLen = sizeof(SDbVgVersion) * dbNum, .value = dbs};

  tscDebug("hb got %d expired db, valueLen:%d", dbNum, kv.valueLen);

  taosHashPut(req->info, &kv.key, sizeof(kv.key), &kv, sizeof(kv));

  return TSDB_CODE_SUCCESS;
}

int32_t hbQueryHbReqHandle(SClientHbKey *connKey, void* param, SClientHbReq *req) {
  int64_t *clusterId = (int64_t *)param;
  struct SCatalog *pCatalog = NULL;

  int32_t code = catalogGetHandle(*clusterId, &pCatalog);
  if (code != TSDB_CODE_SUCCESS) {
    tscWarn("catalogGetHandle failed, clusterId:%"PRIx64", error:%s", *clusterId, tstrerror(code));
    return code;
  }
  
  code = hbGetExpiredDBInfo(connKey, pCatalog, req);
  if (TSDB_CODE_SUCCESS != code) {
    return code;
  }


  return TSDB_CODE_SUCCESS;
}

int32_t hbMqHbReqHandle(SClientHbKey *connKey, void* param, SClientHbReq *req) {

}

void hbMgrInitMqHbHandle() {
  clientHbMgr.reqHandle[HEARTBEAT_TYPE_QUERY] = hbQueryHbReqHandle;
  clientHbMgr.reqHandle[HEARTBEAT_TYPE_MQ] = hbMqHbReqHandle;
  clientHbMgr.rspHandle[HEARTBEAT_TYPE_QUERY] = hbQueryHbRspHandle;
  clientHbMgr.rspHandle[HEARTBEAT_TYPE_MQ] = hbMqHbRspHandle;
}

static FORCE_INLINE void hbMgrInitHandle() {
  // init all handle
  hbMgrInitMqHbHandle();
}

void hbFreeReq(void *req) {
  SClientHbReq *pReq = (SClientHbReq *)req;
  tFreeReqKvHash(pReq->info);
}



SClientHbBatchReq* hbGatherAllInfo(SAppHbMgr *pAppHbMgr) {
  SClientHbBatchReq* pBatchReq = malloc(sizeof(SClientHbBatchReq));
  if (pBatchReq == NULL) {
    terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
    return NULL;
  }
  int32_t connKeyCnt = atomic_load_32(&pAppHbMgr->connKeyCnt);
  pBatchReq->reqs = taosArrayInit(connKeyCnt, sizeof(SClientHbReq));

  int32_t code = 0;
  void *pIter = taosHashIterate(pAppHbMgr->activeInfo, NULL);
  while (pIter != NULL) {
    SClientHbReq* pOneReq = pIter;

    SHbConnInfo * info = taosHashGet(pAppHbMgr->connInfo, &pOneReq->connKey, sizeof(SClientHbKey));
    if (info) {
      code = (*clientHbMgr.reqHandle[pOneReq->connKey.hbType])(&pOneReq->connKey, info->param, pOneReq);
      if (code) {
        taosHashCancelIterate(pAppHbMgr->activeInfo, pIter);
        break;
      }
    }

    taosArrayPush(pBatchReq->reqs, pOneReq);

    pIter = taosHashIterate(pAppHbMgr->activeInfo, pIter);
  }

  if (code) {
    taosArrayDestroyEx(pBatchReq->reqs, hbFreeReq);
    tfree(pBatchReq);
  }

  return pBatchReq;
}


void hbClearReqInfo(SAppHbMgr *pAppHbMgr) {
  void *pIter = taosHashIterate(pAppHbMgr->activeInfo, NULL);
  while (pIter != NULL) {
    SClientHbReq* pOneReq = pIter;

    tFreeReqKvHash(pOneReq->info);
    taosHashClear(pOneReq->info);

    pIter = taosHashIterate(pAppHbMgr->activeInfo, pIter);
  }
}



static void* hbThreadFunc(void* param) {
  setThreadName("hb");
  while (1) {
    int8_t threadStop = atomic_val_compare_exchange_8(&clientHbMgr.threadStop, 1, 2);
    if(1 == threadStop) {
      break;
    }

    pthread_mutex_lock(&clientHbMgr.lock);

    int sz = taosArrayGetSize(clientHbMgr.appHbMgrs);
    for(int i = 0; i < sz; i++) {
      SAppHbMgr* pAppHbMgr = taosArrayGetP(clientHbMgr.appHbMgrs, i);

      int32_t connCnt = atomic_load_32(&pAppHbMgr->connKeyCnt);
      if (connCnt == 0) {
        continue;
      }
      SClientHbBatchReq* pReq = hbGatherAllInfo(pAppHbMgr);
      if (pReq == NULL) {
        continue;
      }
      int tlen = tSerializeSClientHbBatchReq(NULL, pReq);
      void *buf = malloc(tlen);
      if (buf == NULL) {
        terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
        tFreeClientHbBatchReq(pReq, false);
        hbClearReqInfo(pAppHbMgr);
        break;
      }
      void *abuf = buf;
      tSerializeSClientHbBatchReq(&abuf, pReq);
      SMsgSendInfo *pInfo = malloc(sizeof(SMsgSendInfo));
      if (pInfo == NULL) {
        terrno = TSDB_CODE_TSC_OUT_OF_MEMORY;
        tFreeClientHbBatchReq(pReq, false);
        hbClearReqInfo(pAppHbMgr);
        free(buf);
        break;
      }
      pInfo->fp = hbMqAsyncCallBack;
      pInfo->msgInfo.pData = buf;
      pInfo->msgInfo.len = tlen;
      pInfo->msgType = TDMT_MND_HEARTBEAT;
      pInfo->param = strdup(pAppHbMgr->key);
      pInfo->requestId = generateRequestId();
      pInfo->requestObjRefId = 0;

      SAppInstInfo *pAppInstInfo = pAppHbMgr->pAppInstInfo;
      int64_t transporterId = 0;
      SEpSet epSet = getEpSet_s(&pAppInstInfo->mgmtEp);
      asyncSendMsgToServer(pAppInstInfo->pTransporter, &epSet, &transporterId, pInfo);
      tFreeClientHbBatchReq(pReq, false);      
      hbClearReqInfo(pAppHbMgr);

      atomic_add_fetch_32(&pAppHbMgr->reportCnt, 1);
    }

    pthread_mutex_unlock(&clientHbMgr.lock);
    
    taosMsleep(HEARTBEAT_INTERVAL);
  }
  return NULL;
}

static int32_t hbCreateThread() {
  pthread_attr_t thAttr;
  pthread_attr_init(&thAttr);
  pthread_attr_setdetachstate(&thAttr, PTHREAD_CREATE_JOINABLE);

  if (pthread_create(&clientHbMgr.thread, &thAttr, hbThreadFunc, NULL) != 0) {
    terrno = TAOS_SYSTEM_ERROR(errno);
    return -1;
  }
  pthread_attr_destroy(&thAttr);
  return 0;
}

static void hbStopThread() {
  if (atomic_val_compare_exchange_8(&clientHbMgr.threadStop, 0, 1)) {
    tscDebug("hb thread already stopped");
    return;
  }
  
  while (2 != atomic_load_8(&clientHbMgr.threadStop)) {
    usleep(10);
  }

  tscDebug("hb thread stopped");  
}

SAppHbMgr* appHbMgrInit(SAppInstInfo* pAppInstInfo, char *key) {
  hbMgrInit();
  SAppHbMgr* pAppHbMgr = malloc(sizeof(SAppHbMgr)); 
  if (pAppHbMgr == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }
  // init stat
  pAppHbMgr->startTime = taosGetTimestampMs();
  pAppHbMgr->connKeyCnt = 0;
  pAppHbMgr->reportCnt = 0;
  pAppHbMgr->reportBytes = 0;
  pAppHbMgr->key = strdup(key);

  // init app info
  pAppHbMgr->pAppInstInfo = pAppInstInfo;

  // init hash info
  pAppHbMgr->activeInfo = taosHashInit(64, hbKeyHashFunc, 1, HASH_ENTRY_LOCK);

  if (pAppHbMgr->activeInfo == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    free(pAppHbMgr);
    return NULL;
  }
  pAppHbMgr->activeInfo->freeFp = tFreeClientHbReq;
  // init getInfoFunc
  pAppHbMgr->connInfo = taosHashInit(64, hbKeyHashFunc, 1, HASH_ENTRY_LOCK);

  if (pAppHbMgr->connInfo == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    free(pAppHbMgr);
    return NULL;
  }

  pthread_mutex_lock(&clientHbMgr.lock);
  taosArrayPush(clientHbMgr.appHbMgrs, &pAppHbMgr);
  pthread_mutex_unlock(&clientHbMgr.lock);
  
  return pAppHbMgr;
}

void appHbMgrCleanup(SAppHbMgr* pAppHbMgr) {
  if (NULL == pAppHbMgr) {
    return;
  }
  
  pthread_mutex_lock(&clientHbMgr.lock);

  int sz = taosArrayGetSize(clientHbMgr.appHbMgrs);
  for (int i = 0; i < sz; i++) {
    SAppHbMgr* pTarget = taosArrayGetP(clientHbMgr.appHbMgrs, i);
    if (pAppHbMgr == pTarget) {
      taosHashCleanup(pTarget->activeInfo);
      pTarget->activeInfo = NULL;
      taosHashCleanup(pTarget->connInfo);
      pTarget->connInfo = NULL;
    }
  }

  pthread_mutex_unlock(&clientHbMgr.lock);
}

int hbMgrInit() {
  // init once
  int8_t old = atomic_val_compare_exchange_8(&clientHbMgr.inited, 0, 1);
  if (old == 1) return 0;

  clientHbMgr.appHbMgrs = taosArrayInit(0, sizeof(void*));
  pthread_mutex_init(&clientHbMgr.lock, NULL);

  // init handle funcs
  hbMgrInitHandle();

  // init backgroud thread
  hbCreateThread();

  return 0;
}

void hbMgrCleanUp() {
  hbStopThread();
  
  // destroy all appHbMgr
  int8_t old = atomic_val_compare_exchange_8(&clientHbMgr.inited, 1, 0);
  if (old == 0) return;

  pthread_mutex_lock(&clientHbMgr.lock);
  taosArrayDestroy(clientHbMgr.appHbMgrs);  
  pthread_mutex_unlock(&clientHbMgr.lock);
  
  clientHbMgr.appHbMgrs = NULL;
}

int hbRegisterConnImpl(SAppHbMgr* pAppHbMgr, SClientHbKey connKey, SHbConnInfo *info) {
  // init hash in activeinfo
  void* data = taosHashGet(pAppHbMgr->activeInfo, &connKey, sizeof(SClientHbKey));
  if (data != NULL) {
    return 0;
  }
  SClientHbReq hbReq;
  hbReq.connKey = connKey;
  hbReq.info = taosHashInit(64, hbKeyHashFunc, 1, HASH_ENTRY_LOCK);
  
  taosHashPut(pAppHbMgr->activeInfo, &connKey, sizeof(SClientHbKey), &hbReq, sizeof(SClientHbReq));
 
  // init hash
  if (info != NULL) {
    SClientHbReq * pReq = taosHashGet(pAppHbMgr->activeInfo, &connKey, sizeof(SClientHbKey));
    info->req = pReq;
    taosHashPut(pAppHbMgr->connInfo, &connKey, sizeof(SClientHbKey), info, sizeof(SHbConnInfo));
  }

  atomic_add_fetch_32(&pAppHbMgr->connKeyCnt, 1);
  return 0;
}

int hbRegisterConn(SAppHbMgr* pAppHbMgr, int32_t connId, int64_t clusterId, int32_t hbType) {
  SClientHbKey connKey = {.connId = connId, .hbType = HEARTBEAT_TYPE_QUERY};
  SHbConnInfo info = {0};

  switch (hbType) {
    case HEARTBEAT_TYPE_QUERY: {
      int64_t *pClusterId = malloc(sizeof(int64_t));
      *pClusterId = clusterId;

      info.param = pClusterId;
      break;
    }
    case HEARTBEAT_TYPE_MQ: {
      break;
    }
    default:
      break;
  }
  
  return hbRegisterConnImpl(pAppHbMgr, connKey, &info);
}

void hbDeregisterConn(SAppHbMgr* pAppHbMgr, SClientHbKey connKey) {
  int32_t code = 0;
  code = taosHashRemove(pAppHbMgr->activeInfo, &connKey, sizeof(SClientHbKey));
  code = taosHashRemove(pAppHbMgr->connInfo, &connKey, sizeof(SClientHbKey));
  if (code) {
    return;
  }
  atomic_sub_fetch_32(&pAppHbMgr->connKeyCnt, 1);
  if (atomic_load_32(&pAppHbMgr->connKeyCnt) <= 0) {
    appHbMgrCleanup(pAppHbMgr);
  }
}

int hbAddConnInfo(SAppHbMgr *pAppHbMgr, SClientHbKey connKey, void* key, void* value, int32_t keyLen, int32_t valueLen) {
  // find req by connection id
  SClientHbReq* pReq = taosHashGet(pAppHbMgr->activeInfo, &connKey, sizeof(SClientHbKey));
  ASSERT(pReq != NULL);

  taosHashPut(pReq->info, key, keyLen, value, valueLen);

  return 0;
}

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

#ifndef TDENGINE_CLIENTINT_H
#define TDENGINE_CLIENTINT_H

#ifdef __cplusplus
extern "C" {
#endif

#include "taos.h"
#include "common.h"
#include "tmsg.h"
#include "tdef.h"
#include "tep.h"
#include "thash.h"
#include "tlist.h"
#include "tmsgtype.h"
#include "trpc.h"
#include "query.h"

#define HEARTBEAT_INTERVAL 1500  // ms

typedef struct SAppInstInfo SAppInstInfo;

typedef int32_t (*FHbRspHandle)(SClientHbRsp* pReq);

typedef struct SAppHbMgr {
  // statistics
  int32_t reportCnt;
  int32_t connKeyCnt;
  int64_t reportBytes;  // not implemented
  int64_t startTime;
  // ctl
  SRWLatch lock;  // lock is used in serialization
  // connection
  SAppInstInfo* pAppInstInfo;
  // info
  SHashObj* activeInfo;    // hash<SClientHbKey, SClientHbReq>
  SHashObj* getInfoFuncs;  // hash<SClientHbKey, FGetConnInfo>
} SAppHbMgr;

typedef struct SClientHbMgr {
  int8_t inited;
  // ctl
  int8_t          threadStop;
  pthread_t       thread;
  pthread_mutex_t lock;       // used when app init and cleanup
  SArray*         appHbMgrs;  // SArray<SAppHbMgr*> one for each cluster
  FHbRspHandle    handle[HEARTBEAT_TYPE_MAX];
} SClientHbMgr;

// TODO: embed param into function
// return type: SArray<Skv>
typedef SArray* (*FGetConnInfo)(SClientHbKey connKey, void* param);

typedef struct SQueryExecMetric {
  int64_t      start;    // start timestamp
  int64_t      parsed;   // start to parse
  int64_t      send;     // start to send to server
  int64_t      rsp;      // receive response from server
} SQueryExecMetric;

typedef struct SInstanceSummary {
  uint64_t     numOfInsertsReq;
  uint64_t     numOfInsertRows;
  uint64_t     insertElapsedTime;
  uint64_t     insertBytes;         // submit to tsdb since launched.

  uint64_t     fetchBytes;
  uint64_t     queryElapsedTime;
  uint64_t     numOfSlowQueries;
  uint64_t     totalRequests;
  uint64_t     currentRequests;      // the number of SRequestObj
} SInstanceSummary;

typedef struct SHeartBeatInfo {
  void  *pTimer;   // timer, used to send request msg to mnode
} SHeartBeatInfo;

struct SAppInstInfo {
  int64_t          numOfConns;
  SCorEpSet        mgmtEp;
  SInstanceSummary summary;
  SList            *pConnList;  // STscObj linked list
  int64_t          clusterId;
  void             *pTransporter;
  struct SAppHbMgr *pAppHbMgr;
};

typedef struct SAppInfo {
  int64_t         startTime;
  char            appName[TSDB_APP_NAME_LEN];
  char           *ep;
  int32_t         pid;
  int32_t         numOfThreads;
  SHashObj       *pInstMap;
  pthread_mutex_t mutex;
} SAppInfo;

typedef struct STscObj {
  char             user[TSDB_USER_LEN];
  char             pass[TSDB_PASSWORD_LEN];
  char             db[TSDB_DB_FNAME_LEN];
  char             ver[128];
  int32_t          acctId;
  uint32_t         connId;
  int32_t          connType;
  uint64_t         id;       // ref ID returned by taosAddRef
  pthread_mutex_t  mutex;     // used to protect the operation on db
  int32_t          numOfReqs; // number of sqlObj bound to this connection
  SAppInstInfo    *pAppInfo;
} STscObj;

typedef struct SMqConsumer {
  STscObj* pTscObj;
} SMqConsumer;

typedef struct SReqResultInfo {
  const char  *pRspMsg;
  const char  *pData;
  TAOS_FIELD  *fields;
  uint32_t     numOfCols;
  int32_t     *length;
  TAOS_ROW     row;
  char       **pCol;
  uint32_t     numOfRows;
  uint64_t     totalRows;
  uint32_t     current;
  bool         completed;
} SReqResultInfo;

typedef struct SShowReqInfo {
  int64_t         execId;        // showId/queryId
  int32_t         vgId;
  SArray         *pArray;        // SArray<SVgroupInfo>
  int32_t         currentIndex;  // current accessed vgroup index.
} SShowReqInfo;

typedef struct SRequestSendRecvBody {
  tsem_t            rspSem;        // not used now
  void*             fp;
  SShowReqInfo      showInfo;      // todo this attribute will be removed after the query framework being completed.
  SDataBuf          requestMsg;
  struct SSchJob   *pQueryJob;     // query job, created according to sql query DAG.
  struct SQueryDag *pDag;          // the query dag, generated according to the sql statement.
  SReqResultInfo    resInfo;
} SRequestSendRecvBody;

#define ERROR_MSG_BUF_DEFAULT_SIZE  512

typedef struct SRequestObj {
  uint64_t         requestId;
  int32_t          type;   // request type
  STscObj         *pTscObj;
  char            *sqlstr;  // sql string
  int32_t          sqlLen;
  int64_t          self;
  char            *msgBuf;
  void            *pInfo;   // sql parse info, generated by parser module
  int32_t          code;
  SQueryExecMetric metric;
  SRequestSendRecvBody body;
} SRequestObj;

extern SAppInfo appInfo;
extern int32_t  clientReqRefPool;
extern int32_t  clientConnRefPool;

extern int (*handleRequestRspFp[TDMT_MAX])(void*, const SDataBuf* pMsg, int32_t code);
int genericRspCallback(void* param, const SDataBuf* pMsg, int32_t code);
SMsgSendInfo* buildMsgInfoImpl(SRequestObj* pReqObj);

int   taos_init();

void* createTscObj(const char* user, const char* auth, const char *db, SAppInstInfo* pAppInfo);
void  destroyTscObj(void*pObj);

uint64_t generateRequestId();

void *createRequest(STscObj* pObj, __taos_async_fn_t fp, void* param, int32_t type);
void  destroyRequest(SRequestObj* pRequest);

char *getDbOfConnection(STscObj* pObj);
void  setConnectionDB(STscObj* pTscObj, const char* db);

void taos_init_imp(void);
int  taos_options_imp(TSDB_OPTION option, const char *str);

void* openTransporter(const char *user, const char *auth, int32_t numOfThreads);

void processMsgFromServer(void* parent, SRpcMsg* pMsg, SEpSet* pEpSet);

void initMsgHandleFp();

TAOS *taos_connect_internal(const char *ip, const char *user, const char *pass, const char *auth, const char *db, uint16_t port);

void *doFetchRow(SRequestObj* pRequest);

void  setResultDataPtr(SReqResultInfo* pResultInfo, TAOS_FIELD* pFields, int32_t numOfCols, int32_t numOfRows);

// --- heartbeat 
// global, called by mgmt
int  hbMgrInit();
void hbMgrCleanUp();
int  hbHandleRsp(SClientHbBatchRsp* hbRsp);

// cluster level
SAppHbMgr* appHbMgrInit(SAppInstInfo* pAppInstInfo);
void appHbMgrCleanup(SAppHbMgr* pAppHbMgr);

// conn level
int  hbRegisterConn(SAppHbMgr* pAppHbMgr, SClientHbKey connKey, FGetConnInfo func);
void hbDeregisterConn(SAppHbMgr* pAppHbMgr, SClientHbKey connKey);

int hbAddConnInfo(SAppHbMgr* pAppHbMgr, SClientHbKey connKey, void* key, void* value, int32_t keyLen, int32_t valueLen);

// --- mq
void hbMgrInitMqHbRspHandle();


#ifdef __cplusplus
}
#endif

#endif  // TDENGINE_CLIENTINT_H

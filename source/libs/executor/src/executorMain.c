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

#include <tsdb.h>
#include "dataSinkMgt.h"
#include "exception.h"
#include "os.h"
#include "tarray.h"
#include "tcache.h"
#include "tglobal.h"
#include "tmsg.h"

#include "thash.h"
#include "executorimpl.h"
#include "executor.h"
#include "tlosertree.h"
#include "ttypes.h"
#include "query.h"

typedef struct STaskMgmt {
  pthread_mutex_t lock;
  SCacheObj      *qinfoPool;      // query handle pool
  int32_t         vgId;
  bool            closed;
} STaskMgmt;

static void taskMgmtKillTaskFn(void* handle, void* param1) {
  void** fp = (void**)handle;
  qKillTask(*fp);
}

static void freeqinfoFn(void *qhandle) {
  void** handle = qhandle;
  if (handle == NULL || *handle == NULL) {
    return;
  }

  qKillTask(*handle);
  qDestroyTask(*handle);
}

void freeParam(STaskParam *param) {
  tfree(param->sql);
  tfree(param->tagCond);
  tfree(param->tbnameCond);
  tfree(param->pTableIdList);
  taosArrayDestroy(param->pOperator);
  tfree(param->pExprs);
  tfree(param->pSecExprs);

  tfree(param->pExpr);
  tfree(param->pSecExpr);

  tfree(param->pGroupColIndex);
  tfree(param->pTagColumnInfo);
  tfree(param->pGroupbyExpr);
  tfree(param->prevResult);
}

int32_t qCreateExecTask(void* readHandle, int32_t vgId, uint64_t taskId, SSubplan* pSubplan, qTaskInfo_t* pTaskInfo, DataSinkHandle* handle) {
  assert(readHandle != NULL && pSubplan != NULL);
  SExecTaskInfo** pTask = (SExecTaskInfo**)pTaskInfo;

  int32_t code = createExecTaskInfoImpl(pSubplan, pTask, readHandle, taskId);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }

  SDataSinkMgtCfg cfg = {.maxDataBlockNum = 1000, .maxDataBlockNumPerQuery = 100};
  code = dsDataSinkMgtInit(&cfg);
  if (code != TSDB_CODE_SUCCESS) {
    goto _error;
  }
  if (handle) {
    code = dsCreateDataSinker(pSubplan->pDataSink, handle);
  }

  _error:
  // if failed to add ref for all tables in this query, abort current query
  return code;
}

#ifdef TEST_IMPL
// wait moment
int waitMoment(SQInfo* pQInfo){
  if(pQInfo->sql) {
    int ms = 0;
    char* pcnt = strstr(pQInfo->sql, " count(*)");
    if(pcnt) return 0;
    
    char* pos = strstr(pQInfo->sql, " t_");
    if(pos){
      pos += 3;
      ms = atoi(pos);
      while(*pos >= '0' && *pos <= '9'){
        pos ++;
      }
      char unit_char = *pos;
      if(unit_char == 'h'){
        ms *= 3600*1000;
      } else if(unit_char == 'm'){
        ms *= 60*1000;
      } else if(unit_char == 's'){
        ms *= 1000;
      }
    }
    if(ms == 0) return 0;
    printf("test wait sleep %dms. sql=%s ...\n", ms, pQInfo->sql);
    
    if(ms < 1000) {
      taosMsleep(ms);
    } else {
      int used_ms = 0;
      while(used_ms < ms) {
        taosMsleep(1000);
        used_ms += 1000;
        if(isTaskKilled(pQInfo)){
          printf("test check query is canceled, sleep break.%s\n", pQInfo->sql);
          break;
        }
      }
    }
  }
  return 1;
}
#endif

int32_t qExecTask(qTaskInfo_t tinfo, SSDataBlock** pRes, uint64_t *useconds) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*)tinfo;
  int64_t        threadId = taosGetSelfPthreadId();

  *pRes = NULL;
  int64_t curOwner = 0;
  if ((curOwner = atomic_val_compare_exchange_64(&pTaskInfo->owner, 0, threadId)) != 0) {
    qError("%s-%p execTask is now executed by thread:%p", GET_TASKID(pTaskInfo), pTaskInfo,
           (void*)curOwner);
    pTaskInfo->code = TSDB_CODE_QRY_IN_EXEC;
    return pTaskInfo->code;
  }

  if (pTaskInfo->cost.start == 0) {
    pTaskInfo->cost.start = taosGetTimestampMs();
  }

  if (isTaskKilled(pTaskInfo)) {
    qDebug("%s already killed, abort", GET_TASKID(pTaskInfo));
    return TSDB_CODE_SUCCESS;
  }

  // error occurs, record the error code and return to client
  int32_t ret = setjmp(pTaskInfo->env);
  if (ret != TSDB_CODE_SUCCESS) {
    publishQueryAbortEvent(pTaskInfo, ret);
    pTaskInfo->code = ret;
    qDebug("%s task abort due to error/cancel occurs, code:%s", GET_TASKID(pTaskInfo),
           tstrerror(pTaskInfo->code));
    return pTaskInfo->code;
  }

  qDebug("%s execTask is launched", GET_TASKID(pTaskInfo));

  bool newgroup = false;
  publishOperatorProfEvent(pTaskInfo->pRoot, QUERY_PROF_BEFORE_OPERATOR_EXEC);
  int64_t st = 0;

  st = taosGetTimestampUs();
  *pRes = pTaskInfo->pRoot->exec(pTaskInfo->pRoot, &newgroup);

  uint64_t el = (taosGetTimestampUs() - st);
  pTaskInfo->cost.elapsedTime += el;

  publishOperatorProfEvent(pTaskInfo->pRoot, QUERY_PROF_AFTER_OPERATOR_EXEC);

  if (NULL == *pRes) {
    *useconds = pTaskInfo->cost.elapsedTime;
  }

  int32_t current = (*pRes != NULL)? (*pRes)->info.rows:0;
  pTaskInfo->totalRows += current;

  qDebug("%s task suspended, %d rows returned, total:%" PRId64 " rows, in sinkNode:%d, elapsed:%.2f ms",
         GET_TASKID(pTaskInfo), current, pTaskInfo->totalRows, 0, el/1000.0);

  atomic_store_64(&pTaskInfo->owner, 0);
  return pTaskInfo->code;
}

void* qGetResultRetrieveMsg(qTaskInfo_t qinfo) {
  SQInfo* pQInfo = (SQInfo*) qinfo;
  assert(pQInfo != NULL);

  return pQInfo->rspContext;
}

int32_t qKillTask(qTaskInfo_t qinfo) {
  SExecTaskInfo *pTaskInfo = (SExecTaskInfo *)qinfo;

  if (pTaskInfo == NULL) {
    return TSDB_CODE_QRY_INVALID_QHANDLE;
  }

  qDebug("%s execTask killed", GET_TASKID(pTaskInfo));
  setTaskKilled(pTaskInfo);

  // Wait for the query executing thread being stopped/
  // Once the query is stopped, the owner of qHandle will be cleared immediately.
  while (pTaskInfo->owner != 0) {
    taosMsleep(100);
  }

  return TSDB_CODE_SUCCESS;
}

int32_t qAsyncKillTask(qTaskInfo_t qinfo) {
  SExecTaskInfo *pTaskInfo = (SExecTaskInfo *)qinfo;

  if (pTaskInfo == NULL) {
    return TSDB_CODE_QRY_INVALID_QHANDLE;
  }

  qDebug("%s execTask async killed", GET_TASKID(pTaskInfo));
  setTaskKilled(pTaskInfo);

  return TSDB_CODE_SUCCESS;
}

int32_t qIsTaskCompleted(qTaskInfo_t qinfo) {
  SExecTaskInfo *pTaskInfo = (SExecTaskInfo *)qinfo;

  if (pTaskInfo == NULL /*|| !isValidQInfo(pTaskInfo)*/) {
    return TSDB_CODE_QRY_INVALID_QHANDLE;
  }

  return isTaskKilled(pTaskInfo) || Q_STATUS_EQUAL(pTaskInfo->status, TASK_OVER);
}

void qDestroyTask(qTaskInfo_t qTaskHandle) {
  SExecTaskInfo* pTaskInfo = (SExecTaskInfo*) qTaskHandle;
  qDebug("%s execTask completed, numOfRows:%"PRId64, GET_TASKID(pTaskInfo), pTaskInfo->totalRows);

  queryCostStatis(pTaskInfo);   // print the query cost summary
  doDestroyTask(pTaskInfo);
}

#if 0
//kill by qid
int32_t qKillQueryByQId(void* pMgmt, int64_t qId, int32_t waitMs, int32_t waitCount) {
  int32_t error = TSDB_CODE_SUCCESS;
  void** handle = qAcquireTask(pMgmt, qId);
  if(handle == NULL) return terrno;

  SQInfo* pQInfo = (SQInfo*)(*handle);
  if (pQInfo == NULL || !isValidQInfo(pQInfo)) {
    return TSDB_CODE_QRY_INVALID_QHANDLE;
  }
  qWarn("%s be killed(no memory commit).", pQInfo->qId);
  setTaskKilled(pQInfo);

  // wait query stop
  int32_t loop = 0;
  while (pQInfo->owner != 0) {
    taosMsleep(waitMs);
    if(loop++ > waitCount){
      error = TSDB_CODE_FAILED;
      break;
    }
  }

  qReleaseTask(pMgmt, (void **)&handle, true);
  return error;
}

#endif

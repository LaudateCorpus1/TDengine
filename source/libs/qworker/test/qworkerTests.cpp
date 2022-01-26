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

#include <gtest/gtest.h>
#include <tglobal.h>
#include <iostream>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wwrite-strings"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wformat"
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#pragma GCC diagnostic ignored "-Wpointer-arith"

#include "os.h"

#include "taos.h"
#include "tdef.h"
#include "tvariant.h"
#include "tep.h"
#include "trpc.h"
#include "planner.h"
#include "qworker.h"
#include "stub.h"
#include "addr_any.h"
#include "executor.h"
#include "dataSinkMgt.h"


namespace {

#define qwtTestQueryQueueSize 1000000
#define qwtTestFetchQueueSize 1000000

int32_t qwtTestMaxExecTaskUsec = 2;
int32_t qwtTestReqMaxDelayUsec = 2;

uint64_t qwtTestQueryId = 0;
bool qwtTestEnableSleep = true;
bool qwtTestStop = false;
bool qwtTestDeadLoop = false;
int32_t qwtTestMTRunSec = 60;
int32_t qwtTestPrintNum = 100000;
int32_t qwtTestCaseIdx = 0;
int32_t qwtTestCaseNum = 4;
bool qwtTestCaseFinished = false;
tsem_t qwtTestQuerySem;
tsem_t qwtTestFetchSem;
int32_t qwtTestQuitThreadNum = 0;


int32_t qwtTestQueryQueueRIdx = 0;
int32_t qwtTestQueryQueueWIdx = 0;
int32_t qwtTestQueryQueueNum = 0;
SRWLatch qwtTestQueryQueueLock = 0;
struct SRpcMsg *qwtTestQueryQueue[qwtTestQueryQueueSize] = {0};

int32_t qwtTestFetchQueueRIdx = 0;
int32_t qwtTestFetchQueueWIdx = 0;
int32_t qwtTestFetchQueueNum = 0;
SRWLatch qwtTestFetchQueueLock = 0;
struct SRpcMsg *qwtTestFetchQueue[qwtTestFetchQueueSize] = {0};


int32_t qwtTestSinkBlockNum = 0;
int32_t qwtTestSinkMaxBlockNum = 0;
bool qwtTestSinkQueryEnd = false;
SRWLatch qwtTestSinkLock = 0;
int32_t qwtTestSinkLastLen = 0;


SSubQueryMsg qwtqueryMsg = {0};
SRpcMsg qwtfetchRpc = {0};
SResFetchReq qwtfetchMsg = {0};
SRpcMsg qwtreadyRpc = {0};
SResReadyReq qwtreadyMsg = {0};
SRpcMsg qwtdropRpc = {0};
STaskDropReq qwtdropMsg = {0};  
SSchTasksStatusReq qwtstatusMsg = {0};


void qwtInitLogFile() {
  const char    *defaultLogFileNamePrefix = "taosdlog";
  const int32_t  maxLogFileNum = 10;

  tsAsyncLog = 0;
  qDebugFlag = 159;

  char temp[128] = {0};
  sprintf(temp, "%s/%s", tsLogDir, defaultLogFileNamePrefix);
  if (taosInitLog(temp, tsNumOfLogLines, maxLogFileNum) < 0) {
    printf("failed to open log file in directory:%s\n", tsLogDir);
  }

}

void qwtBuildQueryReqMsg(SRpcMsg *queryRpc) {
  qwtqueryMsg.queryId = htobe64(atomic_add_fetch_64(&qwtTestQueryId, 1));
  qwtqueryMsg.sId = htobe64(1);
  qwtqueryMsg.taskId = htobe64(1);
  qwtqueryMsg.contentLen = htonl(100);
  queryRpc->msgType = TDMT_VND_QUERY;
  queryRpc->pCont = &qwtqueryMsg;
  queryRpc->contLen = sizeof(SSubQueryMsg) + 100;
}

void qwtBuildReadyReqMsg(SResReadyReq *readyMsg, SRpcMsg *readyRpc) {
  readyMsg->sId = htobe64(1);
  readyMsg->queryId = htobe64(atomic_load_64(&qwtTestQueryId));
  readyMsg->taskId = htobe64(1);
  readyRpc->msgType = TDMT_VND_RES_READY;
  readyRpc->pCont = readyMsg;
  readyRpc->contLen = sizeof(SResReadyReq);
}

void qwtBuildFetchReqMsg(SResFetchReq *fetchMsg, SRpcMsg *fetchRpc) {
  fetchMsg->sId = htobe64(1);
  fetchMsg->queryId = htobe64(atomic_load_64(&qwtTestQueryId));
  fetchMsg->taskId = htobe64(1);
  fetchRpc->msgType = TDMT_VND_FETCH;
  fetchRpc->pCont = fetchMsg;
  fetchRpc->contLen = sizeof(SResFetchReq);
}

void qwtBuildDropReqMsg(STaskDropReq *dropMsg, SRpcMsg *dropRpc) {
  dropMsg->sId = htobe64(1);
  dropMsg->queryId = htobe64(atomic_load_64(&qwtTestQueryId));
  dropMsg->taskId = htobe64(1);
  dropRpc->msgType = TDMT_VND_DROP_TASK;
  dropRpc->pCont = dropMsg;
  dropRpc->contLen = sizeof(STaskDropReq);
}

void qwtBuildStatusReqMsg(SSchTasksStatusReq *statusMsg, SRpcMsg *statusRpc) {
  statusMsg->sId = htobe64(1);
  statusRpc->pCont = statusMsg;
  statusRpc->contLen = sizeof(SSchTasksStatusReq);
  statusRpc->msgType = TDMT_VND_TASKS_STATUS;
}

int32_t qwtStringToPlan(const char* str, SSubplan** subplan) {
  *subplan = (SSubplan *)0x1;
  return 0;
}

int32_t qwtPutReqToFetchQueue(void *node, struct SRpcMsg *pMsg) {
  taosWLockLatch(&qwtTestFetchQueueLock);
  struct SRpcMsg *newMsg = (struct SRpcMsg *)calloc(1, sizeof(struct SRpcMsg));
  memcpy(newMsg, pMsg, sizeof(struct SRpcMsg));  
  qwtTestFetchQueue[qwtTestFetchQueueWIdx++] = newMsg;
  if (qwtTestFetchQueueWIdx >= qwtTestFetchQueueSize) {
    qwtTestFetchQueueWIdx = 0;
  }
  
  qwtTestFetchQueueNum++;

  if (qwtTestFetchQueueWIdx == qwtTestFetchQueueRIdx) {
    printf("Fetch queue is full");
    assert(0);
  }
  taosWUnLockLatch(&qwtTestFetchQueueLock);
  
  tsem_post(&qwtTestFetchSem);
  
  return 0;
}


int32_t qwtPutReqToQueue(void *node, struct SRpcMsg *pMsg) {
  taosWLockLatch(&qwtTestQueryQueueLock);
  struct SRpcMsg *newMsg = (struct SRpcMsg *)calloc(1, sizeof(struct SRpcMsg));
  memcpy(newMsg, pMsg, sizeof(struct SRpcMsg));
  qwtTestQueryQueue[qwtTestQueryQueueWIdx++] = newMsg;
  if (qwtTestQueryQueueWIdx >= qwtTestQueryQueueSize) {
    qwtTestQueryQueueWIdx = 0;
  }
  
  qwtTestQueryQueueNum++;

  if (qwtTestQueryQueueWIdx == qwtTestQueryQueueRIdx) {
    printf("query queue is full");
    assert(0);
  }
  taosWUnLockLatch(&qwtTestQueryQueueLock);
  
  tsem_post(&qwtTestQuerySem);
  
  return 0;
}



void qwtRpcSendResponse(const SRpcMsg *pRsp) {

  switch (pRsp->msgType) {
    case TDMT_VND_QUERY_RSP: {
      SQueryTableRsp *rsp = (SQueryTableRsp *)pRsp->pCont;

      if (0 == pRsp->code) {
        qwtBuildReadyReqMsg(&qwtreadyMsg, &qwtreadyRpc);    
        qwtPutReqToFetchQueue((void *)0x1, &qwtreadyRpc);
      } else {
        qwtBuildDropReqMsg(&qwtdropMsg, &qwtdropRpc);
        qwtPutReqToFetchQueue((void *)0x1, &qwtdropRpc);
      }
      
      rpcFreeCont(rsp);
      break;
    }
    case TDMT_VND_RES_READY_RSP: {
      SResReadyRsp *rsp = (SResReadyRsp *)pRsp->pCont;
      
      if (0 == pRsp->code) {
        qwtBuildFetchReqMsg(&qwtfetchMsg, &qwtfetchRpc);
        qwtPutReqToFetchQueue((void *)0x1, &qwtfetchRpc);
      } else {
        qwtBuildDropReqMsg(&qwtdropMsg, &qwtdropRpc);
        qwtPutReqToFetchQueue((void *)0x1, &qwtdropRpc);
      }
      rpcFreeCont(rsp);
      break;
    }
    case TDMT_VND_FETCH_RSP: {
      SRetrieveTableRsp *rsp = (SRetrieveTableRsp *)pRsp->pCont;
  
      if (0 == pRsp->code && 0 == rsp->completed) {
        qwtBuildFetchReqMsg(&qwtfetchMsg, &qwtfetchRpc);
        qwtPutReqToFetchQueue((void *)0x1, &qwtfetchRpc);
        return;
      }

      qwtBuildDropReqMsg(&qwtdropMsg, &qwtdropRpc);
      qwtPutReqToFetchQueue((void *)0x1, &qwtdropRpc);
      rpcFreeCont(rsp);
      
      break;
    }
    case TDMT_VND_DROP_TASK_RSP: {
      STaskDropRsp *rsp = (STaskDropRsp *)pRsp->pCont;
      rpcFreeCont(rsp);

      qwtTestCaseFinished = true;
      break;
    }
  }

  
  return;
}

int32_t qwtCreateExecTask(void* tsdb, int32_t vgId, struct SSubplan* pPlan, qTaskInfo_t* pTaskInfo, DataSinkHandle* handle) {
  int32_t idx = abs((++qwtTestCaseIdx) % qwtTestCaseNum);

  qwtTestSinkBlockNum = 0;
  qwtTestSinkMaxBlockNum = rand() % 100 + 1;
  qwtTestSinkQueryEnd = false;
  
  if (0 == idx) {
    *pTaskInfo = (qTaskInfo_t)qwtTestCaseIdx;
    *handle = (DataSinkHandle)qwtTestCaseIdx+1;
  } else if (1 == idx) {
    *pTaskInfo = NULL;
    *handle = NULL;
  } else if (2 == idx) {
    *pTaskInfo = (qTaskInfo_t)qwtTestCaseIdx;
    *handle = NULL;
  } else if (3 == idx) {
    *pTaskInfo = NULL;
    *handle = (DataSinkHandle)qwtTestCaseIdx;
  }
  
  return 0;
}

int32_t qwtExecTask(qTaskInfo_t tinfo, SSDataBlock** pRes, uint64_t *useconds) {
  int32_t endExec = 0;
  
  if (NULL == tinfo) {
    *pRes = NULL;
    *useconds = 0;
  } else {
    if (qwtTestSinkQueryEnd) {
      *pRes = NULL;
      *useconds = rand() % 10;
      return 0;
    }
    
    endExec = rand() % 5;
    
    int32_t runTime = 0;
    if (qwtTestEnableSleep && qwtTestMaxExecTaskUsec > 0) {
      runTime = rand() % qwtTestMaxExecTaskUsec;
    }

    if (qwtTestEnableSleep) {
      if (runTime) {
        usleep(runTime);
      }
    }
      
    if (endExec) {
      *pRes = (SSDataBlock*)calloc(1, sizeof(SSDataBlock));
      (*pRes)->info.rows = rand() % 1000;
    } else {
      *pRes = NULL;
      *useconds = rand() % 10;
    }
  }
  
  return 0;
}

int32_t qwtKillTask(qTaskInfo_t qinfo) {
  return 0;
}

void qwtDestroyTask(qTaskInfo_t qHandle) {
}


int32_t qwtPutDataBlock(DataSinkHandle handle, const SInputData* pInput, bool* pContinue) {
  if (NULL == handle || NULL == pInput || NULL == pContinue) {
    assert(0);
  }

  free((void *)pInput->pData);

  taosWLockLatch(&qwtTestSinkLock);

  qwtTestSinkBlockNum++;

  if (qwtTestSinkBlockNum >= qwtTestSinkMaxBlockNum) {
    *pContinue = false;
  } else {
    *pContinue = true;
  }
  taosWUnLockLatch(&qwtTestSinkLock);
  
  return 0;
}

void qwtEndPut(DataSinkHandle handle, uint64_t useconds) {
  if (NULL == handle) {
    assert(0);
  }

  qwtTestSinkQueryEnd = true;
}

void qwtGetDataLength(DataSinkHandle handle, int32_t* pLen, bool* pQueryEnd) {
  static int32_t in = 0;

  if (in > 0) {
    assert(0);
  }

  atomic_add_fetch_32(&in, 1);
  
  if (NULL == handle) {
    assert(0);
  }

  taosWLockLatch(&qwtTestSinkLock);
  if (qwtTestSinkBlockNum > 0) {
    *pLen = rand() % 100 + 1;
    qwtTestSinkBlockNum--;
  } else {
    *pLen = 0;
  }
  qwtTestSinkLastLen = *pLen;
  taosWUnLockLatch(&qwtTestSinkLock);

  *pQueryEnd = qwtTestSinkQueryEnd;

  atomic_sub_fetch_32(&in, 1);
}

int32_t qwtGetDataBlock(DataSinkHandle handle, SOutputData* pOutput) {
  taosWLockLatch(&qwtTestSinkLock);
  if (qwtTestSinkLastLen > 0) {
    pOutput->numOfRows = rand() % 10 + 1;
    pOutput->compressed = 1;
    pOutput->queryEnd = qwtTestSinkQueryEnd;
    if (qwtTestSinkBlockNum == 0) {
      pOutput->bufStatus = DS_BUF_EMPTY;
    } else if (qwtTestSinkBlockNum <= qwtTestSinkMaxBlockNum*0.5) {
      pOutput->bufStatus = DS_BUF_LOW;
    } else {
      pOutput->bufStatus = DS_BUF_FULL;
    }
    pOutput->useconds = rand() % 10 + 1;
    pOutput->precision = 1;
  } else if (qwtTestSinkLastLen == 0) {
    pOutput->numOfRows = 0;
    pOutput->compressed = 1;
    pOutput->pData = NULL;
    pOutput->queryEnd = qwtTestSinkQueryEnd;
    if (qwtTestSinkBlockNum == 0) {
      pOutput->bufStatus = DS_BUF_EMPTY;
    } else if (qwtTestSinkBlockNum <= qwtTestSinkMaxBlockNum*0.5) {
      pOutput->bufStatus = DS_BUF_LOW;
    } else {
      pOutput->bufStatus = DS_BUF_FULL;
    }
    pOutput->useconds = rand() % 10 + 1;
    pOutput->precision = 1;
  } else {
    assert(0);
  }
  taosWUnLockLatch(&qwtTestSinkLock);
  
  return 0;
}

void qwtDestroyDataSinker(DataSinkHandle handle) {

}



void stubSetStringToPlan() {
  static Stub stub;
  stub.set(qStringToSubplan, qwtStringToPlan);
  {
    AddrAny any("libplanner.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^qStringToSubplan$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtStringToPlan);
    }
  }
}

void stubSetExecTask() {
  static Stub stub;
  stub.set(qExecTask, qwtExecTask);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^qExecTask$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtExecTask);
    }
  }
}



void stubSetCreateExecTask() {
  static Stub stub;
  stub.set(qCreateExecTask, qwtCreateExecTask);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^qCreateExecTask$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtCreateExecTask);
    }
  }
}

void stubSetAsyncKillTask() {
  static Stub stub;
  stub.set(qAsyncKillTask, qwtKillTask);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^qAsyncKillTask$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtKillTask);
    }
  }
}

void stubSetDestroyTask() {
  static Stub stub;
  stub.set(qDestroyTask, qwtDestroyTask);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^qDestroyTask$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtDestroyTask);
    }
  }
}


void stubSetDestroyDataSinker() {
  static Stub stub;
  stub.set(dsDestroyDataSinker, qwtDestroyDataSinker);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^dsDestroyDataSinker$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtDestroyDataSinker);
    }
  }
}

void stubSetGetDataLength() {
  static Stub stub;
  stub.set(dsGetDataLength, qwtGetDataLength);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^dsGetDataLength$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtGetDataLength);
    }
  }
}

void stubSetEndPut() {
  static Stub stub;
  stub.set(dsEndPut, qwtEndPut);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^dsEndPut$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtEndPut);
    }
  }
}

void stubSetPutDataBlock() {
  static Stub stub;
  stub.set(dsPutDataBlock, qwtPutDataBlock);
  {
    AddrAny any("libexecutor.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^dsPutDataBlock$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtPutDataBlock);
    }
  }
}

void stubSetRpcSendResponse() {
  static Stub stub;
  stub.set(rpcSendResponse, qwtRpcSendResponse);
  {
    AddrAny any("libtransport.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^rpcSendResponse$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtRpcSendResponse);
    }
  }
}

void stubSetGetDataBlock() {
  static Stub stub;
  stub.set(dsGetDataBlock, qwtGetDataBlock);
  {
    AddrAny any("libtransport.so");
    std::map<std::string,void*> result;
    any.get_global_func_addr_dynsym("^dsGetDataBlock$", result);
    for (const auto& f : result) {
      stub.set(f.second, qwtGetDataBlock);
    }
  }
}


void *queryThread(void *param) {
  SRpcMsg queryRpc = {0};
  int32_t code = 0;
  uint32_t n = 0;
  void *mockPointer = (void *)0x1;    
  void *mgmt = param;

  while (!qwtTestStop) {
    qwtBuildQueryReqMsg(&queryRpc);
    qWorkerProcessQueryMsg(mockPointer, mgmt, &queryRpc);    
    if (qwtTestEnableSleep) {
      usleep(rand()%5);
    }
    if (++n % qwtTestPrintNum == 0) {
      printf("query:%d\n", n);
    }
  }

  return NULL;
}

void *readyThread(void *param) {
  SRpcMsg readyRpc = {0};
  int32_t code = 0;
  uint32_t n = 0;  
  void *mockPointer = (void *)0x1;    
  void *mgmt = param;
  SResReadyReq readyMsg = {0};

  while (!qwtTestStop) {
    qwtBuildReadyReqMsg(&readyMsg, &readyRpc);
    code = qWorkerProcessReadyMsg(mockPointer, mgmt, &readyRpc);
    if (qwtTestEnableSleep) {
      usleep(rand()%5);
    }
    if (++n % qwtTestPrintNum == 0) {
      printf("ready:%d\n", n);
    }    
  }

  return NULL;
}

void *fetchThread(void *param) {
  SRpcMsg fetchRpc = {0};
  int32_t code = 0;
  uint32_t n = 0;  
  void *mockPointer = (void *)0x1;    
  void *mgmt = param;
  SResFetchReq fetchMsg = {0};

  while (!qwtTestStop) {
    qwtBuildFetchReqMsg(&fetchMsg, &fetchRpc);
    code = qWorkerProcessFetchMsg(mockPointer, mgmt, &fetchRpc);
    if (qwtTestEnableSleep) {
      usleep(rand()%5);
    }
    if (++n % qwtTestPrintNum == 0) {
      printf("fetch:%d\n", n);
    }    
  }

  return NULL;
}

void *dropThread(void *param) {
  SRpcMsg dropRpc = {0};
  int32_t code = 0;
  uint32_t n = 0;  
  void *mockPointer = (void *)0x1;    
  void *mgmt = param;
  STaskDropReq dropMsg = {0};  

  while (!qwtTestStop) {
    qwtBuildDropReqMsg(&dropMsg, &dropRpc);
    code = qWorkerProcessDropMsg(mockPointer, mgmt, &dropRpc);
    if (qwtTestEnableSleep) {
      usleep(rand()%5);
    }
    if (++n % qwtTestPrintNum == 0) {
      printf("drop:%d\n", n);
    }    
  }

  return NULL;
}

void *statusThread(void *param) {
  SRpcMsg statusRpc = {0};
  int32_t code = 0;
  uint32_t n = 0;  
  void *mockPointer = (void *)0x1;    
  void *mgmt = param;
  SSchTasksStatusReq statusMsg = {0};

  while (!qwtTestStop) {
    qwtBuildStatusReqMsg(&statusMsg, &statusRpc);
    code = qWorkerProcessStatusMsg(mockPointer, mgmt, &statusRpc);
    if (qwtTestEnableSleep) {
      usleep(rand()%5);
    }
    if (++n % qwtTestPrintNum == 0) {
      printf("status:%d\n", n);
    }    
  }

  return NULL;
}


void *qwtclientThread(void *param) {
  int32_t code = 0;
  uint32_t n = 0;
  void *mgmt = param;
  void *mockPointer = (void *)0x1;    
  SRpcMsg queryRpc = {0};

  sleep(1);

  while (!qwtTestStop) {
    qwtTestCaseFinished = false;
    
    qwtBuildQueryReqMsg(&queryRpc);
    qwtPutReqToQueue((void *)0x1, &queryRpc);

    while (!qwtTestCaseFinished) {
      usleep(1);
    }
    
    
    if (++n % qwtTestPrintNum == 0) {
      printf("case run:%d\n", n);
    }
  }

  atomic_add_fetch_32(&qwtTestQuitThreadNum, 1);

  return NULL;
}

void *queryQueueThread(void *param) {
  void *mockPointer = (void *)0x1;   
  SRpcMsg *queryRpc = NULL;
  void *mgmt = param;

  while (true) {
    tsem_wait(&qwtTestQuerySem);

    if (qwtTestStop && qwtTestQueryQueueNum <= 0 && qwtTestCaseFinished) {
      break;
    }

    taosWLockLatch(&qwtTestQueryQueueLock);
    if (qwtTestQueryQueueNum <= 0 || qwtTestQueryQueueRIdx == qwtTestQueryQueueWIdx) {
      printf("query queue is empty\n");
      assert(0);
    }
    
    queryRpc = qwtTestQueryQueue[qwtTestQueryQueueRIdx++];
    
    if (qwtTestQueryQueueRIdx >= qwtTestQueryQueueSize) {
      qwtTestQueryQueueRIdx = 0;
    }
    
    qwtTestQueryQueueNum--;
    taosWUnLockLatch(&qwtTestQueryQueueLock);


    if (qwtTestEnableSleep && qwtTestReqMaxDelayUsec > 0) {
      int32_t delay = rand() % qwtTestReqMaxDelayUsec;

      if (delay) {
        usleep(delay);
      }
    }
    
    if (TDMT_VND_QUERY == queryRpc->msgType) {
      qWorkerProcessQueryMsg(mockPointer, mgmt, queryRpc);
    } else if (TDMT_VND_QUERY_CONTINUE == queryRpc->msgType) {
      qWorkerProcessCQueryMsg(mockPointer, mgmt, queryRpc);
    } else {
      printf("unknown msg in query queue, type:%d\n", queryRpc->msgType);
      assert(0);
    }

    free(queryRpc);

    if (qwtTestStop && qwtTestQueryQueueNum <= 0 && qwtTestCaseFinished) {
      break;
    }
  }

  atomic_add_fetch_32(&qwtTestQuitThreadNum, 1);

  return NULL;
}

void *fetchQueueThread(void *param) {
  void *mockPointer = (void *)0x1;   
  SRpcMsg *fetchRpc = NULL;
  void *mgmt = param;

  while (true) {
    tsem_wait(&qwtTestFetchSem);

    if (qwtTestStop && qwtTestFetchQueueNum <= 0 && qwtTestCaseFinished) {
      break;
    }    

    taosWLockLatch(&qwtTestFetchQueueLock);
    if (qwtTestFetchQueueNum <= 0 || qwtTestFetchQueueRIdx == qwtTestFetchQueueWIdx) {
      printf("Fetch queue is empty\n");
      assert(0);
    }
    
    fetchRpc = qwtTestFetchQueue[qwtTestFetchQueueRIdx++];
    
    if (qwtTestFetchQueueRIdx >= qwtTestFetchQueueSize) {
      qwtTestFetchQueueRIdx = 0;
    }
    
    qwtTestFetchQueueNum--;
    taosWUnLockLatch(&qwtTestFetchQueueLock);

    if (qwtTestEnableSleep && qwtTestReqMaxDelayUsec > 0) {
      int32_t delay = rand() % qwtTestReqMaxDelayUsec;

      if (delay) {
        usleep(delay);
      }
    }

    switch (fetchRpc->msgType) {
      case TDMT_VND_FETCH:
        qWorkerProcessFetchMsg(mockPointer, mgmt, fetchRpc);
        break;
      case TDMT_VND_RES_READY:
        qWorkerProcessReadyMsg(mockPointer, mgmt, fetchRpc);
        break;
      case TDMT_VND_TASKS_STATUS:
        qWorkerProcessStatusMsg(mockPointer, mgmt, fetchRpc);
        break;
      case TDMT_VND_CANCEL_TASK:
        qWorkerProcessCancelMsg(mockPointer, mgmt, fetchRpc);
        break;
      case TDMT_VND_DROP_TASK:
        qWorkerProcessDropMsg(mockPointer, mgmt, fetchRpc);
        break;
      default:
        printf("unknown msg type:%d in fetch queue", fetchRpc->msgType);
        assert(0);
        break;
    }

    free(fetchRpc);

    if (qwtTestStop && qwtTestFetchQueueNum <= 0 && qwtTestCaseFinished) {
      break;
    }    
  }

  atomic_add_fetch_32(&qwtTestQuitThreadNum, 1);

  return NULL;
}



}

#if 0

TEST(seqTest, normalCase) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;
  SRpcMsg queryRpc = {0};
  SRpcMsg readyRpc = {0};
  SRpcMsg fetchRpc = {0};
  SRpcMsg dropRpc = {0};
  SRpcMsg statusRpc = {0};

  qwtInitLogFile();

  qwtBuildQueryReqMsg(&queryRpc);
  qwtBuildReadyReqMsg(&qwtreadyMsg, &readyRpc);
  qwtBuildFetchReqMsg(&qwtfetchMsg, &fetchRpc);
  qwtBuildDropReqMsg(&qwtdropMsg, &dropRpc);
  
  stubSetStringToPlan();
  stubSetRpcSendResponse();
  stubSetExecTask();
  stubSetCreateExecTask();
  stubSetAsyncKillTask();
  stubSetDestroyTask();
  stubSetDestroyDataSinker();
  stubSetGetDataLength();
  stubSetEndPut();
  stubSetPutDataBlock();
  stubSetGetDataBlock();
  
  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue);
  ASSERT_EQ(code, 0);

  code = qWorkerProcessQueryMsg(mockPointer, mgmt, &queryRpc);
  ASSERT_EQ(code, 0);

  code = qWorkerProcessReadyMsg(mockPointer, mgmt, &readyRpc);
  ASSERT_EQ(code, 0);

  code = qWorkerProcessFetchMsg(mockPointer, mgmt, &fetchRpc);
  ASSERT_EQ(code, 0);

  code = qWorkerProcessDropMsg(mockPointer, mgmt, &dropRpc);
  ASSERT_EQ(code, 0);

  qwtBuildStatusReqMsg(&qwtstatusMsg, &statusRpc);
  code = qWorkerProcessStatusMsg(mockPointer, mgmt, &statusRpc);
  ASSERT_EQ(code, 0);

  qWorkerDestroy(&mgmt);
}

TEST(seqTest, cancelFirst) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;
  SRpcMsg queryRpc = {0};
  SRpcMsg dropRpc = {0};
  SRpcMsg statusRpc = {0};

  qwtInitLogFile();
  
  qwtBuildQueryReqMsg(&queryRpc);
  qwtBuildDropReqMsg(&qwtdropMsg, &dropRpc);
  qwtBuildStatusReqMsg(&qwtstatusMsg, &statusRpc);

  stubSetStringToPlan();
  stubSetRpcSendResponse();
  
  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue);
  ASSERT_EQ(code, 0);

  qwtBuildStatusReqMsg(&qwtstatusMsg, &statusRpc);
  code = qWorkerProcessStatusMsg(mockPointer, mgmt, &statusRpc);
  ASSERT_EQ(code, 0);

  code = qWorkerProcessDropMsg(mockPointer, mgmt, &dropRpc);
  ASSERT_EQ(code, 0);

  qwtBuildStatusReqMsg(&qwtstatusMsg, &statusRpc);
  code = qWorkerProcessStatusMsg(mockPointer, mgmt, &statusRpc);
  ASSERT_EQ(code, 0);

  code = qWorkerProcessQueryMsg(mockPointer, mgmt, &queryRpc);
  ASSERT_TRUE(0 != code);

  qwtBuildStatusReqMsg(&qwtstatusMsg, &statusRpc);
  code = qWorkerProcessStatusMsg(mockPointer, mgmt, &statusRpc);
  ASSERT_EQ(code, 0);

  qWorkerDestroy(&mgmt);
}

TEST(seqTest, randCase) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;
  SRpcMsg queryRpc = {0};
  SRpcMsg readyRpc = {0};
  SRpcMsg fetchRpc = {0};
  SRpcMsg dropRpc = {0};
  SRpcMsg statusRpc = {0};
  SResReadyReq readyMsg = {0};
  SResFetchReq fetchMsg = {0};
  STaskDropReq dropMsg = {0};  
  SSchTasksStatusReq statusMsg = {0};

  qwtInitLogFile();
  
  stubSetStringToPlan();
  stubSetRpcSendResponse();
  stubSetCreateExecTask();

  srand(time(NULL));
  
  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue);
  ASSERT_EQ(code, 0);

  int32_t t = 0;
  int32_t maxr = 10001;
  while (true) {
    int32_t r = rand() % maxr;
    
    if (r >= 0 && r < maxr/5) {
      printf("Query,%d\n", t++);      
      qwtBuildQueryReqMsg(&queryRpc);
      code = qWorkerProcessQueryMsg(mockPointer, mgmt, &queryRpc);
    } else if (r >= maxr/5 && r < maxr * 2/5) {
      printf("Ready,%d\n", t++);
      qwtBuildReadyReqMsg(&readyMsg, &readyRpc);
      code = qWorkerProcessReadyMsg(mockPointer, mgmt, &readyRpc);
      if (qwtTestEnableSleep) {
        usleep(1);
      }
    } else if (r >= maxr * 2/5 && r < maxr* 3/5) {
      printf("Fetch,%d\n", t++);
      qwtBuildFetchReqMsg(&fetchMsg, &fetchRpc);
      code = qWorkerProcessFetchMsg(mockPointer, mgmt, &fetchRpc);
      if (qwtTestEnableSleep) {
        usleep(1);
      }
    } else if (r >= maxr * 3/5 && r < maxr * 4/5) {
      printf("Drop,%d\n", t++);
      qwtBuildDropReqMsg(&dropMsg, &dropRpc);
      code = qWorkerProcessDropMsg(mockPointer, mgmt, &dropRpc);
      if (qwtTestEnableSleep) {
        usleep(1);
      }
    } else if (r >= maxr * 4/5 && r < maxr-1) {
      printf("Status,%d\n", t++);
      qwtBuildStatusReqMsg(&statusMsg, &statusRpc);
      code = qWorkerProcessStatusMsg(mockPointer, mgmt, &statusRpc);
      ASSERT_EQ(code, 0);
      if (qwtTestEnableSleep) {
        usleep(1);
      }      
    } else {
      printf("QUIT RAND NOW");
      break;
    }
  }

  qWorkerDestroy(&mgmt);
}

TEST(seqTest, multithreadRand) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;

  qwtInitLogFile();
  
  stubSetStringToPlan();
  stubSetRpcSendResponse();

  srand(time(NULL));
  
  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue);
  ASSERT_EQ(code, 0);

  pthread_attr_t thattr;
  pthread_attr_init(&thattr);

  pthread_t t1,t2,t3,t4,t5;
  pthread_create(&(t1), &thattr, queryThread, mgmt);
  pthread_create(&(t2), &thattr, readyThread, NULL);
  pthread_create(&(t3), &thattr, fetchThread, NULL);
  pthread_create(&(t4), &thattr, dropThread, NULL);
  pthread_create(&(t5), &thattr, statusThread, NULL);

  while (true) {
    if (qwtTestDeadLoop) {
      sleep(1);
    } else {
      sleep(qwtTestMTRunSec);
      break;
    }
  }
  
  qwtTestStop = true;
  sleep(3);
  
  qWorkerDestroy(&mgmt);
}

#endif

TEST(rcTest, shortExecshortDelay) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;

  qwtInitLogFile();
  
  stubSetStringToPlan();
  stubSetRpcSendResponse();
  stubSetExecTask();
  stubSetCreateExecTask();
  stubSetAsyncKillTask();
  stubSetDestroyTask();
  stubSetDestroyDataSinker();
  stubSetGetDataLength();
  stubSetEndPut();
  stubSetPutDataBlock();
  stubSetGetDataBlock();

  srand(time(NULL));
  qwtTestStop = false;
  qwtTestQuitThreadNum = 0;

  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue, NULL);
  ASSERT_EQ(code, 0);

  qwtTestMaxExecTaskUsec = 0;
  qwtTestReqMaxDelayUsec = 0;

  tsem_init(&qwtTestQuerySem, 0, 0);
  tsem_init(&qwtTestFetchSem, 0, 0);

  pthread_attr_t thattr;
  pthread_attr_init(&thattr);

  pthread_t t1,t2,t3,t4,t5;
  pthread_create(&(t1), &thattr, qwtclientThread, mgmt);
  pthread_create(&(t2), &thattr, queryQueueThread, mgmt);
  pthread_create(&(t3), &thattr, fetchQueueThread, mgmt);

  while (true) {
    if (qwtTestDeadLoop) {
      sleep(1);
    } else {
      sleep(qwtTestMTRunSec);
      break;
    }
  }
  
  qwtTestStop = true;

  while (true) {
    if (qwtTestQuitThreadNum == 3) {
      break;
    }
    
    sleep(1);

    if (qwtTestCaseFinished) {
      if (qwtTestQuitThreadNum < 3) { 
        tsem_post(&qwtTestQuerySem);
        tsem_post(&qwtTestFetchSem);

        usleep(10);
      }
    }
    
  }

  qwtTestQueryQueueNum = 0;
  qwtTestQueryQueueRIdx = 0;
  qwtTestQueryQueueWIdx = 0;
  qwtTestQueryQueueLock = 0;
  qwtTestFetchQueueNum = 0;
  qwtTestFetchQueueRIdx = 0;
  qwtTestFetchQueueWIdx = 0;
  qwtTestFetchQueueLock = 0;
  
  qWorkerDestroy(&mgmt);  
}

TEST(rcTest, longExecshortDelay) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;

  qwtInitLogFile();
  
  stubSetStringToPlan();
  stubSetRpcSendResponse();
  stubSetExecTask();
  stubSetCreateExecTask();
  stubSetAsyncKillTask();
  stubSetDestroyTask();
  stubSetDestroyDataSinker();
  stubSetGetDataLength();
  stubSetEndPut();
  stubSetPutDataBlock();
  stubSetGetDataBlock();

  srand(time(NULL));
  qwtTestStop = false;
  qwtTestQuitThreadNum = 0;

  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue, NULL);
  ASSERT_EQ(code, 0);

  qwtTestMaxExecTaskUsec = 1000000;
  qwtTestReqMaxDelayUsec = 0;

  tsem_init(&qwtTestQuerySem, 0, 0);
  tsem_init(&qwtTestFetchSem, 0, 0);

  pthread_attr_t thattr;
  pthread_attr_init(&thattr);

  pthread_t t1,t2,t3,t4,t5;
  pthread_create(&(t1), &thattr, qwtclientThread, mgmt);
  pthread_create(&(t2), &thattr, queryQueueThread, mgmt);
  pthread_create(&(t3), &thattr, fetchQueueThread, mgmt);

  while (true) {
    if (qwtTestDeadLoop) {
      sleep(1);
    } else {
      sleep(qwtTestMTRunSec);
      break;
    }
  }
  
  qwtTestStop = true;

 
  while (true) {
    if (qwtTestQuitThreadNum == 3) {
      break;
    }
    
    sleep(1);

    if (qwtTestCaseFinished) {
      if (qwtTestQuitThreadNum < 3) { 
        tsem_post(&qwtTestQuerySem);
        tsem_post(&qwtTestFetchSem);
        
        usleep(10);
      }
    }
    
  }

  qwtTestQueryQueueNum = 0;
  qwtTestQueryQueueRIdx = 0;
  qwtTestQueryQueueWIdx = 0;
  qwtTestQueryQueueLock = 0;
  qwtTestFetchQueueNum = 0;
  qwtTestFetchQueueRIdx = 0;
  qwtTestFetchQueueWIdx = 0;
  qwtTestFetchQueueLock = 0;
  
  qWorkerDestroy(&mgmt);
}


TEST(rcTest, shortExeclongDelay) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;

  qwtInitLogFile();
  
  stubSetStringToPlan();
  stubSetRpcSendResponse();
  stubSetExecTask();
  stubSetCreateExecTask();
  stubSetAsyncKillTask();
  stubSetDestroyTask();
  stubSetDestroyDataSinker();
  stubSetGetDataLength();
  stubSetEndPut();
  stubSetPutDataBlock();
  stubSetGetDataBlock();

  srand(time(NULL));
  qwtTestStop = false;
  qwtTestQuitThreadNum = 0;

  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue, NULL);
  ASSERT_EQ(code, 0);

  qwtTestMaxExecTaskUsec = 0;
  qwtTestReqMaxDelayUsec = 1000000;

  tsem_init(&qwtTestQuerySem, 0, 0);
  tsem_init(&qwtTestFetchSem, 0, 0);

  pthread_attr_t thattr;
  pthread_attr_init(&thattr);

  pthread_t t1,t2,t3,t4,t5;
  pthread_create(&(t1), &thattr, qwtclientThread, mgmt);
  pthread_create(&(t2), &thattr, queryQueueThread, mgmt);
  pthread_create(&(t3), &thattr, fetchQueueThread, mgmt);

  while (true) {
    if (qwtTestDeadLoop) {
      sleep(1);
    } else {
      sleep(qwtTestMTRunSec);
      break;
    }
  }
  
  qwtTestStop = true;


  while (true) {
    if (qwtTestQuitThreadNum == 3) {
      break;
    }
    
    sleep(1);

    if (qwtTestCaseFinished) {
      if (qwtTestQuitThreadNum < 3) { 
        tsem_post(&qwtTestQuerySem);
        tsem_post(&qwtTestFetchSem);
        
        usleep(10);
      }
    }
    
  }

  qwtTestQueryQueueNum = 0;
  qwtTestQueryQueueRIdx = 0;
  qwtTestQueryQueueWIdx = 0;
  qwtTestQueryQueueLock = 0;
  qwtTestFetchQueueNum = 0;
  qwtTestFetchQueueRIdx = 0;
  qwtTestFetchQueueWIdx = 0;
  qwtTestFetchQueueLock = 0;
  
  qWorkerDestroy(&mgmt);
}


#if 0
TEST(rcTest, dropTest) {
  void *mgmt = NULL;
  int32_t code = 0;
  void *mockPointer = (void *)0x1;

  qwtInitLogFile();
  
  stubSetStringToPlan();
  stubSetRpcSendResponse();
  stubSetExecTask();
  stubSetCreateExecTask();
  stubSetAsyncKillTask();
  stubSetDestroyTask();
  stubSetDestroyDataSinker();
  stubSetGetDataLength();
  stubSetEndPut();
  stubSetPutDataBlock();
  stubSetGetDataBlock();

  srand(time(NULL));
  
  code = qWorkerInit(NODE_TYPE_VNODE, 1, NULL, &mgmt, mockPointer, qwtPutReqToQueue);
  ASSERT_EQ(code, 0);

  tsem_init(&qwtTestQuerySem, 0, 0);
  tsem_init(&qwtTestFetchSem, 0, 0);

  pthread_attr_t thattr;
  pthread_attr_init(&thattr);

  pthread_t t1,t2,t3,t4,t5;
  pthread_create(&(t1), &thattr, clientThread, mgmt);
  pthread_create(&(t2), &thattr, queryQueueThread, mgmt);
  pthread_create(&(t3), &thattr, fetchQueueThread, mgmt);

  while (true) {
    if (qwtTestDeadLoop) {
      sleep(1);
    } else {
      sleep(qwtTestMTRunSec);
      break;
    }
  }
  
  qwtTestStop = true;
  sleep(3);
  
  qWorkerDestroy(&mgmt);
}
#endif


int main(int argc, char** argv) {
  srand(time(NULL));
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

#pragma GCC diagnostic pop

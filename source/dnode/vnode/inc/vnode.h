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

#ifndef _TD_VNODE_H_
#define _TD_VNODE_H_

#include "os.h"
#include "trpc.h"

#include "meta.h"
#include "tarray.h"
#include "tfs.h"
#include "tsdb.h"
#include "wal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------ TYPES EXPOSED ------------------------ */
typedef struct SVnode SVnode;
typedef struct SDnode SDnode;
typedef int32_t (*PutReqToVQueryQFp)(SDnode *pDnode, struct SRpcMsg *pReq);
typedef int32_t (*SendReqToDnodeFp)(SDnode *pDnode, struct SEpSet *epSet, struct SRpcMsg *rpcMsg);

typedef struct STqCfg {
  // TODO
  int32_t reserved;
} STqCfg;


typedef struct SVnodeCfg {
  int32_t  vgId;
  SDnode  *pDnode;
  STfs    *pTfs;
  uint64_t wsize;
  uint64_t ssize;
  uint64_t lsize;
  bool     isHeapAllocator;
  uint32_t ttl;
  uint32_t keep;
  bool     isWeak;
  STsdbCfg tsdbCfg;
  SMetaCfg metaCfg;
  STqCfg   tqCfg;
  SWalCfg  walCfg;
} SVnodeCfg;

typedef struct {
  int32_t           sver;
  const char       *timezone;
  const char       *locale;
  const char       *charset;
  uint16_t          nthreads;  // number of commit threads. 0 for no threads and a schedule queue should be given (TODO)
  PutReqToVQueryQFp putReqToVQueryQFp;
  SendReqToDnodeFp  sendReqToDnodeFp;
} SVnodeOpt;

typedef struct STqReadHandle {
  int64_t        ver;
  uint64_t       tbUid;
  SSubmitMsg*    pMsg;
  SSubmitBlk*    pBlock;
  SSubmitMsgIter msgIter;
  SSubmitBlkIter blkIter;
  SMeta*         pMeta;
  SArray*        pColIdList;
} STqReadHandle;

/* ------------------------ SVnode ------------------------ */
/**
 * @brief Initialize the vnode module
 *
 * @param pOption Option of the vnode mnodule
 * @return int 0 for success and -1 for failure
 */
int vnodeInit(const SVnodeOpt *pOption);

/**
 * @brief Cleanup the vnode module
 *
 */
void vnodeCleanup();

/**
 * @brief Open a VNODE.
 *
 * @param path path of the vnode
 * @param pVnodeCfg options of the vnode
 * @return SVnode* The vnode object
 */
SVnode *vnodeOpen(const char *path, const SVnodeCfg *pVnodeCfg);

/**
 * @brief Close a VNODE
 *
 * @param pVnode The vnode object to close
 */
void vnodeClose(SVnode *pVnode);

/**
 * @brief Destroy a VNODE.
 *
 * @param path Path of the VNODE.
 */
void vnodeDestroy(const char *path);

/**
 * @brief Process an array of write messages.
 *
 * @param pVnode The vnode object.
 * @param pMsgs The array of SRpcMsg
 * @return int 0 for success, -1 for failure
 */
int vnodeProcessWMsgs(SVnode *pVnode, SArray *pMsgs);

/**
 * @brief Apply a write request message.
 *
 * @param pVnode The vnode object.
 * @param pMsg The request message
 * @param pRsp The response message
 * @return int 0 for success, -1 for failure
 */
int vnodeApplyWMsg(SVnode *pVnode, SRpcMsg *pMsg, SRpcMsg **pRsp);

/**
 * @brief Process a consume message.
 *
 * @param pVnode The vnode object.
 * @param pMsg The request message
 * @param pRsp The response message
 * @return int 0 for success, -1 for failure
 */
int vnodeProcessCMsg(SVnode *pVnode, SRpcMsg *pMsg, SRpcMsg **pRsp);

/**
 * @brief Process the sync request
 *
 * @param pVnode
 * @param pMsg
 * @param pRsp
 * @return int
 */
int vnodeProcessSyncReq(SVnode *pVnode, SRpcMsg *pMsg, SRpcMsg **pRsp);

/**
 * @brief Process a query message.
 *
 * @param pVnode The vnode object.
 * @param pMsg The request message
 * @return int 0 for success, -1 for failure
 */
int vnodeProcessQueryMsg(SVnode *pVnode, SRpcMsg *pMsg);

/**
 * @brief Process a fetch message.
 *
 * @param pVnode The vnode object.
 * @param pMsg The request message
 * @return int 0 for success, -1 for failure
 */
int vnodeProcessFetchMsg(SVnode *pVnode, SRpcMsg *pMsg);

/* ------------------------ SVnodeCfg ------------------------ */
/**
 * @brief Initialize VNODE options.
 *
 * @param pOptions The options object to be initialized. It should not be NULL.
 */
void vnodeOptionsInit(SVnodeCfg *pOptions);

/**
 * @brief Clear VNODE options.
 *
 * @param pOptions Options to clear.
 */
void vnodeOptionsClear(SVnodeCfg *pOptions);

/* ------------------------ FOR COMPILE ------------------------ */

int32_t vnodeAlter(SVnode *pVnode, const SVnodeCfg *pCfg);
int32_t vnodeCompact(SVnode *pVnode);
int32_t vnodeSync(SVnode *pVnode);
int32_t vnodeGetLoad(SVnode *pVnode, SVnodeLoad *pLoad);

/* ------------------------- TQ QUERY -------------------------- */

STqReadHandle* tqInitSubmitMsgScanner(SMeta* pMeta);

static FORCE_INLINE void tqReadHandleSetColIdList(STqReadHandle* pReadHandle, SArray* pColIdList) {
  pReadHandle->pColIdList = pColIdList;
}

static FORCE_INLINE void tqReadHandleSetTbUid(STqReadHandle* pHandle, uint64_t tbUid) {
  pHandle->tbUid = tbUid;
}

void           tqReadHandleSetMsg(STqReadHandle* pHandle, SSubmitMsg* pMsg, int64_t ver);
bool           tqNextDataBlock(STqReadHandle* pHandle);
int            tqRetrieveDataBlockInfo(STqReadHandle* pHandle, SDataBlockInfo* pBlockInfo);
// return SArray<SColumnInfoData>
SArray*        tqRetrieveDataBlock(STqReadHandle* pHandle);


#ifdef __cplusplus
}
#endif

#endif /*_TD_VNODE_H_*/

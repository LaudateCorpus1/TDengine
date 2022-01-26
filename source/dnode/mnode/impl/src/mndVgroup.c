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

#define _DEFAULT_SOURCE
#include "mndVgroup.h"
#include "mndDb.h"
#include "mndDnode.h"
#include "mndMnode.h"
#include "mndShow.h"
#include "mndTrans.h"

#define TSDB_VGROUP_VER_NUMBER 1
#define TSDB_VGROUP_RESERVE_SIZE 64

static SSdbRow *mndVgroupActionDecode(SSdbRaw *pRaw);
static int32_t  mndVgroupActionInsert(SSdb *pSdb, SVgObj *pVgroup);
static int32_t  mndVgroupActionDelete(SSdb *pSdb, SVgObj *pVgroup);
static int32_t  mndVgroupActionUpdate(SSdb *pSdb, SVgObj *pOld, SVgObj *pNew);

static int32_t mndProcessCreateVnodeRsp(SMnodeMsg *pRsp);
static int32_t mndProcessAlterVnodeRsp(SMnodeMsg *pRsp);
static int32_t mndProcessDropVnodeRsp(SMnodeMsg *pRsp);
static int32_t mndProcessSyncVnodeRsp(SMnodeMsg *pRsp);
static int32_t mndProcessCompactVnodeRsp(SMnodeMsg *pRsp);

static int32_t mndGetVgroupMeta(SMnodeMsg *pReq, SShowObj *pShow, STableMetaRsp *pMeta);
static int32_t mndRetrieveVgroups(SMnodeMsg *pReq, SShowObj *pShow, char *data, int32_t rows);
static void    mndCancelGetNextVgroup(SMnode *pMnode, void *pIter);
static int32_t mndGetVnodeMeta(SMnodeMsg *pReq, SShowObj *pShow, STableMetaRsp *pMeta);
static int32_t mndRetrieveVnodes(SMnodeMsg *pReq, SShowObj *pShow, char *data, int32_t rows);
static void    mndCancelGetNextVnode(SMnode *pMnode, void *pIter);

int32_t mndInitVgroup(SMnode *pMnode) {
  SSdbTable table = {.sdbType = SDB_VGROUP,
                     .keyType = SDB_KEY_INT32,
                     .encodeFp = (SdbEncodeFp)mndVgroupActionEncode,
                     .decodeFp = (SdbDecodeFp)mndVgroupActionDecode,
                     .insertFp = (SdbInsertFp)mndVgroupActionInsert,
                     .updateFp = (SdbUpdateFp)mndVgroupActionDelete,
                     .deleteFp = (SdbDeleteFp)mndVgroupActionUpdate};

  mndSetMsgHandle(pMnode, TDMT_DND_CREATE_VNODE_RSP, mndProcessCreateVnodeRsp);
  mndSetMsgHandle(pMnode, TDMT_DND_ALTER_VNODE_RSP, mndProcessAlterVnodeRsp);
  mndSetMsgHandle(pMnode, TDMT_DND_DROP_VNODE_RSP, mndProcessDropVnodeRsp);
  mndSetMsgHandle(pMnode, TDMT_DND_SYNC_VNODE_RSP, mndProcessSyncVnodeRsp);
  mndSetMsgHandle(pMnode, TDMT_DND_COMPACT_VNODE_RSP, mndProcessCompactVnodeRsp);

  mndAddShowMetaHandle(pMnode, TSDB_MGMT_TABLE_VGROUP, mndGetVgroupMeta);
  mndAddShowRetrieveHandle(pMnode, TSDB_MGMT_TABLE_VGROUP, mndRetrieveVgroups);
  mndAddShowFreeIterHandle(pMnode, TSDB_MGMT_TABLE_VGROUP, mndCancelGetNextVgroup);
  mndAddShowMetaHandle(pMnode, TSDB_MGMT_TABLE_VNODES, mndGetVnodeMeta);
  mndAddShowRetrieveHandle(pMnode, TSDB_MGMT_TABLE_VNODES, mndRetrieveVnodes);
  mndAddShowFreeIterHandle(pMnode, TSDB_MGMT_TABLE_VNODES, mndCancelGetNextVnode);

  return sdbSetTable(pMnode->pSdb, table);
}

void mndCleanupVgroup(SMnode *pMnode) {}

SSdbRaw *mndVgroupActionEncode(SVgObj *pVgroup) {
  terrno = TSDB_CODE_OUT_OF_MEMORY;

  SSdbRaw *pRaw = sdbAllocRaw(SDB_VGROUP, TSDB_VGROUP_VER_NUMBER, sizeof(SVgObj) + TSDB_VGROUP_RESERVE_SIZE);
  if (pRaw == NULL) goto VG_ENCODE_OVER;

  int32_t dataPos = 0;
  SDB_SET_INT32(pRaw, dataPos, pVgroup->vgId, VG_ENCODE_OVER)
  SDB_SET_INT64(pRaw, dataPos, pVgroup->createdTime, VG_ENCODE_OVER)
  SDB_SET_INT64(pRaw, dataPos, pVgroup->updateTime, VG_ENCODE_OVER)
  SDB_SET_INT32(pRaw, dataPos, pVgroup->version, VG_ENCODE_OVER)
  SDB_SET_INT32(pRaw, dataPos, pVgroup->hashBegin, VG_ENCODE_OVER)
  SDB_SET_INT32(pRaw, dataPos, pVgroup->hashEnd, VG_ENCODE_OVER)
  SDB_SET_BINARY(pRaw, dataPos, pVgroup->dbName, TSDB_DB_FNAME_LEN, VG_ENCODE_OVER)
  SDB_SET_INT64(pRaw, dataPos, pVgroup->dbUid, VG_ENCODE_OVER)
  SDB_SET_INT8(pRaw, dataPos, pVgroup->replica, VG_ENCODE_OVER)
  for (int8_t i = 0; i < pVgroup->replica; ++i) {
    SVnodeGid *pVgid = &pVgroup->vnodeGid[i];
    SDB_SET_INT32(pRaw, dataPos, pVgid->dnodeId, VG_ENCODE_OVER)
  }
  SDB_SET_RESERVE(pRaw, dataPos, TSDB_VGROUP_RESERVE_SIZE, VG_ENCODE_OVER)
  SDB_SET_DATALEN(pRaw, dataPos, VG_ENCODE_OVER)

  terrno = 0;

VG_ENCODE_OVER:
  if (terrno != 0) {
    mError("vgId:%d, failed to encode to raw:%p since %s", pVgroup->vgId, pRaw, terrstr());
    sdbFreeRaw(pRaw);
    return NULL;
  }

  mTrace("vgId:%d, encode to raw:%p, row:%p", pVgroup->vgId, pRaw, pVgroup);
  return pRaw;
}

SSdbRow *mndVgroupActionDecode(SSdbRaw *pRaw) {
  terrno = TSDB_CODE_OUT_OF_MEMORY;

  int8_t sver = 0;
  if (sdbGetRawSoftVer(pRaw, &sver) != 0) goto VG_DECODE_OVER;

  if (sver != TSDB_VGROUP_VER_NUMBER) {
    terrno = TSDB_CODE_SDB_INVALID_DATA_VER;
    goto VG_DECODE_OVER;
  }

  SSdbRow *pRow = sdbAllocRow(sizeof(SVgObj));
  if (pRow == NULL) goto VG_DECODE_OVER;

  SVgObj *pVgroup = sdbGetRowObj(pRow);
  if (pVgroup == NULL) goto VG_DECODE_OVER;

  int32_t dataPos = 0;
  SDB_GET_INT32(pRaw, dataPos, &pVgroup->vgId, VG_DECODE_OVER)
  SDB_GET_INT64(pRaw, dataPos, &pVgroup->createdTime, VG_DECODE_OVER)
  SDB_GET_INT64(pRaw, dataPos, &pVgroup->updateTime, VG_DECODE_OVER)
  SDB_GET_INT32(pRaw, dataPos, &pVgroup->version, VG_DECODE_OVER)
  SDB_GET_INT32(pRaw, dataPos, &pVgroup->hashBegin, VG_DECODE_OVER)
  SDB_GET_INT32(pRaw, dataPos, &pVgroup->hashEnd, VG_DECODE_OVER)
  SDB_GET_BINARY(pRaw, dataPos, pVgroup->dbName, TSDB_DB_FNAME_LEN, VG_DECODE_OVER)
  SDB_GET_INT64(pRaw, dataPos, &pVgroup->dbUid, VG_DECODE_OVER)
  SDB_GET_INT8(pRaw, dataPos, &pVgroup->replica, VG_DECODE_OVER)
  for (int8_t i = 0; i < pVgroup->replica; ++i) {
    SVnodeGid *pVgid = &pVgroup->vnodeGid[i];
    SDB_GET_INT32(pRaw, dataPos, &pVgid->dnodeId, VG_DECODE_OVER)
    if (pVgroup->replica == 1) {
      pVgid->role = TAOS_SYNC_STATE_LEADER;
    }
  }
  SDB_GET_RESERVE(pRaw, dataPos, TSDB_VGROUP_RESERVE_SIZE, VG_DECODE_OVER)

  terrno = 0;

VG_DECODE_OVER:
  if (terrno != 0) {
    mError("vgId:%d, failed to decode from raw:%p since %s", pVgroup->vgId, pRaw, terrstr());
    tfree(pRow);
    return NULL;
  }

  mTrace("vgId:%d, decode from raw:%p, row:%p", pVgroup->vgId, pRaw, pVgroup);
  return pRow;
}

static int32_t mndVgroupActionInsert(SSdb *pSdb, SVgObj *pVgroup) {
  mTrace("vgId:%d, perform insert action, row:%p", pVgroup->vgId, pVgroup);
  return 0;
}

static int32_t mndVgroupActionDelete(SSdb *pSdb, SVgObj *pVgroup) {
  mTrace("vgId:%d, perform delete action, row:%p", pVgroup->vgId, pVgroup);
  return 0;
}

static int32_t mndVgroupActionUpdate(SSdb *pSdb, SVgObj *pOld, SVgObj *pNew) {
  mTrace("vgId:%d, perform update action, old row:%p new row:%p", pOld->vgId, pOld, pNew);
  pOld->updateTime = pNew->updateTime;
  pOld->version = pNew->version;
  pOld->hashBegin = pNew->hashBegin;
  pOld->hashEnd = pNew->hashEnd;
  pOld->replica = pNew->replica;
  memcpy(pOld->vnodeGid, pNew->vnodeGid, TSDB_MAX_REPLICA * sizeof(SVnodeGid));
  return 0;
}

SVgObj *mndAcquireVgroup(SMnode *pMnode, int32_t vgId) {
  SSdb   *pSdb = pMnode->pSdb;
  SVgObj *pVgroup = sdbAcquire(pSdb, SDB_VGROUP, &vgId);
  if (pVgroup == NULL && terrno == TSDB_CODE_SDB_OBJ_NOT_THERE) {
    terrno = TSDB_CODE_MND_VGROUP_NOT_EXIST;
  }
  return pVgroup;
}

void mndReleaseVgroup(SMnode *pMnode, SVgObj *pVgroup) {
  SSdb *pSdb = pMnode->pSdb;
  sdbRelease(pSdb, pVgroup);
}

SCreateVnodeReq *mndBuildCreateVnodeReq(SMnode *pMnode, SDnodeObj *pDnode, SDbObj *pDb, SVgObj *pVgroup) {
  SCreateVnodeReq *pCreate = calloc(1, sizeof(SCreateVnodeReq));
  if (pCreate == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  pCreate->vgId = htonl(pVgroup->vgId);
  pCreate->dnodeId = htonl(pDnode->id);
  memcpy(pCreate->db, pDb->name, TSDB_DB_FNAME_LEN);
  pCreate->dbUid = htobe64(pDb->uid);
  pCreate->vgVersion = htonl(pVgroup->version);
  pCreate->cacheBlockSize = htonl(pDb->cfg.cacheBlockSize);
  pCreate->totalBlocks = htonl(pDb->cfg.totalBlocks);
  pCreate->daysPerFile = htonl(pDb->cfg.daysPerFile);
  pCreate->daysToKeep0 = htonl(pDb->cfg.daysToKeep0);
  pCreate->daysToKeep1 = htonl(pDb->cfg.daysToKeep1);
  pCreate->daysToKeep2 = htonl(pDb->cfg.daysToKeep2);
  pCreate->minRows = htonl(pDb->cfg.minRows);
  pCreate->maxRows = htonl(pDb->cfg.maxRows);
  pCreate->commitTime = htonl(pDb->cfg.commitTime);
  pCreate->fsyncPeriod = htonl(pDb->cfg.fsyncPeriod);
  pCreate->walLevel = pDb->cfg.walLevel;
  pCreate->precision = pDb->cfg.precision;
  pCreate->compression = pDb->cfg.compression;
  pCreate->quorum = pDb->cfg.quorum;
  pCreate->update = pDb->cfg.update;
  pCreate->cacheLastRow = pDb->cfg.cacheLastRow;
  pCreate->replica = pVgroup->replica;
  pCreate->selfIndex = -1;

  for (int32_t v = 0; v < pVgroup->replica; ++v) {
    SReplica  *pReplica = &pCreate->replicas[v];
    SVnodeGid *pVgid = &pVgroup->vnodeGid[v];
    SDnodeObj *pVgidDnode = mndAcquireDnode(pMnode, pVgid->dnodeId);
    if (pVgidDnode == NULL) {
      free(pCreate);
      return NULL;
    }

    pReplica->id = htonl(pVgidDnode->id);
    pReplica->port = htons(pVgidDnode->port);
    memcpy(pReplica->fqdn, pVgidDnode->fqdn, TSDB_FQDN_LEN);
    mndReleaseDnode(pMnode, pVgidDnode);

    if (pDnode->id == pVgid->dnodeId) {
      pCreate->selfIndex = v;
    }
  }

  if (pCreate->selfIndex == -1) {
    free(pCreate);
    terrno = TSDB_CODE_MND_APP_ERROR;
    return NULL;
  }

  return pCreate;
}

SDropVnodeReq *mndBuildDropVnodeReq(SMnode *pMnode, SDnodeObj *pDnode, SDbObj *pDb, SVgObj *pVgroup) {
  SDropVnodeReq *pDrop = calloc(1, sizeof(SDropVnodeReq));
  if (pDrop == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  pDrop->dnodeId = htonl(pDnode->id);
  pDrop->vgId = htonl(pVgroup->vgId);
  memcpy(pDrop->db, pDb->name, TSDB_DB_FNAME_LEN);
  pDrop->dbUid = htobe64(pDb->uid);

  return pDrop;
}

static bool mndResetDnodesArrayFp(SMnode *pMnode, void *pObj, void *p1, void *p2, void *p3) {
  SDnodeObj *pDnode = pObj;
  pDnode->numOfVnodes = 0;
  return true;
}

static bool mndBuildDnodesArrayFp(SMnode *pMnode, void *pObj, void *p1, void *p2, void *p3) {
  SDnodeObj *pDnode = pObj;
  SArray    *pArray = p1;

  int64_t curMs = taosGetTimestampMs();
  bool    online = mndIsDnodeOnline(pMnode, pDnode, curMs);
  bool    isMnode = mndIsMnode(pMnode, pDnode->id);
  pDnode->numOfVnodes = mndGetVnodesNum(pMnode, pDnode->id);

  mDebug("dnode:%d, vnodes:%d supportVnodes:%d isMnode:%d online:%d", pDnode->id, pDnode->numOfVnodes,
         pDnode->numOfSupportVnodes, isMnode, online);

  if (isMnode) {
    pDnode->numOfVnodes++;
  }

  if (online && pDnode->numOfSupportVnodes > 0) {
    taosArrayPush(pArray, pDnode);
  }
  return true;
}

static SArray *mndBuildDnodesArray(SMnode *pMnode) {
  SSdb   *pSdb = pMnode->pSdb;
  int32_t numOfDnodes = mndGetDnodeSize(pMnode);

  SArray *pArray = taosArrayInit(numOfDnodes, sizeof(SDnodeObj));
  if (pArray == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return NULL;
  }

  sdbTraverse(pSdb, SDB_DNODE, mndResetDnodesArrayFp, NULL, NULL, NULL);
  sdbTraverse(pSdb, SDB_DNODE, mndBuildDnodesArrayFp, pArray, NULL, NULL);
  return pArray;
}

static int32_t mndCompareDnodeVnodes(SDnodeObj *pDnode1, SDnodeObj *pDnode2) {
  float d1Score = (float)pDnode1->numOfVnodes / pDnode1->numOfSupportVnodes;
  float d2Score = (float)pDnode2->numOfVnodes / pDnode2->numOfSupportVnodes;
  return d1Score >= d2Score ? 1 : 0;
}

static int32_t mndGetAvailableDnode(SMnode *pMnode, SVgObj *pVgroup, SArray *pArray) {
  SSdb   *pSdb = pMnode->pSdb;
  int32_t allocedVnodes = 0;
  void   *pIter = NULL;

  taosArraySort(pArray, (__compar_fn_t)mndCompareDnodeVnodes);

  int32_t size = taosArrayGetSize(pArray);
  if (size < pVgroup->replica) {
    mError("db:%s, vgId:%d, no enough online dnodes:%d to alloc %d replica", pVgroup->dbName, pVgroup->vgId, size,
           pVgroup->replica);
    terrno = TSDB_CODE_MND_NO_ENOUGH_DNODES;
    return -1;
  }

  for (int32_t v = 0; v < pVgroup->replica; ++v) {
    SVnodeGid *pVgid = &pVgroup->vnodeGid[v];
    SDnodeObj *pDnode = taosArrayGet(pArray, v);
    if (pDnode == NULL || pDnode->numOfVnodes > pDnode->numOfSupportVnodes) {
      terrno = TSDB_CODE_MND_NO_ENOUGH_DNODES;
      return -1;
    }

    pVgid->dnodeId = pDnode->id;
    if (pVgroup->replica == 1) {
      pVgid->role = TAOS_SYNC_STATE_LEADER;
    } else {
      pVgid->role = TAOS_SYNC_STATE_FOLLOWER;
    }

    mDebug("db:%s, vgId:%d, vn:%d dnode:%d is alloced", pVgroup->dbName, pVgroup->vgId, v, pVgid->dnodeId);
    pDnode->numOfVnodes++;
  }

  return 0;
}

int32_t mndAllocVgroup(SMnode *pMnode, SDbObj *pDb, SVgObj **ppVgroups) {
  int32_t code = -1;
  SArray *pArray = NULL;
  SVgObj *pVgroups = NULL;

  pVgroups = calloc(pDb->cfg.numOfVgroups, sizeof(SVgObj));
  if (pVgroups == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    goto ALLOC_VGROUP_OVER;
  }

  pArray = mndBuildDnodesArray(pMnode);
  if (pArray == NULL) {
    goto ALLOC_VGROUP_OVER;
  }

  mDebug("db:%s, total %d dnodes used to create %d vgroups (%d vnodes)", pDb->name, (int32_t)taosArrayGetSize(pArray),
         pDb->cfg.numOfVgroups, pDb->cfg.numOfVgroups * pDb->cfg.replications);

  int32_t  allocedVgroups = 0;
  int32_t  maxVgId = sdbGetMaxId(pMnode->pSdb, SDB_VGROUP);
  uint32_t hashMin = 0;
  uint32_t hashMax = UINT32_MAX;
  uint32_t hashInterval = (hashMax - hashMin) / pDb->cfg.numOfVgroups;

  if (maxVgId < 2) maxVgId = 2;

  for (uint32_t v = 0; v < pDb->cfg.numOfVgroups; v++) {
    SVgObj *pVgroup = &pVgroups[v];
    pVgroup->vgId = maxVgId++;
    pVgroup->createdTime = taosGetTimestampMs();
    pVgroup->updateTime = pVgroups->createdTime;
    pVgroup->version = 1;
    pVgroup->hashBegin = hashMin + hashInterval * v;
    if (v == pDb->cfg.numOfVgroups - 1) {
      pVgroup->hashEnd = hashMax;
    } else {
      pVgroup->hashEnd = hashMin + hashInterval * (v + 1) - 1;
    }

    memcpy(pVgroup->dbName, pDb->name, TSDB_DB_FNAME_LEN);
    pVgroup->dbUid = pDb->uid;
    pVgroup->replica = pDb->cfg.replications;

    if (mndGetAvailableDnode(pMnode, pVgroup, pArray) != 0) {
      terrno = TSDB_CODE_MND_NO_ENOUGH_DNODES;
      goto ALLOC_VGROUP_OVER;
    }

    allocedVgroups++;
  }

  *ppVgroups = pVgroups;
  code = 0;

  mDebug("db:%s, %d vgroups is alloced, replica:%d", pDb->name, pDb->cfg.numOfVgroups, pDb->cfg.replications);

ALLOC_VGROUP_OVER:
  if (code != 0) free(pVgroups);
  taosArrayDestroy(pArray);
  return code;
}

SEpSet mndGetVgroupEpset(SMnode *pMnode, SVgObj *pVgroup) {
  SEpSet epset = {0};

  for (int32_t v = 0; v < pVgroup->replica; ++v) {
    SVnodeGid *pVgid = &pVgroup->vnodeGid[v];
    SDnodeObj *pDnode = mndAcquireDnode(pMnode, pVgid->dnodeId);
    if (pDnode == NULL) continue;

    if (pVgid->role == TAOS_SYNC_STATE_LEADER) {
      epset.inUse = epset.numOfEps;
    }

    addEpIntoEpSet(&epset, pDnode->fqdn, pDnode->port);
    mndReleaseDnode(pMnode, pDnode);
  }

  return epset;
}

static int32_t mndProcessCreateVnodeRsp(SMnodeMsg *pRsp) {
  mndTransProcessRsp(pRsp);
  return 0;
}

static int32_t mndProcessAlterVnodeRsp(SMnodeMsg *pRsp) {
  mndTransProcessRsp(pRsp);
  return 0;
}

static int32_t mndProcessDropVnodeRsp(SMnodeMsg *pRsp) {
  mndTransProcessRsp(pRsp);
  return 0;
}

static int32_t mndProcessSyncVnodeRsp(SMnodeMsg *pRsp) { return 0; }

static int32_t mndProcessCompactVnodeRsp(SMnodeMsg *pRsp) { return 0; }

static bool mndGetVgroupMaxReplicaFp(SMnode *pMnode, void *pObj, void *p1, void *p2, void *p3) {
  SVgObj  *pVgroup = pObj;
  int64_t  uid = *(int64_t *)p1;
  int8_t  *pReplica = p2;
  int32_t *pNumOfVgroups = p3;

  if (pVgroup->dbUid == uid) {
    *pReplica = TMAX(*pReplica, pVgroup->replica);
    (*pNumOfVgroups)++;
  }

  return true;
}

static int32_t mndGetVgroupMaxReplica(SMnode *pMnode, char *dbName, int8_t *pReplica, int32_t *pNumOfVgroups) {
  SSdb   *pSdb = pMnode->pSdb;
  SDbObj *pDb = mndAcquireDb(pMnode, dbName);
  if (pDb == NULL) {
    terrno = TSDB_CODE_MND_DB_NOT_SELECTED;
    return -1;
  }

  *pReplica = 1;
  *pNumOfVgroups = 0;
  sdbTraverse(pSdb, SDB_VGROUP, mndGetVgroupMaxReplicaFp, &pDb->uid, pReplica, pNumOfVgroups);
  mndReleaseDb(pMnode, pDb);
  return 0;
}

static int32_t mndGetVgroupMeta(SMnodeMsg *pReq, SShowObj *pShow, STableMetaRsp *pMeta) {
  SMnode *pMnode = pReq->pMnode;
  SSdb   *pSdb = pMnode->pSdb;

  if (mndGetVgroupMaxReplica(pMnode, pShow->db, &pShow->replica, &pShow->numOfRows) != 0) {
    return -1;
  }

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->pSchema;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "vgId");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "tables");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  for (int32_t i = 0; i < pShow->replica; ++i) {
    pShow->bytes[cols] = 2;
    pSchema[cols].type = TSDB_DATA_TYPE_SMALLINT;
    snprintf(pSchema[cols].name, TSDB_COL_NAME_LEN, "v%d_dnode", i + 1);
    pSchema[cols].bytes = htonl(pShow->bytes[cols]);
    cols++;

    pShow->bytes[cols] = 9 + VARSTR_HEADER_SIZE;
    pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
    snprintf(pSchema[cols].name, TSDB_COL_NAME_LEN, "v%d_status", i + 1);
    pSchema[cols].bytes = htonl(pShow->bytes[cols]);
    cols++;
  }

  pMeta->numOfColumns = htonl(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];
  strcpy(pMeta->tbFname, mndShowStr(pShow->type));

  return 0;
}

static int32_t mndRetrieveVgroups(SMnodeMsg *pReq, SShowObj *pShow, char *data, int32_t rows) {
  SMnode *pMnode = pReq->pMnode;
  SSdb   *pSdb = pMnode->pSdb;
  int32_t numOfRows = 0;
  SVgObj *pVgroup = NULL;
  int32_t cols = 0;
  char   *pWrite;

  SDbObj *pDb = mndAcquireDb(pMnode, pShow->db);
  if (pDb == NULL) return 0;

  while (numOfRows < rows) {
    pShow->pIter = sdbFetch(pSdb, SDB_VGROUP, pShow->pIter, (void **)&pVgroup);
    if (pShow->pIter == NULL) break;

    if (pVgroup->dbUid == pDb->uid) {
      cols = 0;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = pVgroup->vgId;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(int32_t *)pWrite = pVgroup->numOfTables;
      cols++;

      for (int32_t i = 0; i < pShow->replica; ++i) {
        pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
        *(int16_t *)pWrite = pVgroup->vnodeGid[i].dnodeId;
        cols++;

        const char *role = mndGetRoleStr(pVgroup->vnodeGid[i].role);
        pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
        STR_WITH_MAXSIZE_TO_VARSTR(pWrite, role, pShow->bytes[cols]);
        cols++;
      }
      numOfRows++;
    }

    sdbRelease(pSdb, pVgroup);
  }

  mndReleaseDb(pMnode, pDb);
  mndVacuumResult(data, pShow->numOfColumns, numOfRows, rows, pShow);
  pShow->numOfReads += numOfRows;
  return numOfRows;
}

static void mndCancelGetNextVgroup(SMnode *pMnode, void *pIter) {
  SSdb *pSdb = pMnode->pSdb;
  sdbCancelFetch(pSdb, pIter);
}

static bool mndGetVnodesNumFp(SMnode *pMnode, void *pObj, void *p1, void *p2, void *p3) {
  SVgObj  *pVgroup = pObj;
  int32_t  dnodeId = *(int32_t *)p1;
  int32_t *pNumOfVnodes = (int32_t *)p2;

  for (int32_t v = 0; v < pVgroup->replica; ++v) {
    if (pVgroup->vnodeGid[v].dnodeId == dnodeId) {
      (*pNumOfVnodes)++;
    }
  }

  return true;
}

int32_t mndGetVnodesNum(SMnode *pMnode, int32_t dnodeId) {
  int32_t numOfVnodes = 0;
  sdbTraverse(pMnode->pSdb, SDB_VGROUP, mndGetVnodesNumFp, &dnodeId, &numOfVnodes, NULL);
  return numOfVnodes;
}

static int32_t mndGetVnodeMeta(SMnodeMsg *pReq, SShowObj *pShow, STableMetaRsp *pMeta) {
  SMnode *pMnode = pReq->pMnode;
  SSdb   *pSdb = pMnode->pSdb;

  int32_t  cols = 0;
  SSchema *pSchema = pMeta->pSchema;

  pShow->bytes[cols] = 4;
  pSchema[cols].type = TSDB_DATA_TYPE_INT;
  strcpy(pSchema[cols].name, "vgId");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  pShow->bytes[cols] = 12 + VARSTR_HEADER_SIZE;
  pSchema[cols].type = TSDB_DATA_TYPE_BINARY;
  strcpy(pSchema[cols].name, "status");
  pSchema[cols].bytes = htonl(pShow->bytes[cols]);
  cols++;

  pMeta->numOfColumns = htonl(cols);
  pShow->numOfColumns = cols;

  pShow->offset[0] = 0;
  for (int32_t i = 1; i < cols; ++i) {
    pShow->offset[i] = pShow->offset[i - 1] + pShow->bytes[i - 1];
  }

  int32_t dnodeId = 0;
  if (pShow->payloadLen > 0) {
    dnodeId = atoi(pShow->payload);
  }

  pShow->replica = dnodeId;
  pShow->numOfRows = mndGetVnodesNum(pMnode, dnodeId);
  pShow->rowSize = pShow->offset[cols - 1] + pShow->bytes[cols - 1];
  strcpy(pMeta->tbFname, mndShowStr(pShow->type));

  return 0;
}

static int32_t mndRetrieveVnodes(SMnodeMsg *pReq, SShowObj *pShow, char *data, int32_t rows) {
  SMnode *pMnode = pReq->pMnode;
  SSdb   *pSdb = pMnode->pSdb;
  int32_t numOfRows = 0;
  SVgObj *pVgroup = NULL;
  char   *pWrite;
  int32_t cols = 0;
  int32_t dnodeId = pShow->replica;

  while (numOfRows < rows) {
    pShow->pIter = sdbFetch(pSdb, SDB_VGROUP, pShow->pIter, (void **)&pVgroup);
    if (pShow->pIter == NULL) break;

    for (int32_t i = 0; i < pVgroup->replica && numOfRows < rows; ++i) {
      SVnodeGid *pVgid = &pVgroup->vnodeGid[i];
      if (pVgid->dnodeId != dnodeId) continue;

      cols = 0;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      *(uint32_t *)pWrite = pVgroup->vgId;
      cols++;

      pWrite = data + pShow->offset[cols] * rows + pShow->bytes[cols] * numOfRows;
      STR_TO_VARSTR(pWrite, mndGetRoleStr(pVgid->role));
      cols++;
      numOfRows++;
    }

    sdbRelease(pSdb, pVgroup);
  }

  mndVacuumResult(data, pShow->numOfColumns, numOfRows, rows, pShow);
  pShow->numOfReads += numOfRows;
  return numOfRows;
}

static void mndCancelGetNextVnode(SMnode *pMnode, void *pIter) {
  SSdb *pSdb = pMnode->pSdb;
  sdbCancelFetch(pSdb, pIter);
}
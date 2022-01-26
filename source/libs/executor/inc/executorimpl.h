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
#ifndef TDENGINE_EXECUTORIMPL_H
#define TDENGINE_EXECUTORIMPL_H

#include "os.h"
#include "common.h"
#include "ttszip.h"
#include "tvariant.h"

#include "dataSinkMgt.h"
#include "executil.h"
#include "planner.h"
#include "taosdef.h"
#include "tarray.h"
#include "tfilter.h"
#include "thash.h"
#include "tlockfree.h"
#include "tpagedfile.h"
#include "executor.h"

struct SColumnFilterElem;

typedef int32_t (*__block_search_fn_t)(char* data, int32_t num, int64_t key, int32_t order);

#define IS_QUERY_KILLED(_q) ((_q)->code == TSDB_CODE_TSC_QUERY_CANCELLED)
#define Q_STATUS_EQUAL(p, s)  (((p) & (s)) != 0u)
#define QUERY_IS_ASC_QUERY(q) (GET_FORWARD_DIRECTION_FACTOR((q)->order.order) == QUERY_ASC_FORWARD_STEP)

#define GET_TABLEGROUP(q, _index)   ((SArray*) taosArrayGetP((q)->tableqinfoGroupInfo.pGroupList, (_index)))

#define GET_NUM_OF_RESULTS(_r) (((_r)->outputBuf) == NULL? 0:((_r)->outputBuf)->info.rows)

#define NEEDTO_COMPRESS_QUERY(size) ((size) > tsCompressColData? 1 : 0)

enum {
  // when this task starts to execute, this status will set
  TASK_NOT_COMPLETED = 0x1u,

  /* Task is over
   * 1. this status is used in one row result query process, e.g., count/sum/first/last/ avg...etc.
   * 2. when all data within queried time window, it is also denoted as query_completed
   */
  TASK_COMPLETED = 0x2u,

  /* when the result is not completed return to client, this status will be
   * usually used in case of interval query with interpolation option
   */
  TASK_OVER = 0x4u,
};

typedef struct SResultRowCell {
  uint64_t     groupId;
  SResultRow  *pRow;
} SResultRowCell;

/**
 * If the number of generated results is greater than this value,
 * query query will be halt and return results to client immediate.
 */
typedef struct SRspResultInfo {
  int64_t total;      // total generated result size in rows
  int32_t capacity;   // capacity of current result output buffer
  int32_t threshold;  // result size threshold in rows.
} SRspResultInfo;

typedef struct SColumnFilterElem {
  int16_t           bytes;  // column length
  __filter_func_t   fp;
  SColumnFilterInfo filterInfo;
  void              *q;
} SColumnFilterElem;

typedef struct SSingleColumnFilterInfo {
  void*              pData;
  void*              pData2;  //used for nchar column
  int32_t            numOfFilters;
  SColumnInfo        info;
  SColumnFilterElem* pFilters;
} SSingleColumnFilterInfo;

typedef struct STableQueryInfo {
  TSKEY       lastKey;
  int32_t     groupIndex;     // group id in table list
  SVariant    tag;
  STimeWindow win;            // todo remove it later
  STSCursor   cur;
  void*       pTable;         // for retrieve the page id list
  SResultRowInfo resInfo;
} STableQueryInfo;

typedef enum {
  QUERY_PROF_BEFORE_OPERATOR_EXEC = 0,
  QUERY_PROF_AFTER_OPERATOR_EXEC,
  QUERY_PROF_QUERY_ABORT
} EQueryProfEventType;

typedef struct {
  EQueryProfEventType eventType;
  int64_t eventTime;

  union {
    uint8_t operatorType; //for operator event
    int32_t abortCode; //for query abort event
  };
} SQueryProfEvent;

typedef struct {
  uint8_t operatorType;
  int64_t sumSelfTime;
  int64_t sumRunTimes;
} SOperatorProfResult;

typedef struct STaskCostInfo {
  int64_t   created;
  int64_t   start;
  int64_t   end;

  uint64_t  loadStatisTime;
  uint64_t  loadFileBlockTime;
  uint64_t  loadDataInCacheTime;
  uint64_t  loadStatisSize;
  uint64_t  loadFileBlockSize;
  uint64_t  loadDataInCacheSize;

  uint64_t  loadDataTime;
  uint64_t  totalRows;
  uint64_t  totalCheckedRows;
  uint32_t  totalBlocks;
  uint32_t  loadBlocks;
  uint32_t  loadBlockStatis;
  uint32_t  discardBlocks;
  uint64_t  elapsedTime;
  uint64_t  firstStageMergeTime;
  uint64_t  winInfoSize;
  uint64_t  tableInfoSize;
  uint64_t  hashSize;
  uint64_t  numOfTimeWindows;

  SArray   *queryProfEvents;  //SArray<SQueryProfEvent>
  SHashObj *operatorProfResults; //map<operator_type, SQueryProfEvent>
} STaskCostInfo;

typedef struct {
  int64_t vgroupLimit;
  int64_t ts;
} SOrderedPrjQueryInfo;

typedef struct {
  char*   tags;
  SArray* pResult;  // SArray<SStddevInterResult>
} SInterResult;

// The basic query information extracted from the SQueryInfo tree to support the
// execution of query in a data node.
typedef struct STaskAttr {
  SLimit           limit;
  SLimit           slimit;

  // todo comment it
  bool             stableQuery;      // super table query or not
  bool             topBotQuery;      // TODO used bitwise flag
  bool             groupbyColumn;    // denote if this is a groupby normal column query
  bool             hasTagResults;    // if there are tag values in final result or not
  bool             timeWindowInterpo;// if the time window start/end required interpolation
  bool             queryBlockDist;    // if query data block distribution
  bool             stabledev;        // super table stddev query
  bool             tsCompQuery;      // is tscomp query
  bool             diffQuery;        // is diff query
  bool             simpleAgg;
  bool             pointInterpQuery; // point interpolation query
  bool             needReverseScan;  // need reverse scan
  bool             distinct;         // distinct  query or not
  bool             stateWindow;       // window State on sub/normal table
  bool             createFilterOperator; // if filter operator is needed
  bool             multigroupResult; // multigroup result can exist in one SSDataBlock
  int32_t          interBufSize;     // intermediate buffer sizse

  int32_t          havingNum;        // having expr number

  SOrder           order;
  int16_t          numOfCols;
  int16_t          numOfTags;

  STimeWindow      window;
  SInterval        interval;
  SSessionWindow   sw;
  int16_t          precision;
  int16_t          numOfOutput;
  int16_t          fillType;

  int32_t          srcRowSize;       // todo extract struct
  int32_t          resultRowSize;
  int32_t          intermediateResultRowSize; // intermediate result row size, in case of top-k query.
  int32_t          maxTableColumnWidth;
  int32_t          tagLen;           // tag value length of current query
  SGroupbyExpr    *pGroupbyExpr;

  SExprInfo*       pExpr1;
  SExprInfo*       pExpr2;
  int32_t          numOfExpr2;
  SExprInfo*       pExpr3;
  int32_t          numOfExpr3;

  SColumnInfo*     tableCols;
  SColumnInfo*     tagColList;
  int32_t          numOfFilterCols;
  int64_t*         fillVal;
  SOrderedPrjQueryInfo prjInfo;        // limit value for each vgroup, only available in global order projection query.

  SSingleColumnFilterInfo* pFilterInfo;
//  SFilterInfo     *pFilters;
  
  void*            tsdb;
//  SMemRef          memRef;
  STableGroupInfo  tableGroupInfo;       // table <tid, last_key> list  SArray<STableKeyInfo>
  int32_t          vgId;
  SArray          *pUdfInfo;             // no need to free
} STaskAttr;

typedef SSDataBlock* (*__operator_fn_t)(void* param, bool* newgroup);
typedef void (*__optr_cleanup_fn_t)(void* param, int32_t num);

struct SOperatorInfo;

typedef struct STaskIdInfo {
  uint64_t       queryId;    // this is also a request id
  uint64_t       subplanId;
  uint64_t       templateId;
  char          *str;
} STaskIdInfo;

typedef struct SExecTaskInfo {
  STaskIdInfo     id;
  char           *content;
  uint32_t        status;
  STimeWindow     window;
  STaskCostInfo   cost;
  int64_t         owner;       // if it is in execution
  int32_t         code;
  uint64_t        totalRows;   // total number of rows
  STableGroupInfo tableqinfoGroupInfo;  // this is a group array list, including SArray<STableQueryInfo*> structure
  char           *sql;         // query sql string
  jmp_buf         env;         //
  struct SOperatorInfo  *pRoot;
} SExecTaskInfo;

typedef struct STaskRuntimeEnv {
  jmp_buf               env;
  STaskAttr*            pQueryAttr;
  uint32_t              status;           // query status
  void*                 qinfo;
  uint8_t               scanFlag;         // denotes reversed scan of data or not
  void*                 pTsdbReadHandle;

  int32_t               prevGroupId;      // previous executed group id
  bool                  enableGroupData;
  SDiskbasedResultBuf*  pResultBuf;       // query result buffer based on blocked-wised disk file
  SHashObj*             pResultRowHashTable; // quick locate the window object for each result
  SHashObj*             pResultRowListSet;   // used to check if current ResultRowInfo has ResultRow object or not
  SArray*               pResultRowArrayList; // The array list that contains the Result rows
  char*                 keyBuf;           // window key buffer
  SResultRowPool*       pool;             // The window result objects pool, all the resultRow Objects are allocated and managed by this object.
  char**                prevRow;

  SArray*               prevResult;       // intermediate result, SArray<SInterResult>
  STSBuf*               pTsBuf;           // timestamp filter list
  STSCursor             cur;

  char*                 tagVal;           // tag value of current data block
  struct SScalarFunctionSupport   * scalarSup;

  SSDataBlock          *outputBuf;
  STableGroupInfo       tableqinfoGroupInfo;  // this is a group array list, including SArray<STableQueryInfo*> structure
  struct SOperatorInfo *proot;
  SGroupResInfo         groupResInfo;
  int64_t               currentOffset;   // dynamic offset value

  STableQueryInfo      *current;
  SRspResultInfo        resultInfo;
  SHashObj             *pTableRetrieveTsMap;
  struct SUdfInfo      *pUdfInfo;
} STaskRuntimeEnv;

enum {
  OP_IN_EXECUTING   = 1,
  OP_RES_TO_RETURN  = 2,
  OP_EXEC_DONE      = 3,
};

typedef struct SOperatorInfo {
  uint8_t               operatorType;
  bool                  blockingOptr;  // block operator or not
  uint8_t               status;        // denote if current operator is completed
  int32_t               numOfOutput;   // number of columns of the current operator results
  char                 *name;          // name, used to show the query execution plan
  void                 *info;          // extension attribution
  SExprInfo            *pExpr;
  STaskRuntimeEnv      *pRuntimeEnv;   // todo remove it
  SExecTaskInfo        *pTaskInfo;

  struct SOperatorInfo **pDownstream;  // downstram pointer list
  int32_t               numOfDownstream; // number of downstream. The value is always ONE expect for join operator
  __operator_fn_t       exec;
  __optr_cleanup_fn_t   cleanup;
} SOperatorInfo;

enum {
  QUERY_RESULT_NOT_READY = 1,
  QUERY_RESULT_READY     = 2,
};

typedef struct {
  int32_t      numOfTags;
  int32_t      numOfCols;
  SColumnInfo *colList;
} SQueriedTableInfo;

typedef struct SQInfo {
  void*            signature;
  uint64_t         qId;
  int32_t          code;        // error code to returned to client
  int64_t          owner;       // if it is in execution

  STaskRuntimeEnv runtimeEnv;
  STaskAttr       query;
  void*            pBuf;        // allocated buffer for STableQueryInfo, sizeof(STableQueryInfo)*numOfTables;

  pthread_mutex_t  lock;        // used to synchronize the rsp/query threads
  tsem_t           ready;
  int32_t          dataReady;   // denote if query result is ready or not
  void*            rspContext;  // response context
  int64_t          startExecTs; // start to exec timestamp
  char*            sql;         // query sql string
  STaskCostInfo    summary;
} SQInfo;

typedef struct STaskParam {
  char            *sql;
  char            *tagCond;
  char            *colCond;
  char            *tbnameCond;
  char            *prevResult;
  SArray          *pTableIdList;
  SSqlExpr       **pExpr;
  SSqlExpr       **pSecExpr;
  SExprInfo       *pExprs;
  SExprInfo       *pSecExprs;

  SFilterInfo     *pFilters;

  SColIndex       *pGroupColIndex;
  SColumnInfo     *pTagColumnInfo;
  SGroupbyExpr *pGroupbyExpr;
  int32_t          tableScanOperator;
  SArray          *pOperator;
  struct SUdfInfo        *pUdfInfo;
} STaskParam;

typedef struct SExchangeInfo {
  SArray            *pSources;
  tsem_t             ready;
  void              *pTransporter;
  SRetrieveTableRsp *pRsp;
  SSDataBlock       *pResult;
  int32_t            current;
  uint64_t           rowsOfCurrentSource;

  uint64_t           totalSize;   // total load bytes from remote
  uint64_t           totalRows;   // total number of rows
  uint64_t           totalElapsed;// total elapsed time
} SExchangeInfo;

typedef struct STableScanInfo {
  void           *pTsdbReadHandle;
  int32_t         numOfBlocks;     // extract basic running information.
  int32_t         numOfSkipped;
  int32_t         numOfBlockStatis;
  int64_t         numOfRows;
                 
  int32_t         order;        // scan order
  int32_t         times;        // repeat counts
  int32_t         current;
  int32_t         reverseTimes; // 0 by default

  SQLFunctionCtx *pCtx;         // next operator query context
  SResultRowInfo *pResultRowInfo;
  int32_t        *rowCellInfoOffset;
  SExprInfo      *pExpr;
  SSDataBlock     block;
  int32_t         numOfOutput;
  int64_t         elapsedTime;
  int32_t         prevGroupId;  // previous table group id

  int32_t         scanFlag;     // table scan flag to denote if it is a repeat/reverse/main scan
} STableScanInfo;

typedef struct STagScanInfo {
  SColumnInfo* pCols;
  SSDataBlock* pRes;
  int32_t      totalTables;
  int32_t      curPos;
} STagScanInfo;

typedef struct SStreamBlockScanInfo {
  SSDataBlock *pRes;        // result SSDataBlock
  SColumnInfo *pCols;       // the output column info
  uint64_t     numOfRows;   // total scanned rows
  uint64_t     numOfExec;   // execution times
  void        *readerHandle;// stream block reader handle
} SStreamBlockScanInfo;

typedef struct SOptrBasicInfo {
  SResultRowInfo    resultRowInfo;
  int32_t          *rowCellInfoOffset;  // offset value for each row result cell info
  SQLFunctionCtx   *pCtx;
  SSDataBlock      *pRes;
} SOptrBasicInfo;

typedef struct SOptrBasicInfo STableIntervalOperatorInfo;

typedef struct SAggOperatorInfo {
  SOptrBasicInfo        binfo;
  uint32_t              seed;
  SDiskbasedResultBuf  *pResultBuf;       // query result buffer based on blocked-wised disk file
  SHashObj*             pResultRowHashTable; // quick locate the window object for each result
  SHashObj*             pResultRowListSet;   // used to check if current ResultRowInfo has ResultRow object or not
  SArray*               pResultRowArrayList; // The array list that contains the Result rows
  char*                 keyBuf;           // window key buffer
  SResultRowPool*       pool;             // The window result objects pool, all the resultRow Objects are allocated and managed by this object.
  STableQueryInfo      *current;
} SAggOperatorInfo;

typedef struct SProjectOperatorInfo {
  SOptrBasicInfo binfo;
  int32_t        bufCapacity;
  uint32_t       seed;

  SSDataBlock   *existDataBlock;
} SProjectOperatorInfo;

typedef struct SLimitOperatorInfo {
  int64_t   limit;
  int64_t   total;
} SLimitOperatorInfo;

typedef struct SSLimitOperatorInfo {
  int64_t      groupTotal;
  int64_t      currentGroupOffset;

  int64_t      rowsTotal;
  int64_t      currentOffset;
  SLimit       limit;
  SLimit       slimit;

  char       **prevRow;
  SArray      *orderColumnList;
  bool         hasPrev;
  bool         ignoreCurrentGroup;
  bool         multigroupResult;
  SSDataBlock *pRes;   // result buffer
  SSDataBlock *pPrevBlock;
  int64_t      capacity;
  int64_t      threshold;
} SSLimitOperatorInfo;

typedef struct SFilterOperatorInfo {
  SSingleColumnFilterInfo *pFilterInfo;
  int32_t numOfFilterCols;
} SFilterOperatorInfo;

typedef struct SFillOperatorInfo {
  struct SFillInfo   *pFillInfo;
  SSDataBlock *pRes;
  int64_t      totalInputRows;
  void       **p;
  SSDataBlock *existNewGroupBlock;
  bool         multigroupResult;
} SFillOperatorInfo;

typedef struct SGroupbyOperatorInfo {
  SOptrBasicInfo binfo;
  int32_t        colIndex;
  char          *prevData;   // previous group by value
} SGroupbyOperatorInfo;

typedef struct SSWindowOperatorInfo {
  SOptrBasicInfo binfo;
  STimeWindow    curWindow;  // current time window
  TSKEY          prevTs;     // previous timestamp
  int32_t        numOfRows;  // number of rows
  int32_t        start;      // start row index
  bool           reptScan;    // next round scan
} SSWindowOperatorInfo;

typedef struct SStateWindowOperatorInfo {
  SOptrBasicInfo binfo;
  STimeWindow    curWindow;  // current time window
  int32_t        numOfRows;  // number of rows
  int32_t        colIndex;      // start row index
  int32_t        start;
  char*          prevData;    // previous data 
  bool           reptScan;
} SStateWindowOperatorInfo;

typedef struct SDistinctDataInfo {
  int32_t index;
  int32_t type;
  int32_t bytes;
} SDistinctDataInfo; 

typedef struct SDistinctOperatorInfo {
  SHashObj         *pSet;
  SSDataBlock      *pRes;
  bool              recordNullVal;  //has already record the null value, no need to try again
  int64_t           threshold;
  int64_t           outputCapacity;
  int32_t           totalBytes; 
  char*             buf;
  SArray*           pDistinctDataInfo; 
} SDistinctOperatorInfo;

struct SGlobalMerger;

typedef struct SMultiwayMergeInfo {
  struct SGlobalMerger *pMerge;
  SOptrBasicInfo       binfo;
  int32_t              bufCapacity;
  int64_t              seed;
  char               **prevRow;
  SArray              *orderColumnList;
  int32_t              resultRowFactor;

  bool                 hasGroupColData;
  char               **currentGroupColData;
  SArray              *groupColumnList;
  bool                 hasDataBlockForNewGroup;
  SSDataBlock         *pExistBlock;

  SArray              *udfInfo;
  bool                 hasPrev;
  bool                 multiGroupResults;
} SMultiwayMergeInfo;

// todo support the disk-based sort
typedef struct SOrderOperatorInfo {
  int32_t      colIndex;
  int32_t      order;
  SSDataBlock *pDataBlock;
} SOrderOperatorInfo;

SOperatorInfo* createExchangeOperatorInfo(const SArray* pSources, const SArray* pSchema, SExecTaskInfo* pTaskInfo);

SOperatorInfo* createDataBlocksOptScanInfo(void* pTsdbReadHandle, int32_t order, int32_t numOfOutput, int32_t repeatTime, int32_t reverseTime, SExecTaskInfo* pTaskInfo);
SOperatorInfo* createTableScanOperatorInfo(void* pTsdbReadHandle, int32_t order, int32_t numOfOutput, int32_t repeatTime, SExecTaskInfo* pTaskInfo);
SOperatorInfo* createTableSeqScanOperator(void* pTsdbReadHandle, STaskRuntimeEnv* pRuntimeEnv);
SOperatorInfo* createSubmitBlockScanOperatorInfo(void *pSubmitBlockReadHandle, int32_t numOfOutput, SExecTaskInfo* pTaskInfo);

SOperatorInfo* createAggregateOperatorInfo(SOperatorInfo* downstream, SArray* pExprInfo, SExecTaskInfo* pTaskInfo);
SOperatorInfo* createProjectOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createLimitOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream);
SOperatorInfo* createTimeIntervalOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createAllTimeIntervalOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createSWindowOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createFillOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput, bool multigroupResult);
SOperatorInfo* createGroupbyOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createMultiTableAggOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createMultiTableTimeIntervalOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createAllMultiTableTimeIntervalOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createTagScanOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createDistinctOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createTableBlockInfoScanOperator(void* pTsdbReadHandle, STaskRuntimeEnv* pRuntimeEnv);
SOperatorInfo* createMultiwaySortOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SExprInfo* pExpr, int32_t numOfOutput,
                                              int32_t numOfRows, void* merger);
SOperatorInfo* createGlobalAggregateOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput, void* param, SArray* pUdfInfo, bool groupResultMixedUp);
SOperatorInfo* createStatewindowOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput);
SOperatorInfo* createSLimitOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput, void* merger, bool multigroupResult);
SOperatorInfo* createFilterOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr,
                                        int32_t numOfOutput, SColumnInfo* pCols, int32_t numOfFilter);

SOperatorInfo* createJoinOperatorInfo(SOperatorInfo** pdownstream, int32_t numOfDownstream, SSchema* pSchema, int32_t numOfOutput);
SOperatorInfo* createOrderOperatorInfo(STaskRuntimeEnv* pRuntimeEnv, SOperatorInfo* downstream, SExprInfo* pExpr, int32_t numOfOutput, SOrder* pOrderVal);

//SSDataBlock* doGlobalAggregate(void* param, bool* newgroup);
//SSDataBlock* doMultiwayMergeSort(void* param, bool* newgroup);
//SSDataBlock* doSLimit(void* param, bool* newgroup);

//int32_t doCreateFilterInfo(SColumnInfo* pCols, int32_t numOfCols, int32_t numOfFilterCols, SSingleColumnFilterInfo** pFilterInfo, uint64_t qId);
void doSetFilterColumnInfo(SSingleColumnFilterInfo* pFilterInfo, int32_t numOfFilterCols, SSDataBlock* pBlock);
bool doFilterDataBlock(SSingleColumnFilterInfo* pFilterInfo, int32_t numOfFilterCols, int32_t numOfRows, int8_t* p);
void doCompactSDataBlock(SSDataBlock* pBlock, int32_t numOfRows, int8_t* p);

SSDataBlock* createOutputBuf(SExprInfo* pExpr, int32_t numOfOutput, int32_t numOfRows);

void* destroyOutputBuf(SSDataBlock* pBlock);
void* doDestroyFilterInfo(SSingleColumnFilterInfo* pFilterInfo, int32_t numOfFilterCols);

void setInputDataBlock(SOperatorInfo* pOperator, SQLFunctionCtx* pCtx, SSDataBlock* pBlock, int32_t order);
void finalizeQueryResult(SOperatorInfo* pOperator, SQLFunctionCtx* pCtx, SResultRowInfo* pResultRowInfo, int32_t* rowCellInfoOffset);
void updateOutputBuf(SOptrBasicInfo* pBInfo, int32_t *bufCapacity, int32_t numOfInputRows);
void clearOutputBuf(SOptrBasicInfo* pBInfo, int32_t *bufCapacity);
void copyTsColoum(SSDataBlock* pRes, SQLFunctionCtx* pCtx, int32_t numOfOutput);

void freeParam(STaskParam *param);
int32_t createQueryFunc(SQueriedTableInfo* pTableInfo, int32_t numOfOutput, SExprInfo** pExprInfo,
                        SSqlExpr** pExprMsg, SColumnInfo* pTagCols, int32_t queryType, void* pMsg, struct SUdfInfo* pUdfInfo);

int32_t createIndirectQueryFuncExprFromMsg(SQueryTableReq *pQueryMsg, int32_t numOfOutput, SExprInfo **pExprInfo,
                                           SSqlExpr **pExpr, SExprInfo *prevExpr, struct SUdfInfo *pUdfInfo);

int32_t createQueryFilter(char *data, uint16_t len, SFilterInfo** pFilters);

SGroupbyExpr *createGroupbyExprFromMsg(SQueryTableReq *pQueryMsg, SColIndex *pColIndex, int32_t *code);

int32_t initQInfo(STsBufInfo* pTsBufInfo, void* tsdb, void* sourceOptr, SQInfo* pQInfo, STaskParam* param, char* start,
                  int32_t prevResultLen, void* merger);

int32_t createFilterInfo(STaskAttr* pQueryAttr, uint64_t qId);
void freeColumnFilterInfo(SColumnFilterInfo* pFilter, int32_t numOfFilters);

STableQueryInfo *createTableQueryInfo(STaskAttr* pQueryAttr, void* pTable, bool groupbyColumn, STimeWindow win, void* buf);
STableQueryInfo* createTmpTableQueryInfo(STimeWindow win);

int32_t buildArithmeticExprFromMsg(SExprInfo *pArithExprInfo, void *pQueryMsg);

bool isTaskKilled(SExecTaskInfo *pTaskInfo);
int32_t checkForQueryBuf(size_t numOfTables);
bool checkNeedToCompressQueryCol(SQInfo *pQInfo);
void setQueryStatus(STaskRuntimeEnv *pRuntimeEnv, int8_t status);

bool onlyQueryTags(STaskAttr* pQueryAttr);
//void destroyUdfInfo(struct SUdfInfo* pUdfInfo);

int32_t doDumpQueryResult(SQInfo *pQInfo, char *data, int8_t compressed, int32_t *compLen);

size_t getResultSize(SQInfo *pQInfo, int64_t *numOfRows);
void setTaskKilled(SExecTaskInfo *pTaskInfo);

void publishOperatorProfEvent(SOperatorInfo* operatorInfo, EQueryProfEventType eventType);
void publishQueryAbortEvent(SExecTaskInfo * pTaskInfo, int32_t code);

void calculateOperatorProfResults(SQInfo* pQInfo);
void queryCostStatis(SExecTaskInfo *pTaskInfo);

void doDestroyTask(SExecTaskInfo *pTaskInfo);
void freeQueryAttr(STaskAttr *pQuery);

int32_t getMaximumIdleDurationSec();

void doInvokeUdf(struct SUdfInfo* pUdfInfo, SQLFunctionCtx *pCtx, int32_t idx, int32_t type);
void setTaskStatus(SExecTaskInfo *pTaskInfo, int8_t status);
int32_t createExecTaskInfoImpl(SSubplan* pPlan, SExecTaskInfo** pTaskInfo, void* readerHandle, uint64_t taskId);

#endif  // TDENGINE_EXECUTORIMPL_H

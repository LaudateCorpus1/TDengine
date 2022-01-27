#include "os.h"
#include "tref.h"
#include "trpc.h"
#include "clientInt.h"
#include "clientLog.h"
#include "query.h"
#include "tmsg.h"
#include "tglobal.h"
#include "catalog.h"

#define TSC_VAR_NOT_RELEASE 1
#define TSC_VAR_RELEASED    0

static int32_t sentinel = TSC_VAR_NOT_RELEASE;

int taos_options(TSDB_OPTION option, const void *arg, ...) {
  static int32_t lock = 0;

  for (int i = 1; atomic_val_compare_exchange_32(&lock, 0, 1) != 0; ++i) {
    if (i % 1000 == 0) {
      tscInfo("haven't acquire lock after spin %d times.", i);
      sched_yield();
    }
  }

  int ret = taos_options_imp(option, (const char*)arg);
  atomic_store_32(&lock, 0);
  return ret;
}

// this function may be called by user or system, or by both simultaneously.
void taos_cleanup(void) {
  tscInfo("start to cleanup client environment");

  if (atomic_val_compare_exchange_32(&sentinel, TSC_VAR_NOT_RELEASE, TSC_VAR_RELEASED) != TSC_VAR_NOT_RELEASE) {
    return;
  }

  int32_t id = clientReqRefPool;
  clientReqRefPool = -1;
  taosCloseRef(id);

  cleanupTaskQueue();

  id = clientConnRefPool;
  clientConnRefPool = -1;
  taosCloseRef(id);

  hbMgrCleanUp();

  rpcCleanup();
  catalogDestroy();
  taosCloseLog();

  tscInfo("all local resources released");
}

TAOS *taos_connect(const char *ip, const char *user, const char *pass, const char *db, uint16_t port) {
  int32_t p = (port != 0) ? port : tsServerPort;

  tscDebug("try to connect to %s:%u, user:%s db:%s", ip, p, user, db);
  if (user == NULL) {
    user = TSDB_DEFAULT_USER;
  }

  if (pass == NULL) {
    pass = TSDB_DEFAULT_PASS;
  }

  return taos_connect_internal(ip, user, pass, NULL, db, p);
}

void taos_close(TAOS* taos) {
  if (taos == NULL) {
    return;
  }

  STscObj *pTscObj = (STscObj *)taos;
  tscDebug("0x%"PRIx64" try to close connection, numOfReq:%d", pTscObj->id, pTscObj->numOfReqs);

  taosRemoveRef(clientConnRefPool, pTscObj->id);
}

int taos_errno(TAOS_RES *tres) {
  if (tres == NULL) {
    return terrno;
  }

  return ((SRequestObj*) tres)->code;
}

const char *taos_errstr(TAOS_RES *res) {
  SRequestObj *pRequest = (SRequestObj *) res;

  if (pRequest == NULL) {
    return (const char*) tstrerror(terrno);
  }

  if (strlen(pRequest->msgBuf) > 0 || pRequest->code == TSDB_CODE_RPC_FQDN_ERROR) {
    return pRequest->msgBuf;
  } else {
    return (const char*)tstrerror(pRequest->code);
  }
}

void taos_free_result(TAOS_RES *res) {
  SRequestObj* pRequest = (SRequestObj*) res;
  destroyRequest(pRequest);
}

int  taos_field_count(TAOS_RES *res) {
  if (res == NULL) {
    return 0;
  }

  SRequestObj* pRequest = (SRequestObj*) res;
  SReqResultInfo* pResInfo = &pRequest->body.resInfo;
  return pResInfo->numOfCols;
}

int  taos_num_fields(TAOS_RES *res) {
  return taos_field_count(res);
}

TAOS_FIELD *taos_fetch_fields(TAOS_RES *res) {
  if (taos_num_fields(res) == 0) {
    return NULL;
  }

  SReqResultInfo* pResInfo = &(((SRequestObj*) res)->body.resInfo);
  return pResInfo->fields;
}

TAOS_RES *taos_query(TAOS *taos, const char *sql) {
  if (taos == NULL || sql == NULL) {
    return NULL;
  }

  return taos_query_l(taos, sql, (int32_t) strlen(sql));
}

TAOS_ROW taos_fetch_row(TAOS_RES *pRes) {
  if (pRes == NULL) {
    return NULL;
  }

  SRequestObj *pRequest = (SRequestObj *) pRes;
  if (pRequest->type == TSDB_SQL_RETRIEVE_EMPTY_RESULT ||
      pRequest->type == TSDB_SQL_INSERT ||
      pRequest->code != TSDB_CODE_SUCCESS ||
      taos_num_fields(pRes) == 0) {
    return NULL;
  }

  return doFetchRow(pRequest);
}

int  taos_print_row(char *str, TAOS_ROW row, TAOS_FIELD *fields, int num_fields) {
  int32_t len = 0;
  for (int i = 0; i < num_fields; ++i) {
    if (i > 0) {
      str[len++] = ' ';
    }

    if (row[i] == NULL) {
      len += sprintf(str + len, "%s", TSDB_DATA_NULL_STR);
      continue;
    }

    switch (fields[i].type) {
      case TSDB_DATA_TYPE_TINYINT:
        len += sprintf(str + len, "%d", *((int8_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_UTINYINT:
        len += sprintf(str + len, "%u", *((uint8_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_SMALLINT:
        len += sprintf(str + len, "%d", *((int16_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_USMALLINT:
        len += sprintf(str + len, "%u", *((uint16_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_INT:
        len += sprintf(str + len, "%d", *((int32_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_UINT:
        len += sprintf(str + len, "%u", *((uint32_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_BIGINT:
        len += sprintf(str + len, "%" PRId64, *((int64_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_UBIGINT:
        len += sprintf(str + len, "%" PRIu64, *((uint64_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_FLOAT: {
        float fv = 0;
        fv = GET_FLOAT_VAL(row[i]);
        len += sprintf(str + len, "%f", fv);
      } break;

      case TSDB_DATA_TYPE_DOUBLE: {
        double dv = 0;
        dv = GET_DOUBLE_VAL(row[i]);
        len += sprintf(str + len, "%lf", dv);
      } break;

      case TSDB_DATA_TYPE_BINARY:
      case TSDB_DATA_TYPE_NCHAR: {
        int32_t charLen = varDataLen((char*)row[i] - VARSTR_HEADER_SIZE);
        if (fields[i].type == TSDB_DATA_TYPE_BINARY) {
          assert(charLen <= fields[i].bytes && charLen >= 0);
        } else {
          assert(charLen <= fields[i].bytes * TSDB_NCHAR_SIZE && charLen >= 0);
        }

        memcpy(str + len, row[i], charLen);
        len += charLen;
      } break;

      case TSDB_DATA_TYPE_TIMESTAMP:
        len += sprintf(str + len, "%" PRId64, *((int64_t *)row[i]));
        break;

      case TSDB_DATA_TYPE_BOOL:
        len += sprintf(str + len, "%d", *((int8_t *)row[i]));
      default:
        break;
    }
  }

  return len;
}

int* taos_fetch_lengths(TAOS_RES *res) {
  if (res == NULL) {
    return NULL;
  }

  return ((SRequestObj*) res)->body.resInfo.length;
}

const char *taos_data_type(int type) {
  switch (type) {
    case TSDB_DATA_TYPE_NULL:            return "TSDB_DATA_TYPE_NULL";
    case TSDB_DATA_TYPE_BOOL:            return "TSDB_DATA_TYPE_BOOL";
    case TSDB_DATA_TYPE_TINYINT:         return "TSDB_DATA_TYPE_TINYINT";
    case TSDB_DATA_TYPE_SMALLINT:        return "TSDB_DATA_TYPE_SMALLINT";
    case TSDB_DATA_TYPE_INT:             return "TSDB_DATA_TYPE_INT";
    case TSDB_DATA_TYPE_BIGINT:          return "TSDB_DATA_TYPE_BIGINT";
    case TSDB_DATA_TYPE_FLOAT:           return "TSDB_DATA_TYPE_FLOAT";
    case TSDB_DATA_TYPE_DOUBLE:          return "TSDB_DATA_TYPE_DOUBLE";
    case TSDB_DATA_TYPE_BINARY:          return "TSDB_DATA_TYPE_BINARY";
    case TSDB_DATA_TYPE_TIMESTAMP:       return "TSDB_DATA_TYPE_TIMESTAMP";
    case TSDB_DATA_TYPE_NCHAR:           return "TSDB_DATA_TYPE_NCHAR";
    default: return "UNKNOWN";
  }
}

const char *taos_get_client_info() { return version; }

int taos_affected_rows(TAOS_RES *res) {
  if (res == NULL) {
    return 0;
  }

  SRequestObj* pRequest = (SRequestObj*) res;
  SReqResultInfo* pResInfo = &pRequest->body.resInfo;
  return pResInfo->numOfRows;
}

int taos_result_precision(TAOS_RES *res) { return TSDB_TIME_PRECISION_MILLI; }

int taos_select_db(TAOS *taos, const char *db) {
  STscObj *pObj = (STscObj *)taos;
  if (pObj == NULL) {
    terrno = TSDB_CODE_TSC_DISCONNECTED;
    return TSDB_CODE_TSC_DISCONNECTED;
  }

  if (db == NULL || strlen(db) == 0) {
    terrno = TSDB_CODE_TSC_INVALID_INPUT;
    return terrno;
  }

  char sql[256] = {0};
  snprintf(sql, tListLen(sql), "use %s", db);

  TAOS_RES* pRequest = taos_query(taos, sql);
  int32_t code = taos_errno(pRequest);

  taos_free_result(pRequest);
  return code;
}

void taos_stop_query(TAOS_RES *res) {
  if (res == NULL) {
    return;
  }

  SRequestObj* pRequest = (SRequestObj*) res;
  int32_t numOfFields = taos_num_fields(pRequest);

  // It is not a query, no need to stop.
  if (numOfFields == 0) {
    return;
  }

//  scheduleCancelJob(pRequest->body.pQueryJob);
}

bool taos_is_null(TAOS_RES *res, int32_t row, int32_t col) {
  return false;
}

int  taos_fetch_block(TAOS_RES *res, TAOS_ROW *rows) {
  return 0;
}

int  taos_validate_sql(TAOS *taos, const char *sql) {
  return true;
}

const char *taos_get_server_info(TAOS *taos) {
  if (taos == NULL) {
    return NULL;
  }

  STscObj* pTscObj = (STscObj*) taos;
  return pTscObj->ver;
}

void taos_query_a(TAOS *taos, const char *sql, __taos_async_fn_t fp, void *param) {
  // TODO
}

void taos_fetch_rows_a(TAOS_RES *res, __taos_async_fn_t fp, void *param) {
  // TODO
}
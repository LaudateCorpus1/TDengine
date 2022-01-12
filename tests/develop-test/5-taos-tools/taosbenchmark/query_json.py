###################################################################
#           Copyright (c) 2016 by TAOS Technologies, Inc.
#                     All rights reserved.
#
#  This file is proprietary and confidential to TAOS Technologies.
#  No part of this file may be reproduced, stored, transmitted,
#  disclosed or used in any form or by any means other than as
#  expressly provided by the written permission from Jianhui Tao
#
###################################################################

# -*- coding: utf-8 -*-
import os
from util.log import *
from util.cases import *
from util.sql import *
from util.dnodes import *


class TDTestCase:
    def caseDescription(self):
        '''
        [TD-11510] taosBenchmark test cases
        '''
        return

    def init(self, conn, logSql):
        tdLog.debug("start to execute %s" % __file__)
        tdSql.init(conn.cursor(), logSql)

    def run(self):
        tdSql.execute("create database if not exists db")
        tdSql.execute("use db")
        tdSql.execute("create table stb (ts timestamp, c0 int)  tags (t0 int)")
        tdSql.execute("insert into stb_0 using stb tags (0) values (now, 0)")
        tdSql.execute("insert into stb_1 using stb tags (1) values (now, 1)")
        tdSql.execute("insert into stb_2 using stb tags (2) values (now, 2)")
        cmd = "taosBenchmark -f ./5-taos-tools/taosbenchmark/json/taosc_query.json"
        tdLog.info("%s" % cmd)
        os.system("%s" % cmd)
        with open("%s" % "taosc_query_specified-0", 'r+') as f1:
            for line in f1.readlines():
                queryTaosc = line.strip().split()[0]
                assert queryTaosc == '3' , "result is %s != expect: 3" % queryTaosc

        with open("%s" % "taosc_query_super-0", 'r+') as f1:
            for line in f1.readlines():
                queryTaosc = line.strip().split()[0]
                assert queryTaosc == '1', "result is %s != expect: 1" % queryTaosc

    def stop(self):
        tdSql.close()
        tdLog.success("%s successfully executed" % __file__)


tdCases.addWindows(__file__, TDTestCase())
tdCases.addLinux(__file__, TDTestCase())
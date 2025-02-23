/*
 * Copyright(c) 2019 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#include "mpt/mpt.h"
#include "procs/ppud.h"


/*
 * Checks whether participant user_data QoS changes work.
 */
#define TEST_A_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, true, 10)
#define TEST_B_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, false, 0)
MPT_TestProcess(qos, ppuserdata, a, ppud, TEST_A_ARGS);
MPT_TestProcess(qos, ppuserdata, b, ppud, TEST_B_ARGS);
MPT_Test(qos, ppuserdata, .init=ppud_init, .fini=ppud_fini);
#undef TEST_A_ARGS
#undef TEST_B_ARGS

/*
 * Checks whether reader/writer user_data QoS changes work.
 */
#define TEST_A_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, "rwuserdata", true, 10)
#define TEST_B_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, "rwuserdata", false, 0)
MPT_TestProcess(qos, rwuserdata, a, rwud, TEST_A_ARGS);
MPT_TestProcess(qos, rwuserdata, b, rwud, TEST_B_ARGS);
MPT_Test(qos, rwuserdata, .init=ppud_init, .fini=ppud_fini);
#undef TEST_A_ARGS
#undef TEST_B_ARGS

/*
 * Checks whether topic_data QoS changes become visible in reader/writer.
 */
#define TEST_A_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, "rwtopicdata", true, 10)
#define TEST_B_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, "rwtopicdata", false, 0)
MPT_TestProcess(qos, rwtopicdata, a, rwtd, TEST_A_ARGS);
MPT_TestProcess(qos, rwtopicdata, b, rwtd, TEST_B_ARGS);
MPT_Test(qos, rwtopicdata, .init=ppud_init, .fini=ppud_fini);
#undef TEST_A_ARGS
#undef TEST_B_ARGS

/*
 * Checks whether group_data QoS changes become visible in reader/writer.
 */
#define TEST_A_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, "rwgroupdata", true, 10)
#define TEST_B_ARGS MPT_ArgValues(DDS_DOMAIN_DEFAULT, "rwgroupdata", false, 0)
MPT_TestProcess(qos, rwgroupdata, a, rwgd, TEST_A_ARGS);
MPT_TestProcess(qos, rwgroupdata, b, rwgd, TEST_B_ARGS);
MPT_Test(qos, rwgroupdata, .init=ppud_init, .fini=ppud_fini);
#undef TEST_A_ARGS
#undef TEST_B_ARGS

/*
 * Copyright(c) 2006 to 2019 ADLINK Technology Limited and others
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v. 2.0 which is available at
 * http://www.eclipse.org/legal/epl-2.0, or the Eclipse Distribution License
 * v. 1.0 which is available at
 * http://www.eclipse.org/org/documents/edl-v10.php.
 *
 * SPDX-License-Identifier: EPL-2.0 OR BSD-3-Clause
 */
#ifndef DDSI_THREADMON_H
#define DDSI_THREADMON_H

#if defined (__cplusplus)
extern "C" {
#endif

struct ddsi_threadmon;

struct ddsi_threadmon *ddsi_threadmon_new (void);
dds_return_t ddsi_threadmon_start (struct ddsi_threadmon *sl);
void ddsi_threadmon_stop (struct ddsi_threadmon *sl);
void ddsi_threadmon_free (struct ddsi_threadmon *sl);
void ddsi_threadmon_statechange_barrier (struct ddsi_threadmon *sl);

#if defined (__cplusplus)
}
#endif

#endif /* DDSI_THREADMON_H */

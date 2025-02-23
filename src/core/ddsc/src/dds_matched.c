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
#include <assert.h>
#include <string.h>

#include "dds/dds.h"
#include "dds/version.h"
#include "dds/ddsi/q_config.h"
#include "dds/ddsi/q_globals.h"
#include "dds/ddsi/q_entity.h"
#include "dds/ddsi/q_thread.h"
#include "dds/ddsi/q_bswap.h"
#include "dds__writer.h"
#include "dds__reader.h"
#include "dds__topic.h"

dds_return_t dds_get_matched_subscriptions (dds_entity_t writer, dds_instance_handle_t *rds, size_t nrds)
{
  dds_writer *wr;
  dds_return_t rc;
  if (rds == NULL && nrds > 0)
    return DDS_RETCODE_BAD_PARAMETER;
  if ((rc = dds_writer_lock (writer, &wr)) != DDS_RETCODE_OK)
    return rc;
  else
  {
    const int32_t nrds_max = (nrds > INT32_MAX) ? INT32_MAX : (int32_t) nrds;
    int32_t nrds_act = 0;
    ddsrt_avl_iter_t it;
    /* FIXME: this ought not be so tightly coupled to the lower layer */
    thread_state_awake (lookup_thread_state ());
    ddsrt_mutex_lock (&wr->m_wr->e.lock);
    for (const struct wr_prd_match *m = ddsrt_avl_iter_first (&wr_readers_treedef, &wr->m_wr->readers, &it);
         m != NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct proxy_reader *prd;
      if ((prd = ephash_lookup_proxy_reader_guid (&m->prd_guid)) != NULL)
      {
        if (nrds_act < nrds_max)
          rds[nrds_act] = prd->e.iid;
        nrds_act++;
      }
    }
    for (const struct wr_rd_match *m = ddsrt_avl_iter_first (&wr_local_readers_treedef, &wr->m_wr->local_readers, &it);
         m != NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct reader *rd;
      if ((rd = ephash_lookup_reader_guid (&m->rd_guid)) != NULL)
      {
        if (nrds_act < nrds_max)
          rds[nrds_act] = rd->e.iid;
        nrds_act++;
      }
    }
    ddsrt_mutex_unlock (&wr->m_wr->e.lock);
    thread_state_asleep (lookup_thread_state ());
    dds_writer_unlock (wr);
    return nrds_act;
  }
}

dds_return_t dds_get_matched_publications (dds_entity_t reader, dds_instance_handle_t *wrs, size_t nwrs)
{
  dds_reader *rd;
  dds_return_t rc;
  if (wrs == NULL && nwrs > 0)
    return DDS_RETCODE_BAD_PARAMETER;
  if ((rc = dds_reader_lock (reader, &rd)) != DDS_RETCODE_OK)
    return rc;
  else
  {
    const int32_t nwrs_max = (nwrs > INT32_MAX) ? INT32_MAX : (int32_t) nwrs;
    int32_t nwrs_act = 0;
    ddsrt_avl_iter_t it;
    /* FIXME: this ought not be so tightly coupled to the lower layer */
    thread_state_awake (lookup_thread_state ());
    ddsrt_mutex_lock (&rd->m_rd->e.lock);
    for (const struct rd_pwr_match *m = ddsrt_avl_iter_first (&rd_writers_treedef, &rd->m_rd->writers, &it);
         m != NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct proxy_writer *pwr;
      if ((pwr = ephash_lookup_proxy_writer_guid (&m->pwr_guid)) != NULL)
      {
        if (nwrs_act < nwrs_max)
          wrs[nwrs_act] = pwr->e.iid;
        nwrs_act++;
      }
    }
    for (const struct rd_wr_match *m = ddsrt_avl_iter_first (&rd_local_writers_treedef, &rd->m_rd->local_writers, &it);
         m != NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct writer *wr;
      if ((wr = ephash_lookup_writer_guid (&m->wr_guid)) != NULL)
      {
        if (nwrs_act < nwrs_max)
          wrs[nwrs_act] = wr->e.iid;
        nwrs_act++;
      }
    }
    ddsrt_mutex_unlock (&rd->m_rd->e.lock);
    thread_state_asleep (lookup_thread_state ());
    dds_reader_unlock (rd);
    return nwrs_act;
  }
}

static dds_builtintopic_endpoint_t *make_builtintopic_endpoint (const nn_guid_t *guid, const nn_guid_t *ppguid, dds_instance_handle_t ppiid, const dds_qos_t *qos)
{
  dds_builtintopic_endpoint_t *ep;
  nn_guid_t tmp;
  ep = dds_alloc (sizeof (*ep));
  tmp = nn_hton_guid (*guid);
  memcpy (&ep->key, &tmp, sizeof (ep->key));
  ep->participant_instance_handle = ppiid;
  tmp = nn_hton_guid (*ppguid);
  memcpy (&ep->participant_key, &tmp, sizeof (ep->participant_key));
  ep->qos = dds_create_qos ();
  nn_xqos_mergein_missing (ep->qos, qos, ~(QP_TOPIC_NAME | QP_TYPE_NAME));
  ep->topic_name = dds_string_dup (qos->topic_name);
  ep->type_name = dds_string_dup (qos->type_name);
  return ep;
}

dds_builtintopic_endpoint_t *dds_get_matched_subscription_data (dds_entity_t writer, dds_instance_handle_t ih)
{
  dds_writer *wr;
  dds_return_t rc;
  if ((rc = dds_writer_lock (writer, &wr)) != DDS_RETCODE_OK)
    return NULL;
  else
  {
    dds_builtintopic_endpoint_t *ret = NULL;
    ddsrt_avl_iter_t it;
    /* FIXME: this ought not be so tightly coupled to the lower layer, and not be so inefficient besides */
    thread_state_awake (lookup_thread_state ());
    ddsrt_mutex_lock (&wr->m_wr->e.lock);
    for (const struct wr_prd_match *m = ddsrt_avl_iter_first (&wr_readers_treedef, &wr->m_wr->readers, &it);
         m != NULL && ret == NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct proxy_reader *prd;
      if ((prd = ephash_lookup_proxy_reader_guid (&m->prd_guid)) != NULL)
      {
        if (prd->e.iid == ih)
          ret = make_builtintopic_endpoint (&prd->e.guid, &prd->c.proxypp->e.guid, prd->c.proxypp->e.iid, prd->c.xqos);
      }
    }
    for (const struct wr_rd_match *m = ddsrt_avl_iter_first (&wr_local_readers_treedef, &wr->m_wr->local_readers, &it);
         m != NULL && ret == NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct reader *rd;
      if ((rd = ephash_lookup_reader_guid (&m->rd_guid)) != NULL)
      {
        if (rd->e.iid == ih)
          ret = make_builtintopic_endpoint (&rd->e.guid, &rd->c.pp->e.guid, rd->c.pp->e.iid, rd->xqos);
      }
    }
    ddsrt_mutex_unlock (&wr->m_wr->e.lock);
    thread_state_asleep (lookup_thread_state ());
    dds_writer_unlock (wr);
    return ret;
  }
}

dds_builtintopic_endpoint_t *dds_get_matched_publication_data (dds_entity_t reader, dds_instance_handle_t ih)
{
  dds_reader *rd;
  dds_return_t rc;
  if ((rc = dds_reader_lock (reader, &rd)) != DDS_RETCODE_OK)
    return NULL;
  else
  {
    dds_builtintopic_endpoint_t *ret = NULL;
    ddsrt_avl_iter_t it;
    /* FIXME: this ought not be so tightly coupled to the lower layer, and not be so inefficient besides */
    thread_state_awake (lookup_thread_state ());
    ddsrt_mutex_lock (&rd->m_rd->e.lock);
    for (const struct rd_pwr_match *m = ddsrt_avl_iter_first (&rd_writers_treedef, &rd->m_rd->writers, &it);
         m != NULL && ret == NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct proxy_writer *pwr;
      if ((pwr = ephash_lookup_proxy_writer_guid (&m->pwr_guid)) != NULL)
      {
        if (pwr->e.iid == ih)
          ret = make_builtintopic_endpoint (&pwr->e.guid, &pwr->c.proxypp->e.guid, pwr->c.proxypp->e.iid, pwr->c.xqos);
      }
    }
    for (const struct rd_wr_match *m = ddsrt_avl_iter_first (&rd_local_writers_treedef, &rd->m_rd->local_writers, &it);
         m != NULL && ret == NULL;
         m = ddsrt_avl_iter_next (&it))
    {
      struct writer *wr;
      if ((wr = ephash_lookup_writer_guid (&m->wr_guid)) != NULL)
      {
        if (wr->e.iid == ih)
          ret = make_builtintopic_endpoint (&wr->e.guid, &wr->c.pp->e.guid, wr->c.pp->e.iid, wr->xqos);
      }
    }
    ddsrt_mutex_unlock (&rd->m_rd->e.lock);
    thread_state_asleep (lookup_thread_state ());
    dds_reader_unlock (rd);
    return ret;
  }
}

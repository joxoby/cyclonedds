/*
 * Copyright(c) 2006 to 2018 ADLINK Technology Limited and others
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
#include "dds__reader.h"
#include "dds__readcond.h"
#include "dds__rhc.h"
#include "dds__entity.h"
#include "dds/ddsi/q_ephash.h"
#include "dds/ddsi/q_entity.h"
#include "dds/ddsi/q_thread.h"

static dds_return_t dds_readcond_delete (dds_entity *e) ddsrt_nonnull_all;

static dds_return_t dds_readcond_delete (dds_entity *e)
{
  dds_rhc_remove_readcondition ((dds_readcond *) e);
  return DDS_RETCODE_OK;
}

dds_readcond *dds_create_readcond (dds_reader *rd, dds_entity_kind_t kind, uint32_t mask, dds_querycondition_filter_fn filter)
{
  dds_readcond *cond = dds_alloc (sizeof (*cond));
  assert ((kind == DDS_KIND_COND_READ && filter == 0) || (kind == DDS_KIND_COND_QUERY && filter != 0));
  (void) dds_entity_init (&cond->m_entity, &rd->m_entity, kind, NULL, NULL, 0);
  cond->m_entity.m_deriver.delete = dds_readcond_delete;
  cond->m_rhc = rd->m_rd->rhc;
  cond->m_sample_states = mask & DDS_ANY_SAMPLE_STATE;
  cond->m_view_states = mask & DDS_ANY_VIEW_STATE;
  cond->m_instance_states = mask & DDS_ANY_INSTANCE_STATE;
  cond->m_rd_guid = rd->m_entity.m_guid;
  if (kind == DDS_KIND_COND_QUERY)
  {
    cond->m_query.m_filter = filter;
    cond->m_query.m_qcmask = 0;
  }
  if (!dds_rhc_add_readcondition (cond))
  {
    /* FIXME: current entity management code can't deal with an error late in the creation of the
       entity because it doesn't allow deleting it again ... instead use a hack to signal a problem
       to the caller and let that one handle it. */
    cond->m_entity.m_deriver.delete = 0;
  }
  return cond;
}

dds_entity_t dds_create_readcondition (dds_entity_t reader, uint32_t mask)
{
  dds_reader *rd;
  dds_return_t rc;
  if ((rc = dds_reader_lock (reader, &rd)) != DDS_RETCODE_OK)
    return rc;
  else
  {
    dds_entity_t hdl;
    dds_readcond *cond = dds_create_readcond(rd, DDS_KIND_COND_READ, mask, 0);
    assert (cond);
    assert (cond->m_entity.m_deriver.delete);
    hdl = cond->m_entity.m_hdllink.hdl;
    dds_reader_unlock (rd);
    return hdl;
  }
}

dds_entity_t dds_get_datareader (dds_entity_t condition)
{
  struct dds_entity *e;
  dds_return_t rc;
  if ((rc = dds_entity_claim (condition, &e)) != DDS_RETCODE_OK)
    return rc;
  else
  {
    dds_entity_t rdh;
    switch (dds_entity_kind (e))
    {
      case DDS_KIND_COND_READ:
      case DDS_KIND_COND_QUERY:
        assert (dds_entity_kind (e->m_parent) == DDS_KIND_READER);
        rdh = e->m_parent->m_hdllink.hdl;
        break;
      default:
        rdh = DDS_RETCODE_ILLEGAL_OPERATION;
        break;
    }
    dds_entity_release (e);
    return rdh;
  }
}

dds_return_t dds_get_mask (dds_entity_t condition, uint32_t *mask)
{
  dds_entity *entity;
  dds_return_t rc;

  if (mask == NULL)
    return DDS_RETCODE_BAD_PARAMETER;

  if ((rc = dds_entity_lock (condition, DDS_KIND_DONTCARE, &entity)) != DDS_RETCODE_OK)
    return rc;
  else if (dds_entity_kind (entity) != DDS_KIND_COND_READ && dds_entity_kind (entity) != DDS_KIND_COND_QUERY)
  {
    dds_entity_unlock (entity);
    return DDS_RETCODE_ILLEGAL_OPERATION;
  }
  else
  {
    dds_readcond *cond = (dds_readcond *) entity;
    *mask = (cond->m_sample_states | cond->m_view_states | cond->m_instance_states);
    dds_entity_unlock (entity);
    return DDS_RETCODE_OK;
  }
}

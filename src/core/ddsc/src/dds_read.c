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
#include <string.h>
#include "dds__entity.h"
#include "dds__reader.h"
#include "dds/ddsi/ddsi_tkmap.h"
#include "dds__rhc.h"
#include "dds/ddsi/q_thread.h"
#include "dds/ddsi/q_ephash.h"
#include "dds/ddsi/q_entity.h"
#include "dds/ddsi/q_globals.h"
#include "dds/ddsi/ddsi_sertopic.h"

static dds_return_t dds_read_lock (dds_entity_t hdl, dds_reader **reader, dds_readcond **condition, bool only_reader)
{
  dds_return_t rc;
  dds_entity *entity, *parent_entity;
  if ((rc = dds_entity_lock (hdl, DDS_KIND_DONTCARE, &entity)) != DDS_RETCODE_OK)
  {
    return rc;
  }
  else if (dds_entity_kind (entity) == DDS_KIND_READER)
  {
    *reader = (dds_reader *) entity;
    *condition = NULL;
    return DDS_RETCODE_OK;
  }
  else if (only_reader)
  {
    dds_entity_unlock (entity);
    return DDS_RETCODE_ILLEGAL_OPERATION;
  }
  else if (dds_entity_kind (entity) != DDS_KIND_COND_READ && dds_entity_kind (entity) != DDS_KIND_COND_QUERY)
  {
    dds_entity_unlock (entity);
    return DDS_RETCODE_ILLEGAL_OPERATION;
  }
  else if ((rc = dds_entity_lock (entity->m_parent->m_hdllink.hdl, DDS_KIND_READER, &parent_entity)) != DDS_RETCODE_OK)
  {
    dds_entity_unlock (entity);
    return rc;
  }
  else
  {
    *reader = (dds_reader *) parent_entity;
    *condition = (dds_readcond *) entity;
    return DDS_RETCODE_OK;
  }
}

static void dds_read_unlock (dds_reader *reader, dds_readcond *condition)
{
  dds_entity_unlock (&reader->m_entity);
  if (condition)
    dds_entity_unlock (&condition->m_entity);
}

/*
  dds_read_impl: Core read/take function. Usually maxs is size of buf and si
  into which samples/status are written, when set to zero is special case
  indicating that size set from number of samples in cache and also that cache
  has been locked. This is used to support C++ API reading length unlimited
  which is interpreted as "all relevant samples in cache".
*/
static dds_return_t dds_read_impl (bool take, dds_entity_t reader_or_condition, void **buf, size_t bufsz, uint32_t maxs, dds_sample_info_t *si, uint32_t mask, dds_instance_handle_t hand, bool lock, bool only_reader)
{
  struct thread_state1 * const ts1 = lookup_thread_state ();
  dds_return_t ret = DDS_RETCODE_OK;
  struct dds_reader *rd;
  struct dds_readcond *cond;
  unsigned nodata_cleanups = 0;
#define NC_CLEAR_LOAN_OUT 1u
#define NC_FREE_BUF 2u
#define NC_RESET_BUF 4u

  if (buf == NULL || si == NULL || maxs == 0 || bufsz == 0 || bufsz < maxs)
    return DDS_RETCODE_BAD_PARAMETER;

  thread_state_awake (ts1);
  if ((ret = dds_read_lock (reader_or_condition, &rd, &cond, only_reader)) != DDS_RETCODE_OK)
    goto fail_awake;

  if (hand != DDS_HANDLE_NIL)
  {
    if (ddsi_tkmap_find_by_id (gv.m_tkmap, hand) == NULL) {
      ret = DDS_RETCODE_PRECONDITION_NOT_MET;
      goto fail_awake_lock;
    }
  }

  /* Allocate samples if not provided (assuming all or none provided) */
  if (buf[0] == NULL)
  {
    /* Allocate, use or reallocate loan cached on reader */
    if (rd->m_loan_out)
    {
      ddsi_sertopic_realloc_samples (buf, rd->m_topic->m_stopic, NULL, 0, maxs);
      nodata_cleanups = NC_FREE_BUF | NC_RESET_BUF;
    }
    else
    {
      if (rd->m_loan)
      {
        if (rd->m_loan_size >= maxs)
          buf[0] = rd->m_loan;
        else
        {
          ddsi_sertopic_realloc_samples (buf, rd->m_topic->m_stopic, rd->m_loan, rd->m_loan_size, maxs);
          rd->m_loan = buf[0];
          rd->m_loan_size = maxs;
        }
        nodata_cleanups = NC_RESET_BUF;
      }
      else
      {
        ddsi_sertopic_realloc_samples (buf, rd->m_topic->m_stopic, NULL, 0, maxs);
        rd->m_loan = buf[0];
        rd->m_loan_size = maxs;
      }
      rd->m_loan_out = true;
      nodata_cleanups |= NC_CLEAR_LOAN_OUT;
    }
  }

  /* read/take resets data available status -- must reset before reading because
     the actual writing is protected by RHC lock, not by rd->m_entity.m_lock */
  ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
  dds_entity_status_reset (&rd->m_entity, DDS_DATA_AVAILABLE_STATUS);
  /* reset DATA_ON_READERS status on subscriber after successful read/take */
  if (dds_entity_kind (rd->m_entity.m_parent) == DDS_KIND_SUBSCRIBER)
    dds_entity_status_reset (rd->m_entity.m_parent, DDS_DATA_ON_READERS_STATUS);
  ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);

  if (take)
    ret = dds_rhc_take (rd->m_rd->rhc, lock, buf, si, maxs, mask, hand, cond);
  else
    ret = dds_rhc_read (rd->m_rd->rhc, lock, buf, si, maxs, mask, hand, cond);

  /* if no data read, restore the state to what it was before the call, with the sole
     exception of holding on to a buffer we just allocated and that is pointed to by
     rd->m_loan */
  if (ret <= 0 && nodata_cleanups)
  {
    if (nodata_cleanups & NC_CLEAR_LOAN_OUT)
      rd->m_loan_out = false;
    if (nodata_cleanups & NC_FREE_BUF)
      ddsi_sertopic_free_samples (rd->m_topic->m_stopic, buf[0], maxs, DDS_FREE_ALL);
    if (nodata_cleanups & NC_RESET_BUF)
      buf[0] = NULL;
  }
  dds_read_unlock(rd, cond);
  thread_state_asleep (ts1);
  return ret;

#undef NC_CLEAR_LOAN_OUT
#undef NC_FREE_BUF
#undef NC_RESET_BUF

fail_awake_lock:
  dds_read_unlock (rd, cond);
fail_awake:
  thread_state_asleep (ts1);
  return ret;
}

static dds_return_t dds_readcdr_impl (bool take, dds_entity_t reader_or_condition, struct ddsi_serdata **buf, uint32_t maxs, dds_sample_info_t *si, uint32_t mask, dds_instance_handle_t hand, bool lock)
{
  struct thread_state1 * const ts1 = lookup_thread_state ();
  dds_return_t ret = DDS_RETCODE_OK;
  struct dds_reader *rd;
  struct dds_readcond *cond;

  assert (take);
  assert (buf);
  assert (si);
  assert (hand == DDS_HANDLE_NIL);
  assert (maxs > 0);
  (void)take;

  thread_state_awake (ts1);
  if ((ret = dds_read_lock (reader_or_condition, &rd, &cond, false)) == DDS_RETCODE_OK)
  {
    /* read/take resets data available status -- must reset before reading because
       the actual writing is protected by RHC lock, not by rd->m_entity.m_lock */
    ddsrt_mutex_lock (&rd->m_entity.m_observers_lock);
    dds_entity_status_reset (&rd->m_entity, DDS_DATA_AVAILABLE_STATUS);
    /* reset DATA_ON_READERS status on subscriber after successful read/take */
    if (dds_entity_kind (rd->m_entity.m_parent) == DDS_KIND_SUBSCRIBER)
      dds_entity_status_reset (rd->m_entity.m_parent, DDS_DATA_ON_READERS_STATUS);
    ddsrt_mutex_unlock (&rd->m_entity.m_observers_lock);

    ret = dds_rhc_takecdr (rd->m_rd->rhc, lock, buf, si, maxs, mask & DDS_ANY_SAMPLE_STATE, mask & DDS_ANY_VIEW_STATE, mask & DDS_ANY_INSTANCE_STATE, hand);
    dds_read_unlock (rd, cond);
  }
  thread_state_asleep (ts1);
  return ret;
}

dds_return_t dds_read (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = (uint32_t) bufsz;
  }
  return dds_read_impl (false, rd_or_cnd, buf, bufsz, maxs, si, NO_STATE_MASK_SET, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_read_wl (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, uint32_t maxs)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (false, rd_or_cnd, buf, maxs, maxs, si, NO_STATE_MASK_SET, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_read_mask (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs, uint32_t mask)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = (uint32_t)bufsz;
  }
  return dds_read_impl (false, rd_or_cnd, buf, bufsz, maxs, si, mask, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_read_mask_wl (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, uint32_t maxs, uint32_t mask)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (false, rd_or_cnd, buf, maxs, maxs, si, mask, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_read_instance (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs, dds_instance_handle_t handle)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (false, rd_or_cnd, buf, bufsz, maxs, si, NO_STATE_MASK_SET, handle, lock, false);
}

dds_return_t dds_read_instance_wl (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, uint32_t maxs, dds_instance_handle_t handle)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (false, rd_or_cnd, buf, maxs, maxs, si, NO_STATE_MASK_SET, handle, lock, false);
}

dds_return_t dds_read_instance_mask (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs, dds_instance_handle_t handle, uint32_t mask)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (false, rd_or_cnd, buf, bufsz, maxs, si, mask, handle, lock, false);
}

dds_return_t dds_read_instance_mask_wl (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, uint32_t maxs, dds_instance_handle_t handle, uint32_t mask)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (false, rd_or_cnd, buf, maxs, maxs, si, mask, handle, lock, false);
}

dds_return_t dds_read_next (dds_entity_t reader, void **buf, dds_sample_info_t *si)
{
  uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
  return dds_read_impl (false, reader, buf, 1u, 1u, si, mask, DDS_HANDLE_NIL, true, true);
}

dds_return_t dds_read_next_wl (
                 dds_entity_t reader,
                 void **buf,
                 dds_sample_info_t *si)
{
  uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
  return dds_read_impl (false, reader, buf, 1u, 1u, si, mask, DDS_HANDLE_NIL, true, true);
}

dds_return_t dds_take (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = (uint32_t)bufsz;
  }
  return dds_read_impl (true, rd_or_cnd, buf, bufsz, maxs, si, NO_STATE_MASK_SET, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_take_wl (dds_entity_t rd_or_cnd, void ** buf, dds_sample_info_t * si, uint32_t maxs)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (true, rd_or_cnd, buf, maxs, maxs, si, NO_STATE_MASK_SET, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_take_mask (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs, uint32_t mask)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = (uint32_t) bufsz;
  }
  return dds_read_impl (true, rd_or_cnd, buf, bufsz, maxs, si, mask, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_take_mask_wl (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, uint32_t maxs, uint32_t mask)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl (true, rd_or_cnd, buf, maxs, maxs, si, mask, DDS_HANDLE_NIL, lock, false);
}

dds_return_t dds_takecdr (dds_entity_t rd_or_cnd, struct ddsi_serdata **buf, uint32_t maxs, dds_sample_info_t *si, uint32_t mask)
{
  bool lock = true;
  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_readcdr_impl (true, rd_or_cnd, buf, maxs, si, mask, DDS_HANDLE_NIL, lock);
}

dds_return_t dds_take_instance (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs, dds_instance_handle_t handle)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl(true, rd_or_cnd, buf, bufsz, maxs, si, NO_STATE_MASK_SET, handle, lock, false);
}

dds_return_t dds_take_instance_wl (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, uint32_t maxs, dds_instance_handle_t handle)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl(true, rd_or_cnd, buf, maxs, maxs, si, NO_STATE_MASK_SET, handle, lock, false);
}

dds_return_t dds_take_instance_mask (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, size_t bufsz, uint32_t maxs, dds_instance_handle_t handle, uint32_t mask)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl(true, rd_or_cnd, buf, bufsz, maxs, si, mask, handle, lock, false);
}

dds_return_t dds_take_instance_mask_wl (dds_entity_t rd_or_cnd, void **buf, dds_sample_info_t *si, uint32_t maxs, dds_instance_handle_t handle, uint32_t mask)
{
  bool lock = true;

  if (handle == DDS_HANDLE_NIL)
    return DDS_RETCODE_PRECONDITION_NOT_MET;

  if (maxs == DDS_READ_WITHOUT_LOCK)
  {
    lock = false;
    /* FIXME: Fix the interface. */
    maxs = 100;
  }
  return dds_read_impl(true, rd_or_cnd, buf, maxs, maxs, si, mask, handle, lock, false);
}

dds_return_t dds_take_next (dds_entity_t reader, void **buf, dds_sample_info_t *si)
{
  uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
  return dds_read_impl (true, reader, buf, 1u, 1u, si, mask, DDS_HANDLE_NIL, true, true);
}

dds_return_t dds_take_next_wl (dds_entity_t reader, void **buf, dds_sample_info_t *si)
{
  uint32_t mask = DDS_NOT_READ_SAMPLE_STATE | DDS_ANY_VIEW_STATE | DDS_ANY_INSTANCE_STATE;
  return dds_read_impl (true, reader, buf, 1u, 1u, si, mask, DDS_HANDLE_NIL, true, true);
}

dds_return_t dds_return_loan (dds_entity_t reader_or_condition, void **buf, int32_t bufsz)
{
  const struct ddsi_sertopic *st;
  dds_reader *rd;
  dds_readcond *cond;
  dds_return_t ret = DDS_RETCODE_OK;

  if (buf == NULL || (*buf == NULL && bufsz > 0))
    return DDS_RETCODE_BAD_PARAMETER;

  if ((ret = dds_read_lock(reader_or_condition, &rd, &cond, false)) != DDS_RETCODE_OK)
    return ret;

  st = rd->m_topic->m_stopic;
  for (int32_t i = 0; i < bufsz; i++)
    ddsi_sertopic_free_sample (st, buf[i], DDS_FREE_CONTENTS);

  /* If possible return loan buffer to reader */
  if (rd->m_loan != 0 && (buf[0] == rd->m_loan))
  {
    rd->m_loan_out = false;
    ddsi_sertopic_zero_samples (st, rd->m_loan, rd->m_loan_size);
    buf[0] = NULL;
  }

  dds_read_unlock(rd, cond);
  return DDS_RETCODE_OK;
}

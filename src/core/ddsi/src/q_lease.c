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
#include <ctype.h>
#include <stddef.h>
#include <assert.h>

#include "dds/ddsrt/heap.h"
#include "dds/ddsrt/sync.h"

#include "dds/ddsrt/fibheap.h"

#include "dds/ddsi/ddsi_serdata_default.h"
#include "dds/ddsi/q_protocol.h"
#include "dds/ddsi/q_rtps.h"
#include "dds/ddsi/q_misc.h"
#include "dds/ddsi/q_config.h"
#include "dds/ddsi/q_log.h"
#include "dds/ddsi/q_plist.h"
#include "dds/ddsi/q_unused.h"
#include "dds/ddsi/q_xevent.h"
#include "dds/ddsi/q_addrset.h"
#include "dds/ddsi/q_ddsi_discovery.h"
#include "dds/ddsi/q_radmin.h"
#include "dds/ddsi/q_ephash.h"
#include "dds/ddsi/q_entity.h"
#include "dds/ddsi/q_globals.h"
#include "dds/ddsi/q_xmsg.h"
#include "dds/ddsi/q_bswap.h"
#include "dds/ddsi/q_transmit.h"
#include "dds/ddsi/q_lease.h"
#include "dds/ddsi/q_gc.h"

/* This is absolute bottom for signed integers, where -x = x and yet x
   != 0 -- and note that it had better be 2's complement machine! */
#define TSCHED_NOT_ON_HEAP INT64_MIN

struct lease {
  ddsrt_fibheap_node_t heapnode;
  nn_etime_t tsched;  /* access guarded by leaseheap_lock */
  nn_etime_t tend;    /* access guarded by lock_lease/unlock_lease */
  dds_duration_t tdur;      /* constant (renew depends on it) */
  struct entity_common *entity; /* constant */
};

static int compare_lease_tsched (const void *va, const void *vb);

static const ddsrt_fibheap_def_t lease_fhdef = DDSRT_FIBHEAPDEF_INITIALIZER(offsetof (struct lease, heapnode), compare_lease_tsched);

static void force_lease_check (void)
{
  gcreq_enqueue(gcreq_new(gv.gcreq_queue, gcreq_free));
}

static int compare_lease_tsched (const void *va, const void *vb)
{
  const struct lease *a = va;
  const struct lease *b = vb;
  return (a->tsched.v == b->tsched.v) ? 0 : (a->tsched.v < b->tsched.v) ? -1 : 1;
}

void lease_management_init (void)
{
  int i;
  ddsrt_mutex_init (&gv.leaseheap_lock);
  for (i = 0; i < N_LEASE_LOCKS; i++)
    ddsrt_mutex_init (&gv.lease_locks[i]);
  ddsrt_fibheap_init (&lease_fhdef, &gv.leaseheap);
}

void lease_management_term (void)
{
  int i;
  assert (ddsrt_fibheap_min (&lease_fhdef, &gv.leaseheap) == NULL);
  for (i = 0; i < N_LEASE_LOCKS; i++)
    ddsrt_mutex_destroy (&gv.lease_locks[i]);
  ddsrt_mutex_destroy (&gv.leaseheap_lock);
}

static ddsrt_mutex_t *lock_lease_addr (struct lease const * const l)
{
  uint32_t u = (uint16_t) ((uintptr_t) l >> 3);
  uint32_t v = u * 0xb4817365;
  uint32_t idx = v >> (32 - N_LEASE_LOCKS_LG2);
  return &gv.lease_locks[idx];
}

static void lock_lease (const struct lease *l)
{
  ddsrt_mutex_lock (lock_lease_addr (l));
}

static void unlock_lease (const struct lease *l)
{
  ddsrt_mutex_unlock (lock_lease_addr (l));
}

struct lease *lease_new (nn_etime_t texpire, dds_duration_t tdur, struct entity_common *e)
{
  struct lease *l;
  if ((l = ddsrt_malloc (sizeof (*l))) == NULL)
    return NULL;
  DDS_TRACE("lease_new(tdur %"PRId64" guid "PGUIDFMT") @ %p\n", tdur, PGUID (e->guid), (void *) l);
  l->tdur = tdur;
  l->tend = texpire;
  l->tsched.v = TSCHED_NOT_ON_HEAP;
  l->entity = e;
  return l;
}

void lease_register (struct lease *l)
{
  DDS_TRACE("lease_register(l %p guid "PGUIDFMT")\n", (void *) l, PGUID (l->entity->guid));
  ddsrt_mutex_lock (&gv.leaseheap_lock);
  lock_lease (l);
  assert (l->tsched.v == TSCHED_NOT_ON_HEAP);
  if (l->tend.v != T_NEVER)
  {
    l->tsched = l->tend;
    ddsrt_fibheap_insert (&lease_fhdef, &gv.leaseheap, l);
  }
  unlock_lease (l);
  ddsrt_mutex_unlock (&gv.leaseheap_lock);

  /* check_and_handle_lease_expiration runs on GC thread and the only way to be sure that it wakes up in time is by forcing re-evaluation (strictly speaking only needed if this is the first lease to expire, but this operation is quite rare anyway) */
  force_lease_check();
}

void lease_free (struct lease *l)
{
  DDS_TRACE("lease_free(l %p guid "PGUIDFMT")\n", (void *) l, PGUID (l->entity->guid));
  ddsrt_mutex_lock (&gv.leaseheap_lock);
  if (l->tsched.v != TSCHED_NOT_ON_HEAP)
    ddsrt_fibheap_delete (&lease_fhdef, &gv.leaseheap, l);
  ddsrt_mutex_unlock (&gv.leaseheap_lock);
  ddsrt_free (l);

  /* see lease_register() */
  force_lease_check();
}

void lease_renew (struct lease *l, nn_etime_t tnowE)
{
  nn_etime_t tend_new = add_duration_to_etime (tnowE, l->tdur);
  int did_update;
  lock_lease (l);
  /* do not touch tend if moving forward or if already expired */
  if (tend_new.v <= l->tend.v || tnowE.v >= l->tend.v)
    did_update = 0;
  else
  {
    l->tend = tend_new;
    did_update = 1;
  }
  unlock_lease (l);

  if (did_update && (dds_get_log_mask() & DDS_LC_TRACE))
  {
    int32_t tsec, tusec;
    DDS_TRACE(" L(");
    if (l->entity->guid.entityid.u == NN_ENTITYID_PARTICIPANT)
      DDS_TRACE(":%"PRIx32, l->entity->guid.entityid.u);
    else
      DDS_TRACE(""PGUIDFMT"", PGUID (l->entity->guid));
    etime_to_sec_usec (&tsec, &tusec, tend_new);
    DDS_TRACE(" %"PRId32".%06"PRId32")", tsec, tusec);
  }
}

void lease_set_expiry (struct lease *l, nn_etime_t when)
{
  bool trigger = false;
  assert (when.v >= 0);
  ddsrt_mutex_lock (&gv.leaseheap_lock);
  lock_lease (l);
  l->tend = when;
  if (l->tend.v < l->tsched.v)
  {
    /* moved forward and currently scheduled (by virtue of
       TSCHED_NOT_ON_HEAP == INT64_MIN) */
    l->tsched = l->tend;
    ddsrt_fibheap_decrease_key (&lease_fhdef, &gv.leaseheap, l);
    trigger = true;
  }
  else if (l->tsched.v == TSCHED_NOT_ON_HEAP && l->tend.v < T_NEVER)
  {
    /* not currently scheduled, with a finite new expiry time */
    l->tsched = l->tend;
    ddsrt_fibheap_insert (&lease_fhdef, &gv.leaseheap, l);
    trigger = true;
  }
  unlock_lease (l);
  ddsrt_mutex_unlock (&gv.leaseheap_lock);

  /* see lease_register() */
  if (trigger)
    force_lease_check();
}

int64_t check_and_handle_lease_expiration (nn_etime_t tnowE)
{
  struct lease *l;
  int64_t delay;
  ddsrt_mutex_lock (&gv.leaseheap_lock);
  while ((l = ddsrt_fibheap_min (&lease_fhdef, &gv.leaseheap)) != NULL && l->tsched.v <= tnowE.v)
  {
    nn_guid_t g = l->entity->guid;
    enum entity_kind k = l->entity->kind;

    assert (l->tsched.v != TSCHED_NOT_ON_HEAP);
    ddsrt_fibheap_extract_min (&lease_fhdef, &gv.leaseheap);

    lock_lease (l);
    if (tnowE.v < l->tend.v)
    {
      if (l->tend.v == T_NEVER) {
        /* don't reinsert if it won't expire */
        l->tsched.v = TSCHED_NOT_ON_HEAP;
        unlock_lease (l);
      } else {
        l->tsched = l->tend;
        unlock_lease (l);
        ddsrt_fibheap_insert (&lease_fhdef, &gv.leaseheap, l);
      }
      continue;
    }

    DDS_LOG(DDS_LC_DISCOVERY, "lease expired: l %p guid "PGUIDFMT" tend %"PRId64" < now %"PRId64"\n", (void *) l, PGUID (g), l->tend.v, tnowE.v);

    /* If the proxy participant is relying on another participant for
       writing its discovery data (on the privileged participant,
       i.e., its ddsi2 instance), we can't afford to drop it while the
       privileged one is still considered live.  If we do and it was a
       temporary asymmetrical thing and the ddsi2 instance never lost
       its liveliness, we will not rediscover the endpoints of this
       participant because we will not rediscover the ddsi2
       participant.

       So IF it is dependent on another one, we renew the lease for a
       very short while if the other one is still alive.  If it is a
       real case of lost liveliness, the other one will be gone soon
       enough; if not, we should get a sign of life soon enough.

       In this case, we simply abort the current iteration of the loop
       after renewing the lease and continue with the next one.

       This trick would fail if the ddsi2 participant can lose its
       liveliness and regain it before we re-check the liveliness of
       the dependent participants, and so the interval here must
       significantly less than the pruning time for the
       deleted_participants admin.

       I guess that means there is a really good argument for the SPDP
       and SEDP writers to be per-participant! */
    if (k == EK_PROXY_PARTICIPANT)
    {
      struct proxy_participant *proxypp;
      if ((proxypp = ephash_lookup_proxy_participant_guid (&g)) != NULL &&
          ephash_lookup_proxy_participant_guid (&proxypp->privileged_pp_guid) != NULL)
      {
        DDS_LOG(DDS_LC_DISCOVERY, "but postponing because privileged pp "PGUIDFMT" is still live\n",
                PGUID (proxypp->privileged_pp_guid));
        l->tsched = l->tend = add_duration_to_etime (tnowE, 200 * T_MILLISECOND);
        unlock_lease (l);
        ddsrt_fibheap_insert (&lease_fhdef, &gv.leaseheap, l);
        continue;
      }
    }

    unlock_lease (l);

    l->tsched.v = TSCHED_NOT_ON_HEAP;
    ddsrt_mutex_unlock (&gv.leaseheap_lock);

    switch (k)
    {
      case EK_PARTICIPANT:
        delete_participant (&g);
        break;
      case EK_PROXY_PARTICIPANT:
        delete_proxy_participant_by_guid (&g, now(), 1);
        break;
      case EK_WRITER:
        delete_writer_nolinger (&g);
        break;
      case EK_PROXY_WRITER:
        delete_proxy_writer (&g, now(), 1);
        break;
      case EK_READER:
        (void)delete_reader (&g);
        break;
      case EK_PROXY_READER:
        delete_proxy_reader (&g, now(), 1);
        break;
    }

    ddsrt_mutex_lock (&gv.leaseheap_lock);
  }

  delay = (l == NULL) ? T_NEVER : (l->tsched.v - tnowE.v);
  ddsrt_mutex_unlock (&gv.leaseheap_lock);
  return delay;
}

/******/

static void debug_print_rawdata (const char *msg, const void *data, size_t len)
{
  const unsigned char *c = data;
  size_t i;
  DDS_TRACE("%s<", msg);
  for (i = 0; i < len; i++)
  {
    if (32 < c[i] && c[i] <= 127)
      DDS_TRACE("%s%c", (i > 0 && (i%4) == 0) ? " " : "", c[i]);
    else
      DDS_TRACE("%s\\x%02x", (i > 0 && (i%4) == 0) ? " " : "", c[i]);
  }
  DDS_TRACE(">");
}

void handle_PMD (UNUSED_ARG (const struct receiver_state *rst), nn_wctime_t timestamp, unsigned statusinfo, const void *vdata, uint32_t len)
{
  const struct CDRHeader *data = vdata; /* built-ins not deserialized (yet) */
  const int bswap = (data->identifier == CDR_LE) ^ (DDSRT_ENDIAN == DDSRT_LITTLE_ENDIAN);
  struct proxy_participant *pp;
  nn_guid_t ppguid;
  DDS_TRACE(" PMD ST%x", statusinfo);
  if (data->identifier != CDR_LE && data->identifier != CDR_BE)
  {
    DDS_TRACE(" PMD data->identifier %u !?\n", ntohs (data->identifier));
    return;
  }
  switch (statusinfo & (NN_STATUSINFO_DISPOSE | NN_STATUSINFO_UNREGISTER))
  {
    case 0:
      if (offsetof (ParticipantMessageData_t, value) > len - sizeof (struct CDRHeader))
        debug_print_rawdata (" SHORT1", data, len);
      else
      {
        const ParticipantMessageData_t *pmd = (ParticipantMessageData_t *) (data + 1);
        nn_guid_prefix_t p = nn_ntoh_guid_prefix (pmd->participantGuidPrefix);
        uint32_t kind = ntohl (pmd->kind);
        uint32_t length = bswap ? bswap4u (pmd->length) : pmd->length;
        DDS_TRACE(" pp %"PRIx32":%"PRIx32":%"PRIx32" kind %u data %u", p.u[0], p.u[1], p.u[2], kind, length);
        if (len - sizeof (struct CDRHeader) - offsetof (ParticipantMessageData_t, value) < length)
          debug_print_rawdata (" SHORT2", pmd->value, len - sizeof (struct CDRHeader) - offsetof (ParticipantMessageData_t, value));
        else
          debug_print_rawdata ("", pmd->value, length);
        ppguid.prefix = p;
        ppguid.entityid.u = NN_ENTITYID_PARTICIPANT;
        if ((pp = ephash_lookup_proxy_participant_guid (&ppguid)) == NULL)
          DDS_TRACE(" PPunknown");
        else
        {
          /* Renew lease if arrival of this message didn't already do so, also renew the lease
             of the virtual participant used for DS-discovered endpoints */
#if 0 // FIXME: superfluous ... receipt of the message already did it */
          lease_renew (ddsrt_atomic_ldvoidp (&pp->lease), now_et ());
#endif
        }
      }
      break;

    case NN_STATUSINFO_DISPOSE:
    case NN_STATUSINFO_UNREGISTER:
    case NN_STATUSINFO_DISPOSE | NN_STATUSINFO_UNREGISTER:
      /* Serialized key; BE or LE doesn't matter as both fields are
         defined as octets.  */
      if (len < sizeof (struct CDRHeader) + sizeof (nn_guid_prefix_t))
        debug_print_rawdata (" SHORT3", data, len);
      else
      {
        ppguid.prefix = nn_ntoh_guid_prefix (*((nn_guid_prefix_t *) (data + 1)));
        ppguid.entityid.u = NN_ENTITYID_PARTICIPANT;
        if (delete_proxy_participant_by_guid (&ppguid, timestamp, 0) < 0)
          DDS_TRACE(" unknown");
        else
          DDS_TRACE(" delete");
      }
      break;
  }
  DDS_TRACE("\n");
}

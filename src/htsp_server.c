/*
 *  tvheadend, HTSP interface
 *  Copyright (C) 2007 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "atomic.h"
#include "channels.h"
#include "subscriptions.h"
#include "tcp.h"
#include "packet.h"
#include "access.h"
#include "htsp_server.h"
#include "streaming.h"
#include "htsmsg_binary.h"
#include "epg.h"
#include "plumbing/tsfix.h"
#include "imagecache.h"
#include "descrambler.h"
#include "notify.h"
#if ENABLE_TIMESHIFT
#include "timeshift.h"
#endif

#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#if ENABLE_ANDROID
#include <sys/vfs.h>
#define statvfs statfs
#define fstatvfs fstatfs
#else
#include <sys/statvfs.h>
#endif
#include "settings.h"
#include <sys/time.h>
#include <limits.h>

/* **************************************************************************
 * Datatypes and variables
 * *************************************************************************/

static void *htsp_server, *htsp_server_2;

#define HTSP_PROTO_VERSION 17

#define HTSP_ASYNC_OFF  0x00
#define HTSP_ASYNC_ON   0x01
#define HTSP_ASYNC_EPG  0x02

#define HTSP_ASYNC_AUX_CHTAG   0x01
#define HTSP_ASYNC_AUX_DVR     0x02
#define HTSP_ASYNC_AUX_AUTOREC 0x03

#define HTSP_PRIV_MASK (ACCESS_HTSP_STREAMING)

extern char *dvr_storage;

LIST_HEAD(htsp_connection_list, htsp_connection);
LIST_HEAD(htsp_subscription_list, htsp_subscription);
LIST_HEAD(htsp_file_list, htsp_file);

TAILQ_HEAD(htsp_msg_queue, htsp_msg);
TAILQ_HEAD(htsp_msg_q_queue, htsp_msg_q);

static struct htsp_connection_list htsp_async_connections;
static struct htsp_connection_list htsp_connections;

static void htsp_streaming_input(void *opaque, streaming_message_t *sm);

/**
 *
 */
typedef struct htsp_msg {
  TAILQ_ENTRY(htsp_msg) hm_link;

  htsmsg_t *hm_msg;
  int hm_payloadsize;         /* For maintaining stats about streaming
				 buffer depth */

  pktbuf_t *hm_pb;      /* For keeping reference to packet payload.
			   hm_msg can contain messages that points
			   to packet payload so to avoid copy we
			   keep a reference here */
} htsp_msg_t;


/**
 *
 */
typedef struct htsp_msg_q {
  struct htsp_msg_queue hmq_q;

  TAILQ_ENTRY(htsp_msg_q) hmq_link;
  int hmq_strict_prio;      /* Serve this queue 'til it's empty */
  int hmq_length;
  int hmq_payload;          /* Bytes of streaming payload that's enqueued */
  int hmq_dead;
} htsp_msg_q_t;

/**
 *
 */
typedef struct htsp_connection {
  LIST_ENTRY(htsp_connection) htsp_link;

  int htsp_fd;
  struct sockaddr_storage *htsp_peer;

  uint32_t htsp_version;

  char *htsp_logname;
  char *htsp_peername;
  char *htsp_username;
  char *htsp_clientname;
  char *htsp_language; // for async updates

  /**
   * Async mode
   */
  int htsp_async_mode;
  LIST_ENTRY(htsp_connection) htsp_async_link;

  /**
   * Writer thread
   */
  pthread_t htsp_writer_thread;

  int htsp_writer_run;

  struct htsp_msg_q_queue htsp_active_output_queues;

  pthread_mutex_t htsp_out_mutex;
  pthread_cond_t htsp_out_cond;

  htsp_msg_q_t htsp_hmq_ctrl;
  htsp_msg_q_t htsp_hmq_epg;
  htsp_msg_q_t htsp_hmq_qstatus;

  struct htsp_subscription_list htsp_subscriptions;
  struct htsp_subscription_list htsp_dead_subscriptions;
  struct htsp_file_list htsp_files;
  int htsp_file_id;

  access_t *htsp_granted_access;

  uint8_t htsp_challenge[32];

} htsp_connection_t;


/**
 *
 */
typedef struct htsp_subscription {
  htsp_connection_t *hs_htsp;

  LIST_ENTRY(htsp_subscription) hs_link;

  int hs_sid;  /* Subscription ID (set by client) */

  th_subscription_t *hs_s; // Temporary

  streaming_target_t hs_input;
  profile_chain_t    hs_prch;

  htsp_msg_q_t hs_q;

  time_t hs_last_report; /* Last queue status report sent */

  int hs_dropstats[PKT_NTYPES];

  int hs_wait_for_video;

  int hs_90khz;

  int hs_queue_depth;

#define NUM_FILTERED_STREAMS (64*8)

  uint64_t hs_filtered_streams[8]; // one bit per stream

  int hs_first;

} htsp_subscription_t;


/**
 *
 */
typedef struct htsp_file {
  LIST_ENTRY(htsp_file) hf_link;
  int hf_id;  // ID sent to client
  int hf_fd;  // Our file descriptor
  char *hf_path; // For logging
} htsp_file_t;

#define HTSP_DEFAULT_QUEUE_DEPTH 500000

/* **************************************************************************
 * Support routines
 * *************************************************************************/

static void
htsp_disable_stream(htsp_subscription_t *hs, unsigned int id)
{
  if(id < NUM_FILTERED_STREAMS)
    hs->hs_filtered_streams[id / 64] |= 1 << (id & 63);
}


static void
htsp_enable_stream(htsp_subscription_t *hs, unsigned int id)
{
  if(id < NUM_FILTERED_STREAMS)
    hs->hs_filtered_streams[id / 64] &= ~(1 << (id & 63));
}


static inline int
htsp_is_stream_enabled(htsp_subscription_t *hs, unsigned int id)
{
  if(id < NUM_FILTERED_STREAMS)
    return !(hs->hs_filtered_streams[id / 64] & (1 << (id & 63)));
  return 1;
}

/**
 *
 */
static void
htsp_update_logname(htsp_connection_t *htsp)
{
  char buf[100];

  snprintf(buf, sizeof(buf), "%s%s%s%s%s%s",
	   htsp->htsp_peername,
	   htsp->htsp_username || htsp->htsp_clientname ? " [ " : "",
	   htsp->htsp_username ?: "",
	   htsp->htsp_username && htsp->htsp_clientname ? " | " : "",
	   htsp->htsp_clientname ?: "",
	   htsp->htsp_username || htsp->htsp_clientname ? " ]" : "");

  tvh_str_update(&htsp->htsp_logname, buf);
}

/**
 *
 */
static void
htsp_msg_destroy(htsp_msg_t *hm)
{
  htsmsg_destroy(hm->hm_msg);
  if(hm->hm_pb != NULL)
    pktbuf_ref_dec(hm->hm_pb);
  free(hm);
}

/**
 *
 */
static void
htsp_init_queue(htsp_msg_q_t *hmq, int strict_prio)
{
  TAILQ_INIT(&hmq->hmq_q);
  hmq->hmq_length = 0;
  hmq->hmq_strict_prio = strict_prio;
}

/**
 *
 */
static void
htsp_flush_queue(htsp_connection_t *htsp, htsp_msg_q_t *hmq, int dead)
{
  htsp_msg_t *hm;

  pthread_mutex_lock(&htsp->htsp_out_mutex);

  if(hmq->hmq_length)
    TAILQ_REMOVE(&htsp->htsp_active_output_queues, hmq, hmq_link);

  while((hm = TAILQ_FIRST(&hmq->hmq_q)) != NULL) {
    TAILQ_REMOVE(&hmq->hmq_q, hm, hm_link);
    htsp_msg_destroy(hm);
  }

  // reset
  hmq->hmq_length = 0;
  hmq->hmq_payload = 0;
  hmq->hmq_dead = dead;
  pthread_mutex_unlock(&htsp->htsp_out_mutex);
}

/**
 *
 */
static void
htsp_subscription_destroy(htsp_connection_t *htsp, htsp_subscription_t *hs)
{
  LIST_REMOVE(hs, hs_link);
  LIST_INSERT_HEAD(&htsp->htsp_dead_subscriptions, hs, hs_link);

  subscription_unsubscribe(hs->hs_s);

  if(hs->hs_prch.prch_st != NULL)
    profile_chain_close(&hs->hs_prch);

  htsp_flush_queue(htsp, &hs->hs_q, 1);
}

/**
 *
 */
static void
htsp_subscription_free(htsp_connection_t *htsp, htsp_subscription_t *hs)
{
  LIST_REMOVE(hs, hs_link);
  htsp_flush_queue(htsp, &hs->hs_q, 1);
  free(hs);
}

/**
 *
 */
static void
htsp_send(htsp_connection_t *htsp, htsmsg_t *m, pktbuf_t *pb,
	  htsp_msg_q_t *hmq, int payloadsize)
{
  htsp_msg_t *hm = malloc(sizeof(htsp_msg_t));

  hm->hm_msg = m;
  hm->hm_pb = pb;
  if(pb != NULL)
    pktbuf_ref_inc(pb);
  hm->hm_payloadsize = payloadsize;
  
  pthread_mutex_lock(&htsp->htsp_out_mutex);

  assert(!hmq->hmq_dead);

  TAILQ_INSERT_TAIL(&hmq->hmq_q, hm, hm_link);

  if(hmq->hmq_length == 0) {
    /* Activate queue */

    if(hmq->hmq_strict_prio) {
      TAILQ_INSERT_HEAD(&htsp->htsp_active_output_queues, hmq, hmq_link);
    } else {
      TAILQ_INSERT_TAIL(&htsp->htsp_active_output_queues, hmq, hmq_link);
    }
  }

  hmq->hmq_length++;
  hmq->hmq_payload += payloadsize;
  pthread_cond_signal(&htsp->htsp_out_cond);
  pthread_mutex_unlock(&htsp->htsp_out_mutex);
}

/**
 *
 */
static void
htsp_send_message(htsp_connection_t *htsp, htsmsg_t *m, htsp_msg_q_t *hmq)
{
  htsp_send(htsp, m, NULL, hmq ?: &htsp->htsp_hmq_ctrl, 0);
}

/** 
 * Simple function to respond with an error
 */
static htsmsg_t *
htsp_error(const char *err)
{
  htsmsg_t *r = htsmsg_create_map();
  htsmsg_add_str(r, "error", err);
  return r;
}

/**
 *
 */
static void
htsp_reply(htsp_connection_t *htsp, htsmsg_t *in, htsmsg_t *out)
{
  uint32_t seq;

  if(!htsmsg_get_u32(in, "seq", &seq))
    htsmsg_add_u32(out, "seq", seq);

  htsp_send_message(htsp, out, NULL);
}

/**
 * Update challenge
 */
static int
htsp_generate_challenge(htsp_connection_t *htsp)
{
  int fd, n;

  if((fd = tvh_open("/dev/urandom", O_RDONLY, 0)) < 0)
    return -1;
  
  n = read(fd, &htsp->htsp_challenge, 32);
  close(fd);
  return n != 32;
}

/**
 * Cehck if user can access the channel
 */
static inline int
htsp_user_access_channel(htsp_connection_t *htsp, channel_t *ch)
{
  return channel_access(ch, htsp->htsp_granted_access, 0);
}

#define HTSP_CHECK_CHANNEL_ACCESS(htsp, ch)\
if (!htsp_user_access_channel(htsp, ch))\
  return htsp_error("User cannot access this channel");

/* **************************************************************************
 * File helpers
 * *************************************************************************/

static uint32_t
htsp_channel_tag_get_identifier(channel_tag_t *ct)
{
  static int prev = 0;
  if (ct->ct_htsp_id == 0)
    ct->ct_htsp_id = ++prev;
  return ct->ct_htsp_id;
}

static channel_tag_t *
htsp_channel_tag_find_by_identifier(htsp_connection_t *htsp, uint32_t id)
{
  channel_tag_t *ct;

  TAILQ_FOREACH(ct, &channel_tags, ct_link) {
    if (!channel_tag_access(ct, htsp->htsp_granted_access, 0))
      continue;
    if (id == ct->ct_htsp_id)
      return ct;
  }
  return NULL;
}

/**
 *
 */
static htsmsg_t *
htsp_file_open(htsp_connection_t *htsp, const char *path, int fd)
{
  struct stat st;

  if (fd <= 0) {
    fd = tvh_open(path, O_RDONLY, 0);
    tvhlog(LOG_DEBUG, "htsp", "Opening file %s -- %s", path, fd < 0 ? strerror(errno) : "OK");
    if(fd == -1)
      return htsp_error("Unable to open file");
  }

  htsp_file_t *hf = calloc(1, sizeof(htsp_file_t));
  hf->hf_fd = fd;
  hf->hf_id = ++htsp->htsp_file_id;
  hf->hf_path = strdup(path);
  LIST_INSERT_HEAD(&htsp->htsp_files, hf, hf_link);

  htsmsg_t *rep = htsmsg_create_map();
  htsmsg_add_u32(rep, "id", hf->hf_id);

  if(!fstat(hf->hf_fd, &st)) {
    htsmsg_add_s64(rep, "size", st.st_size);
    htsmsg_add_s64(rep, "mtime", st.st_mtime);
  }

  return rep;
}

/**
 *
 */
static htsp_file_t *
htsp_file_find(const htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_file_t *hf;

  int id = htsmsg_get_u32_or_default(in, "id", 0);

  LIST_FOREACH(hf, &htsp->htsp_files, hf_link) {
    if(hf->hf_id == id)
      return hf;
  }
  return NULL;
}

/**
 *
 */
static void
htsp_file_destroy(htsp_file_t *hf)
{
  tvhlog(LOG_DEBUG, "htsp", "Closed opened file %s", hf->hf_path);
  free(hf->hf_path);
  close(hf->hf_fd);
  LIST_REMOVE(hf, hf_link);
  free(hf);
}

/* **************************************************************************
 * Output message generators
 * *************************************************************************/

/**
 *
 */
static htsmsg_t *
htsp_build_channel(channel_t *ch, const char *method, htsp_connection_t *htsp)
{
  channel_tag_mapping_t *ctm;
  channel_service_mapping_t *csm;
  channel_tag_t *ct;
  service_t *t;
  epg_broadcast_t *now, *next = NULL;
  int64_t chnum = channel_get_number(ch);
  const char *icon;

  htsmsg_t *out = htsmsg_create_map();
  htsmsg_t *tags = htsmsg_create_list();
  htsmsg_t *services = htsmsg_create_list();

  htsmsg_add_u32(out, "channelId", channel_get_id(ch));
  htsmsg_add_u32(out, "channelNumber", channel_get_major(chnum));
  if (channel_get_minor(chnum))
    htsmsg_add_u32(out, "channelNumberMinor", channel_get_minor(chnum));

  htsmsg_add_str(out, "channelName", channel_get_name(ch));
  if ((icon = channel_get_icon(ch))) {

    /* Handle older clients */
    if ((strstr(icon, "imagecache") == icon) && htsp->htsp_version < 8) {
      struct sockaddr_storage addr;
      socklen_t addrlen;
      size_t p = 0;
      char url[256];
      char buf[50];
      addrlen = sizeof(addr);
      getsockname(htsp->htsp_fd, (struct sockaddr*)&addr, &addrlen);
      tcp_get_ip_str((struct sockaddr*)&addr, buf, 50);
      strcpy(url, "http://");
      p = strlen(url);
      p += snprintf(url+p, sizeof(url)-p, "%s%s%s:%d%s",
                    (addr.ss_family == AF_INET6)?"[":"",
                    buf,
                    (addr.ss_family == AF_INET6)?"]":"",
                    tvheadend_webui_port,
                    tvheadend_webroot ?: "");
      snprintf(url+p, sizeof(url)-p, "/%s", icon);
      htsmsg_add_str(out, "channelIcon", url);
    } else {
      if (htsp->htsp_version < 15) {
        /* older clients expects '/imagecache/' */
        static char buf[64];
        if (strncmp(icon, "imagecache/", 11) == 0) {
          snprintf(buf, sizeof(buf), "/%s", icon);
          icon = buf;
        }
      }
      htsmsg_add_str(out, "channelIcon", icon);
    }
  }

  now  = ch->ch_epg_now;
  next = ch->ch_epg_next;
  htsmsg_add_u32(out, "eventId", now ? now->id : 0);
  htsmsg_add_u32(out, "nextEventId", next ? next->id : 0);

  LIST_FOREACH(ctm, &ch->ch_ctms, ctm_channel_link) {
    ct = ctm->ctm_tag;
    if(channel_tag_access(ct, htsp->htsp_granted_access, 0))
      htsmsg_add_u32(tags, NULL, htsp_channel_tag_get_identifier(ct));
  }

  LIST_FOREACH(csm, &ch->ch_services, csm_chn_link) {
    t = csm->csm_svc;
    htsmsg_t *svcmsg = htsmsg_create_map();
    uint16_t caid;
    htsmsg_add_str(svcmsg, "name", service_nicename(t));
    htsmsg_add_str(svcmsg, "type", service_servicetype_txt(t));
    if((caid = service_get_encryption(t)) != 0) {
      htsmsg_add_u32(svcmsg, "caid", caid);
      htsmsg_add_str(svcmsg, "caname", descrambler_caid2name(caid));
    }
    htsmsg_add_msg(services, NULL, svcmsg);
  }

  htsmsg_add_msg(out, "services", services);
  htsmsg_add_msg(out, "tags", tags);
  if (method)
    htsmsg_add_str(out, "method", method);
  return out;
}

/**
 *
 */
static htsmsg_t *
htsp_build_tag(channel_tag_t *ct, const char *method, int include_channels)
{
  channel_tag_mapping_t *ctm;
  htsmsg_t *out = htsmsg_create_map();
  htsmsg_t *members = include_channels ? htsmsg_create_list() : NULL;
 
  htsmsg_add_u32(out, "tagId", htsp_channel_tag_get_identifier(ct));
  htsmsg_add_u32(out, "tagIndex", ct->ct_index);

  htsmsg_add_str(out, "tagName", ct->ct_name);
  htsmsg_add_str(out, "tagIcon", channel_tag_get_icon(ct));
  htsmsg_add_u32(out, "tagTitledIcon", ct->ct_titled_icon);

  if(members != NULL) {
    LIST_FOREACH(ctm, &ct->ct_ctms, ctm_tag_link)
      htsmsg_add_u32(members, NULL, channel_get_id(ctm->ctm_channel));
    htsmsg_add_msg(out, "members", members);
  }

  htsmsg_add_str(out, "method", method);
  return out;
}

/**
 *
 */
static htsmsg_t *
htsp_build_dvrentry(dvr_entry_t *de, const char *method)
{
  htsmsg_t *out = htsmsg_create_map();
  const char *s = NULL, *error = NULL;
  const char *p;

  htsmsg_add_u32(out, "id", idnode_get_short_uuid(&de->de_id));
  if (de->de_channel)
    htsmsg_add_u32(out, "channel", channel_get_id(de->de_channel));

  if (de->de_bcast)
    htsmsg_add_u32(out, "eventId",  de->de_bcast->id);

  if (de->de_autorec)
    htsmsg_add_str(out, "autorecId", idnode_uuid_as_str(&de->de_autorec->dae_id));

  htsmsg_add_s64(out, "start",       de->de_start);
  htsmsg_add_s64(out, "stop",        de->de_stop);
  htsmsg_add_s64(out, "startExtra",  dvr_entry_get_extra_time_pre(de));
  htsmsg_add_s64(out, "stopExtra",   dvr_entry_get_extra_time_post(de));
  htsmsg_add_u32(out, "retention",   dvr_entry_get_retention(de));
  htsmsg_add_u32(out, "priority",    de->de_pri);
  htsmsg_add_u32(out, "contentType", de->de_content_type);

  if( de->de_title && (s = lang_str_get(de->de_title, NULL)))
    htsmsg_add_str(out, "title", s);
  if( de->de_desc && (s = lang_str_get(de->de_desc, NULL)))
    htsmsg_add_str(out, "description", s);

  if( de->de_filename && de->de_config ) {
    if ((p = tvh_strbegins(de->de_filename, de->de_config->dvr_storage)))
      htsmsg_add_str(out, "path", p);
  }

  switch(de->de_sched_state) {
  case DVR_SCHEDULED:
    s = "scheduled";
    break;
  case DVR_RECORDING:
    s = "recording";
    if (de->de_rec_state == DVR_RS_ERROR)
      error = streaming_code2txt(de->de_last_error);
    break;
  case DVR_COMPLETED:
    s = "completed";
    if(dvr_get_filesize(de) == -1)
      error = "File missing";
    else if(de->de_last_error)
      error = streaming_code2txt(de->de_last_error);
    break;
  case DVR_MISSED_TIME:
    s = "missed";
    break;
  case DVR_NOSTATE:
    s = "invalid";
    break;
  }

  htsmsg_add_str(out, "state", s);
  if(error)
    htsmsg_add_str(out, "error", error);
  htsmsg_add_str(out, "method", method);
  return out;
}

/**
 *
 */
static htsmsg_t *
htsp_build_autorecentry(dvr_autorec_entry_t *dae, const char *method)
{
  htsmsg_t *out = htsmsg_create_map();
  int tad;

  htsmsg_add_str(out, "id",          idnode_uuid_as_str(&dae->dae_id));
  htsmsg_add_u32(out, "enabled",     dae->dae_enabled);
  htsmsg_add_u32(out, "maxDuration", dae->dae_maxduration);
  htsmsg_add_u32(out, "minDuration", dae->dae_minduration);
  htsmsg_add_u32(out, "retention",   dae->dae_retention);
  htsmsg_add_u32(out, "daysOfWeek",  dae->dae_weekdays);
  if (dae->dae_start >= 0 && dae->dae_start_window >= 0) {
    if (dae->dae_start > dae->dae_start_window)
      tad = 24 * 60 - dae->dae_start + dae->dae_start_window;
    else
      tad = dae->dae_start_window - dae->dae_start;
  } else {
    tad = -1;
  }
  htsmsg_add_s32(out, "approxTime",
                 dae->dae_start >= 0 && tad >= 0 ?
                   ((dae->dae_start + tad / 2) % (24 * 60)) : -1);
  htsmsg_add_u32(out, "start",       dae->dae_start);
  htsmsg_add_u32(out, "startWindow", dae->dae_start_window);
  htsmsg_add_u32(out, "priority",    dae->dae_pri);
  htsmsg_add_s64(out, "startExtra",  dae->dae_start_extra);
  htsmsg_add_s64(out, "stopExtra",   dae->dae_stop_extra);

  if(dae->dae_title)
    htsmsg_add_str(out, "title", dae->dae_title);

  if(dae->dae_channel)
    htsmsg_add_u32(out, "channel", channel_get_id(dae->dae_channel));

  htsmsg_add_str(out, "method", method);

  return out;
}

/**
 *
 */
static htsmsg_t *
htsp_build_event
  (epg_broadcast_t *e, const char *method, const char *lang, time_t update,
   htsp_connection_t *htsp )
{
  htsmsg_t *out;
  epg_broadcast_t *n;
  dvr_entry_t *de;
  epg_genre_t *g;
  epg_episode_num_t epnum;
  const char *str;
  epg_episode_t *ee = e->episode;

  /* Ignore? */
  if (update) {
    int ignore = 1;
    if      (e->updated > update) ignore = 0;
    else if (e->serieslink && e->serieslink->updated > update) ignore = 0;
    else if (ee) {
           if (ee->updated > update) ignore = 0;
      else if (ee->brand  && ee->brand->updated > update)  ignore = 0;
      else if (ee->season && ee->season->updated > update) ignore = 0;
    }
    if (ignore) return NULL;
  }

  out = htsmsg_create_map();

  if (method)
    htsmsg_add_str(out, "method", method);

  htsmsg_add_u32(out, "eventId", e->id);
  htsmsg_add_u32(out, "channelId", channel_get_id(e->channel));
  htsmsg_add_s64(out, "start", e->start);
  htsmsg_add_s64(out, "stop", e->stop);
  if ((str = epg_broadcast_get_title(e, lang)))
    htsmsg_add_str(out, "title", str);
  if ((str = epg_broadcast_get_description(e, lang))) {
    htsmsg_add_str(out, "description", str);
    if ((str = epg_broadcast_get_summary(e, lang)))
      htsmsg_add_str(out, "summary", str);
  } else if((str = epg_broadcast_get_summary(e, lang)))
    htsmsg_add_str(out, "description", str);
  if (e->serieslink) {
    htsmsg_add_u32(out, "serieslinkId", e->serieslink->id);
    if (e->serieslink->uri)
      htsmsg_add_str(out, "serieslinkUri", e->serieslink->uri);
  }

  if (ee) {
    htsmsg_add_u32(out, "episodeId", ee->id);
    if (ee->uri && strncasecmp(ee->uri,"tvh://",6))  /* tvh:// uris are internal */
      htsmsg_add_str(out, "episodeUri", ee->uri);
    if (ee->brand)
      htsmsg_add_u32(out, "brandId", ee->brand->id);
    if (ee->season)
      htsmsg_add_u32(out, "seasonId", ee->season->id);
    if((g = LIST_FIRST(&ee->genre))) {
      uint32_t code = g->code;
      if (htsp->htsp_version < 6) code = (code >> 4) & 0xF;
      htsmsg_add_u32(out, "contentType", code);
    }
    if (ee->age_rating)
      htsmsg_add_u32(out, "ageRating", ee->age_rating);
    if (ee->star_rating)
      htsmsg_add_u32(out, "starRating", ee->star_rating);
    if (ee->first_aired)
      htsmsg_add_s64(out, "firstAired", ee->first_aired);
    epg_episode_get_epnum(ee, &epnum);
    if (epnum.s_num) {
      htsmsg_add_u32(out, "seasonNumber", epnum.s_num);
      if (epnum.s_cnt)
        htsmsg_add_u32(out, "seasonCount", epnum.s_cnt);
    }
    if (epnum.e_num) {
      htsmsg_add_u32(out, "episodeNumber", epnum.e_num);
      if (epnum.e_cnt)
        htsmsg_add_u32(out, "episodeCount", epnum.e_cnt);
    }
    if (epnum.p_num) {
      htsmsg_add_u32(out, "partNumber", epnum.p_num);
      if (epnum.p_cnt)
        htsmsg_add_u32(out, "partCount", epnum.p_cnt);
    }
    if (epnum.text)
      htsmsg_add_str(out, "episodeOnscreen", epnum.text);
    if (ee->image)
      htsmsg_add_str(out, "image", ee->image);
  }

  if((de = dvr_entry_find_by_event(e)) != NULL) {
    htsmsg_add_u32(out, "dvrId", idnode_get_short_uuid(&de->de_id));
  }

  if ((n = epg_broadcast_get_next(e)))
    htsmsg_add_u32(out, "nextEventId", n->id);

  return out;
}

/* **************************************************************************
 * Message handlers
 * *************************************************************************/

/**
 * Hello, for protocol version negotiation
 */
static htsmsg_t *
htsp_method_hello(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *r;
  uint32_t v;
  const char *name;

  if(htsmsg_get_u32(in, "htspversion", &v))
    return htsp_error("Missing argument 'htspversion'");

  if((name = htsmsg_get_str(in, "clientname")) == NULL)
    return htsp_error("Missing argument 'clientname'");

  r = htsmsg_create_map();

  tvh_str_update(&htsp->htsp_clientname, htsmsg_get_str(in, "clientname"));

  tvhlog(LOG_INFO, "htsp", "%s: Welcomed client software: %s (HTSPv%d)",
	 htsp->htsp_logname, name, v);

  htsmsg_add_u32(r, "htspversion", HTSP_PROTO_VERSION);
  htsmsg_add_str(r, "servername", "HTS Tvheadend");
  htsmsg_add_str(r, "serverversion", tvheadend_version);
  htsmsg_add_bin(r, "challenge", htsp->htsp_challenge, 32);
  if (tvheadend_webroot)
    htsmsg_add_str(r, "webroot", tvheadend_webroot);

  /* Capabilities */
  htsmsg_add_msg(r, "servercapability", tvheadend_capabilities_list(1));

  /* Set version to lowest num */
  htsp->htsp_version = MIN(HTSP_PROTO_VERSION, v);

  htsp_update_logname(htsp);
  return r;
}

/**
 * Try to authenticate
 */
static htsmsg_t *
htsp_method_authenticate(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *r = htsmsg_create_map();

  if(!(htsp->htsp_granted_access->aa_rights & HTSP_PRIV_MASK))
    htsmsg_add_u32(r, "noaccess", 1);
  
  return r;
}

/**
 * Get total and free disk space on configured path
 */
static htsmsg_t *
htsp_method_getDiskSpace(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  struct statvfs diskdata;
  dvr_config_t *cfg = dvr_config_find_by_name_default(NULL);

  if(statvfs(cfg->dvr_storage,&diskdata) == -1)
    return htsp_error("Unable to stat path");
  
  out = htsmsg_create_map();
  htsmsg_add_s64(out, "freediskspace",
		 diskdata.f_bsize * (int64_t)diskdata.f_bavail);
  htsmsg_add_s64(out, "totaldiskspace",
		 diskdata.f_bsize * (int64_t)diskdata.f_blocks);
  return out;
}

/**
 * Get system time and diff to GMT
 */
static htsmsg_t *
htsp_method_getSysTime(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  struct timeval tv;
  struct timezone tz;

  if(gettimeofday(&tv, &tz) == -1)
    return htsp_error("Unable to get system time"); 

  out = htsmsg_create_map();
  htsmsg_add_s32(out, "time", tv.tv_sec);
  htsmsg_add_s32(out, "timezone", tz.tz_minuteswest / 60);
  return out;
}

/**
 * Switch the HTSP connection into async mode
 */
static htsmsg_t *
htsp_method_async(htsp_connection_t *htsp, htsmsg_t *in)
{
  channel_t *ch;
  channel_tag_t *ct;
  dvr_entry_t *de;
  dvr_autorec_entry_t *dae;
  htsmsg_t *m;
  uint32_t epg = 0;
  int64_t lastUpdate = 0;
  int64_t epgMaxTime = 0;
  const char *lang;
  epg_broadcast_t *ebc;

  /* Get optional flags */
  htsmsg_get_u32(in, "epg", &epg);
  htsmsg_get_s64(in, "lastUpdate", &lastUpdate);
  htsmsg_get_s64(in, "epgMaxTime", &epgMaxTime);
  if ((lang = htsmsg_get_str(in, "language")))
    htsp->htsp_language = strdup(lang);

  /* First, just OK the async request */
  htsp_reply(htsp, in, htsmsg_create_map()); 

  if(htsp->htsp_async_mode)
    return NULL; /* already in async mode */

  htsp->htsp_async_mode = HTSP_ASYNC_ON;
  if(epg)
    htsp->htsp_async_mode |= HTSP_ASYNC_EPG;

  /* Send all enabled and external tags */
  TAILQ_FOREACH(ct, &channel_tags, ct_link)
    if(channel_tag_access(ct, htsp->htsp_granted_access, 0))
      htsp_send_message(htsp, htsp_build_tag(ct, "tagAdd", 0), NULL);
  
  /* Send all channels */
  CHANNEL_FOREACH(ch)
    if (htsp_user_access_channel(htsp,ch))
      htsp_send_message(htsp, htsp_build_channel(ch, "channelAdd", htsp), NULL);
  
  /* Send all enabled and external tags (now with channel mappings) */
  TAILQ_FOREACH(ct, &channel_tags, ct_link)
    if(channel_tag_access(ct, htsp->htsp_granted_access, 0))
      htsp_send_message(htsp, htsp_build_tag(ct, "tagUpdate", 1), NULL);

  /* Send all autorecs */
  TAILQ_FOREACH(dae, &autorec_entries, dae_link)
    htsp_send_message(htsp, htsp_build_autorecentry(dae, "autorecEntryAdd"), NULL);

  /* Send all DVR entries */
  LIST_FOREACH(de, &dvrentries, de_global_link)
    if (htsp_user_access_channel(htsp,de->de_channel))
      htsp_send_message(htsp, htsp_build_dvrentry(de, "dvrEntryAdd"), NULL);

  /* Send EPG updates */
  if (epg) {
    CHANNEL_FOREACH(ch) {
      if (!htsp_user_access_channel(htsp, ch)) continue;
      RB_FOREACH(ebc, &ch->ch_epg_schedule, sched_link) {
        if (epgMaxTime && ebc->start > epgMaxTime) break;
        htsmsg_t *e = htsp_build_event(ebc, "eventAdd", lang, lastUpdate, htsp);
        if (e) htsp_send_message(htsp, e, NULL);
      }
    }
  }

  /* Notify that initial sync has been completed */
  m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "initialSyncCompleted");
  htsp_send_message(htsp, m, NULL);

  /* Insert in list so it will get all updates */
  LIST_INSERT_HEAD(&htsp_async_connections, htsp, htsp_async_link);

  return NULL;
}

/**
 * Get information about the given event
 */
static htsmsg_t *
htsp_method_getChannel(htsp_connection_t *htsp, htsmsg_t *in)
{
  uint32_t channelId;
  channel_t *ch = NULL;

  if (htsmsg_get_u32(in, "channelId", &channelId))
    return htsp_error("Missing argument 'channelId'");
  if (!(ch = channel_find_by_id(channelId)))
    return htsp_error("Channel does not exist");

  return htsp_build_channel(ch, NULL, htsp);
}

/**
 * Get information about the given event
 */
static htsmsg_t *
htsp_method_getEvent(htsp_connection_t *htsp, htsmsg_t *in)
{
  uint32_t eventId;
  epg_broadcast_t *e;
  const char *lang;
  
  if(htsmsg_get_u32(in, "eventId", &eventId))
    return htsp_error("Missing argument 'eventId'");
  lang = htsmsg_get_str(in, "language") ?: htsp->htsp_language;

  if((e = epg_broadcast_find_by_id(eventId)) == NULL)
    return htsp_error("Event does not exist");

  return htsp_build_event(e, NULL, lang, 0, htsp);
}

/**
 * Get information about the given event + 
 * n following events
 */
static htsmsg_t *
htsp_method_getEvents(htsp_connection_t *htsp, htsmsg_t *in)
{
  uint32_t u32, numFollowing;
  int64_t maxTime = 0;
  htsmsg_t *out, *events;
  epg_broadcast_t *e = NULL;
  channel_t *ch = NULL;
  const char *lang;

  /* Optional fields */
  if (!htsmsg_get_u32(in, "channelId", &u32))
    if (!(ch = channel_find_by_id(u32)))
      return htsp_error("Channel does not exist");
  if (!htsmsg_get_u32(in, "eventId", &u32))
    if (!(e = epg_broadcast_find_by_id(u32)))
      return htsp_error("Event does not exist");

  /* Check access */
  if (ch && !htsp_user_access_channel(htsp, ch))
    return htsp_error("User does not have access");

  numFollowing = htsmsg_get_u32_or_default(in, "numFollowing", 0);
  maxTime      = htsmsg_get_s64_or_default(in, "maxTime", 0);
  lang         = htsmsg_get_str(in, "language") ?: htsp->htsp_language;

  /* Use event as starting point */
  if (e || ch) {
    if (!e) e = ch->ch_epg_now ?: ch->ch_epg_next;

    /* Output */
    events = htsmsg_create_list();
    while (e) {
      if (maxTime && e->start > maxTime) break;
      htsmsg_add_msg(events, NULL, htsp_build_event(e, NULL, lang, 0, htsp));
      if (numFollowing == 1) break;
      if (numFollowing) numFollowing--;
      e = epg_broadcast_get_next(e);
    }

  /* All channels */
  } else {
    events = htsmsg_create_list();
    CHANNEL_FOREACH(ch) {
      int num = numFollowing;
      RB_FOREACH(e, &ch->ch_epg_schedule, sched_link) {
        if (maxTime && e->start > maxTime) break;
        htsmsg_add_msg(events, NULL, htsp_build_event(e, NULL, lang, 0, htsp));
        if (num == 1) break;
        if (num) num--;
      }
    }
  }
  
  /* Send */
  out = htsmsg_create_map();
  htsmsg_add_msg(out, "events", events);
  return out;
}

/**
 *
 * do an epg query
 */
static htsmsg_t *
htsp_method_epgQuery(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out, *array;
  const char *query;
  int i;
  uint32_t u32, full;
  channel_t *ch = NULL;
  channel_tag_t *ct = NULL;
  epg_query_t eq;
  const char *lang;
  int min_duration;
  int max_duration;

  /* Required */
  if( (query = htsmsg_get_str(in, "query")) == NULL )
    return htsp_error("Missing argument 'query'");

  memset(&eq, 0, sizeof(eq));
  
  /* Optional */
  if(!(htsmsg_get_u32(in, "channelId", &u32))) {
    if (!(ch = channel_find_by_id(u32)))
      return htsp_error("Channel does not exist");
    else
      eq.channel = strdup(idnode_uuid_as_str(&ch->ch_id));
  }
  if(!(htsmsg_get_u32(in, "tagId", &u32))) {
    if (!(ct = htsp_channel_tag_find_by_identifier(htsp, u32)))
      return htsp_error("Channel tag does not exist");
    else
      eq.channel_tag = strdup(idnode_uuid_as_str(&ct->ct_id));
  }
  if (!htsmsg_get_u32(in, "contentType", &u32)) {
    if(htsp->htsp_version < 6) u32 <<= 4;
    eq.genre_count = 1;
    eq.genre = eq.genre_static;
    eq.genre[0] = u32;
  }
  lang = htsmsg_get_str(in, "language") ?: htsp->htsp_language;
  eq.lang = lang ? strdup(lang) : NULL;
  full = htsmsg_get_u32_or_default(in, "full", 0);

  min_duration = htsmsg_get_u32_or_default(in, "minduration", 0);
  max_duration = htsmsg_get_u32_or_default(in, "maxduration", INT_MAX);
  eq.duration.comp = EC_RG;
  eq.duration.val1 = min_duration;
  eq.duration.val2 = max_duration;
  tvhtrace("htsp", "min_duration %d and max_duration %d", min_duration, max_duration);

  /* Check access */
  if (ch && !htsp_user_access_channel(htsp, ch))
    return htsp_error("User does not have access");

  /* Query */
  epg_query(&eq, htsp->htsp_granted_access);

  /* Create Reply */
  out = htsmsg_create_map();
  if( eq.entries ) {
    array = htsmsg_create_list();
    for(i = 0; i < eq.entries; ++i) {
      if (full)
        htsmsg_add_msg(array, NULL,
                       htsp_build_event(eq.result[i], NULL, lang, 0, htsp));
      else
        htsmsg_add_u32(array, NULL, eq.result[i]->id);
    }
    htsmsg_add_msg(out, full ? "events" : "eventIds", array);
  }
  
  epg_query_free(&eq);
  
  return out;
}

static htsmsg_t *
htsp_method_getEpgObject(htsp_connection_t *htsp, htsmsg_t *in)
{
  uint32_t id, u32;
  epg_object_type_t type;
  epg_object_t *eo;
  htsmsg_t *out;

  // TODO: should really block access based on channel, but actually
  //       that's really hard to do here!

  /* Required fields */
  if (htsmsg_get_u32(in, "id", &id))
    return htsp_error("Missing argument: id");

  /* Optional fields */
  if (!htsmsg_get_u32(in, "type", &u32) && (u32 <= EPG_TYPEMAX))
    type = u32;
  else
    type = EPG_UNDEF;

  /* Get object */
  if (!(eo = epg_object_find_by_id(id, type)))
    return htsp_error("Invalid EPG object request");

  /* Serialize */
  if (!(out = epg_object_serialize(eo)))
    return htsp_error("Unknown: failed to serialize object");

  return out;
}

static const char *
htsp_dvr_config_name( htsp_connection_t *htsp, const char *config_name )
{
  dvr_config_t *cfg = NULL, *cfg2;
  access_t *perm = htsp->htsp_granted_access;
  htsmsg_field_t *f;
  const char *uuid;

  lock_assert(&global_lock);

  config_name = config_name ?: "";

  if (perm->aa_dvrcfgs == NULL)
    return config_name; /* no change */

  HTSMSG_FOREACH(f, perm->aa_dvrcfgs) {
    uuid = htsmsg_field_get_str(f) ?: "";
    if (strcmp(uuid, config_name) == 0)
      return config_name;
    cfg2 = dvr_config_find_by_uuid(uuid);
    if (cfg2 && strcmp(cfg2->dvr_config_name, config_name) == 0)
      return uuid;
    if (!cfg)
      cfg = cfg2;
  }

  if (!cfg && perm->aa_username)
    tvhlog(LOG_INFO, "htsp", "User '%s' has no valid dvr config in ACL, using default...", perm->aa_username);

  return cfg ? idnode_uuid_as_str(&cfg->dvr_id) : NULL;
}

/**
 *
 */
static htsmsg_t *
htsp_method_getDvrConfigs(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out, *l, *c;
  htsmsg_field_t *f;
  dvr_config_t *cfg;
  const char *uuid, *s;

  l = htsmsg_create_list();

  LIST_FOREACH(cfg, &dvrconfigs, config_link)
    if (cfg->dvr_enabled) {
      uuid = idnode_uuid_as_str(&cfg->dvr_id);
      if (htsp->htsp_granted_access->aa_dvrcfgs) {
        HTSMSG_FOREACH(f, htsp->htsp_granted_access->aa_dvrcfgs) {
          if (!(s = htsmsg_field_get_str(f)))
            continue;
          if (strcmp(s, uuid) == 0)
            break;
        }
        if (f == NULL)
          continue;
      }
      c = htsmsg_create_map();
      htsmsg_add_str(c, "uuid", idnode_uuid_as_str(&cfg->dvr_id));
      htsmsg_add_str(c, "name", cfg->dvr_config_name ?: "");
      htsmsg_add_str(c, "comment", cfg->dvr_comment ?: "");
      htsmsg_add_msg(l, NULL, c);
    }

  out = htsmsg_create_map();

  htsmsg_add_msg(out, "dvrconfigs", l);

  return out;
}

/**
 * add a Dvrentry
 */
static htsmsg_t * 
htsp_method_addDvrEntry(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  uint32_t eventid;
  epg_broadcast_t *e = NULL;
  dvr_entry_t *de;
  dvr_entry_sched_state_t dvr_status;
  const char *dvr_config_name, *title, *desc, *creator, *lang, *comment;
  int64_t start, stop, start_extra, stop_extra;
  uint32_t u32, priority, retention;
  channel_t *ch = NULL;

  /* Options */
  dvr_config_name = htsp_dvr_config_name(htsp, htsmsg_get_str(in, "configName"));
  if(htsmsg_get_s64(in, "startExtra", &start_extra))
    start_extra = 0;
  if(htsmsg_get_s64(in, "stopExtra", &stop_extra))
    stop_extra  = 0;
  if(!htsmsg_get_u32(in, "channelId", &u32))
    ch = channel_find_by_id(u32);
  if(!htsmsg_get_u32(in, "eventId", &eventid)) {
    e = epg_broadcast_find_by_id(eventid);
    ch = e ? e->channel : ch;
  }
  if(htsmsg_get_u32(in, "priority", &priority))
    priority = DVR_PRIO_NORMAL;
  if(htsmsg_get_u32(in, "retention", &retention))
    retention = 0;
  comment = htsmsg_get_str(in, "comment");
  creator = htsp->htsp_username;
  if (!(lang        = htsmsg_get_str(in, "language")))
    lang    = htsp->htsp_language;

  /* Check access */
  if (ch && !htsp_user_access_channel(htsp, ch))
    return htsp_error("User does not have access");

  /* Manual timer */
  if (!e) {
    if (!ch)
      return htsp_error("Channel does not exist");

    /* Required attributes */
    if (htsmsg_get_s64(in, "start", &start) ||
        htsmsg_get_s64(in, "stop", &stop) ||
        !(title = htsmsg_get_str(in, "title")))
      return htsp_error("Invalid arguments");

    /* Optional attributes */
    if (!(desc = htsmsg_get_str(in, "description")))
      desc = "";

    // create the dvr entry
    de = dvr_entry_create_htsp(dvr_config_name, ch, start, stop,
                               start_extra, stop_extra,
                               title, desc, lang, 0,
                               htsp->htsp_granted_access->aa_username,
                               creator, NULL,
                               priority, retention, comment);

  /* Event timer */
  } else {
    de = dvr_entry_create_by_event(dvr_config_name, e, 
                                   start_extra, stop_extra,
                                   htsp->htsp_granted_access->aa_username,
                                   creator, NULL,
                                   priority, retention, comment);
  }

  dvr_status = de != NULL ? de->de_sched_state : DVR_NOSTATE;
  
  //create response
  out = htsmsg_create_map();
  
  switch(dvr_status) {
  case DVR_SCHEDULED:
  case DVR_RECORDING:
  case DVR_MISSED_TIME:
  case DVR_COMPLETED:
    htsmsg_add_u32(out, "id", idnode_get_short_uuid(&de->de_id));
    htsmsg_add_u32(out, "success", 1);
    break;  
  case DVR_NOSTATE:
    htsmsg_add_str(out, "error", "Could not add dvrEntry");
    htsmsg_add_u32(out, "success", 0);
    break;
  }
  return out;
}

/**
 * update a Dvrentry
 */
static htsmsg_t *
htsp_method_updateDvrEntry(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  uint32_t dvrEntryId;
  dvr_entry_t *de;
  time_t start, stop, start_extra, stop_extra, priority, retention;
  const char *title, *desc, *lang;
    
  if(htsmsg_get_u32(in, "id", &dvrEntryId))
    return htsp_error("Missing argument 'id'");
  
  if( (de = dvr_entry_find_by_id(dvrEntryId)) == NULL) 
    return htsp_error("id not found");

  /* Check access */
  if (!htsp_user_access_channel(htsp, de->de_channel))
    return htsp_error("User does not have access");

  start       = htsmsg_get_s64_or_default(in, "start",      0);
  stop        = htsmsg_get_s64_or_default(in, "stop",       0);
  start_extra = htsmsg_get_s64_or_default(in, "startExtra", 0);
  stop_extra  = htsmsg_get_s64_or_default(in, "stopExtra",  0);
  retention   = htsmsg_get_u32_or_default(in, "retention",  0);
  priority    = htsmsg_get_u32_or_default(in, "priority",   DVR_PRIO_NORMAL);
  title       = htsmsg_get_str(in, "title");
  desc        = htsmsg_get_str(in, "description");
  lang        = htsmsg_get_str(in, "language");
  if (!lang) lang = htsp->htsp_language;

  de = dvr_entry_update(de, title, desc, lang, start, stop,
                        start_extra, stop_extra, priority, retention);

  //create response
  out = htsmsg_create_map();
  htsmsg_add_u32(out, "success", 1);
  
  return out;
}

/**
 * cancel a Dvrentry
 */
static htsmsg_t *
htsp_method_cancelDvrEntry(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  uint32_t dvrEntryId;
  dvr_entry_t *de;

  if(htsmsg_get_u32(in, "id", &dvrEntryId))
    return htsp_error("Missing argument 'id'");

  if( (de = dvr_entry_find_by_id(dvrEntryId)) == NULL)
    return htsp_error("id not found");

  /* Check access */
  if (!htsp_user_access_channel(htsp, de->de_channel))
    return htsp_error("User does not have access");

  dvr_entry_cancel(de);

  //create response
  out = htsmsg_create_map();
  htsmsg_add_u32(out, "success", 1);

  return out;
}

/**
 * delete a Dvrentry
 */
static htsmsg_t *
htsp_method_deleteDvrEntry(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  uint32_t dvrEntryId;
  dvr_entry_t *de;

  if(htsmsg_get_u32(in, "id", &dvrEntryId))
    return htsp_error("Missing argument 'id'");

  if( (de = dvr_entry_find_by_id(dvrEntryId)) == NULL)
    return htsp_error("id not found");

  /* Check access */
  if (!htsp_user_access_channel(htsp, de->de_channel))
    return htsp_error("User does not have access");

  dvr_entry_cancel_delete(de);

  //create response
  out = htsmsg_create_map();
  htsmsg_add_u32(out, "success", 1);

  return out;
}

/**
 * add a Dvr autorec entry
 */
static htsmsg_t *
htsp_method_addAutorecEntry(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  dvr_autorec_entry_t *dae;
  const char *dvr_config_name, *title, *creator, *comment;
  int64_t start_extra, stop_extra;
  uint32_t u32, days_of_week, priority, min_duration, max_duration, retention;
  int32_t approx_time, start, start_window;
  channel_t *ch = NULL;

  /* Options */
  if(!(title = htsmsg_get_str(in, "title")))
    return htsp_error("Invalid arguments");
  dvr_config_name = htsp_dvr_config_name(htsp, htsmsg_get_str(in, "configName"));
  if(!htsmsg_get_u32(in, "channelId", &u32))
    ch = channel_find_by_id(u32);
  if(htsmsg_get_u32(in, "maxDuration", &max_duration))
    max_duration = 0;    // 0 = any
  if(htsmsg_get_u32(in, "minDuration", &min_duration))
    min_duration = 0;    // 0 = any
  if(htsmsg_get_u32(in, "retention", &retention))
    retention = 0;       // 0 = dvr config
  if(htsmsg_get_u32(in, "daysOfWeek", &days_of_week))
    days_of_week = 0x7f; // all days
  if(htsmsg_get_u32(in, "priority", &priority))
    priority = DVR_PRIO_NORMAL;
  if(htsmsg_get_s32(in, "approxTime", &approx_time))
    approx_time = -1;
  if(htsmsg_get_s32(in, "start", &start))
    start = -1;
  if(htsmsg_get_s32(in, "startWindow", &start_window))
    start_window = -1;
  if (start < 0 || start_window < 0)
    start = start_window = -1;
  if (start < 0 && approx_time >= 0) {
    start = approx_time - 15;
    if (start < 0)
      start += 24 * 60;
    start_window = start + 30;
    if (start_window >= 24 * 60)
      start_window -= 24 * 60;
  }
  if(htsmsg_get_s64(in, "startExtra", &start_extra))
    start_extra = 0;     // 0 = dvr config
  if(htsmsg_get_s64(in, "stopExtra", &stop_extra))
    stop_extra  = 0;     // 0 = dvr config
  creator = htsp->htsp_username;
  if (!(comment = htsmsg_get_str(in, "comment")))
    comment = "";

  /* Check access */
  if (ch && !htsp_user_access_channel(htsp, ch))
    return htsp_error("User does not have access");

  dae = dvr_autorec_create_htsp(dvr_config_name, title, ch, start, start_window, days_of_week,
      start_extra, stop_extra, priority, retention, min_duration, max_duration,
      htsp->htsp_granted_access->aa_username, creator, comment);

  /* create response */
  out = htsmsg_create_map();

  if (dae) {
    htsmsg_add_str(out, "id", idnode_uuid_as_str(&dae->dae_id));
    htsmsg_add_u32(out, "success", 1);
  }
  else {
    htsmsg_add_str(out, "error", "Could not add autorec entry");
    htsmsg_add_u32(out, "success", 0);
  }
  return out;
}

/**
 * delete a Dvr autorec entry
 */
static htsmsg_t *
htsp_method_deleteAutorecEntry(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  const char *daeId;
  dvr_autorec_entry_t *dae;

  if (!(daeId = htsmsg_get_str(in, "id")))
    return htsp_error("Missing argument 'id'");

  if((dae = dvr_autorec_find_by_uuid(daeId)) == NULL)
    return htsp_error("id not found");

  /* Check access */
  if (!htsp_user_access_channel(htsp, dae->dae_channel))
    return htsp_error("User does not have access");

  autorec_destroy_by_id(daeId, 1);
  
  /* create response */
  out = htsmsg_create_map();
  htsmsg_add_u32(out, "success", 1);

  return out;
}

/**
 * Return cutpoint data for a recording (if present). 
 *
 * Request message fields:
 * id                 u32    required   DVR entry id
 *
 * Result message fields:
 * cutpoints          msg[]  optional   List of cutpoint entries, if a file is 
 *                                      found and has some valid data.
 *
 * Cutpoint fields:
 * start              u32    required   Cut start time in ms.
 * end                u32    required   Cut end time in ms.
 * type               u32    required   Action type: 
 *                                      0=Cut, 1=Mute, 2=Scene, 
 *                                      3=Commercial break.
 **/
static htsmsg_t *
htsp_method_getDvrCutpoints(htsp_connection_t *htsp, htsmsg_t *in)
{
  uint32_t dvrEntryId;
  dvr_entry_t *de;
  if (htsmsg_get_u32(in, "id", &dvrEntryId))
    return htsp_error("Missing argument 'id'");

  if( (de = dvr_entry_find_by_id(dvrEntryId)) == NULL)
    return htsp_error("id not found");

  /* Check access */
  if (!htsp_user_access_channel(htsp, de->de_channel))
    return htsp_error("User does not have access");

  htsmsg_t *msg = htsmsg_create_map();

  dvr_cutpoint_list_t *list = dvr_get_cutpoint_list(de);

  if (list != NULL) {
    htsmsg_t *cutpoint_list = htsmsg_create_list();
    dvr_cutpoint_t *cp;
    TAILQ_FOREACH(cp, list, dc_link) {
      htsmsg_t *cutpoint = htsmsg_create_map();
      htsmsg_add_u32(cutpoint, "start", cp->dc_start_ms);
      htsmsg_add_u32(cutpoint, "end", cp->dc_end_ms);
      htsmsg_add_u32(cutpoint, "type", cp->dc_type);

      htsmsg_add_msg(cutpoint_list, NULL, cutpoint);
    }
    htsmsg_add_msg(msg, "cutpoints", cutpoint_list);
  }
  
  // Cleanup...
  dvr_cutpoint_list_destroy(list);

  return msg;
}

/**
 * Request a ticket for a http url pointing to a channel or dvr
 */
static htsmsg_t *
htsp_method_getTicket(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out;
  uint32_t id;
  char path[255];
  const char *ticket = NULL;
  channel_t *ch;
  dvr_entry_t *de;

  if(!htsmsg_get_u32(in, "channelId", &id)) {
    if (!(ch = channel_find_by_id(id)))
      return htsp_error("Invalid channelId");
    if (!htsp_user_access_channel(htsp, ch))
      return htsp_error("User does not have access");

    snprintf(path, sizeof(path), "/stream/channelid/%d", id);
    ticket = access_ticket_create(path, htsp->htsp_granted_access);
  } else if(!htsmsg_get_u32(in, "dvrId", &id)) {
    if (!(de = dvr_entry_find_by_id(id)))
      return htsp_error("DVR entry does not exist");
    if (!htsp_user_access_channel(htsp, de->de_channel))
      return htsp_error("User does not have access");

    snprintf(path, sizeof(path), "/dvrfile/%d", id);
    ticket = access_ticket_create(path, htsp->htsp_granted_access);
  } else {
    return htsp_error("Missing argument 'channelId' or 'dvrId'");
  }

  out = htsmsg_create_map();

  htsmsg_add_str(out, "path", path);
  htsmsg_add_str(out, "ticket", ticket);

  return out;
}

/**
 * Request subscription for a channel
 */
static htsmsg_t *
htsp_method_subscribe(htsp_connection_t *htsp, htsmsg_t *in)
{
  uint32_t chid, sid, weight, req90khz, timeshiftPeriod = 0;
  const char *str, *profile_id;
  channel_t *ch;
  htsp_subscription_t *hs;
  profile_t *pro;

  if(htsmsg_get_u32(in, "subscriptionId", &sid))
    return htsp_error("Missing argument 'subscriptionId'");

  if(!htsmsg_get_u32(in, "channelId", &chid)) {
    if((ch = channel_find_by_id(chid)) == NULL)
      return htsp_error("Requested channel does not exist");
  } else if((str = htsmsg_get_str(in, "channelName")) != NULL) {
    if((ch = channel_find_by_name(str)) == NULL)
      return htsp_error("Requested channel does not exist");
  } else {
    return htsp_error("Missing argument 'channelId' or 'channelName'");
  }
  if (!htsp_user_access_channel(htsp, ch))
    return htsp_error("User does not have access");

  weight = htsmsg_get_u32_or_default(in, "weight", 150);
  req90khz = htsmsg_get_u32_or_default(in, "90khz", 0);

  profile_id = htsmsg_get_str(in, "profile");

#if ENABLE_TIMESHIFT
  if (timeshift_enabled) {
    timeshiftPeriod = htsmsg_get_u32_or_default(in, "timeshiftPeriod", 0);
    if (!timeshift_unlimited_period)
      timeshiftPeriod = MIN(timeshiftPeriod, timeshift_max_period);
  }
#endif

  /* Initialize the HTSP subscription structure */

  hs = calloc(1, sizeof(htsp_subscription_t));

  hs->hs_htsp = htsp;
  hs->hs_90khz = req90khz;
  hs->hs_queue_depth = htsmsg_get_u32_or_default(in, "queueDepth",
						 HTSP_DEFAULT_QUEUE_DEPTH);
  htsp_init_queue(&hs->hs_q, 0);

  hs->hs_sid = sid;
  streaming_target_init(&hs->hs_input, htsp_streaming_input, hs, 0);

#if ENABLE_TIMESHIFT
  if (timeshiftPeriod != 0) {
    if (timeshiftPeriod == ~0)
      tvhlog(LOG_DEBUG, "htsp", "using timeshift buffer (unlimited)");
    else
      tvhlog(LOG_DEBUG, "htsp", "using timeshift buffer (%u mins)", timeshiftPeriod / 60);
  }
#endif

  pro = profile_find_by_list(htsp->htsp_granted_access->aa_profiles, profile_id, "htsp");
  profile_chain_init(&hs->hs_prch, pro, ch);
  if (profile_chain_work(&hs->hs_prch, &hs->hs_input, timeshiftPeriod, 0)) {
    tvhlog(LOG_ERR, "htsp", "unable to create profile chain '%s'", pro->pro_name);
    free(hs);
    return htsp_error("Stream setup error");
  }

  /*
   * We send the reply now to avoid the user getting the 'subscriptionStart'
   * async message before the reply to 'subscribe'.
   *
   * Send some optional boolean flags back to the subscriber so it can infer
   * if we support those
   *
   */
  htsmsg_t *rep = htsmsg_create_map();
  if(req90khz)
    htsmsg_add_u32(rep, "90khz", 1);
  if(hs->hs_prch.prch_sharer->prsh_tsfix)
    htsmsg_add_u32(rep, "normts", 1);

#if ENABLE_TIMESHIFT
  if(timeshiftPeriod)
    htsmsg_add_u32(rep, "timeshiftPeriod", timeshiftPeriod);
#endif

  htsp_reply(htsp, in, rep);

  /*
   * subscribe now...
   */
  LIST_INSERT_HEAD(&htsp->htsp_subscriptions, hs, hs_link);

  tvhdebug("htsp", "%s - subscribe to %s using profile %s\n",
           htsp->htsp_logname, channel_get_name(ch), pro->pro_name ?: "");
  hs->hs_s = subscription_create_from_channel(&hs->hs_prch, weight,
					      htsp->htsp_logname,
					      SUBSCRIPTION_STREAMING,
					      htsp->htsp_peername,
					      htsp->htsp_username,
					      htsp->htsp_clientname);
  return NULL;
}

/**
 * Request unsubscription for a channel
 */
static htsmsg_t *
htsp_method_unsubscribe(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_subscription_t *s;
  uint32_t sid;

  if(htsmsg_get_u32(in, "subscriptionId", &sid))
    return htsp_error("Missing argument 'subscriptionId'");

  LIST_FOREACH(s, &htsp->htsp_subscriptions, hs_link)
    if(s->hs_sid == sid)
      break;
  
  /*
   * We send the reply here or the user will get the 'subscriptionStop'
   * async message before the reply to 'unsubscribe'.
   */
  htsp_reply(htsp, in, htsmsg_create_map());

  if(s == NULL)
    return NULL; /* Subscription did not exist, but we don't really care */

  htsp_subscription_destroy(htsp, s);
  return NULL;
}

/**
 * Change weight for a subscription
 */
static htsmsg_t *
htsp_method_change_weight(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_subscription_t *hs;
  uint32_t sid, weight;

  if(htsmsg_get_u32(in, "subscriptionId", &sid))
    return htsp_error("Missing argument 'subscriptionId'");

  weight = htsmsg_get_u32_or_default(in, "weight", 150);

  LIST_FOREACH(hs, &htsp->htsp_subscriptions, hs_link)
    if(hs->hs_sid == sid)
      break;
  
  if(hs == NULL)
    return htsp_error("Requested subscription does not exist");

  htsp_reply(htsp, in, htsmsg_create_map());

  subscription_change_weight(hs->hs_s, weight);
  return NULL;
}

/**
 * Skip stream
 */
static htsmsg_t *
htsp_method_skip(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_subscription_t *hs;
  uint32_t sid, abs;
  int64_t s64;
  streaming_skip_t skip;

  if(htsmsg_get_u32(in, "subscriptionId", &sid))
    return htsp_error("Missing argument 'subscriptionId'");

  LIST_FOREACH(hs, &htsp->htsp_subscriptions, hs_link)
    if(hs->hs_sid == sid)
      break;

  if(hs == NULL)
    return htsp_error("Requested subscription does not exist");

  abs = htsmsg_get_u32_or_default(in, "absolute", 0);

  if(!htsmsg_get_s64(in, "time", &s64)) {
    skip.type = abs ? SMT_SKIP_ABS_TIME : SMT_SKIP_REL_TIME;
    skip.time = hs->hs_90khz ? s64 : ts_rescale_i(s64, 1000000);
    tvhtrace("htsp", "skip: %s %"PRId64" (%s)\n", abs ? "abs" : "rel",
             skip.time, hs->hs_90khz ? "90kHz" : "1MHz");
  } else if (!htsmsg_get_s64(in, "size", &s64)) {
    skip.type = abs ? SMT_SKIP_ABS_SIZE : SMT_SKIP_REL_SIZE;
    skip.size = s64;
    tvhtrace("htsp", "skip: %s by size %"PRId64, abs ? "abs" : "rel", s64);
  } else {
    return htsp_error("Missing argument 'time' or 'size'");
  }

  subscription_set_skip(hs->hs_s, &skip);

  htsp_reply(htsp, in, htsmsg_create_map());
  return NULL;
}

/*
 * Set stream speed
 */
static htsmsg_t *
htsp_method_speed(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_subscription_t *hs;
  uint32_t sid;
  int32_t speed;

  if(htsmsg_get_u32(in, "subscriptionId", &sid))
    return htsp_error("Missing argument 'subscriptionId'");
  if(htsmsg_get_s32(in, "speed", &speed))
    return htsp_error("Missing argument 'speed'");

  LIST_FOREACH(hs, &htsp->htsp_subscriptions, hs_link)
    if(hs->hs_sid == sid)
      break;

  if(hs == NULL)
    return htsp_error("Requested subscription does not exist");

  tvhtrace("htsp", "speed: %d", speed);
  subscription_set_speed(hs->hs_s, speed);

  htsp_reply(htsp, in, htsmsg_create_map());
  return NULL;
}

/*
 * Revert to live
 */
static htsmsg_t *
htsp_method_live(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_subscription_t *hs;
  uint32_t sid;
  streaming_skip_t skip;

  if(htsmsg_get_u32(in, "subscriptionId", &sid))
    return htsp_error("Missing argument 'subscriptionId'");

  LIST_FOREACH(hs, &htsp->htsp_subscriptions, hs_link)
    if(hs->hs_sid == sid)
      break;

  if(hs == NULL)
    return htsp_error("Requested subscription does not exist");

  skip.type = SMT_SKIP_LIVE;
  tvhtrace("htsp", "live");
  subscription_set_skip(hs->hs_s, &skip);

  htsp_reply(htsp, in, htsmsg_create_map());
  return NULL;
}

/**
 * Change filters for a subscription
 */
static htsmsg_t *
htsp_method_filter_stream(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_subscription_t *hs;
  uint32_t sid;
  htsmsg_t *l;
  if(htsmsg_get_u32(in, "subscriptionId", &sid))
    return htsp_error("Missing argument 'subscriptionId'");

  LIST_FOREACH(hs, &htsp->htsp_subscriptions, hs_link)
    if(hs->hs_sid == sid)
      break;

  if(hs == NULL)
    return htsp_error("Requested subscription does not exist");

  if((l = htsmsg_get_list(in, "enable")) != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, l) {
      if(f->hmf_type == HMF_S64)
        htsp_enable_stream(hs, f->hmf_s64);
    }
  }

  if((l = htsmsg_get_list(in, "disable")) != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, l) {
      if(f->hmf_type == HMF_S64)
        htsp_disable_stream(hs, f->hmf_s64);
    }
  }
  return htsmsg_create_map();
}


/**
 * Open file
 */
static htsmsg_t *
htsp_method_file_open(htsp_connection_t *htsp, htsmsg_t *in)
{
  const char *str, *s2;
  const char *filename = NULL;
  if((str = htsmsg_get_str(in, "file")) == NULL)
    return htsp_error("Missing argument 'file'");

  // optional leading slash
  if (*str == '/')
    str++;

  if((s2 = tvh_strbegins(str, "dvr/")) != NULL ||
     (s2 = tvh_strbegins(str, "dvrfile/")) != NULL) {
    dvr_entry_t *de = dvr_entry_find_by_id(atoi(s2));
    if(de == NULL)
      return htsp_error("DVR entry does not exist");

    if (!htsp_user_access_channel(htsp, de->de_channel))
      return htsp_error("User does not have access");

    if(de->de_filename == NULL)
      return htsp_error("DVR entry does not have a file yet");

    filename = de->de_filename;
    return htsp_file_open(htsp, filename, 0);

  } else if ((s2 = tvh_strbegins(str, "imagecache/")) != NULL) {
    int fd = imagecache_open(atoi(s2));
    if (fd < 0)
      return htsp_error("failed to open image");
    return htsp_file_open(htsp, str, fd);

  } else {
    return htsp_error("Unknown file");
  }
}

/**
 *
 */
static htsmsg_t *
htsp_method_file_read(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_file_t *hf = htsp_file_find(htsp, in);
  htsmsg_t *rep = NULL;
  const char *e = NULL;
  int64_t off;
  int64_t size;
  int fd;

  if(hf == NULL)
    return htsp_error("Unknown file id");

  if(htsmsg_get_s64(in, "size", &size))
    return htsp_error("Missing field 'size'");

  fd = hf->hf_fd;

  pthread_mutex_unlock(&global_lock);

  /* Seek (optional) */
  if (!htsmsg_get_s64(in, "offset", &off))
    if(lseek(fd, off, SEEK_SET) != off) {
      e = "Seek error";
      goto error;
    }

  /* Read */
  void *m = malloc(size);
  if(m == NULL) {
    e = "Too big segment";
    goto error;
  }

  int r = read(fd, m, size);
  if(r < 0) {
    free(m);
    e = "Read error";
    goto error;
  }

  rep = htsmsg_create_map();
  htsmsg_add_bin(rep, "data", m, r);
  free(m);

error:
  pthread_mutex_lock(&global_lock);
  return e ? htsp_error(e) : rep;
}

/**
 *
 */
static htsmsg_t *
htsp_method_file_close(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_file_t *hf = htsp_file_find(htsp, in);

  if(hf == NULL)
    return htsp_error("Unknown file id");

  htsp_file_destroy(hf);
  return htsmsg_create_map();
}

/**
 *
 */
static htsmsg_t *
htsp_method_file_stat(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_file_t *hf = htsp_file_find(htsp, in);
  htsmsg_t *rep;
  struct stat st;
  int fd;

  if(hf == NULL)
    return htsp_error("Unknown file id");

  fd = hf->hf_fd;

  pthread_mutex_unlock(&global_lock);
  rep = htsmsg_create_map();
  if(!fstat(fd, &st)) {
    htsmsg_add_s64(rep, "size", st.st_size);
    htsmsg_add_s64(rep, "mtime", st.st_mtime);
  }
  pthread_mutex_lock(&global_lock);

  return rep;
}

/**
 *
 */
static htsmsg_t *
htsp_method_file_seek(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsp_file_t *hf = htsp_file_find(htsp, in);
  htsmsg_t *rep;
  const char *str;
  int64_t off;
  int fd, whence;

  if(hf == NULL)
    return htsp_error("Unknown file id");

  if (htsmsg_get_s64(in, "offset", &off))
    return htsp_error("Missing field 'offset'");

  if ((str = htsmsg_get_str(in, "whence"))) {
    if (!strcmp(str, "SEEK_SET"))
      whence = SEEK_SET;
    else if (!strcmp(str, "SEEK_CUR"))
      whence = SEEK_CUR;
    else if (!strcmp(str, "SEEK_END"))
      whence = SEEK_END;
    else
      return htsp_error("Field 'whence' contained invalid value");
  } else {
    whence = SEEK_SET;
  }

  fd = hf->hf_fd;
  pthread_mutex_unlock(&global_lock);

  if ((off = lseek(fd, off, whence)) < 0) {
    pthread_mutex_lock(&global_lock);
    return htsp_error("Seek error");
  }

  rep = htsmsg_create_map();
  htsmsg_add_s64(rep, "offset", off);

  pthread_mutex_lock(&global_lock);
  return rep;
}

/**
 *
 */
static htsmsg_t *
htsp_method_getProfiles(htsp_connection_t *htsp, htsmsg_t *in)
{
  htsmsg_t *out, *l;

  l = htsmsg_create_list();
  profile_get_htsp_list(l, htsp->htsp_granted_access->aa_profiles);

  out = htsmsg_create_map();

  htsmsg_add_msg(out, "profiles", l);

  return out;
}

/**
 * HTSP methods
 */
struct {
  const char *name;
  htsmsg_t *(*fn)(htsp_connection_t *htsp, htsmsg_t *in);
  int privmask;
} htsp_methods[] = {
  { "hello",                    htsp_method_hello,              ACCESS_ANONYMOUS},
  { "authenticate",             htsp_method_authenticate,       ACCESS_ANONYMOUS},
  { "getDiskSpace",             htsp_method_getDiskSpace,       ACCESS_HTSP_STREAMING},
  { "getSysTime",               htsp_method_getSysTime,         ACCESS_HTSP_STREAMING},
  { "enableAsyncMetadata",      htsp_method_async,              ACCESS_HTSP_STREAMING},
  { "getChannel",               htsp_method_getChannel,         ACCESS_HTSP_STREAMING},
  { "getEvent",                 htsp_method_getEvent,           ACCESS_HTSP_STREAMING},
  { "getEvents",                htsp_method_getEvents,          ACCESS_HTSP_STREAMING},
  { "epgQuery",                 htsp_method_epgQuery,           ACCESS_HTSP_STREAMING},
  { "getEpgObject",             htsp_method_getEpgObject,       ACCESS_HTSP_STREAMING},
  { "getDvrConfigs",            htsp_method_getDvrConfigs,      ACCESS_HTSP_RECORDER},
  { "addDvrEntry",              htsp_method_addDvrEntry,        ACCESS_HTSP_RECORDER},
  { "updateDvrEntry",           htsp_method_updateDvrEntry,     ACCESS_HTSP_RECORDER},
  { "cancelDvrEntry",           htsp_method_cancelDvrEntry,     ACCESS_HTSP_RECORDER},
  { "deleteDvrEntry",           htsp_method_deleteDvrEntry,     ACCESS_HTSP_RECORDER},
  { "addAutorecEntry",          htsp_method_addAutorecEntry,    ACCESS_HTSP_RECORDER},
  { "deleteAutorecEntry",       htsp_method_deleteAutorecEntry, ACCESS_HTSP_RECORDER},
  { "getDvrCutpoints",          htsp_method_getDvrCutpoints,    ACCESS_HTSP_RECORDER},
  { "getTicket",                htsp_method_getTicket,          ACCESS_HTSP_STREAMING},
  { "subscribe",                htsp_method_subscribe,          ACCESS_HTSP_STREAMING},
  { "unsubscribe",              htsp_method_unsubscribe,        ACCESS_HTSP_STREAMING},
  { "subscriptionChangeWeight", htsp_method_change_weight,      ACCESS_HTSP_STREAMING},
  { "subscriptionSeek",         htsp_method_skip,               ACCESS_HTSP_STREAMING},
  { "subscriptionSkip",         htsp_method_skip,               ACCESS_HTSP_STREAMING},
  { "subscriptionSpeed",        htsp_method_speed,              ACCESS_HTSP_STREAMING},
  { "subscriptionLive",         htsp_method_live,               ACCESS_HTSP_STREAMING},
  { "subscriptionFilterStream", htsp_method_filter_stream,      ACCESS_HTSP_STREAMING},
  { "getProfiles",              htsp_method_getProfiles,        ACCESS_HTSP_STREAMING},
  { "fileOpen",                 htsp_method_file_open,          ACCESS_HTSP_RECORDER},
  { "fileRead",                 htsp_method_file_read,          ACCESS_HTSP_RECORDER},
  { "fileClose",                htsp_method_file_close,         ACCESS_HTSP_RECORDER},
  { "fileStat",                 htsp_method_file_stat,          ACCESS_HTSP_RECORDER},
  { "fileSeek",                 htsp_method_file_seek,          ACCESS_HTSP_RECORDER},
};

#define NUM_METHODS (sizeof(htsp_methods) / sizeof(htsp_methods[0]))

/* **************************************************************************
 * Message processing
 * *************************************************************************/

/**
 * Raise privs by field in message
 */
static int
htsp_authenticate(htsp_connection_t *htsp, htsmsg_t *m)
{
  const char *username;
  const void *digest;
  size_t digestlen;
  access_t *rights;
  int privgain;

  if((username = htsmsg_get_str(m, "username")) == NULL)
    return 0;

  if(strcmp(htsp->htsp_username ?: "", username)) {
    tvhlog(LOG_INFO, "htsp", "%s: Identified as user %s", 
	   htsp->htsp_logname, username);
    tvh_str_update(&htsp->htsp_username, username);
    htsp_update_logname(htsp);
    notify_reload("connections");
  }

  if(htsmsg_get_bin(m, "digest", &digest, &digestlen))
    return 0;

  rights = access_get_hashed(username, digest, htsp->htsp_challenge,
			     (struct sockaddr *)htsp->htsp_peer);

  privgain = (rights->aa_rights |
              htsp->htsp_granted_access->aa_rights) !=
                htsp->htsp_granted_access->aa_rights;
    
  if(privgain)
    tvhlog(LOG_INFO, "htsp", "%s: Privileges raised", htsp->htsp_logname);

  access_destroy(htsp->htsp_granted_access);
  htsp->htsp_granted_access = rights;
  return privgain;
}

/**
 * timeout is in ms, 0 means infinite timeout
 */
static int
htsp_read_message(htsp_connection_t *htsp, htsmsg_t **mp, int timeout)
{
  int v;
  uint32_t len;
  uint8_t data[4];
  void *buf;

  v = timeout ? tcp_read_timeout(htsp->htsp_fd, data, 4, timeout) : 
    tcp_read(htsp->htsp_fd, data, 4);

  if(v != 0)
    return v;

  len = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
  if(len > 1024 * 1024)
    return EMSGSIZE;
  if((buf = malloc(len)) == NULL)
    return ENOMEM;

  v = timeout ? tcp_read_timeout(htsp->htsp_fd, buf, len, timeout) : 
    tcp_read(htsp->htsp_fd, buf, len);
  
  if(v != 0) {
    free(buf);
    return v;
  }

  /* buf will be tied to the message.
   * NB: If the message can not be deserialized buf will be free'd by the
   * function.
   */
  *mp = htsmsg_binary_deserialize(buf, len, buf);
  if(*mp == NULL)
    return EBADMSG;

  return 0;
}

/*
 * Status callback
 */
static void
htsp_server_status ( void *opaque, htsmsg_t *m )
{
  htsp_connection_t *htsp = opaque;
  htsmsg_add_str(m, "type", "HTSP");
  if (htsp->htsp_username)
    htsmsg_add_str(m, "user", htsp->htsp_username);
}

/**
 *
 */
static int
htsp_read_loop(htsp_connection_t *htsp)
{
  htsmsg_t *m = NULL, *reply;
  int r, i;
  const char *method;
  void *tcp_id = NULL;;

  if(htsp_generate_challenge(htsp)) {
    tvhlog(LOG_ERR, "htsp", "%s: Unable to generate challenge",
	   htsp->htsp_logname);
    return 1;
  }

  pthread_mutex_lock(&global_lock);

  htsp->htsp_granted_access = 
    access_get_by_addr((struct sockaddr *)htsp->htsp_peer);

  tcp_id = tcp_connection_launch(htsp->htsp_fd, htsp_server_status,
                                 htsp->htsp_granted_access);

  pthread_mutex_unlock(&global_lock);

  if (tcp_id == NULL)
    return 0;

  tvhlog(LOG_INFO, "htsp", "Got connection from %s", htsp->htsp_logname);

  /* Session main loop */

  while(tvheadend_running) {
readmsg:
    if((r = htsp_read_message(htsp, &m, 0)) != 0)
      break;

    pthread_mutex_lock(&global_lock);
    if (htsp_authenticate(htsp, m)) {
      tcp_connection_land(tcp_id);
      tcp_id = tcp_connection_launch(htsp->htsp_fd, htsp_server_status,
                                     htsp->htsp_granted_access);
      if (tcp_id == NULL) {
        htsmsg_destroy(m);
        pthread_mutex_unlock(&global_lock);
        return 1;
      }
    }

    if((method = htsmsg_get_str(m, "method")) != NULL) {
      tvhtrace("htsp", "%s - method %s", htsp->htsp_logname, method);
      for(i = 0; i < NUM_METHODS; i++) {
        if(!strcmp(method, htsp_methods[i].name)) {

          if((htsp->htsp_granted_access->aa_rights &
              htsp_methods[i].privmask) !=
                htsp_methods[i].privmask) {

      	    pthread_mutex_unlock(&global_lock);
            /* Classic authentication failed delay */
            usleep(250000);

            reply = htsmsg_create_map();
            htsmsg_add_u32(reply, "noaccess", 1);
            htsp_reply(htsp, m, reply);

            htsmsg_destroy(m);
            goto readmsg;

          } else {
            reply = htsp_methods[i].fn(htsp, m);
          }
          break;
        }
      }

      if(i == NUM_METHODS) {
        reply = htsp_error("Method not found");
      }

    } else {
      reply = htsp_error("No 'method' argument");
    }

    pthread_mutex_unlock(&global_lock);

    if(reply != NULL) /* Methods can do all the replying inline */
      htsp_reply(htsp, m, reply);

    htsmsg_destroy(m);
  }

  pthread_mutex_lock(&global_lock);
  tcp_connection_land(tcp_id);
  pthread_mutex_unlock(&global_lock);
  return tvheadend_running ? r : 0;
}

/**
 *
 */
static void *
htsp_write_scheduler(void *aux)
{
  htsp_connection_t *htsp = aux;
  htsp_msg_q_t *hmq;
  htsp_msg_t *hm;
  void *dptr;
  size_t dlen;
  int r;

  pthread_mutex_lock(&htsp->htsp_out_mutex);

  while(htsp->htsp_writer_run) {

    if((hmq = TAILQ_FIRST(&htsp->htsp_active_output_queues)) == NULL) {
      /* Nothing to be done, go to sleep */
      pthread_cond_wait(&htsp->htsp_out_cond, &htsp->htsp_out_mutex);
      continue;
    }

    hm = TAILQ_FIRST(&hmq->hmq_q);
    TAILQ_REMOVE(&hmq->hmq_q, hm, hm_link);
    hmq->hmq_length--;
    hmq->hmq_payload -= hm->hm_payloadsize;

    TAILQ_REMOVE(&htsp->htsp_active_output_queues, hmq, hmq_link);
    if(hmq->hmq_length) {
      /* Still messages to be sent, put back in active queues */
      if(hmq->hmq_strict_prio) {
        TAILQ_INSERT_HEAD(&htsp->htsp_active_output_queues, hmq, hmq_link);
      } else {
        TAILQ_INSERT_TAIL(&htsp->htsp_active_output_queues, hmq, hmq_link);
      }
    }

    pthread_mutex_unlock(&htsp->htsp_out_mutex);

    if (htsmsg_binary_serialize(hm->hm_msg, &dptr, &dlen, INT32_MAX) != 0) {
      tvhlog(LOG_WARNING, "htsp", "%s: failed to serialize data",
             htsp->htsp_logname);
      htsp_msg_destroy(hm);
      pthread_mutex_lock(&htsp->htsp_out_mutex);
      continue;
    }

    htsp_msg_destroy(hm);

    r = tvh_write(htsp->htsp_fd, dptr, dlen);
    free(dptr);
    pthread_mutex_lock(&htsp->htsp_out_mutex);
    
    if (r) {
      tvhlog(LOG_INFO, "htsp", "%s: Write error -- %s",
             htsp->htsp_logname, strerror(errno));
      break;
    }
  }
  // Shutdown socket to make receive thread terminate entire HTSP connection

  shutdown(htsp->htsp_fd, SHUT_RDWR);
  pthread_mutex_unlock(&htsp->htsp_out_mutex);
  return NULL;
}

/**
 *
 */
static void
htsp_serve(int fd, void **opaque, struct sockaddr_storage *source,
	   struct sockaddr_storage *self)
{
  htsp_connection_t htsp;
  char buf[50];
  htsp_subscription_t *s;
  
  // Note: global_lock held on entry

  tcp_get_ip_str((struct sockaddr*)source, buf, 50);

  memset(&htsp, 0, sizeof(htsp_connection_t));
  *opaque = &htsp;

  TAILQ_INIT(&htsp.htsp_active_output_queues);

  htsp_init_queue(&htsp.htsp_hmq_ctrl, 0);
  htsp_init_queue(&htsp.htsp_hmq_qstatus, 1);
  htsp_init_queue(&htsp.htsp_hmq_epg, 0);

  htsp.htsp_peername = strdup(buf);
  htsp_update_logname(&htsp);

  htsp.htsp_fd = fd;
  htsp.htsp_peer = source;
  htsp.htsp_writer_run = 1;

  LIST_INSERT_HEAD(&htsp_connections, &htsp, htsp_link);
  pthread_mutex_unlock(&global_lock);

  tvhthread_create(&htsp.htsp_writer_thread, NULL,
                   htsp_write_scheduler, &htsp);

  /**
   * Reader loop
   */

  htsp_read_loop(&htsp);

  tvhlog(LOG_INFO, "htsp", "%s: Disconnected", htsp.htsp_logname);

  /**
   * Ok, we're back, other end disconnected. Clean up stuff.
   */

  pthread_mutex_lock(&global_lock);

  /* no async notifications from now */
  if(htsp.htsp_async_mode)
    LIST_REMOVE(&htsp, htsp_async_link);

  /* deregister this client */
  LIST_REMOVE(&htsp, htsp_link);

  /* Beware! Closing subscriptions will invoke a lot of callbacks
     down in the streaming code. So we do this as early as possible
     to avoid any weird lockups */
  while((s = LIST_FIRST(&htsp.htsp_subscriptions)) != NULL)
    htsp_subscription_destroy(&htsp, s);

  pthread_mutex_unlock(&global_lock);

  pthread_mutex_lock(&htsp.htsp_out_mutex);
  htsp.htsp_writer_run = 0;
  pthread_cond_signal(&htsp.htsp_out_cond);
  pthread_mutex_unlock(&htsp.htsp_out_mutex);

  pthread_join(htsp.htsp_writer_thread, NULL);

  while((s = LIST_FIRST(&htsp.htsp_dead_subscriptions)) != NULL)
    htsp_subscription_free(&htsp, s);

  htsp_msg_q_t *hmq;

  TAILQ_FOREACH(hmq, &htsp.htsp_active_output_queues, hmq_link) {
    htsp_msg_t *hm;
    while((hm = TAILQ_FIRST(&hmq->hmq_q)) != NULL) {
      TAILQ_REMOVE(&hmq->hmq_q, hm, hm_link);
      htsp_msg_destroy(hm);
    }
  }

  htsp_file_t *hf;
  while((hf = LIST_FIRST(&htsp.htsp_files)) != NULL)
    htsp_file_destroy(hf);

  close(fd);
  
  /* Free memory (leave lock in place, for parent method) */
  pthread_mutex_lock(&global_lock);
  free(htsp.htsp_logname);
  free(htsp.htsp_peername);
  free(htsp.htsp_username);
  free(htsp.htsp_clientname);
  access_destroy(htsp.htsp_granted_access);
  *opaque = NULL;
}

/*
 * Cancel callback
 */
static void
htsp_server_cancel ( void *opaque )
{
  htsp_connection_t *htsp = opaque;

  if (htsp)
    shutdown(htsp->htsp_fd, SHUT_RDWR);
}

/**
 *  Fire up HTSP server
 */
void
htsp_init(const char *bindaddr)
{
  extern int tvheadend_htsp_port_extra;
  static tcp_server_ops_t ops = {
    .start  = htsp_serve,
    .stop   = NULL,
    .cancel = htsp_server_cancel
  };
  htsp_server = tcp_server_create(bindaddr, tvheadend_htsp_port, &ops, NULL);
  if(tvheadend_htsp_port_extra)
    htsp_server_2 = tcp_server_create(bindaddr, tvheadend_htsp_port_extra, &ops, NULL);
}

/*
 *
 */
void
htsp_register(void)
{
  tcp_server_register(htsp_server);
  if (htsp_server_2)
    tcp_server_register(htsp_server_2);
}

/**
 *  Fire down HTSP server
 */
void
htsp_done(void)
{
  if (htsp_server_2)
    tcp_server_delete(htsp_server_2);
  if (htsp_server)
    tcp_server_delete(htsp_server);
}

/* **************************************************************************
 * Asynchronous updates
 * *************************************************************************/

/**
 *
 */
static void
htsp_async_send(htsmsg_t *m, int mode, int aux_type, void *aux)
{
  htsp_connection_t *htsp;

  lock_assert(&global_lock);
  LIST_FOREACH(htsp, &htsp_async_connections, htsp_async_link)
    if (htsp->htsp_async_mode & mode) {
      if (aux_type == HTSP_ASYNC_AUX_CHTAG &&
          !channel_tag_access(aux, htsp->htsp_granted_access, 0))
        continue;
      htsp_send_message(htsp, htsmsg_copy(m), NULL);
    }
  htsmsg_destroy(m);
}

/**
 * Called from channel.c when a new channel is created
 */
static void
_htsp_channel_update(channel_t *ch, const char *method, htsmsg_t *msg)
{
  htsp_connection_t *htsp;
  LIST_FOREACH(htsp, &htsp_async_connections, htsp_async_link) {
    if (htsp->htsp_async_mode & HTSP_ASYNC_ON &&
        htsp_user_access_channel(htsp,ch)) {
      htsmsg_t *m = msg ? htsmsg_copy(msg)
                        : htsp_build_channel(ch, method, htsp);
      htsp_send_message(htsp, m, NULL);
    }
  }
  htsmsg_destroy(msg);
}

/**
 * EPG subsystem calls this function when the current/next event
 * changes for a channel, e may be NULL if there is no current event.
 *
 * global_lock is held
 */
void
htsp_channel_update_nownext(channel_t *ch)
{
  epg_broadcast_t *now, *next;
  htsmsg_t *m;

  m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "channelUpdate");
  htsmsg_add_u32(m, "channelId", channel_get_id(ch));

  now  = ch->ch_epg_now;
  next = ch->ch_epg_next;
  htsmsg_add_u32(m, "eventId",     now  ? now->id : 0);
  htsmsg_add_u32(m, "nextEventId", next ? next->id : 0);
  _htsp_channel_update(ch, NULL, m);
}

void
htsp_channel_add(channel_t *ch)
{
  _htsp_channel_update(ch, "channelAdd", NULL);
}

/**
 * Called from channel.c when a channel is updated
 */
void
htsp_channel_update(channel_t *ch)
{
  _htsp_channel_update(ch, "channelUpdate", NULL);
}

/**
 * Called from channel.c when a channel is deleted
 */
void
htsp_channel_delete(channel_t *ch)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_u32(m, "channelId", channel_get_id(ch));
  htsmsg_add_str(m, "method", "channelDelete");
  _htsp_channel_update(ch, NULL, m);
}


/**
 * Called from channel.c when a tag is exported
 */
void
htsp_tag_add(channel_tag_t *ct)
{
  htsp_async_send(htsp_build_tag(ct, "tagAdd", 1), HTSP_ASYNC_ON,
                  HTSP_ASYNC_AUX_CHTAG, ct);
}


/**
 * Called from channel.c when an exported tag is changed
 */
void
htsp_tag_update(channel_tag_t *ct)
{
  htsp_async_send(htsp_build_tag(ct, "tagUpdate", 1), HTSP_ASYNC_ON,
                  HTSP_ASYNC_AUX_CHTAG, ct);
}


/**
 * Called from channel.c when an exported tag is deleted
 */
void
htsp_tag_delete(channel_tag_t *ct)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_u32(m, "tagId", htsp_channel_tag_get_identifier(ct));
  htsmsg_add_str(m, "method", "tagDelete");
  htsp_async_send(m, HTSP_ASYNC_ON, HTSP_ASYNC_AUX_CHTAG, ct);
}

/**
 * Called when a DVR entry is updated/added
 */
static void
_htsp_dvr_entry_update(dvr_entry_t *de, const char *method, htsmsg_t *msg)
{
  htsp_connection_t *htsp;
  LIST_FOREACH(htsp, &htsp_async_connections, htsp_async_link) {
    if (htsp->htsp_async_mode & HTSP_ASYNC_ON &&
        htsp_user_access_channel(htsp, de->de_channel)) {
      htsmsg_t *m = msg ? htsmsg_copy(msg)
                        : htsp_build_dvrentry(de, method);
      htsp_send_message(htsp, m, NULL);
    }
  }
  htsmsg_destroy(msg);
}

/**
 * Called from dvr_db.c when a DVR entry is created
 */
void
htsp_dvr_entry_add(dvr_entry_t *de)
{
  _htsp_dvr_entry_update(de, "dvrEntryAdd", NULL);
}


/**
 * Called from dvr_db.c when a DVR entry is updated
 */
void
htsp_dvr_entry_update(dvr_entry_t *de)
{
  _htsp_dvr_entry_update(de, "dvrEntryUpdate", NULL);
}


/**
 * Called from dvr_db.c when a DVR entry is deleted
 */
void
htsp_dvr_entry_delete(dvr_entry_t *de)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_u32(m, "id", idnode_get_short_uuid(&de->de_id));
  htsmsg_add_str(m, "method", "dvrEntryDelete");
  htsp_async_send(m, HTSP_ASYNC_ON, HTSP_ASYNC_AUX_DVR, de);
}

/**
 * Called when a autorec entry is updated/added
 */
static void
_htsp_autorec_entry_update(dvr_autorec_entry_t *dae, const char *method, htsmsg_t *msg)
{
  htsp_connection_t *htsp;
  LIST_FOREACH(htsp, &htsp_async_connections, htsp_async_link) {
    if (htsp->htsp_async_mode & HTSP_ASYNC_ON) {
      if (dae->dae_channel == NULL || htsp_user_access_channel(htsp, dae->dae_channel)) {
        htsmsg_t *m = msg ? htsmsg_copy(msg)
                          : htsp_build_autorecentry(dae, method);
        htsp_send_message(htsp, m, NULL);
      }
    }
  }
  htsmsg_destroy(msg);
}

/**
 * Called from dvr_autorec.c when a autorec entry is added
 */
void
htsp_autorec_entry_add(dvr_autorec_entry_t *dae)
{
  _htsp_autorec_entry_update(dae, "autorecEntryAdd", NULL);
}

/**
 * Called from dvr_autorec.c when a autorec entry is updated
 */
void
htsp_autorec_entry_update(dvr_autorec_entry_t *dae)
{
  _htsp_autorec_entry_update(dae, "autorecEntryUpdate", NULL);
}

/**
 * Called from dvr_autorec.c when a autorec entry is deleted
 */
void
htsp_autorec_entry_delete(dvr_autorec_entry_t *dae)
{
  if(dae == NULL)
    return;

  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "id", idnode_uuid_as_str(&dae->dae_id));
  htsmsg_add_str(m, "method", "autorecEntryDelete");
  htsp_async_send(m, HTSP_ASYNC_ON, HTSP_ASYNC_AUX_AUTOREC, dae);
}

/**
 * Called when a event entry is updated/added
 */
static void
_htsp_event_update(epg_broadcast_t *ebc, const char *method, htsmsg_t *msg)
{
  htsp_connection_t *htsp;
  LIST_FOREACH(htsp, &htsp_async_connections, htsp_async_link) {
    if (htsp->htsp_async_mode & HTSP_ASYNC_EPG &&
        htsp_user_access_channel(htsp,ebc->channel)) {
      htsmsg_t *m = msg ? htsmsg_copy(msg)
                        : htsp_build_event(ebc, method, htsp->htsp_language,
                                           0, htsp);
      htsp_send_message(htsp, m, NULL);
    }
  }
  htsmsg_destroy(msg);
}

/**
 * Event added
 */
void
htsp_event_add(epg_broadcast_t *ebc)
{
  _htsp_event_update(ebc, "eventAdd", NULL);
}

/**
 * Event updated
 */
void
htsp_event_update(epg_broadcast_t *ebc)
{
  _htsp_event_update(ebc, "eventUpdate", NULL);
}

/**
 * Event deleted
 */
void
htsp_event_delete(epg_broadcast_t *ebc)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "eventDelete");
  htsmsg_add_u32(m, "eventId", ebc->id);
  _htsp_event_update(ebc, NULL, m);
}

const static char frametypearray[PKT_NTYPES] = {
  [PKT_I_FRAME] = 'I',
  [PKT_P_FRAME] = 'P',
  [PKT_B_FRAME] = 'B',
};

/**
 * Build a htsmsg from a th_pkt and enqueue it on our HTSP service
 */
static void
htsp_stream_deliver(htsp_subscription_t *hs, th_pkt_t *pkt)
{
  htsmsg_t *m;
  htsp_msg_t *hm;
  htsp_connection_t *htsp = hs->hs_htsp;
  int64_t ts;
  int qlen = hs->hs_q.hmq_payload;
  size_t payloadlen;

  if(!htsp_is_stream_enabled(hs, pkt->pkt_componentindex)) {
    pkt_ref_dec(pkt);
    return;
  }

  if((qlen > hs->hs_queue_depth     && pkt->pkt_frametype == PKT_B_FRAME) ||
     (qlen > hs->hs_queue_depth * 2 && pkt->pkt_frametype == PKT_P_FRAME) || 
     (qlen > hs->hs_queue_depth * 3)) {

    hs->hs_dropstats[pkt->pkt_frametype]++;

    /* Queue size protection */
    pkt_ref_dec(pkt);
    return;
  }

  m = htsmsg_create_map();
 
  htsmsg_add_str(m, "method", "muxpkt");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  htsmsg_add_u32(m, "frametype", frametypearray[pkt->pkt_frametype]);

  htsmsg_add_u32(m, "stream", pkt->pkt_componentindex);
  htsmsg_add_u32(m, "com", pkt->pkt_commercial);

  if(pkt->pkt_pts != PTS_UNSET) {
    int64_t pts = hs->hs_90khz ? pkt->pkt_pts : ts_rescale(pkt->pkt_pts, 1000000);
    htsmsg_add_s64(m, "pts", pts);
  }

  if(pkt->pkt_dts != PTS_UNSET) {
    int64_t dts = hs->hs_90khz ? pkt->pkt_dts : ts_rescale(pkt->pkt_dts, 1000000);
    htsmsg_add_s64(m, "dts", dts);
  }

  uint32_t dur = hs->hs_90khz ? pkt->pkt_duration : ts_rescale(pkt->pkt_duration, 1000000);
  htsmsg_add_u32(m, "duration", dur);
  
  /**
   * Since we will serialize directly we use 'binptr' which is a binary
   * object that just points to data, thus avoiding a copy.
   */
  payloadlen = pktbuf_len(pkt->pkt_payload);
  htsmsg_add_binptr(m, "payload", pktbuf_ptr(pkt->pkt_payload), payloadlen);
  htsp_send(htsp, m, pkt->pkt_payload, &hs->hs_q, payloadlen);
  atomic_add(&hs->hs_s->ths_bytes_out, payloadlen);

  if(hs->hs_last_report != dispatch_clock) {

    /* Send a queue and signal status report every second */

    hs->hs_last_report = dispatch_clock;

    m = htsmsg_create_map();
    htsmsg_add_str(m, "method", "queueStatus");
    htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
    htsmsg_add_u32(m, "packets", hs->hs_q.hmq_length);
    htsmsg_add_u32(m, "bytes", hs->hs_q.hmq_payload);

    /**
     * Figure out real time queue delay 
     */
    
    pthread_mutex_lock(&htsp->htsp_out_mutex);

    int64_t min_dts = PTS_UNSET;
    int64_t max_dts = PTS_UNSET;
    TAILQ_FOREACH(hm, &hs->hs_q.hmq_q, hm_link) {
      if(!hm->hm_msg)
	continue;
      if(htsmsg_get_s64(hm->hm_msg, "dts", &ts))
	continue;
      if(ts == PTS_UNSET)
	continue;
  
      if(min_dts == PTS_UNSET)
	min_dts = ts;
      else
	min_dts = MIN(ts, min_dts);

      if(max_dts == PTS_UNSET)
	max_dts = ts;
      else
	max_dts = MAX(ts, max_dts);
    }

    htsmsg_add_s64(m, "delay", max_dts - min_dts);

    pthread_mutex_unlock(&htsp->htsp_out_mutex);

    htsmsg_add_u32(m, "Bdrops", hs->hs_dropstats[PKT_B_FRAME]);
    htsmsg_add_u32(m, "Pdrops", hs->hs_dropstats[PKT_P_FRAME]);
    htsmsg_add_u32(m, "Idrops", hs->hs_dropstats[PKT_I_FRAME]);

    /* We use a special queue for queue status message so they're not
       blocked by anything else */
    htsp_send_message(hs->hs_htsp, m, &hs->hs_htsp->htsp_hmq_qstatus);
  }
  pkt_ref_dec(pkt);
}

/**
 * Send a 'subscriptionStart' message to client informing about
 * delivery start and all components.
 */
static void
htsp_subscription_start(htsp_subscription_t *hs, const streaming_start_t *ss)
{
  htsmsg_t *m,*streams, *c, *sourceinfo;
  const char *type;
  int i;
  const source_info_t *si;

  tvhdebug("htsp", "%s - subscription start", hs->hs_htsp->htsp_logname);

  for(i = 0; i < ss->ss_num_components; i++) {
    const streaming_start_component_t *ssc = &ss->ss_components[i];
    if (ssc->ssc_disabled) continue;
    if (SCT_ISVIDEO(ssc->ssc_type)) {
      if (ssc->ssc_width == 0 || ssc->ssc_height == 0) {
        hs->hs_wait_for_video = 1;
        return;
      }
      break;
    }
  }
  hs->hs_wait_for_video = 0;

  m = htsmsg_create_map();
  streams = htsmsg_create_list();
  sourceinfo = htsmsg_create_map();
  for(i = 0; i < ss->ss_num_components; i++) {
    const streaming_start_component_t *ssc = &ss->ss_components[i];
    if(ssc->ssc_disabled) continue;

    c = htsmsg_create_map();
    htsmsg_add_u32(c, "index", ssc->ssc_index);
    if (ssc->ssc_type == SCT_MP4A)
      type = "AAC"; /* override */
    else
      type = streaming_component_type2txt(ssc->ssc_type);
    htsmsg_add_str(c, "type", type);
    if(ssc->ssc_lang[0])
      htsmsg_add_str(c, "language", ssc->ssc_lang);
    
    if(ssc->ssc_type == SCT_DVBSUB) {
      htsmsg_add_u32(c, "composition_id", ssc->ssc_composition_id);
      htsmsg_add_u32(c, "ancillary_id", ssc->ssc_ancillary_id);
    }

    if(SCT_ISVIDEO(ssc->ssc_type)) {
      if(ssc->ssc_width)
        htsmsg_add_u32(c, "width", ssc->ssc_width);
      if(ssc->ssc_height)
        htsmsg_add_u32(c, "height", ssc->ssc_height);
      if(ssc->ssc_frameduration)
        htsmsg_add_u32(c, "duration", hs->hs_90khz ? ssc->ssc_frameduration :
                       ts_rescale(ssc->ssc_frameduration, 1000000));
      if (ssc->ssc_aspect_num)
        htsmsg_add_u32(c, "aspect_num", ssc->ssc_aspect_num);
      if (ssc->ssc_aspect_den)
        htsmsg_add_u32(c, "aspect_den", ssc->ssc_aspect_den);
    }

    if (SCT_ISAUDIO(ssc->ssc_type))
    {
      htsmsg_add_u32(c, "audio_type", ssc->ssc_audio_type);
      if (ssc->ssc_channels)
        htsmsg_add_u32(c, "channels", ssc->ssc_channels);
      if (ssc->ssc_sri)
        htsmsg_add_u32(c, "rate", ssc->ssc_sri);
    }

    if (ssc->ssc_gh)
      htsmsg_add_binptr(m, "meta", pktbuf_ptr(ssc->ssc_gh),
		        pktbuf_len(ssc->ssc_gh));

    htsmsg_add_msg(streams, NULL, c);
  }
  
  htsmsg_add_msg(m, "streams", streams);

  si = &ss->ss_si;
  if(si->si_adapter ) htsmsg_add_str(sourceinfo, "adapter",  si->si_adapter );
  if(si->si_mux     ) htsmsg_add_str(sourceinfo, "mux"    ,  si->si_mux     );
  if(si->si_network ) htsmsg_add_str(sourceinfo, "network",  si->si_network );
  if(si->si_provider) htsmsg_add_str(sourceinfo, "provider", si->si_provider);
  if(si->si_service ) htsmsg_add_str(sourceinfo, "service",  si->si_service );
  
  htsmsg_add_msg(m, "sourceinfo", sourceinfo);
 
  htsmsg_add_str(m, "method", "subscriptionStart");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  htsp_send(hs->hs_htsp, m, NULL, &hs->hs_q, 0);
}

/**
 * Send a 'subscriptionStop' stop
 */
static void
htsp_subscription_stop(htsp_subscription_t *hs, const char *err)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "subscriptionStop");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  tvhdebug("htsp", "%s - subscription stop", hs->hs_htsp->htsp_logname);

  if(err != NULL)
    htsmsg_add_str(m, "status", err);

  htsp_send(hs->hs_htsp, m, NULL, &hs->hs_q, 0);
}

/**
 * Send a 'subscriptionGrace' message
 */
static void
htsp_subscription_grace(htsp_subscription_t *hs, int grace)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "subscriptionGrace");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  htsmsg_add_u32(m, "graceTimeout", grace);
  tvhdebug("htsp", "%s - subscription grace %i seconds", hs->hs_htsp->htsp_logname, grace);

  htsp_send(hs->hs_htsp, m, NULL, &hs->hs_q, 0);
}

/**
 * Send a 'subscriptionStatus' message
 */
static void
htsp_subscription_status(htsp_subscription_t *hs, const char *err)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "subscriptionStatus");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);

  if(err != NULL)
    htsmsg_add_str(m, "status", err);

  htsp_send(hs->hs_htsp, m, NULL, &hs->hs_q, 0);
}

/**
 *
 */
static void
htsp_subscription_service_status(htsp_subscription_t *hs, int status)
{
  if(status & TSS_PACKETS) {
    htsp_subscription_status(hs, NULL);
  } else if(status & TSS_ERRORS) {
    htsp_subscription_status(hs, service_tss2text(status));
  }
}

/**
 *
 */
static void
htsp_subscription_signal_status(htsp_subscription_t *hs, signal_status_t *sig)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "signalStatus");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  htsmsg_add_str(m, "feStatus",   sig->status_text);
  if((sig->snr != -2) && (sig->snr_scale == SIGNAL_STATUS_SCALE_RELATIVE))
    htsmsg_add_u32(m, "feSNR",    sig->snr);
  if((sig->signal != -2) && (sig->signal_scale == SIGNAL_STATUS_SCALE_RELATIVE))
    htsmsg_add_u32(m, "feSignal", sig->signal);
  if(sig->ber != -2)
    htsmsg_add_u32(m, "feBER",    sig->ber);
  if(sig->unc != -2)
    htsmsg_add_u32(m, "feUNC",    sig->unc);
  htsp_send_message(hs->hs_htsp, m, &hs->hs_htsp->htsp_hmq_qstatus);
}

/**
 *
 */
static void
htsp_subscription_speed(htsp_subscription_t *hs, int speed)
{
  htsmsg_t *m = htsmsg_create_map();
  tvhdebug("htsp", "%s - subscription speed", hs->hs_htsp->htsp_logname);
  htsmsg_add_str(m, "method", "subscriptionSpeed");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  htsmsg_add_u32(m, "speed", speed);
  htsp_send(hs->hs_htsp, m, NULL, &hs->hs_q, 0);
}

/**
 *
 */
static void
htsp_subscription_skip(htsp_subscription_t *hs, streaming_skip_t *skip)
{
  htsmsg_t *m = htsmsg_create_map();
  tvhdebug("htsp", "%s - subscription skip", hs->hs_htsp->htsp_logname);
  htsmsg_add_str(m, "method", "subscriptionSkip");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);

  /* Flush pkt buffers */
  if (skip->type != SMT_SKIP_ERROR)
    htsp_flush_queue(hs->hs_htsp, &hs->hs_q, 0);

  if (skip->type == SMT_SKIP_ABS_TIME || skip->type == SMT_SKIP_ABS_SIZE)
    htsmsg_add_u32(m, "absolute", 1);
  if (skip->type == SMT_SKIP_ERROR)
    htsmsg_add_u32(m, "error", 1);
  else if (skip->type == SMT_SKIP_ABS_TIME || skip->type == SMT_SKIP_REL_TIME)
    htsmsg_add_s64(m, "time", hs->hs_90khz ? skip->time : ts_rescale(skip->time, 1000000));
  else if (skip->type == SMT_SKIP_ABS_SIZE || skip->type == SMT_SKIP_REL_SIZE)
    htsmsg_add_s64(m, "size", skip->size);
  htsp_send(hs->hs_htsp, m, NULL, &hs->hs_q, 0);
}

/**
 *
 */
#if ENABLE_TIMESHIFT
static void
htsp_subscription_timeshift_status(htsp_subscription_t *hs, timeshift_status_t *status)
{
  htsmsg_t *m = htsmsg_create_map();
  htsmsg_add_str(m, "method", "timeshiftStatus");
  htsmsg_add_u32(m, "subscriptionId", hs->hs_sid);
  htsmsg_add_u32(m, "full", status->full);
  htsmsg_add_s64(m, "shift", hs->hs_90khz ? status->shift : ts_rescale(status->shift, 1000000));
  if (status->pts_start != PTS_UNSET)
    htsmsg_add_s64(m, "start", hs->hs_90khz ? status->pts_start : ts_rescale(status->pts_start, 1000000)) ;
  if (status->pts_end != PTS_UNSET)
    htsmsg_add_s64(m, "end", hs->hs_90khz ? status->pts_end : ts_rescale(status->pts_end, 1000000)) ;
  htsp_send(hs->hs_htsp, m, NULL, &hs->hs_q, 0);
}
#endif

/**
 *
 */
static void
htsp_streaming_input(void *opaque, streaming_message_t *sm)
{
  htsp_subscription_t *hs = opaque;

  switch(sm->sm_type) {
  case SMT_PACKET:
    if (hs->hs_wait_for_video)
      break;
    if (!hs->hs_first)
      tvhdebug("htsp", "%s - first packet", hs->hs_htsp->htsp_logname);
    hs->hs_first = 1;
    htsp_stream_deliver(hs, sm->sm_data);
    // reference is transfered
    sm->sm_data = NULL;
    break;

  case SMT_START:
    htsp_subscription_start(hs, sm->sm_data);
    break;

  case SMT_STOP:
    htsp_subscription_stop(hs, streaming_code2txt(sm->sm_code));
    break;

  case SMT_GRACE:
    htsp_subscription_grace(hs, sm->sm_code);
    break;

  case SMT_SERVICE_STATUS:
    htsp_subscription_service_status(hs, sm->sm_code);
    break;

  case SMT_SIGNAL_STATUS:
    htsp_subscription_signal_status(hs, sm->sm_data);
    break;

  case SMT_NOSTART:
    htsp_subscription_status(hs,  streaming_code2txt(sm->sm_code));
    break;

  case SMT_MPEGTS:
    break;

  case SMT_EXIT:
    abort();

  case SMT_SKIP:
    htsp_subscription_skip(hs, sm->sm_data);
    break;

  case SMT_SPEED:
    htsp_subscription_speed(hs, sm->sm_code);
    break;

  case SMT_TIMESHIFT_STATUS:
#if ENABLE_TIMESHIFT
    htsp_subscription_timeshift_status(hs, sm->sm_data);
#endif
    break;
  }
  streaming_msg_free(sm);
}

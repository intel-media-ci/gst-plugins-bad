/* GStreamer
 * Copyright (C) 2017 Matthew Waters <matthew@centricular.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "gstwebrtcice.h"
/* libnice */
#include <agent.h>
#include "icestream.h"
#include "nicetransport.h"

/* XXX:
 *
 * - are locally generated remote candidates meant to be readded to libnice?
 */

static GstUri *_validate_turn_server (GstWebRTCICE * ice, const gchar * s);

#define GST_CAT_DEFAULT gst_webrtc_ice_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

GQuark
gst_webrtc_ice_error_quark (void)
{
  return g_quark_from_static_string ("gst-webrtc-ice-error-quark");
}

enum
{
  SIGNAL_0,
  ADD_LOCAL_IP_ADDRESS_SIGNAL,
  LAST_SIGNAL,
};

enum
{
  PROP_0,
  PROP_AGENT,
  PROP_ICE_TCP,
  PROP_ICE_UDP,
  PROP_MIN_RTP_PORT,
  PROP_MAX_RTP_PORT,
};

static guint gst_webrtc_ice_signals[LAST_SIGNAL] = { 0 };

struct _GstWebRTCICEPrivate
{
  NiceAgent *nice_agent;

  GArray *nice_stream_map;

  GThread *thread;
  GMainContext *main_context;
  GMainLoop *loop;
  GMutex lock;
  GCond cond;

  GstWebRTCIceOnCandidateFunc on_candidate;
  gpointer on_candidate_data;
  GDestroyNotify on_candidate_notify;
};

#define gst_webrtc_ice_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstWebRTCICE, gst_webrtc_ice,
    GST_TYPE_OBJECT, G_ADD_PRIVATE (GstWebRTCICE)
    GST_DEBUG_CATEGORY_INIT (gst_webrtc_ice_debug, "webrtcice", 0,
        "webrtcice"););

static gboolean
_unlock_pc_thread (GMutex * lock)
{
  g_mutex_unlock (lock);
  return G_SOURCE_REMOVE;
}

static gpointer
_gst_nice_thread (GstWebRTCICE * ice)
{
  g_mutex_lock (&ice->priv->lock);
  ice->priv->main_context = g_main_context_new ();
  ice->priv->loop = g_main_loop_new (ice->priv->main_context, FALSE);

  g_cond_broadcast (&ice->priv->cond);
  g_main_context_invoke (ice->priv->main_context,
      (GSourceFunc) _unlock_pc_thread, &ice->priv->lock);

  g_main_loop_run (ice->priv->loop);

  g_mutex_lock (&ice->priv->lock);
  g_main_context_unref (ice->priv->main_context);
  ice->priv->main_context = NULL;
  g_main_loop_unref (ice->priv->loop);
  ice->priv->loop = NULL;
  g_cond_broadcast (&ice->priv->cond);
  g_mutex_unlock (&ice->priv->lock);

  return NULL;
}

static void
_start_thread (GstWebRTCICE * ice)
{
  g_mutex_lock (&ice->priv->lock);
  ice->priv->thread = g_thread_new (GST_OBJECT_NAME (ice),
      (GThreadFunc) _gst_nice_thread, ice);

  while (!ice->priv->loop)
    g_cond_wait (&ice->priv->cond, &ice->priv->lock);
  g_mutex_unlock (&ice->priv->lock);
}

static void
_stop_thread (GstWebRTCICE * ice)
{
  g_mutex_lock (&ice->priv->lock);
  g_main_loop_quit (ice->priv->loop);
  while (ice->priv->loop)
    g_cond_wait (&ice->priv->cond, &ice->priv->lock);
  g_mutex_unlock (&ice->priv->lock);

  g_thread_unref (ice->priv->thread);
}

struct NiceStreamItem
{
  guint session_id;
  guint nice_stream_id;
  GstWebRTCICEStream *stream;
};

/* TRUE to continue, FALSE to stop */
typedef gboolean (*NiceStreamItemForeachFunc) (struct NiceStreamItem * item,
    gpointer user_data);

static void
_nice_stream_item_foreach (GstWebRTCICE * ice, NiceStreamItemForeachFunc func,
    gpointer data)
{
  int i, len;

  len = ice->priv->nice_stream_map->len;
  for (i = 0; i < len; i++) {
    struct NiceStreamItem *item =
        &g_array_index (ice->priv->nice_stream_map, struct NiceStreamItem,
        i);

    if (!func (item, data))
      break;
  }
}

/* TRUE for match, FALSE otherwise */
typedef gboolean (*NiceStreamItemFindFunc) (struct NiceStreamItem * item,
    gpointer user_data);

struct nice_find
{
  NiceStreamItemFindFunc func;
  gpointer data;
  struct NiceStreamItem *ret;
};

static gboolean
_find_nice_item (struct NiceStreamItem *item, gpointer user_data)
{
  struct nice_find *f = user_data;
  if (f->func (item, f->data)) {
    f->ret = item;
    return FALSE;
  }
  return TRUE;
}

static struct NiceStreamItem *
_nice_stream_item_find (GstWebRTCICE * ice, NiceStreamItemFindFunc func,
    gpointer data)
{
  struct nice_find f;

  f.func = func;
  f.data = data;
  f.ret = NULL;

  _nice_stream_item_foreach (ice, _find_nice_item, &f);

  return f.ret;
}

#define NICE_MATCH_INIT { -1, -1, NULL }

static gboolean
_match (struct NiceStreamItem *item, struct NiceStreamItem *m)
{
  if (m->session_id != -1 && m->session_id != item->session_id)
    return FALSE;
  if (m->nice_stream_id != -1 && m->nice_stream_id != item->nice_stream_id)
    return FALSE;
  if (m->stream != NULL && m->stream != item->stream)
    return FALSE;

  return TRUE;
}

static struct NiceStreamItem *
_find_item (GstWebRTCICE * ice, guint session_id, guint nice_stream_id,
    GstWebRTCICEStream * stream)
{
  struct NiceStreamItem m = NICE_MATCH_INIT;

  m.session_id = session_id;
  m.nice_stream_id = nice_stream_id;
  m.stream = stream;

  return _nice_stream_item_find (ice, (NiceStreamItemFindFunc) _match, &m);
}

static struct NiceStreamItem *
_create_nice_stream_item (GstWebRTCICE * ice, guint session_id)
{
  struct NiceStreamItem item;

  item.session_id = session_id;
  item.nice_stream_id = nice_agent_add_stream (ice->priv->nice_agent, 1);
  item.stream = gst_webrtc_ice_stream_new (ice, item.nice_stream_id);
  g_array_append_val (ice->priv->nice_stream_map, item);

  return _find_item (ice, item.session_id, item.nice_stream_id, item.stream);
}

static void
_parse_userinfo (const gchar * userinfo, gchar ** user, gchar ** pass)
{
  const gchar *colon;

  if (!userinfo) {
    *user = NULL;
    *pass = NULL;
    return;
  }

  colon = g_strstr_len (userinfo, -1, ":");
  if (!colon) {
    *user = g_uri_unescape_string (userinfo, NULL);
    *pass = NULL;
    return;
  }

  /* Check that the first occurence is also the last occurence */
  if (colon != g_strrstr (userinfo, ":"))
    GST_WARNING ("userinfo %s contains more than one ':', will assume that the "
        "first ':' delineates user:pass. You should escape the user and pass "
        "before adding to the URI.", userinfo);

  *user = g_uri_unescape_segment (userinfo, colon, NULL);
  *pass = g_uri_unescape_string (&colon[1], NULL);
}

static gchar *
_resolve_host (GstWebRTCICE * ice, const gchar * host)
{
  GResolver *resolver = g_resolver_get_default ();
  GError *error = NULL;
  GInetAddress *addr;
  GList *addresses;
  gchar *address;

  GST_DEBUG_OBJECT (ice, "Resolving host %s", host);

  if (!(addresses = g_resolver_lookup_by_name (resolver, host, NULL, &error))) {
    GST_ERROR ("%s", error->message);
    g_clear_error (&error);
    return NULL;
  }

  GST_DEBUG_OBJECT (ice, "Resolved %d addresses for host %s",
      g_list_length (addresses), host);

  /* XXX: only the first address is used */
  addr = addresses->data;
  address = g_inet_address_to_string (addr);
  g_resolver_free_addresses (addresses);

  return address;
}

static void
_add_turn_server (GstWebRTCICE * ice, struct NiceStreamItem *item,
    GstUri * turn_server)
{
  gboolean ret;
  gchar *user, *pass;
  const gchar *host, *userinfo, *transport, *scheme;
  NiceRelayType relays[4] = { 0, };
  int i, relay_n = 0;
  gchar *ip = NULL;

  host = gst_uri_get_host (turn_server);
  if (!host) {
    GST_ERROR_OBJECT (ice, "Turn server has no host");
    goto out;
  }
  ip = _resolve_host (ice, host);
  if (!ip) {
    GST_ERROR_OBJECT (ice, "Failed to resolve turn server '%s'", host);
    goto out;
  }

  /* Set the resolved IP as the host since that's what libnice wants */
  gst_uri_set_host (turn_server, ip);

  scheme = gst_uri_get_scheme (turn_server);
  transport = gst_uri_get_query_value (turn_server, "transport");
  userinfo = gst_uri_get_userinfo (turn_server);
  _parse_userinfo (userinfo, &user, &pass);

  if (g_strcmp0 (scheme, "turns") == 0) {
    relays[relay_n++] = NICE_RELAY_TYPE_TURN_TLS;
  } else if (g_strcmp0 (scheme, "turn") == 0) {
    if (!transport || g_strcmp0 (transport, "udp") == 0)
      relays[relay_n++] = NICE_RELAY_TYPE_TURN_UDP;
    if (!transport || g_strcmp0 (transport, "tcp") == 0)
      relays[relay_n++] = NICE_RELAY_TYPE_TURN_TCP;
  }
  g_assert (relay_n < G_N_ELEMENTS (relays));

  for (i = 0; i < relay_n; i++) {
    ret = nice_agent_set_relay_info (ice->priv->nice_agent,
        item->nice_stream_id, NICE_COMPONENT_TYPE_RTP,
        gst_uri_get_host (turn_server), gst_uri_get_port (turn_server),
        user, pass, relays[i]);
    if (!ret) {
      gchar *uri = gst_uri_to_string (turn_server);
      GST_ERROR_OBJECT (ice, "Failed to set TURN server '%s'", uri);
      g_free (uri);
      break;
    }
  }
  g_free (user);
  g_free (pass);

out:
  g_free (ip);
}

typedef struct
{
  GstWebRTCICE *ice;
  struct NiceStreamItem *item;
} AddTurnServerData;

static void
_add_turn_server_func (const gchar * uri, GstUri * turn_server,
    AddTurnServerData * data)
{
  _add_turn_server (data->ice, data->item, turn_server);
}

static void
_add_stun_server (GstWebRTCICE * ice, GstUri * stun_server)
{
  const gchar *msg = "must be of the form stun://<host>:<port>";
  const gchar *host;
  gchar *s = NULL;
  gchar *ip = NULL;
  guint port;

  s = gst_uri_to_string (stun_server);
  GST_DEBUG_OBJECT (ice, "adding stun server, %s", s);

  host = gst_uri_get_host (stun_server);
  if (!host) {
    GST_ERROR_OBJECT (ice, "Stun server '%s' has no host, %s", s, msg);
    goto out;
  }

  port = gst_uri_get_port (stun_server);
  if (port == GST_URI_NO_PORT) {
    GST_INFO_OBJECT (ice, "Stun server '%s' has no port, assuming 3478", s);
    port = 3478;
    gst_uri_set_port (stun_server, port);
  }

  ip = _resolve_host (ice, host);
  if (!ip) {
    GST_ERROR_OBJECT (ice, "Failed to resolve stun server '%s'", host);
    goto out;
  }

  g_object_set (ice->priv->nice_agent, "stun-server", ip,
      "stun-server-port", port, NULL);

out:
  g_free (s);
  g_free (ip);
}

GstWebRTCICEStream *
gst_webrtc_ice_add_stream (GstWebRTCICE * ice, guint session_id)
{
  struct NiceStreamItem m = NICE_MATCH_INIT;
  struct NiceStreamItem *item;
  AddTurnServerData add_data;

  m.session_id = session_id;
  item = _nice_stream_item_find (ice, (NiceStreamItemFindFunc) _match, &m);
  if (item) {
    GST_ERROR_OBJECT (ice, "stream already added with session_id=%u",
        session_id);
    return 0;
  }

  if (ice->stun_server) {
    _add_stun_server (ice, ice->stun_server);
  }

  item = _create_nice_stream_item (ice, session_id);

  if (ice->turn_server) {
    _add_turn_server (ice, item, ice->turn_server);
  }

  add_data.ice = ice;
  add_data.item = item;

  g_hash_table_foreach (ice->turn_servers, (GHFunc) _add_turn_server_func,
      &add_data);

  return item->stream;
}

static void
_on_new_candidate (NiceAgent * agent, NiceCandidate * candidate,
    GstWebRTCICE * ice)
{
  struct NiceStreamItem *item;
  gchar *attr;

  item = _find_item (ice, -1, candidate->stream_id, NULL);
  if (!item) {
    GST_WARNING_OBJECT (ice, "received signal for non-existent stream %u",
        candidate->stream_id);
    return;
  }

  if (!candidate->username || !candidate->password) {
    gboolean got_credentials;
    gchar *ufrag, *password;

    got_credentials = nice_agent_get_local_credentials (ice->priv->nice_agent,
        candidate->stream_id, &ufrag, &password);
    g_warn_if_fail (got_credentials);

    if (!candidate->username)
      candidate->username = ufrag;
    else
      g_free (ufrag);

    if (!candidate->password)
      candidate->password = password;
    else
      g_free (password);
  }

  attr = nice_agent_generate_local_candidate_sdp (agent, candidate);

  if (ice->priv->on_candidate)
    ice->priv->on_candidate (ice, item->session_id, attr,
        ice->priv->on_candidate_data);

  g_free (attr);
}

GstWebRTCICETransport *
gst_webrtc_ice_find_transport (GstWebRTCICE * ice, GstWebRTCICEStream * stream,
    GstWebRTCICEComponent component)
{
  struct NiceStreamItem *item;

  item = _find_item (ice, -1, -1, stream);
  g_return_val_if_fail (item != NULL, NULL);

  return gst_webrtc_ice_stream_find_transport (item->stream, component);
}

#if 0
/* TODO don't rely on libnice to (de)serialize candidates */
static NiceCandidateType
_candidate_type_from_string (const gchar * s)
{
  if (g_strcmp0 (s, "host") == 0) {
    return NICE_CANDIDATE_TYPE_HOST;
  } else if (g_strcmp0 (s, "srflx") == 0) {
    return NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE;
  } else if (g_strcmp0 (s, "prflx") == 0) {     /* FIXME: is the right string? */
    return NICE_CANDIDATE_TYPE_PEER_REFLEXIVE;
  } else if (g_strcmp0 (s, "relay") == 0) {
    return NICE_CANDIDATE_TYPE_RELAY;
  } else {
    g_assert_not_reached ();
    return 0;
  }
}

static const gchar *
_candidate_type_to_string (NiceCandidateType type)
{
  switch (type) {
    case NICE_CANDIDATE_TYPE_HOST:
      return "host";
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
      return "srflx";
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
      return "prflx";
    case NICE_CANDIDATE_TYPE_RELAY:
      return "relay";
    default:
      g_assert_not_reached ();
      return NULL;
  }
}

static NiceCandidateTransport
_candidate_transport_from_string (const gchar * s)
{
  if (g_strcmp0 (s, "UDP") == 0) {
    return NICE_CANDIDATE_TRANSPORT_UDP;
  } else if (g_strcmp0 (s, "TCP tcptype") == 0) {
    return NICE_CANDIDATE_TRANSPORT_TCP_ACTIVE;
  } else if (g_strcmp0 (s, "tcp-passive") == 0) {       /* FIXME: is the right string? */
    return NICE_CANDIDATE_TRANSPORT_TCP_PASSIVE;
  } else if (g_strcmp0 (s, "tcp-so") == 0) {
    return NICE_CANDIDATE_TRANSPORT_TCP_SO;
  } else {
    g_assert_not_reached ();
    return 0;
  }
}

static const gchar *
_candidate_type_to_string (NiceCandidateType type)
{
  switch (type) {
    case NICE_CANDIDATE_TYPE_HOST:
      return "host";
    case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE:
      return "srflx";
    case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE:
      return "prflx";
    case NICE_CANDIDATE_TYPE_RELAY:
      return "relay";
    default:
      g_assert_not_reached ();
      return NULL;
  }
}
#endif

/* parse the address for possible resolution */
static gboolean
get_candidate_address (const gchar * candidate, gchar ** prefix,
    gchar ** address, gchar ** postfix)
{
  char **tokens = NULL;

  if (!g_str_has_prefix (candidate, "a=candidate:")) {
    GST_ERROR ("candidate \"%s\" does not start with \"a=candidate:\"",
        candidate);
    goto failure;
  }

  if (!(tokens = g_strsplit (candidate, " ", 6))) {
    GST_ERROR ("candidate \"%s\" could not be tokenized", candidate);
    goto failure;
  }

  if (g_strv_length (tokens) < 6) {
    GST_ERROR ("candidate \"%s\" tokenization resulted in not enough tokens",
        candidate);
    goto failure;
  }

  if (address)
    *address = g_strdup (tokens[4]);
  tokens[4] = NULL;
  if (prefix)
    *prefix = g_strjoinv (" ", tokens);
  if (postfix)
    *postfix = g_strdup (tokens[5]);

  g_strfreev (tokens);
  return TRUE;

failure:
  if (tokens)
    g_strfreev (tokens);
  return FALSE;
}

/* candidate must start with "a=candidate:" or be NULL*/
void
gst_webrtc_ice_add_candidate (GstWebRTCICE * ice, GstWebRTCICEStream * stream,
    const gchar * candidate)
{
  struct NiceStreamItem *item;
  NiceCandidate *cand;
  GSList *candidates = NULL;

  item = _find_item (ice, -1, -1, stream);
  g_return_if_fail (item != NULL);

  if (candidate == NULL) {
    nice_agent_peer_candidate_gathering_done (ice->priv->nice_agent,
        item->nice_stream_id);
    return;
  }

  cand =
      nice_agent_parse_remote_candidate_sdp (ice->priv->nice_agent,
      item->nice_stream_id, candidate);
  if (!cand) {
    /* might be a .local candidate */
    char *prefix = NULL, *address = NULL, *postfix = NULL;
    char *new_addr, *new_candidate;
    char *new_candv[4] = { NULL, };

    if (!get_candidate_address (candidate, &prefix, &address, &postfix)) {
      GST_WARNING_OBJECT (ice, "Failed to retrieve address from candidate %s",
          candidate);
      goto fail;
    }

    if (!g_str_has_suffix (address, ".local")) {
      GST_WARNING_OBJECT (ice, "candidate address \'%s\' does not end "
          "with \'.local\'", address);
      goto fail;
    }

    /* FIXME: async */
    if (!(new_addr = _resolve_host (ice, address))) {
      GST_WARNING_OBJECT (ice, "Failed to resolve %s", address);
      goto fail;
    }

    new_candv[0] = prefix;
    new_candv[1] = new_addr;
    new_candv[2] = postfix;
    new_candv[3] = NULL;
    new_candidate = g_strjoinv (" ", new_candv);

    GST_DEBUG_OBJECT (ice, "resolved to candidate %s", new_candidate);

    cand =
        nice_agent_parse_remote_candidate_sdp (ice->priv->nice_agent,
        item->nice_stream_id, new_candidate);
    g_free (new_candidate);
    if (!cand) {
      GST_WARNING_OBJECT (ice, "Could not parse candidate \'%s\'",
          new_candidate);
      goto fail;
    }

    g_free (prefix);
    g_free (new_addr);
    g_free (postfix);

    if (0) {
    fail:
      g_free (prefix);
      g_free (address);
      g_free (postfix);
      return;
    }
  }

  if (cand->component_id == 2) {
    /* we only support rtcp-mux so rtcp candidates are useless for us */
    GST_INFO_OBJECT (ice, "Dropping RTCP candidate %s", candidate);
    nice_candidate_free (cand);
    return;
  }

  candidates = g_slist_append (candidates, cand);

  nice_agent_set_remote_candidates (ice->priv->nice_agent, item->nice_stream_id,
      cand->component_id, candidates);

  g_slist_free (candidates);
  nice_candidate_free (cand);
}

gboolean
gst_webrtc_ice_set_remote_credentials (GstWebRTCICE * ice,
    GstWebRTCICEStream * stream, gchar * ufrag, gchar * pwd)
{
  struct NiceStreamItem *item;

  g_return_val_if_fail (ufrag != NULL, FALSE);
  g_return_val_if_fail (pwd != NULL, FALSE);
  item = _find_item (ice, -1, -1, stream);
  g_return_val_if_fail (item != NULL, FALSE);

  GST_DEBUG_OBJECT (ice, "Setting remote ICE credentials on "
      "ICE stream %u ufrag:%s pwd:%s", item->nice_stream_id, ufrag, pwd);

  nice_agent_set_remote_credentials (ice->priv->nice_agent,
      item->nice_stream_id, ufrag, pwd);

  return TRUE;
}

gboolean
gst_webrtc_ice_add_turn_server (GstWebRTCICE * ice, const gchar * uri)
{
  gboolean ret = FALSE;
  GstUri *valid_uri;

  if (!(valid_uri = _validate_turn_server (ice, uri)))
    goto done;

  g_hash_table_insert (ice->turn_servers, g_strdup (uri), valid_uri);

  ret = TRUE;

done:
  return ret;
}

static gboolean
gst_webrtc_ice_add_local_ip_address (GstWebRTCICE * ice, const gchar * address)
{
  gboolean ret = FALSE;
  NiceAddress nice_addr;

  nice_address_init (&nice_addr);

  ret = nice_address_set_from_string (&nice_addr, address);

  if (ret) {
    ret = nice_agent_add_local_address (ice->priv->nice_agent, &nice_addr);
    if (!ret) {
      GST_ERROR_OBJECT (ice, "Failed to add local address to NiceAgent");
    }
  } else {
    GST_ERROR_OBJECT (ice, "Failed to initialize NiceAddress [%s]", address);
  }

  return ret;
}

gboolean
gst_webrtc_ice_set_local_credentials (GstWebRTCICE * ice,
    GstWebRTCICEStream * stream, gchar * ufrag, gchar * pwd)
{
  struct NiceStreamItem *item;

  g_return_val_if_fail (ufrag != NULL, FALSE);
  g_return_val_if_fail (pwd != NULL, FALSE);
  item = _find_item (ice, -1, -1, stream);
  g_return_val_if_fail (item != NULL, FALSE);

  GST_DEBUG_OBJECT (ice, "Setting local ICE credentials on "
      "ICE stream %u ufrag:%s pwd:%s", item->nice_stream_id, ufrag, pwd);

  nice_agent_set_local_credentials (ice->priv->nice_agent, item->nice_stream_id,
      ufrag, pwd);

  return TRUE;
}

gboolean
gst_webrtc_ice_gather_candidates (GstWebRTCICE * ice,
    GstWebRTCICEStream * stream)
{
  struct NiceStreamItem *item;

  item = _find_item (ice, -1, -1, stream);
  g_return_val_if_fail (item != NULL, FALSE);

  GST_DEBUG_OBJECT (ice, "gather candidates for stream %u",
      item->nice_stream_id);

  return gst_webrtc_ice_stream_gather_candidates (stream);
}

void
gst_webrtc_ice_set_is_controller (GstWebRTCICE * ice, gboolean controller)
{
  g_object_set (G_OBJECT (ice->priv->nice_agent), "controlling-mode",
      controller, NULL);
}

gboolean
gst_webrtc_ice_get_is_controller (GstWebRTCICE * ice)
{
  gboolean ret;
  g_object_get (G_OBJECT (ice->priv->nice_agent), "controlling-mode",
      &ret, NULL);
  return ret;
}

void
gst_webrtc_ice_set_force_relay (GstWebRTCICE * ice, gboolean force_relay)
{
  g_object_set (G_OBJECT (ice->priv->nice_agent), "force-relay", force_relay,
      NULL);
}

void
gst_webrtc_ice_set_on_ice_candidate (GstWebRTCICE * ice,
    GstWebRTCIceOnCandidateFunc func, gpointer user_data, GDestroyNotify notify)
{
  if (ice->priv->on_candidate_notify)
    ice->priv->on_candidate_notify (ice->priv->on_candidate_data);
  ice->priv->on_candidate = NULL;

  ice->priv->on_candidate = func;
  ice->priv->on_candidate_data = user_data;
  ice->priv->on_candidate_notify = notify;
}

void
gst_webrtc_ice_set_tos (GstWebRTCICE * ice, GstWebRTCICEStream * stream,
    guint tos)
{
  struct NiceStreamItem *item;

  item = _find_item (ice, -1, -1, stream);
  g_return_if_fail (item != NULL);

  nice_agent_set_stream_tos (ice->priv->nice_agent, item->nice_stream_id, tos);
}

static void
_clear_ice_stream (struct NiceStreamItem *item)
{
  if (!item)
    return;

  if (item->stream) {
    g_signal_handlers_disconnect_by_data (item->stream->ice->priv->nice_agent,
        item->stream);
    gst_object_unref (item->stream);
  }
}

static GstUri *
_validate_turn_server (GstWebRTCICE * ice, const gchar * s)
{
  GstUri *uri = gst_uri_from_string_escaped (s);
  const gchar *userinfo, *scheme;
  GList *keys = NULL, *l;
  gchar *user = NULL, *pass = NULL;
  gboolean turn_tls = FALSE;
  guint port;

  GST_DEBUG_OBJECT (ice, "validating turn server, %s", s);

  if (!uri) {
    GST_ERROR_OBJECT (ice, "Could not parse turn server '%s'", s);
    return NULL;
  }

  scheme = gst_uri_get_scheme (uri);
  if (g_strcmp0 (scheme, "turn") == 0) {
  } else if (g_strcmp0 (scheme, "turns") == 0) {
    turn_tls = TRUE;
  } else {
    GST_ERROR_OBJECT (ice, "unknown scheme '%s'", scheme);
    goto out;
  }

  keys = gst_uri_get_query_keys (uri);
  for (l = keys; l; l = l->next) {
    gchar *key = l->data;

    if (g_strcmp0 (key, "transport") == 0) {
      const gchar *transport = gst_uri_get_query_value (uri, "transport");
      if (!transport) {
      } else if (g_strcmp0 (transport, "udp") == 0) {
      } else if (g_strcmp0 (transport, "tcp") == 0) {
      } else {
        GST_ERROR_OBJECT (ice, "unknown transport value, '%s'", transport);
        goto out;
      }
    } else {
      GST_ERROR_OBJECT (ice, "unknown query key, '%s'", key);
      goto out;
    }
  }

  /* TODO: Implement error checking similar to the stun server below */
  userinfo = gst_uri_get_userinfo (uri);
  _parse_userinfo (userinfo, &user, &pass);
  if (!user) {
    GST_ERROR_OBJECT (ice, "No username specified in '%s'", s);
    goto out;
  }
  if (!pass) {
    GST_ERROR_OBJECT (ice, "No password specified in '%s'", s);
    goto out;
  }

  port = gst_uri_get_port (uri);

  if (port == GST_URI_NO_PORT) {
    if (turn_tls) {
      gst_uri_set_port (uri, 5349);
    } else {
      gst_uri_set_port (uri, 3478);
    }
  }

out:
  g_list_free (keys);
  g_free (user);
  g_free (pass);

  return uri;
}

void
gst_webrtc_ice_set_stun_server (GstWebRTCICE * ice, const gchar * uri_s)
{
  GstUri *uri = gst_uri_from_string_escaped (uri_s);
  const gchar *msg = "must be of the form stun://<host>:<port>";

  GST_DEBUG_OBJECT (ice, "setting stun server, %s", uri_s);

  if (!uri) {
    GST_ERROR_OBJECT (ice, "Couldn't parse stun server '%s', %s", uri_s, msg);
    return;
  }

  if (ice->stun_server)
    gst_uri_unref (ice->stun_server);
  ice->stun_server = uri;
}

gchar *
gst_webrtc_ice_get_stun_server (GstWebRTCICE * ice)
{
  if (ice->stun_server)
    return gst_uri_to_string (ice->stun_server);
  else
    return NULL;
}

void
gst_webrtc_ice_set_turn_server (GstWebRTCICE * ice, const gchar * uri_s)
{
  GstUri *uri = _validate_turn_server (ice, uri_s);

  if (uri) {
    if (ice->turn_server)
      gst_uri_unref (ice->turn_server);
    ice->turn_server = uri;
  }
}

gchar *
gst_webrtc_ice_get_turn_server (GstWebRTCICE * ice)
{
  if (ice->turn_server)
    return gst_uri_to_string (ice->turn_server);
  else
    return NULL;
}

static void
gst_webrtc_ice_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstWebRTCICE *ice = GST_WEBRTC_ICE (object);

  switch (prop_id) {
    case PROP_ICE_TCP:
      g_object_set_property (G_OBJECT (ice->priv->nice_agent),
          "ice-tcp", value);
      break;
    case PROP_ICE_UDP:
      g_object_set_property (G_OBJECT (ice->priv->nice_agent),
          "ice-udp", value);
      break;

    case PROP_MIN_RTP_PORT:
      ice->min_rtp_port = g_value_get_uint (value);
      if (ice->min_rtp_port > ice->max_rtp_port)
        g_warning ("Set min-rtp-port to %u which is larger than"
            " max-rtp-port %u", ice->min_rtp_port, ice->max_rtp_port);
      break;

    case PROP_MAX_RTP_PORT:
      ice->max_rtp_port = g_value_get_uint (value);
      if (ice->min_rtp_port > ice->max_rtp_port)
        g_warning ("Set max-rtp-port to %u which is smaller than"
            " min-rtp-port %u", ice->max_rtp_port, ice->min_rtp_port);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_webrtc_ice_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstWebRTCICE *ice = GST_WEBRTC_ICE (object);

  switch (prop_id) {
    case PROP_AGENT:
      g_value_set_object (value, ice->priv->nice_agent);
      break;
    case PROP_ICE_TCP:
      g_object_get_property (G_OBJECT (ice->priv->nice_agent),
          "ice-tcp", value);
      break;
    case PROP_ICE_UDP:
      g_object_get_property (G_OBJECT (ice->priv->nice_agent),
          "ice-udp", value);
      break;

    case PROP_MIN_RTP_PORT:
      g_value_set_uint (value, ice->min_rtp_port);
      break;

    case PROP_MAX_RTP_PORT:
      g_value_set_uint (value, ice->max_rtp_port);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_webrtc_ice_finalize (GObject * object)
{
  GstWebRTCICE *ice = GST_WEBRTC_ICE (object);

  g_signal_handlers_disconnect_by_data (ice->priv->nice_agent, ice);

  _stop_thread (ice);

  if (ice->priv->on_candidate_notify)
    ice->priv->on_candidate_notify (ice->priv->on_candidate_data);
  ice->priv->on_candidate = NULL;
  ice->priv->on_candidate_notify = NULL;

  if (ice->turn_server)
    gst_uri_unref (ice->turn_server);
  if (ice->stun_server)
    gst_uri_unref (ice->stun_server);

  g_mutex_clear (&ice->priv->lock);
  g_cond_clear (&ice->priv->cond);

  g_array_free (ice->priv->nice_stream_map, TRUE);

  g_object_unref (ice->priv->nice_agent);

  g_hash_table_unref (ice->turn_servers);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_webrtc_ice_constructed (GObject * object)
{
  GstWebRTCICE *ice = GST_WEBRTC_ICE (object);
  NiceAgentOption options = 0;

  _start_thread (ice);

  options |= NICE_AGENT_OPTION_ICE_TRICKLE;
  options |= NICE_AGENT_OPTION_REGULAR_NOMINATION;

  ice->priv->nice_agent = nice_agent_new_full (ice->priv->main_context,
      NICE_COMPATIBILITY_RFC5245, options);
  g_signal_connect (ice->priv->nice_agent, "new-candidate-full",
      G_CALLBACK (_on_new_candidate), ice);

  G_OBJECT_CLASS (parent_class)->constructed (object);
}

static void
gst_webrtc_ice_class_init (GstWebRTCICEClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;

  gobject_class->constructed = gst_webrtc_ice_constructed;
  gobject_class->get_property = gst_webrtc_ice_get_property;
  gobject_class->set_property = gst_webrtc_ice_set_property;
  gobject_class->finalize = gst_webrtc_ice_finalize;

  g_object_class_install_property (gobject_class,
      PROP_AGENT,
      g_param_spec_object ("agent", "ICE agent",
          "ICE agent in use by this object. WARNING! Accessing this property "
          "may have disastrous consequences for the operation of webrtcbin. "
          "Other ICE implementations may not have the same interface.",
          NICE_TYPE_AGENT, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_ICE_TCP,
      g_param_spec_boolean ("ice-tcp", "ICE TCP",
          "Whether the agent should use ICE-TCP when gathering candidates",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
      PROP_ICE_UDP,
      g_param_spec_boolean ("ice-udp", "ICE UDP",
          "Whether the agent should use ICE-UDP when gathering candidates",
          TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstWebRTCICE:min-rtp-port:
   *
   * Minimum port for local rtp port range.
   * min-rtp-port must be <= max-rtp-port
   *
   * Since: 1.20
   */
  g_object_class_install_property (gobject_class,
      PROP_MIN_RTP_PORT,
      g_param_spec_uint ("min-rtp-port", "ICE RTP candidate min port",
          "Minimum port for local rtp port range. "
          "min-rtp-port must be <= max-rtp-port",
          0, 65535, 0, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /**
   * GstWebRTCICE:max-rtp-port:
   *
   * Maximum port for local rtp port range.
   * min-rtp-port must be <= max-rtp-port
   *
   * Since: 1.20
   */
  g_object_class_install_property (gobject_class,
      PROP_MAX_RTP_PORT,
      g_param_spec_uint ("max-rtp-port", "ICE RTP candidate max port",
          "Maximum port for local rtp port range. "
          "max-rtp-port must be >= min-rtp-port",
          0, 65535, 65535,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

  /**
   * GstWebRTCICE::add-local-ip-address:
   * @object: the #GstWebRTCICE
   * @address: The local IP address
   *
   * Add a local IP address to use for ICE candidate gathering.  If none
   * are supplied, they will be discovered automatically. Calling this signal
   * stops automatic ICE gathering.
   *
   * Returns: whether the address could be added.
   */
  gst_webrtc_ice_signals[ADD_LOCAL_IP_ADDRESS_SIGNAL] =
      g_signal_new_class_handler ("add-local-ip-address",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      G_CALLBACK (gst_webrtc_ice_add_local_ip_address), NULL, NULL,
      g_cclosure_marshal_generic, G_TYPE_BOOLEAN, 1, G_TYPE_STRING);
}

static void
gst_webrtc_ice_init (GstWebRTCICE * ice)
{
  ice->priv = gst_webrtc_ice_get_instance_private (ice);

  g_mutex_init (&ice->priv->lock);
  g_cond_init (&ice->priv->cond);

  ice->turn_servers =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
      (GDestroyNotify) gst_uri_unref);

  ice->priv->nice_stream_map =
      g_array_new (FALSE, TRUE, sizeof (struct NiceStreamItem));
  g_array_set_clear_func (ice->priv->nice_stream_map,
      (GDestroyNotify) _clear_ice_stream);
}

GstWebRTCICE *
gst_webrtc_ice_new (const gchar * name)
{
  return g_object_new (GST_TYPE_WEBRTC_ICE, "name", name, NULL);
}

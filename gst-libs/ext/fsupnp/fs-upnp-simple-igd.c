/*
 * Farsight2 - Farsight UPnP IGD abstraction
 *
 * Copyright 2008 Collabora Ltd.
 *  @author: Olivier Crete <olivier.crete@collabora.co.uk>
 * Copyright 2008 Nokia Corp.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 */


#include "fs-upnp-simple-igd.h"
#include "fs-upnp-simple-igd-marshal.h"

#include <string.h>

#include <libgupnp/gupnp-control-point.h>


struct _FsUpnpSimpleIgdPrivate
{
  GMainContext *main_context;

  GUPnPContext *gupnp_context;
  GUPnPControlPoint *cp;

  GPtrArray *service_proxies;

  GPtrArray *mappings;

  gulong avail_handler;
  gulong unavail_handler;

  guint request_timeout;
};

struct Proxy {
  FsUpnpSimpleIgd *parent;
  GUPnPServiceProxy *proxy;

  gchar *external_ip;
  GUPnPServiceProxyAction *external_ip_action;

  GPtrArray *proxymappings;
};

struct Mapping {
  gchar *protocol;
  guint external_port;
  gchar *local_ip;
  guint16 local_port;
  guint32 lease_duration;
  gchar *description;
};

struct ProxyMapping {
  struct Proxy *proxy;
  struct Mapping *mapping;

  GUPnPServiceProxyAction *action;
  GSource *timeout_src;

  gboolean mapped;

  GSource *renew_src;
};

/* signals */
enum
{
  SIGNAL_NEW_EXTERNAL_IP,
  SIGNAL_MAPPED_EXTERNAL_PORT,
  SIGNAL_ERROR_MAPPING_PORT,
  SIGNAL_ERROR,
  LAST_SIGNAL
};

/* props */
enum
{
  PROP_0,
  PROP_REQUEST_TIMEOUT,
  PROP_MAIN_CONTEXT
};


static guint signals[LAST_SIGNAL] = { 0 };


#define FS_UPNP_SIMPLE_IGD_GET_PRIVATE(o)                                 \
  (G_TYPE_INSTANCE_GET_PRIVATE ((o), FS_TYPE_UPNP_SIMPLE_IGD,             \
   FsUpnpSimpleIgdPrivate))


G_DEFINE_TYPE (FsUpnpSimpleIgd, fs_upnp_simple_igd, G_TYPE_OBJECT);


static void fs_upnp_simple_igd_constructed (GObject *object);
static void fs_upnp_simple_igd_dispose (GObject *object);
static void fs_upnp_simple_igd_finalize (GObject *object);
static void fs_upnp_simple_igd_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec);
static void fs_upnp_simple_igd_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec);

static void fs_upnp_simple_igd_gather (FsUpnpSimpleIgd *self,
    struct Proxy *prox);
static void fs_upnp_simple_igd_add_proxy_mapping (FsUpnpSimpleIgd *self,
    struct Proxy *prox,
    struct Mapping *mapping);

static void free_proxy (struct Proxy *prox);
static void free_mapping (struct Mapping *mapping);

static void stop_proxymapping (struct ProxyMapping *pm);

static void fs_upnp_simple_igd_add_port_real (FsUpnpSimpleIgd *self,
    const gchar *protocol,
    guint16 external_port,
    const gchar *local_ip,
    guint16 local_port,
    guint32 lease_duration,
    const gchar *description);
static void fs_upnp_simple_igd_remove_port_real (FsUpnpSimpleIgd *self,
    const gchar *protocol,
    guint external_port);

GQuark
fs_upnp_simple_igd_get_error_domain (void)
{
  return g_quark_from_static_string ("fs-upnp-simple-igd-error");
}


static void
fs_upnp_simple_igd_class_init (FsUpnpSimpleIgdClass *klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (FsUpnpSimpleIgdPrivate));

  gobject_class->constructed = fs_upnp_simple_igd_constructed;
  gobject_class->dispose = fs_upnp_simple_igd_dispose;
  gobject_class->finalize = fs_upnp_simple_igd_finalize;
  gobject_class->set_property = fs_upnp_simple_igd_set_property;
  gobject_class->get_property = fs_upnp_simple_igd_get_property;

  klass->add_port = fs_upnp_simple_igd_add_port_real;
  klass->remove_port = fs_upnp_simple_igd_remove_port_real;

  g_object_class_install_property (gobject_class,
      PROP_REQUEST_TIMEOUT,
      g_param_spec_uint ("request-timeout",
          "The timeout after which a request is considered to have failed",
          "After this timeout, the request is considered to have failed and"
          "is dropped (in seconds).",
          0, G_MAXUINT, 5,
          G_PARAM_READWRITE));

  g_object_class_install_property (gobject_class,
      PROP_MAIN_CONTEXT,
      g_param_spec_pointer ("main-context",
          "The GMainContext to use",
          "This GMainContext will be used for all async activities",
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

   /**
   * FsUpnpSimpleIgd::new-external-ip
   * @self: #FsUpnpSimpleIgd that emitted the signal
   * @ip: The string representing the new external IP
   *
   * This signal means that a new external IP has been found on an IGD.
   *
   */
  signals[SIGNAL_NEW_EXTERNAL_IP] = g_signal_new ("new-external-ip",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__STRING,
      G_TYPE_NONE, 1, G_TYPE_STRING);


  /**
   * FsUpnpSimpleIgd::mapped-external-port
   * @self: #FsUpnpSimpleIgd that emitted the signal
   * @proto: the requested protocol ("UDP" or "TCP")
   * @external_ip: the external IP
   * @replaces_external_ip: if this mapping replaces another mapping,
   *  this is the old external IP
   * @external_port: the external port
   * @local_ip: internal ip this is forwarded to
   * @local_port: the local port
   * @description: the user's selected description
   * @ip: The string representing the new external IP
   *
   * This signal means that an IGD has been found that that adding a port
   * mapping has succeeded.
   *
   */
  signals[SIGNAL_MAPPED_EXTERNAL_PORT] = g_signal_new ("mapped-external-port",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST,
      0,
      NULL,
      NULL,
      _fs_upnp_simple_igd_marshal_VOID__STRING_STRING_STRING_UINT_STRING_UINT_STRING,
      G_TYPE_NONE, 7, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_UINT,
      G_TYPE_STRING, G_TYPE_UINT, G_TYPE_STRING);

  /**
   * FsUpnpSimpleIgd::error-mapping-port
   * @self: #FsUpnpSimpleIgd that emitted the signal
   * @error: a #GError
   * @proto: The requested protocol
   * @external_port: the requested external port
   * @description: the passed description
   *
   * This means that mapping a port on a specific IGD has failed (it may still
   * succeed on other IGDs on the network).
   */
  signals[SIGNAL_ERROR_MAPPING_PORT] = g_signal_new ("error-mapping-port",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
      0,
      NULL,
      NULL,
      _fs_upnp_simple_igd_marshal_VOID__POINTER_STRING_UINT_STRING,
      G_TYPE_NONE, 4, G_TYPE_POINTER, G_TYPE_STRING, G_TYPE_UINT,
      G_TYPE_STRING);

  /**
   * FsUpnpSimpleIgd::error
   * @self: #FsUpnpSimpleIgd that emitted the signal
   * @error: a #GError
   *
   * This means that an asynchronous error has happened.
   *
   */
  signals[SIGNAL_ERROR] = g_signal_new ("error",
      G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
      0,
      NULL,
      NULL,
      g_cclosure_marshal_VOID__POINTER,
      G_TYPE_NONE, 1, G_TYPE_POINTER);
}

static void
fs_upnp_simple_igd_init (FsUpnpSimpleIgd *self)
{
  self->priv = FS_UPNP_SIMPLE_IGD_GET_PRIVATE (self);

  self->priv->request_timeout = 5;

  self->priv->service_proxies = g_ptr_array_new ();
  self->priv->mappings = g_ptr_array_new ();
}

static void
fs_upnp_simple_igd_dispose (GObject *object)
{
  FsUpnpSimpleIgd *self = FS_UPNP_SIMPLE_IGD_CAST (object);

  if (self->priv->avail_handler)
    g_signal_handler_disconnect (self->priv->cp, self->priv->avail_handler);
  self->priv->avail_handler = 0;

  if (self->priv->unavail_handler)
    g_signal_handler_disconnect (self->priv->cp, self->priv->unavail_handler);
  self->priv->unavail_handler = 0;

  while (self->priv->mappings->len)
  {
    free_mapping (
        g_ptr_array_index (self->priv->mappings, 0));
    g_ptr_array_remove_index_fast (self->priv->mappings, 0);
  }

  while (self->priv->service_proxies->len)
  {
    free_proxy (
        g_ptr_array_index (self->priv->service_proxies, 0));
    g_ptr_array_remove_index_fast (self->priv->service_proxies, 0);
  }

  if (self->priv->cp)
    g_object_unref (self->priv->cp);
  self->priv->cp = NULL;

  if (self->priv->gupnp_context)
    g_object_unref (self->priv->gupnp_context);
  self->priv->gupnp_context = NULL;

  G_OBJECT_CLASS (fs_upnp_simple_igd_parent_class)->dispose (object);
}


static void
_external_ip_address_changed (GUPnPServiceProxy *proxy, const gchar *variable,
    GValue *value, gpointer user_data)
{
  struct Proxy *prox = user_data;
  gchar *new_ip;
  guint i;

  g_return_if_fail (G_VALUE_HOLDS_STRING(value));

  new_ip = g_value_dup_string (value);

  for (i=0; i < prox->proxymappings->len; i++)
  {
    struct ProxyMapping *pm = g_ptr_array_index (prox->proxymappings, i);

    if (pm->mapped)
      g_signal_emit (prox->parent, signals[SIGNAL_MAPPED_EXTERNAL_PORT], 0,
          pm->mapping->protocol, new_ip, prox->external_ip,
          pm->mapping->external_port, pm->mapping->local_ip,
          pm->mapping->local_port, pm->mapping->description);
  }

  g_free (prox->external_ip);
  prox->external_ip = new_ip;
}

static void
free_proxy (struct Proxy *prox)
{
  if (prox->external_ip_action)
    gupnp_service_proxy_cancel_action (prox->proxy, prox->external_ip_action);

  gupnp_service_proxy_remove_notify (prox->proxy, "ExternalIPAddress",
      _external_ip_address_changed, prox);

  g_object_unref (prox->proxy);
  g_ptr_array_foreach (prox->proxymappings, (GFunc) stop_proxymapping, NULL);
  g_ptr_array_free (prox->proxymappings, TRUE);
  g_slice_free (struct Proxy, prox);
}

static void
free_mapping (struct Mapping *mapping)
{
  g_free (mapping->protocol);
  g_free (mapping->local_ip);
  g_free (mapping->description);
  g_slice_free (struct Mapping, mapping);
}

static void
fs_upnp_simple_igd_finalize (GObject *object)
{
  FsUpnpSimpleIgd *self = FS_UPNP_SIMPLE_IGD_CAST (object);

  g_main_context_unref (self->priv->main_context);

  g_warn_if_fail (self->priv->service_proxies->len == 0);
  g_ptr_array_free (self->priv->service_proxies, TRUE);

  g_warn_if_fail (self->priv->mappings->len == 0);
  g_ptr_array_free (self->priv->mappings, TRUE);

  G_OBJECT_CLASS (fs_upnp_simple_igd_parent_class)->finalize (object);
}

static void
fs_upnp_simple_igd_get_property (GObject *object, guint prop_id,
    GValue *value, GParamSpec *pspec)
{
  FsUpnpSimpleIgd *self = FS_UPNP_SIMPLE_IGD_CAST (object);

  switch (prop_id) {
    case PROP_REQUEST_TIMEOUT:
      g_value_set_uint (value, self->priv->request_timeout);
      break;
    case PROP_MAIN_CONTEXT:
      g_value_set_pointer (value, self->priv->main_context);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }

}

static void
fs_upnp_simple_igd_set_property (GObject *object, guint prop_id,
    const GValue *value, GParamSpec *pspec)
{
  FsUpnpSimpleIgd *self = FS_UPNP_SIMPLE_IGD_CAST (object);

  switch (prop_id) {
    case PROP_REQUEST_TIMEOUT:
      self->priv->request_timeout = g_value_get_uint (value);
      break;
    case PROP_MAIN_CONTEXT:
      if (!self->priv->main_context && g_value_get_pointer (value))
      {
        self->priv->main_context = g_value_get_pointer (value);
        g_main_context_ref (self->priv->main_context);
      }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
_cp_service_avail (GUPnPControlPoint *cp,
    GUPnPServiceProxy *proxy,
    FsUpnpSimpleIgd *self)
{
  struct Proxy *prox = g_slice_new0 (struct Proxy);
  guint i;

  prox->parent = self;
  prox->proxy = g_object_ref (proxy);
  prox->proxymappings = g_ptr_array_new ();

  fs_upnp_simple_igd_gather (self, prox);

  for (i = 0; i < self->priv->mappings->len; i++)
    fs_upnp_simple_igd_add_proxy_mapping (self, prox,
        g_ptr_array_index (self->priv->mappings, i));

  g_ptr_array_add(self->priv->service_proxies, prox);
}


static void
_cp_service_unavail (GUPnPControlPoint *cp,
    GUPnPServiceProxy *proxy,
    FsUpnpSimpleIgd *self)
{
  guint i;

  for (i=0; i < self->priv->service_proxies->len; i++)
  {
    struct Proxy *prox =
      g_ptr_array_index (self->priv->service_proxies, i);

    if (!strcmp (gupnp_service_info_get_udn (GUPNP_SERVICE_INFO (prox->proxy)),
            gupnp_service_info_get_udn (GUPNP_SERVICE_INFO (prox->proxy))))
    {
      g_ptr_array_foreach (prox->proxymappings, (GFunc) stop_proxymapping,
          NULL);
      free_proxy (prox);
      g_ptr_array_remove_index_fast (self->priv->service_proxies, i);
      break;
    }
  }
}


static void
fs_upnp_simple_igd_constructed (GObject *object)
{
  FsUpnpSimpleIgd *self = FS_UPNP_SIMPLE_IGD_CAST (object);

  if (!self->priv->main_context)
    self->priv->main_context = g_main_context_ref (g_main_context_default ());

  self->priv->gupnp_context = gupnp_context_new (self->priv->main_context,
      NULL, 0, NULL);
  g_return_if_fail (self->priv->gupnp_context);

  self->priv->cp = gupnp_control_point_new (self->priv->gupnp_context,
      "urn:schemas-upnp-org:service:WANIPConnection:1");
  g_return_if_fail (self->priv->cp);

  self->priv->avail_handler = g_signal_connect (self->priv->cp,
      "service-proxy-available",
      G_CALLBACK (_cp_service_avail), self);
  self->priv->unavail_handler = g_signal_connect (self->priv->cp,
      "service-proxy-unavailable",
      G_CALLBACK (_cp_service_unavail), self);

  gssdp_resource_browser_set_active (GSSDP_RESOURCE_BROWSER (self->priv->cp),
      TRUE);

  if (G_OBJECT_CLASS (fs_upnp_simple_igd_parent_class)->constructed)
    G_OBJECT_CLASS (fs_upnp_simple_igd_parent_class)->constructed (object);
}

FsUpnpSimpleIgd *
fs_upnp_simple_igd_new (GMainContext *main_context)
{
  return g_object_new (FS_TYPE_UPNP_SIMPLE_IGD,
      "main-context", main_context, NULL);
}


static void
_service_proxy_got_external_ip_address (GUPnPServiceProxy *proxy,
    GUPnPServiceProxyAction *action,
    gpointer user_data)
{
  struct Proxy *prox = user_data;
  FsUpnpSimpleIgd *self = prox->parent;
  GError *error = NULL;
  gchar *ip = NULL;

  g_return_if_fail (prox->external_ip_action == action);

  prox->external_ip_action = NULL;

  if (gupnp_service_proxy_end_action (proxy, action, &error,
          "NewExternalIPAddress", G_TYPE_STRING, &ip,
          NULL))
  {
    guint i;

    for (i=0; i < prox->proxymappings->len; i++)
    {
      struct ProxyMapping *pm = g_ptr_array_index (prox->proxymappings, i);

      if (pm->mapped)
        g_signal_emit (self, signals[SIGNAL_MAPPED_EXTERNAL_PORT], 0,
            pm->mapping->protocol, ip, prox->external_ip,
            pm->mapping->external_port, pm->mapping->local_ip,
            pm->mapping->local_port, pm->mapping->description);
    }

    g_free (prox->external_ip);
    prox->external_ip = g_strdup (ip);

    g_signal_emit (self, signals[SIGNAL_NEW_EXTERNAL_IP], 0,
        ip);
  }
  else
  {
    g_return_if_fail (error);
    g_signal_emit (self, signals[SIGNAL_ERROR_MAPPING_PORT], error->domain,
        error, pm->mapping->protocol, pm->mapping->external_port,
        pm->mapping->description);
  }
  g_clear_error (&error);
}

static void
fs_upnp_simple_igd_gather (FsUpnpSimpleIgd *self,
    struct Proxy *prox)
{
  prox->external_ip_action = gupnp_service_proxy_begin_action (prox->proxy,
      "GetExternalIPAddress",
      _service_proxy_got_external_ip_address, prox, NULL);

  gupnp_service_proxy_add_notify (prox->proxy, "ExternalIPAddress",
      G_TYPE_STRING, _external_ip_address_changed, prox);

  gupnp_service_proxy_set_subscribed (prox->proxy, TRUE);
}

static void
_service_proxy_renewed_port_mapping (GUPnPServiceProxy *proxy,
    GUPnPServiceProxyAction *action,
    gpointer user_data)
{
  struct ProxyMapping *pm = user_data;
  FsUpnpSimpleIgd *self = pm->proxy->parent;
  GError *error = NULL;

  if (!gupnp_service_proxy_end_action (proxy, action, &error,
          NULL))
  {
    g_return_if_fail (error);
    g_signal_emit (self, signals[SIGNAL_ERROR_MAPPING_PORT], error->domain,
        error, pm->mapping->protocol, pm->mapping->external_port,
        pm->mapping->description);
  }
  g_clear_error (&error);
}

static gboolean
_renew_mapping_timeout (gpointer user_data)
{
  struct ProxyMapping *pm = user_data;

  gupnp_service_proxy_begin_action (pm->proxy->proxy,
      "AddPortMapping",
      _service_proxy_renewed_port_mapping, pm,
      "NewRemoteHost", G_TYPE_STRING, "",
      "NewExternalPort", G_TYPE_UINT, pm->mapping->external_port,
      "NewProtocol", G_TYPE_STRING, pm->mapping->protocol,
      "NewInternalPort", G_TYPE_UINT, pm->mapping->local_port,
      "NewInternalClient", G_TYPE_STRING, pm->mapping->local_ip,
      "NewEnabled", G_TYPE_BOOLEAN, TRUE,
      "NewPortMappingDescription", G_TYPE_STRING, pm->mapping->description,
      "NewLeaseDuration", G_TYPE_UINT, pm->mapping->lease_duration,
      NULL);

  return TRUE;
}

static void
_service_proxy_added_port_mapping (GUPnPServiceProxy *proxy,
    GUPnPServiceProxyAction *action,
    gpointer user_data)
{
  struct ProxyMapping *pm = user_data;
  FsUpnpSimpleIgd *self = pm->proxy->parent;
  GError *error = NULL;

  g_return_if_fail (pm->action == action);

  pm->action = NULL;

  if (gupnp_service_proxy_end_action (proxy, action, &error,
          NULL))
  {
    pm->mapped = TRUE;

    if (pm->proxy->external_ip)
      g_signal_emit (self, signals[SIGNAL_MAPPED_EXTERNAL_PORT], 0,
          pm->mapping->protocol, pm->proxy->external_ip, NULL,
          pm->mapping->external_port, pm->mapping->local_ip,
          pm->mapping->local_port, pm->mapping->description);



    pm->renew_src =
      g_timeout_source_new_seconds (pm->mapping->lease_duration / 2);
    g_source_set_callback (pm->renew_src,
        _renew_mapping_timeout, pm, NULL);
    g_source_attach (pm->renew_src, self->priv->main_context);

  }
  else
  {
    g_return_if_fail (error);
    g_signal_emit (self, signals[SIGNAL_ERROR_MAPPING_PORT], error->domain,
        error, pm->mapping->protocol, pm->mapping->external_port,
        pm->mapping->description);
  }
  g_clear_error (&error);

  stop_proxymapping (pm);
}

static gboolean
_service_proxy_add_mapping_timeout (gpointer user_data)
{
  struct ProxyMapping *pm = user_data;
  FsUpnpSimpleIgd *self = pm->proxy->parent;
  const GError error = {FS_UPNP_SIMPLE_IGD_ERROR,
                        FS_UPNP_SIMPLE_IGD_ERROR_TIMEOUT,
                        "Timeout while mapping port"};

  stop_proxymapping (pm);

  g_signal_emit (self, signals[SIGNAL_ERROR_MAPPING_PORT],
      FS_UPNP_SIMPLE_IGD_ERROR, &error,
      pm->mapping->protocol, pm->mapping->external_port,
      pm->mapping->description);

  return FALSE;
}

static void
fs_upnp_simple_igd_add_proxy_mapping (FsUpnpSimpleIgd *self, struct Proxy *prox,
    struct Mapping *mapping)
{
  struct ProxyMapping *pm = g_slice_new0 (struct ProxyMapping);

  pm->proxy = prox;
  pm->mapping = mapping;

  pm->action = gupnp_service_proxy_begin_action (prox->proxy,
      "AddPortMapping",
      _service_proxy_added_port_mapping, pm,
      "NewRemoteHost", G_TYPE_STRING, "",
      "NewExternalPort", G_TYPE_UINT, mapping->external_port,
      "NewProtocol", G_TYPE_STRING, mapping->protocol,
      "NewInternalPort", G_TYPE_UINT, mapping->local_port,
      "NewInternalClient", G_TYPE_STRING, mapping->local_ip,
      "NewEnabled", G_TYPE_BOOLEAN, TRUE,
      "NewPortMappingDescription", G_TYPE_STRING, mapping->description,
      "NewLeaseDuration", G_TYPE_UINT, mapping->lease_duration,
      NULL);

  pm->timeout_src =
    g_timeout_source_new_seconds (self->priv->request_timeout);
  g_source_set_callback (pm->timeout_src,
      _service_proxy_add_mapping_timeout, pm, NULL);
  g_source_attach (pm->timeout_src, self->priv->main_context);

  g_ptr_array_add (prox->proxymappings, pm);
}

static void
fs_upnp_simple_igd_add_port_real (FsUpnpSimpleIgd *self,
    const gchar *protocol,
    guint16 external_port,
    const gchar *local_ip,
    guint16 local_port,
    guint32 lease_duration,
    const gchar *description)
{
  struct Mapping *mapping = g_slice_new0 (struct Mapping);
  guint i;

  g_return_if_fail (protocol && local_ip);
  g_return_if_fail (!strcmp (protocol, "UDP") || !strcmp (protocol, "TCP"));

  mapping->protocol = g_strdup (protocol);
  mapping->external_port = external_port;
  mapping->local_ip = g_strdup (local_ip);
  mapping->local_port = local_port;
  mapping->lease_duration = lease_duration;
  mapping->description = g_strdup (description);

  if (!mapping->description)
    mapping->description = g_strdup ("");

  g_ptr_array_add (self->priv->mappings, mapping);

  for (i=0; i < self->priv->service_proxies->len; i++)
    fs_upnp_simple_igd_add_proxy_mapping (self,
        g_ptr_array_index (self->priv->service_proxies, i), mapping);
}

void
fs_upnp_simple_igd_add_port (FsUpnpSimpleIgd *self,
    const gchar *protocol,
    guint16 external_port,
    const gchar *local_ip,
    guint16 local_port,
    guint32 lease_duration,
    const gchar *description)
{
  FsUpnpSimpleIgdClass *klass = FS_UPNP_SIMPLE_IGD_GET_CLASS (self);

  g_return_if_fail (klass->add_port);

  klass->add_port (self, protocol, external_port, local_ip, local_port,
      lease_duration, description);
}


static void
_service_proxy_delete_port_mapping (GUPnPServiceProxy *proxy,
    GUPnPServiceProxyAction *action,
    gpointer user_data)
{
  GError *error = NULL;


  if (!gupnp_service_proxy_end_action (proxy, action, &error,
          NULL))
  {
    g_return_if_fail (error);
    g_warning ("Error deleting port mapping: %s", error->message);
  }
  g_clear_error (&error);
}

static void
fs_upnp_simple_igd_remove_port_real (FsUpnpSimpleIgd *self,
    const gchar *protocol,
    guint external_port)
{
  guint i, j;
  struct Mapping *mapping;

  g_return_if_fail (protocol);

  for (i = 0; i < self->priv->mappings->len; i++)
  {
    struct Mapping *tmpmapping = g_ptr_array_index (self->priv->mappings, i);
    if (tmpmapping->external_port == external_port &&
        !strcmp (tmpmapping->protocol, protocol))
    {
      mapping = tmpmapping;
      break;
    }
  }
  g_return_if_fail (mapping);

  g_ptr_array_remove_index_fast (self->priv->mappings, i);

  for (i=0; i < self->priv->service_proxies->len; i++)
  {
    struct Proxy *prox = g_ptr_array_index (self->priv->service_proxies, i);

    for (j=0; j < prox->proxymappings->len; j++)
    {
      struct ProxyMapping *pm = g_ptr_array_index (prox->proxymappings, j);
      if (pm->mapping == mapping)
      {
        stop_proxymapping (pm);

        if (pm->renew_src)
          g_source_destroy (pm->renew_src);
        pm->renew_src = NULL;

        if (pm->mapped)
          gupnp_service_proxy_begin_action (prox->proxy,
              "DeletePortMapping",
              _service_proxy_delete_port_mapping, self,
              "NewRemoteHost", G_TYPE_STRING, "",
              "NewExternalPort", G_TYPE_UINT, mapping->external_port,
              "NewProtocol", G_TYPE_STRING, mapping->protocol,
              NULL);

        g_slice_free (struct ProxyMapping, pm);
        g_ptr_array_remove_index_fast (prox->proxymappings, j);
        j--;
      }
    }
  }

  free_mapping (mapping);
}

void
fs_upnp_simple_igd_remove_port (FsUpnpSimpleIgd *self,
    const gchar *protocol,
    guint external_port)
{
  FsUpnpSimpleIgdClass *klass = FS_UPNP_SIMPLE_IGD_GET_CLASS (self);

  g_return_if_fail (klass->remove_port);

  klass->remove_port (self, protocol, external_port);
}

static void
stop_proxymapping (struct ProxyMapping *pm)
{
  if (pm->action)
    gupnp_service_proxy_cancel_action (pm->proxy->proxy,
        pm->action);
  pm->action = NULL;

  if (pm->timeout_src)
    g_source_destroy (pm->timeout_src);
  pm->timeout_src = NULL;
}

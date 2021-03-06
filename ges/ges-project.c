/* GStreamer Editing Services
 *
 * Copyright (C) <2012> Thibault Saunier <thibault.saunier@collabora.com>
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
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */
/**
 * SECTION: ges-project
 * @short_description: A GESAsset that is used to manage projects
 *
 * The #GESProject is used to control a set of #GESAsset and is a
 * #GESAsset with #GES_TYPE_TIMELINE as @extractable_type itself. That
 * means that you can extract #GESTimeline from a project as followed:
 *
 * |[
 *  GESProject *project;
 *  GESTimeline *timeline;
 *
 *  project = ges_project_new ("file:///path/to/a/valid/project/uri");
 *
 *  // Here you can connect to the various signal to get more infos about
 *  // what is happening and recover from errors if possible
 *  ...
 *
 *  timeline = ges_asset_extract (GES_ASSET (project));
 * ]|
 *
 * The #GESProject class offers a higher level API to handle #GESAsset-s.
 * It lets you request new asset, and it informs you about new assets through
 * a set of signals. Also it handles problem such as missing files/missing
 * #GstElement and lets you try to recover from those.
 */
#include "ges.h"
#include "ges-internal.h"
#include <glib/gstdio.h>

/* TODO We should rely on both extractable_type and @id to identify
 * a Asset, not only @id
 */
G_DEFINE_TYPE (GESProject, ges_project, GES_TYPE_ASSET);

struct _GESProjectPrivate
{
  GHashTable *assets;
  /* Set of asset ID being loaded */
  GHashTable *loading_assets;
  GHashTable *loaded_with_error;
  GESAsset *formatter_asset;

  GList *formatters;

  gchar *uri;

  GList *encoding_profiles;

  GstEncodingProfile *proxy_profile;
  GstElement *proxy_pipeline;
  GESAsset *proxy_asset;
  GESAsset *proxy_parent;
  GList *create_proxies;
  GList *timeline_proxies;
  GHashTable *proxies;
  GHashTable *proxied_assets;
  gboolean proxies_creation_started;
  gboolean proxies_created;
  gchar *proxy_uri;
  gchar *proxies_location;
};

typedef struct EmitLoadedInIdle
{
  GESProject *project;
  GESTimeline *timeline;
} EmitLoadedInIdle;

enum
{
  LOADED_SIGNAL,
  ERROR_LOADING_ASSET,
  ASSET_ADDED_SIGNAL,
  ASSET_REMOVED_SIGNAL,
  MISSING_URI_SIGNAL,
  PROXIES_CREATION_STARTED_SIGNAL,
  PROXIES_CREATION_PAUSED_SIGNAL,
  PROXIES_CREATION_CANCELLED_SIGNAL,
  PROXIES_CREATED_SIGNAL,
  LAST_SIGNAL
};

static guint _signals[LAST_SIGNAL] = { 0 };

static guint nb_projects = 0;

enum
{
  PROP_0,
  PROP_URI,
  PROP_LAST,
};

static GParamSpec *_properties[LAST_SIGNAL] = { 0 };

static gboolean _transcode (GESProject * project, GESAsset * asset);
static gboolean _create_proxy_asset (GESProject * project, const gchar * id,
    GType extractable_type);

static gboolean
_emit_loaded_in_idle (EmitLoadedInIdle * data)
{
  ges_timeline_commit (data->timeline);
  g_signal_emit (data->project, _signals[LOADED_SIGNAL], 0, data->timeline);

  gst_object_unref (data->project);
  gst_object_unref (data->timeline);
  g_slice_free (EmitLoadedInIdle, data);

  return FALSE;
}

static void
ges_project_add_formatter (GESProject * project, GESFormatter * formatter)
{
  GESProjectPrivate *priv = GES_PROJECT (project)->priv;

  ges_formatter_set_project (formatter, project);
  priv->formatters = g_list_append (priv->formatters, formatter);

  gst_object_ref_sink (formatter);
}

static void
ges_project_remove_formatter (GESProject * project, GESFormatter * formatter)
{
  GList *tmp;
  GESProjectPrivate *priv = GES_PROJECT (project)->priv;

  for (tmp = priv->formatters; tmp; tmp = tmp->next) {
    if (tmp->data == formatter) {
      gst_object_unref (formatter);
      priv->formatters = g_list_delete_link (priv->formatters, tmp);

      return;
    }
  }
}

static void
ges_project_set_uri (GESProject * project, const gchar * uri)
{
  GESProjectPrivate *priv;

  g_return_if_fail (GES_IS_PROJECT (project));

  priv = project->priv;
  if (priv->uri) {
    GST_WARNING_OBJECT (project, "Trying to rest URI, this is prohibited");

    return;
  }

  if (uri == NULL || !gst_uri_is_valid (uri)) {
    GST_LOG_OBJECT (project, "Invalid URI: %s", uri);
    return;
  }

  priv->uri = g_strdup (uri);

  /* We use that URI as ID */
  ges_asset_set_id (GES_ASSET (project), uri);

  return;
}

static gboolean
_load_project (GESProject * project, GESTimeline * timeline, GError ** error)
{
  GError *lerr = NULL;
  GESProjectPrivate *priv;
  GESFormatter *formatter;

  priv = GES_PROJECT (project)->priv;

  if (priv->uri == NULL) {
    EmitLoadedInIdle *data = g_slice_new (EmitLoadedInIdle);

    GST_LOG_OBJECT (project, "%s, Loading an empty timeline %s"
        " as no URI set yet", GST_OBJECT_NAME (timeline),
        ges_asset_get_id (GES_ASSET (project)));

    data->timeline = gst_object_ref (timeline);
    data->project = gst_object_ref (project);

    /* Make sure the signal is emitted after the functions ends */
    g_idle_add ((GSourceFunc) _emit_loaded_in_idle, data);
    return TRUE;
  }

  if (priv->formatter_asset == NULL)
    priv->formatter_asset = _find_formatter_asset_for_uri (priv->uri);

  if (priv->formatter_asset == NULL)
    goto failed;

  formatter = GES_FORMATTER (ges_asset_extract (priv->formatter_asset, &lerr));
  if (lerr) {
    GST_WARNING_OBJECT (project, "Could not create the formatter: %s",
        (*error)->message);

    goto failed;
  }

  ges_project_add_formatter (GES_PROJECT (project), formatter);
  ges_formatter_load_from_uri (formatter, timeline, priv->uri, &lerr);
  if (lerr) {
    GST_WARNING_OBJECT (project, "Could not load the timeline,"
        " returning: %s", lerr->message);
    goto failed;
  }

  return TRUE;

failed:
  if (lerr)
    g_propagate_error (error, lerr);
  return FALSE;
}

static gboolean
_uri_missing_accumulator (GSignalInvocationHint * ihint, GValue * return_accu,
    const GValue * handler_return, gpointer data)
{
  const gchar *ret = g_value_get_string (handler_return);

  if (ret && gst_uri_is_valid (ret)) {
    g_value_set_string (return_accu, ret);
    return FALSE;
  }

  return TRUE;
}

/* GESAsset vmethod implementation */
static GESExtractable *
ges_project_extract (GESAsset * project, GError ** error)
{
  GESTimeline *timeline = ges_timeline_new ();

  if (_load_project (GES_PROJECT (project), timeline, error))
    return GES_EXTRACTABLE (timeline);

  gst_object_unref (timeline);
  return NULL;
}

/* GObject vmethod implementation */
static void
_dispose (GObject * object)
{
  GList *tmp;
  GESProjectPrivate *priv = GES_PROJECT (object)->priv;

  if (priv->assets)
    g_hash_table_unref (priv->assets);
  if (priv->loading_assets)
    g_hash_table_unref (priv->loading_assets);
  if (priv->loaded_with_error)
    g_hash_table_unref (priv->loaded_with_error);
  if (priv->formatter_asset)
    gst_object_unref (priv->formatter_asset);
  if (priv->proxy_profile)
    gst_object_unref (priv->proxy_profile);
  if (priv->proxy_pipeline)
    gst_object_unref (priv->proxy_pipeline);
  if (priv->proxies)
    g_hash_table_unref (priv->proxies);
  if (priv->proxied_assets)
    g_hash_table_unref (priv->proxied_assets);
  if (priv->proxy_uri)
    g_free (priv->proxy_uri);
  if (priv->proxies_location)
    g_free (priv->proxies_location);
  if (priv->proxy_asset)
    g_free (priv->proxy_asset);
  if (priv->proxy_parent)
    g_free (priv->proxy_parent);
  if (priv->create_proxies)
    g_list_free_full (priv->create_proxies, g_free);
  if (priv->timeline_proxies)
    g_list_free_full (priv->timeline_proxies, g_free);

  for (tmp = priv->formatters; tmp; tmp = tmp->next)
    ges_project_remove_formatter (GES_PROJECT (object), tmp->data);;

  G_OBJECT_CLASS (ges_project_parent_class)->dispose (object);
}

static void
_finalize (GObject * object)
{
  GESProjectPrivate *priv = GES_PROJECT (object)->priv;

  if (priv->uri)
    g_free (priv->uri);
}

static void
_get_property (GESProject * project, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GESProjectPrivate *priv = project->priv;

  switch (property_id) {
    case PROP_URI:
      g_value_set_string (value, priv->uri);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (project, property_id, pspec);
  }
}

static void
_set_property (GESProject * project, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (property_id) {
    case PROP_URI:
      project->priv->uri = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (project, property_id, pspec);
  }
}

static void
ges_project_class_init (GESProjectClass * klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (GESProjectPrivate));

  klass->asset_added = NULL;
  klass->missing_uri = NULL;
  klass->loading_error = NULL;
  klass->asset_removed = NULL;
  object_class->get_property = (GObjectGetPropertyFunc) _get_property;
  object_class->set_property = (GObjectSetPropertyFunc) _set_property;

  /**
   * GESProject::uri:
   *
   * The location of the project to use.
   */
  _properties[PROP_URI] = g_param_spec_string ("uri", "URI",
      "uri of the project", NULL, G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);

  g_object_class_install_properties (object_class, PROP_LAST, _properties);

  /**
   * GESProject::asset-added:
   * @formatter: the #GESProject
   * @asset: The #GESAsset that has been added to @project
   */
  _signals[ASSET_ADDED_SIGNAL] =
      g_signal_new ("asset-added", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass, asset_added),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 1, GES_TYPE_ASSET);

  /**
   * GESProject::asset-removed:
   * @formatter: the #GESProject
   * @asset: The #GESAsset that has been removed from @project
   */
  _signals[ASSET_REMOVED_SIGNAL] =
      g_signal_new ("asset-removed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass, asset_removed),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 1, GES_TYPE_ASSET);

  /**
   * GESProject::loaded:
   * @project: the #GESProject that is done loading a project.
   * @timeline: The #GESTimeline that complete loading
   */
  _signals[LOADED_SIGNAL] =
      g_signal_new ("loaded", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_FIRST, G_STRUCT_OFFSET (GESProjectClass, loaded),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE,
      1, GES_TYPE_TIMELINE);

  /**
   * GESProject::missing-uri:
   * @project: the #GESProject reporting that a file has moved
   * @error: The error that happened
   * @wrong_asset: The asset with the wrong ID, you should us it and its content
   * only to find out what the new location is.
   *
   * |[
   * static gchar
   * source_moved_cb (GESProject *project, GError *error, GESAsset *asset_with_error)
   * {
   *   return g_strdup ("file:///the/new/uri.ogg");
   * }
   *
   * static int
   * main (int argc, gchar ** argv)
   * {
   *   GESTimeline *timeline;
   *   GESProject *project = ges_project_new ("file:///some/uri.xges");
   *
   *   g_signal_connect (project, "missing-uri", source_moved_cb, NULL);
   *   timeline = ges_asset_extract (GES_ASSET (project));
   * }
   * ]|
   *
   * Returns: (transfer full) (allow-none): The new URI of @wrong_asset
   */
  _signals[MISSING_URI_SIGNAL] =
      g_signal_new ("missing-uri", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass, missing_uri),
      _uri_missing_accumulator, NULL, g_cclosure_marshal_generic,
      G_TYPE_STRING, 2, G_TYPE_ERROR, GES_TYPE_ASSET);

  /**
   * GESProject::error-loading-asset:
   * @project: the #GESProject on which a problem happend when creted a #GESAsset
   * @error: The #GError defining the error that accured, might be %NULL
   * @id: The @id of the asset that failed loading
   * @extractable_type: The @extractable_type of the asset that
   * failed loading
   *
   * Informs you that a #GESAsset could not be created. In case of
   * missing GStreamer plugins, the error will be set to #GST_CORE_ERROR
   * #GST_CORE_ERROR_MISSING_PLUGIN
   */
  _signals[ERROR_LOADING_ASSET] =
      g_signal_new ("error-loading-asset", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass, loading_error),
      NULL, NULL, g_cclosure_marshal_generic,
      G_TYPE_NONE, 3, G_TYPE_ERROR, G_TYPE_STRING, G_TYPE_GTYPE);

  /**
   * GESProject::proxies-created:
   * @project: the #GESProject reporting that a proxies created.
   */
  _signals[PROXIES_CREATED_SIGNAL] =
      g_signal_new ("proxies-created", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass, proxies_created),
      NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 0);

  /**
   * GESProject::proxies-creation-started:
   * @project: the #GESProject reporting that a proxies creation started.
   */
  _signals[PROXIES_CREATION_STARTED_SIGNAL] =
      g_signal_new ("proxies-creation-started", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass,
          proxies_creation_started), NULL, NULL, g_cclosure_marshal_generic,
      G_TYPE_NONE, 0);

  /**
   * GESProject::proxies-creation-paused:
   * @project: the #GESProject reporting that a proxies creation paused.
   */
  _signals[PROXIES_CREATION_PAUSED_SIGNAL] =
      g_signal_new ("proxies-creation-paused", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass,
          proxies_creation_paused), NULL, NULL, g_cclosure_marshal_generic,
      G_TYPE_NONE, 0);

  /**
   * GESProject::proxies-creation-cancelled:
   * @project: the #GESProject reporting that a proxies creation cancelled.
   */
  _signals[PROXIES_CREATION_CANCELLED_SIGNAL] =
      g_signal_new ("proxies-creation-cancelled", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET (GESProjectClass,
          proxies_creation_cancelled), NULL, NULL, g_cclosure_marshal_generic,
      G_TYPE_NONE, 0);

  object_class->dispose = _dispose;
  object_class->dispose = _finalize;

  GES_ASSET_CLASS (klass)->extract = ges_project_extract;
}

static void
ges_project_init (GESProject * project)
{
  GESProjectPrivate *priv = project->priv =
      G_TYPE_INSTANCE_GET_PRIVATE (project,
      GES_TYPE_PROJECT, GESProjectPrivate);

  priv->uri = NULL;
  priv->formatters = NULL;
  priv->formatter_asset = NULL;
  priv->encoding_profiles = NULL;
  priv->proxy_profile = NULL;
  priv->proxies_creation_started = FALSE;
  priv->proxies_created = FALSE;
  priv->proxy_uri = NULL;
  priv->proxies_location = NULL;
  priv->proxy_asset = NULL;
  priv->proxy_parent = NULL;
  priv->create_proxies = NULL;
  priv->timeline_proxies = NULL;
  priv->assets = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, gst_object_unref);
  priv->loading_assets = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, gst_object_unref);
  priv->loaded_with_error = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, NULL);
  priv->proxies = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, gst_object_unref);
  priv->proxied_assets = g_hash_table_new_full (g_str_hash, g_str_equal,
      g_free, gst_object_unref);
}

static void
_send_error_loading_asset (GESProject * project, GESAsset * asset,
    GError * error)
{
  const gchar *id = ges_asset_get_id (asset);

  GST_DEBUG_OBJECT (project, "Sending error loading asset for %s", id);
  g_hash_table_remove (project->priv->loading_assets, id);
  g_hash_table_add (project->priv->loaded_with_error, g_strdup (id));
  g_signal_emit (project, _signals[ERROR_LOADING_ASSET], 0, error, id,
      ges_asset_get_extractable_type (asset));
}

gchar *
ges_project_try_updating_id (GESProject * project, GESAsset * asset,
    GError * error)
{
  gchar *new_id = NULL;
  const gchar *id;

  g_return_val_if_fail (GES_IS_PROJECT (project), NULL);
  g_return_val_if_fail (GES_IS_ASSET (asset), NULL);
  g_return_val_if_fail (error, NULL);

  id = ges_asset_get_id (asset);
  GST_DEBUG_OBJECT (project, "Try to proxy %s", id);
  if (ges_asset_request_id_update (asset, &new_id, error) == FALSE) {
    GST_DEBUG_OBJECT (project, "Type: %s can not be proxied for id: %s",
        g_type_name (G_OBJECT_TYPE (asset)), id);
    _send_error_loading_asset (project, asset, error);

    return NULL;
  }

  if (new_id == NULL) {
    GST_DEBUG_OBJECT (project, "Sending 'missing-uri' signal for %s", id);
    g_signal_emit (project, _signals[MISSING_URI_SIGNAL], 0, error, asset,
        &new_id);
  }

  if (new_id) {
    GST_DEBUG_OBJECT (project, "new id found: %s", new_id);
    if (!ges_asset_set_proxy (asset, new_id)) {
      g_free (new_id);
      new_id = NULL;
    }
  }

  g_hash_table_remove (project->priv->loading_assets, id);

  if (new_id == NULL)
    _send_error_loading_asset (project, asset, error);


  return new_id;
}

static void
new_asset_cb (GESAsset * source, GAsyncResult * res, GESProject * project)
{
  GError *error = NULL;
  gchar *possible_id = NULL;
  GESAsset *asset = ges_asset_request_finish (res, &error);

  if (error) {
    possible_id = ges_project_try_updating_id (project, source, error);

    if (possible_id == NULL)
      return;

    ges_project_create_asset (project, possible_id,
        ges_asset_get_extractable_type (source));

    g_free (possible_id);
    g_error_free (error);
    return;
  }

  ges_project_add_asset (project, asset);
  if (asset)
    gst_object_unref (asset);
}

static gboolean
_add_proxy (GESProject * project, GESAsset * asset)
{
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (GES_IS_ASSET (asset), FALSE);

  if (g_hash_table_lookup (project->priv->proxies, ges_asset_get_id (asset)))
    return FALSE;

  g_hash_table_insert (project->priv->proxies,
      g_strdup (ges_asset_get_id (asset)), gst_object_ref (asset));

  GST_DEBUG_OBJECT (project, "Proxy asset added: %s", ges_asset_get_id (asset));

  return TRUE;
}

static gchar *
_get_outuri (GESProject * project, const gchar * uri)
{
  GESProjectPrivate *priv;
  gchar *outuri;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;

  outuri = g_strconcat (g_strdup (uri), ".proxy", NULL);
  if (priv->proxies_location) {
    outuri =
        (gchar *) g_strconcat (g_strdup (priv->proxies_location),
        g_path_get_basename (g_filename_from_uri (outuri, NULL, NULL)), NULL);
  }

  return g_strdup (outuri);
}

static void
new_proxy_asset_cb (GESAsset * source, GAsyncResult * res, GESProject * project)
{
  GESProjectPrivate *priv;
  GError *error = NULL;
  GESAsset *asset, *extractable_asset;
  GESClip *clip;
  GESLayer *layer;
  GESTimeline *timeline;
  gchar *outuri;
  const gchar *uri;
  GType extractable_type;
  GList *cur_proxy, *cur_clip, *cur_layer, *cur_timeline, *clips, *layers;

  g_return_if_fail (GES_IS_PROJECT (project));

  priv = project->priv;

  asset = ges_asset_request_finish (res, &error);
  if (error) {
    asset = g_hash_table_lookup (priv->assets, priv->proxy_uri);
    /* FIXME: we must check if pipeline NULL, then create proxy asset else add to list for creating */
    _transcode (project, asset);

    if (asset) {
      gst_object_unref (asset);
    }
  } else {
    /* FIXME: look at the GstDiscovererInfo, and check if it matches the GstEncodingProfile you had set */
    _add_proxy (project, asset);
    ges_asset_set_parent (asset, priv->proxy_parent);

    /* Go over all proxies timeline and set proxy asset for clip */
    if (priv->timeline_proxies) {
      cur_timeline = g_list_first (priv->timeline_proxies);
      for (; cur_timeline; cur_timeline = g_list_next (cur_timeline)) {
        timeline = GES_TIMELINE (cur_timeline->data);
        layers = ges_timeline_get_layers (timeline);
        cur_layer = g_list_first (layers);
        for (; cur_layer; cur_layer = g_list_next (cur_layer)) {
          layer = GES_LAYER (cur_layer->data);
          clips = ges_layer_get_clips (layer);
          cur_clip = g_list_first (clips);
          for (; cur_clip; cur_clip = g_list_next (cur_clip)) {
            clip = GES_CLIP (cur_clip->data);
            extractable_asset =
                ges_extractable_get_asset (GES_EXTRACTABLE (clip));
            if (g_strcmp0 (ges_asset_get_id (priv->proxy_parent),
                    ges_asset_get_id (extractable_asset)) == 0) {
              GST_DEBUG_OBJECT (clip, "Set proxy asset %s for clip",
                  ges_asset_get_id (asset));
              ges_extractable_set_asset (GES_EXTRACTABLE (clip), asset);
            }
          }
        }
        ges_timeline_commit (timeline);
      }
    }

    if (asset) {
      gst_object_unref (asset);
    }

    cur_proxy = g_list_previous (priv->create_proxies);
    if (cur_proxy) {
      asset = cur_proxy->data;
      uri = ges_asset_get_id (asset);
      outuri = _get_outuri (project, uri);
      extractable_type = ges_asset_get_extractable_type (asset);
      priv->proxy_parent = asset;
      priv->proxy_uri = (gchar *) uri;
      priv->create_proxies = cur_proxy;

      _create_proxy_asset (project, outuri, extractable_type);
    } else {
      priv->proxies_created = TRUE;
      g_signal_emit (project, _signals[PROXIES_CREATED_SIGNAL], 0, NULL);
    }
  }
}

static gboolean
_create_proxy_asset (GESProject * project, const gchar * id,
    GType extractable_type)
{
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (id != NULL, FALSE);
  g_return_val_if_fail (g_type_is_a (extractable_type, GES_TYPE_EXTRACTABLE),
      FALSE);

  if (g_hash_table_lookup (project->priv->proxies, id)) {
    return FALSE;
  }

  ges_asset_request_async (extractable_type, id, NULL,
      (GAsyncReadyCallback) new_proxy_asset_cb, project);

  return TRUE;
}

static void
pad_added_cb (GstElement * uridecodebin, GstPad * pad, GstElement * encodebin)
{
  GstPad *sinkpad;
  GstCaps *caps;

  /* Ask encodebin for a compatible pad */
  caps = gst_pad_query_caps (pad, NULL);
  g_signal_emit_by_name (encodebin, "request-pad", caps, &sinkpad);
  if (sinkpad == NULL) {
    GST_ERROR ("Couldn't get an encoding channel for pad %s:%s\n",
        GST_DEBUG_PAD_NAME (pad));
    return;
  }

  if (G_UNLIKELY (gst_pad_link (pad, sinkpad) != GST_PAD_LINK_OK)) {
    GST_ERROR ("Couldn't link pads srccaps: %" GST_PTR_FORMAT "sinkcaps: %"
        GST_PTR_FORMAT, gst_pad_query_caps (sinkpad, NULL), caps);
  }

  return;
}

static void
bus_message_cb (GstBus * bus, GstMessage * message, GESProject * project)
{
  GESProjectPrivate *priv;

  g_return_if_fail (GES_IS_PROJECT (project));

  priv = project->priv;

  switch (GST_MESSAGE_TYPE (message)) {
    case GST_MESSAGE_ERROR:

      gst_bus_set_flushing (bus, TRUE);
      gst_element_set_state (priv->proxy_pipeline, GST_STATE_NULL);
      gst_object_unref (priv->proxy_pipeline);
      break;
    case GST_MESSAGE_EOS:{
      GType extractable_type;

      gst_element_set_state (priv->proxy_pipeline, GST_STATE_NULL);
      gst_object_unref (priv->proxy_pipeline);

      if (g_str_has_suffix ((const gchar *) priv->proxy_uri, ".part")) {
        const gchar *oldfilename, *newfilename;

        oldfilename =
            (const gchar *) g_filename_from_uri (priv->proxy_uri, NULL, NULL);
        newfilename = g_strsplit (oldfilename, ".part", 2)[0];

        if (oldfilename && newfilename) {
          g_rename (oldfilename, newfilename);
        }

        g_free (priv->proxy_uri);

        priv->proxy_uri = gst_filename_to_uri (newfilename, NULL);
      }

      extractable_type = ges_asset_get_extractable_type (priv->proxy_asset);
      ges_asset_needs_reload (extractable_type, priv->proxy_uri);
      _create_proxy_asset (project, priv->proxy_uri, extractable_type);

      break;
    }
    default:
      break;
  }
}

#if 0
static gboolean
_proxy_exists (GESProject * project, GESAsset * asset)
{
  gchar *outuri;
  const gchar *uri;
  gboolean proxy_exists = FALSE;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  uri = ges_asset_get_id (GES_ASSET (asset));
  outuri = _get_outuri (project, uri);

  proxy_exists =
      g_file_test (g_filename_from_uri (outuri, NULL, NULL),
      G_FILE_TEST_EXISTS);

  return proxy_exists;
}
#endif

static gboolean
_transcode (GESProject * project, GESAsset * asset)
{
  GstElement *pipeline, *src, *ebin, *sink;
  GstEncodingProfile *profile;
  GstBus *bus;
  GESProjectPrivate *priv;
  gchar *outuri;
  const gchar *uri;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;
  profile = priv->proxy_profile;

  uri = ges_asset_get_id (GES_ASSET (asset));
  outuri = _get_outuri (project, uri);
  outuri = g_strconcat (g_strdup (outuri), ".part", NULL);
  priv->proxy_uri = outuri;
  priv->proxy_asset = asset;

  pipeline = gst_pipeline_new ("encoding-pipeline");
  priv->proxy_pipeline = pipeline;
  src = gst_element_factory_make ("uridecodebin", NULL);

  ebin = gst_element_factory_make ("encodebin", NULL);
  sink = gst_element_make_from_uri (GST_URI_SINK, outuri, "sink", NULL);

  g_object_set (src, "uri", uri, NULL);
  g_object_set (ebin, "profile", profile, NULL);

  g_signal_connect (src, "pad-added", G_CALLBACK (pad_added_cb), ebin);

  gst_bin_add_many (GST_BIN (pipeline), src, ebin, sink, NULL);
  gst_element_link (ebin, sink);

  bus = gst_pipeline_get_bus ((GstPipeline *) pipeline);
  gst_bus_add_signal_watch (bus);
  g_signal_connect (bus, "message", G_CALLBACK (bus_message_cb), project);

  if (gst_element_set_state (pipeline,
          GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    GST_ERROR ("Could not set pipeline state to PLAYING");
    return FALSE;
  }

  return TRUE;
}

static GList *
_get_create_proxies_list (GESProject * project)
{
  GList *ret = NULL;
  GHashTableIter iter;
  gpointer key, value;

  g_return_val_if_fail (GES_IS_PROJECT (project), NULL);

  g_hash_table_iter_init (&iter, project->priv->assets);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    if (GES_IS_URI_CLIP_ASSET (GES_ASSET (value))) {
      ret = g_list_append (ret, gst_object_ref (value));
    }
  }

  return ret;
}

static gboolean
_create_proxies (GESProject * project)
{
  GstEncodingProfile *profile;
  GESProjectPrivate *priv;
  GESAsset *asset;
  gchar *outuri;
  const gchar *uri;
  GType extractable_type;
  GList *cur_proxy;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;
  profile = ges_project_get_proxy_profile (project, NULL);

  if (GST_IS_ENCODING_PROFILE (profile)) {

    if (priv->proxies_creation_started == FALSE) {
      priv->proxies_creation_started = TRUE;
      g_signal_emit (project, _signals[PROXIES_CREATION_STARTED_SIGNAL], 0,
          NULL);
    }

    priv->create_proxies = _get_create_proxies_list (project);

    cur_proxy = g_list_last (priv->create_proxies);
    if (cur_proxy) {
      asset = cur_proxy->data;
      uri = ges_asset_get_id (asset);
      outuri = _get_outuri (project, uri);
      extractable_type = ges_asset_get_extractable_type (asset);
      priv->proxy_parent = asset;
      priv->proxy_uri = (gchar *) uri;
      priv->create_proxies = cur_proxy;

      _create_proxy_asset (project, outuri, extractable_type);
    } else {
      priv->proxies_created = TRUE;
      g_signal_emit (project, _signals[PROXIES_CREATED_SIGNAL], 0, NULL);
    }

    gst_object_unref (profile);
  }

  return TRUE;
}

/**
 * ges_project_set_loaded:
 * @project: The #GESProject from which to emit the "project-loaded" signal
 *
 * Emits the "loaded" signal. This method should be called by sublasses when
 * the project is fully loaded.
 *
 * Returns: %TRUE if the signale could be emitted %FALSE otherwize
 */
gboolean
ges_project_set_loaded (GESProject * project, GESFormatter * formatter)
{
  GST_INFO_OBJECT (project, "Emit project loaded");
  ges_timeline_commit (formatter->timeline);
  g_signal_emit (project, _signals[LOADED_SIGNAL], 0, formatter->timeline);

  if (project->priv->proxies_created == FALSE) {
    _create_proxies (project);
  }

  /* We are now done with that formatter */
  ges_project_remove_formatter (project, formatter);

  return TRUE;
}

void
ges_project_add_loading_asset (GESProject * project, GType extractable_type,
    const gchar * id)
{
  GESAsset *asset;

  if ((asset = ges_asset_cache_lookup (extractable_type, id)))
    g_hash_table_insert (project->priv->loading_assets, g_strdup (id),
        gst_object_ref (asset));
}

/**************************************
 *                                    *
 *         API Implementation         *
 *                                    *
 **************************************/

/**
 * ges_project_create_asset:
 * @project: A #GESProject
 * @id: (allow-none): The id of the asset to create and add to @project
 * @extractable_type: The #GType of the asset to create
 *
 * Create and add a #GESAsset to @project. You should connect to the
 * "asset-added" signal to get the asset when it finally gets added to
 * @project
 *
 * Returns: %TRUE if the asset `ed to be added %FALSE it was already
 * in the project
 */
gboolean
ges_project_create_asset (GESProject * project, const gchar * id,
    GType extractable_type)
{
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (g_type_is_a (extractable_type, GES_TYPE_EXTRACTABLE),
      FALSE);

  if (id == NULL)
    id = g_type_name (extractable_type);

  if (g_hash_table_lookup (project->priv->assets, id) ||
      g_hash_table_lookup (project->priv->loading_assets, id) ||
      g_hash_table_lookup (project->priv->loaded_with_error, id))
    return FALSE;

  /* TODO Add a GCancellable somewhere in our API */
  ges_asset_request_async (extractable_type, id, NULL,
      (GAsyncReadyCallback) new_asset_cb, project);
  ges_project_add_loading_asset (project, extractable_type, id);

  return TRUE;
}

/**
 * ges_project_add_asset:
 * @project: A #GESProject
 * @asset: (transfer none): A #GESAsset to add to @project
 *
 * Adds a #Asset to @project, the project will keep a reference on
 * @asset.
 *
 * Returns: %TRUE if the asset could be added %FALSE it was already
 * in the project
 */
gboolean
ges_project_add_asset (GESProject * project, GESAsset * asset)
{
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  if (g_hash_table_lookup (project->priv->assets, ges_asset_get_id (asset)))
    return FALSE;

  g_hash_table_insert (project->priv->assets,
      g_strdup (ges_asset_get_id (asset)), gst_object_ref (asset));

  g_hash_table_remove (project->priv->loading_assets, ges_asset_get_id (asset));
  GST_DEBUG_OBJECT (project, "Asset added: %s", ges_asset_get_id (asset));
  g_signal_emit (project, _signals[ASSET_ADDED_SIGNAL], 0, asset);

  return TRUE;
}

/**
 * ges_project_remove_asset:
 * @project: A #GESProject
 * @asset: (transfer none): A #GESAsset to remove from @project
 *
 * remove a @asset to from @project.
 *
 * Returns: %TRUE if the asset could be removed %FALSE otherwise
 */
gboolean
ges_project_remove_asset (GESProject * project, GESAsset * asset)
{
  gboolean ret;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  ret = g_hash_table_remove (project->priv->assets, ges_asset_get_id (asset));
  g_signal_emit (project, _signals[ASSET_REMOVED_SIGNAL], 0, asset);

  return ret;
}

/**
 * ges_project_get_asset:
 * @project: A #GESProject
 * @id: The id of the asset to retrieve
 * @extractable_type: The extractable_type of the asset
 * to retrieve from @object
 *
 * Returns: (transfer full) (allow-none): The #GESAsset with
 * @id or %NULL if no asset with @id as an ID
 */
GESAsset *
ges_project_get_asset (GESProject * project, const gchar * id,
    GType extractable_type)
{
  GESAsset *asset;

  g_return_val_if_fail (GES_IS_PROJECT (project), NULL);
  g_return_val_if_fail (g_type_is_a (extractable_type, GES_TYPE_EXTRACTABLE),
      NULL);

  asset = g_hash_table_lookup (project->priv->assets, id);

  if (asset)
    return gst_object_ref (asset);

  return NULL;
}

/**
 * ges_project_list_assets:
 * @project: A #GESProject
 * @filter: Type of assets to list, #GES_TYPE_EXTRACTABLE will list
 * all assets
 *
 * List all @asset contained in @project filtering per extractable_type
 * as defined by @filter. It copies the asset and thus will not be updated
 * in time.
 *
 * Returns: (transfer full) (element-type GESAsset): The list of
 * #GESAsset the object contains
 */
GList *
ges_project_list_assets (GESProject * project, GType filter)
{
  GList *ret = NULL;
  GHashTableIter iter;
  gpointer key, value;

  g_return_val_if_fail (GES_IS_PROJECT (project), NULL);

  g_hash_table_iter_init (&iter, project->priv->assets);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    if (g_type_is_a (ges_asset_get_extractable_type (GES_ASSET (value)),
            filter))
      ret = g_list_append (ret, gst_object_ref (value));
  }

  return ret;
}

/**
 * ges_project_list_proxies:
 * @project: A #GESProject
 * @filter: Type of proxies assets to list, #GES_TYPE_EXTRACTABLE will list
 * all proxies assets
 *
 * List all proxies @asset contained in @project filtering per extractable_type
 * as defined by @filter. It copies the asset and thus will not be updated
 * in time.
 *
 * Returns: (transfer full) (element-type GESAsset): The list of
 * #GESAsset the object contains
 */
GList *
ges_project_list_proxies (GESProject * project, GType filter)
{
  GList *ret = NULL;
  GHashTableIter iter;
  gpointer key, value;

  g_return_val_if_fail (GES_IS_PROJECT (project), NULL);

  g_hash_table_iter_init (&iter, project->priv->proxies);
  while (g_hash_table_iter_next (&iter, &key, &value)) {
    if (g_type_is_a (ges_asset_get_extractable_type (GES_ASSET (value)),
            filter))
      ret = g_list_append (ret, gst_object_ref (value));
  }

  return ret;
}

/**
 * ges_project_save:
 * @project: A #GESProject to save
 * @timeline: The #GESTimeline to save, it must have been extracted from @project
 * @uri: The uri where to save @project and @timeline
 * @formatter_asset: (allow-none): The formatter asset to use or %NULL. If %NULL,
 * will try to save in the same format as the one from which the timeline as been loaded
 * or default to the formatter with highest rank
 * @overwrite: %TRUE to overwrite file if it exists
 * @error: (out) (allow-none): An error to be set in case something wrong happens or %NULL
 *
 * Save the timeline of @project to @uri. You should make sure that @timeline
 * is one of the timelines that have been extracted from @project
 * (using ges_asset_extract (@project);)
 *
 * Returns: %TRUE if the project could be save, %FALSE otherwize
 */
gboolean
ges_project_save (GESProject * project, GESTimeline * timeline,
    const gchar * uri, GESAsset * formatter_asset, gboolean overwrite,
    GError ** error)
{
  GESAsset *tl_asset;
  gboolean ret = TRUE;
  GESFormatter *formatter = NULL;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (formatter_asset == NULL ||
      g_type_is_a (ges_asset_get_extractable_type (formatter_asset),
          GES_TYPE_FORMATTER), FALSE);
  g_return_val_if_fail ((error == NULL || *error == NULL), FALSE);

  tl_asset = ges_extractable_get_asset (GES_EXTRACTABLE (timeline));
  if (tl_asset == NULL && project->priv->uri == NULL) {
    GESAsset *asset = ges_asset_cache_lookup (GES_TYPE_PROJECT, uri);

    if (asset) {
      GST_WARNING_OBJECT (project, "Trying to save project to %s but we already"
          "have %" GST_PTR_FORMAT " for that uri, can not save", uri, asset);
      goto out;
    }

    GST_DEBUG_OBJECT (project, "Timeline %" GST_PTR_FORMAT " has no asset"
        " we have no uri set, so setting ourself as asset", timeline);

    ges_extractable_set_asset (GES_EXTRACTABLE (timeline), GES_ASSET (project));
  } else if (tl_asset != GES_ASSET (project)) {
    GST_WARNING_OBJECT (project, "Timeline %" GST_PTR_FORMAT
        " not created by this project can not save", timeline);

    ret = FALSE;
    goto out;
  }

  if (formatter_asset == NULL)
    formatter_asset = gst_object_ref (ges_formatter_get_default ());

  formatter = GES_FORMATTER (ges_asset_extract (formatter_asset, error));
  if (formatter == NULL) {
    GST_WARNING_OBJECT (project, "Could not create the formatter %p %s: %s",
        formatter_asset, ges_asset_get_id (formatter_asset),
        (error && *error) ? (*error)->message : "Unknown Error");

    ret = FALSE;
    goto out;
  }

  ges_project_add_formatter (project, formatter);
  ret = ges_formatter_save_to_uri (formatter, timeline, uri, overwrite, error);
  if (ret && project->priv->uri == NULL)
    ges_project_set_uri (project, uri);

out:
  if (formatter_asset)
    gst_object_unref (formatter_asset);
  ges_project_remove_formatter (project, formatter);

  return ret;
}

/**
 * ges_project_new:
 * @uri: (allow-none): The uri to be set after creating the project.
 *
 * Creates a new #GESProject and sets its uri to @uri if provided. Note that
 * if @uri is not valid or %NULL, the uri of the project will then be set
 * the first time you save the project. If you then save the project to
 * other locations, it will never be updated again and the first valid URI is
 * the URI it will keep refering to.
 *
 * Returns: A newly created #GESProject
 */
GESProject *
ges_project_new (const gchar * uri)
{
  gchar *id = (gchar *) uri;
  GESProject *project;

  if (uri == NULL)
    id = g_strdup_printf ("project-%i", nb_projects++);

  project = GES_PROJECT (ges_asset_request (GES_TYPE_TIMELINE, id, NULL));

  if (project && uri)
    ges_project_set_uri (project, uri);

  return project;
}

/**
 * ges_project_load:
 * @project: A #GESProject that has an @uri set already
 * @timeline: A blank timeline to load @project into
 * @error: (out) (allow-none): An error to be set in case something wrong happens or %NULL
 *
 * Loads @project into @timeline
 *
 * Returns: %TRUE if the project could be loaded %FALSE otherwize.
 */
gboolean
ges_project_load (GESProject * project, GESTimeline * timeline, GError ** error)
{
  g_return_val_if_fail (GES_IS_TIMELINE (timeline), FALSE);
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (ges_project_get_uri (project), FALSE);
  g_return_val_if_fail (
      (ges_extractable_get_asset (GES_EXTRACTABLE (timeline)) == NULL), FALSE);

  if (!_load_project (project, timeline, error))
    return FALSE;

  ges_extractable_set_asset (GES_EXTRACTABLE (timeline), GES_ASSET (project));

  return TRUE;
}

/**
 * ges_project_get_uri:
 * @project: A #GESProject
 *
 * Retrieve the uri that is currently set on @project
 *
 * Returns: The uri that is set on @project
 */
gchar *
ges_project_get_uri (GESProject * project)
{
  GESProjectPrivate *priv;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;
  if (priv->uri)
    return g_strdup (priv->uri);
  return NULL;
}

/**
 * ges_project_add_encoding_profile:
 * @project: A #GESProject
 * @profile: A #GstEncodingProfile to add to the project. If a profile with
 * the same name already exists, it will be replaced
 *
 * Adds @profile to the project. It lets you save in what format
 * the project has been renders and keep a reference to those formats.
 * Also, those formats will be saves to the project file when possible.
 *
 * Returns: %TRUE if @profile could be added, %FALSE otherwize
 */
gboolean
ges_project_add_encoding_profile (GESProject * project,
    GstEncodingProfile * profile)
{
  GList *tmp;
  GESProjectPrivate *priv;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (GST_IS_ENCODING_PROFILE (profile), FALSE);

  priv = project->priv;
  for (tmp = priv->encoding_profiles; tmp; tmp = tmp->next) {
    GstEncodingProfile *tmpprofile = GST_ENCODING_PROFILE (tmp->data);

    if (g_strcmp0 (gst_encoding_profile_get_name (tmpprofile),
            gst_encoding_profile_get_name (profile)) == 0) {
      GST_INFO_OBJECT (project, "Already have profile: %s, replacing it",
          gst_encoding_profile_get_name (profile));

      gst_object_unref (tmp->data);
      tmp->data = gst_object_ref (profile);
      return TRUE;
    }
  }

  priv->encoding_profiles = g_list_prepend (priv->encoding_profiles,
      gst_object_ref (profile));

  return TRUE;
}

/**
 * ges_project_list_encoding_profiles:
 * @project: A #GESProject
 *
 * Lists the encoding profile that have been set to @project. The first one
 * is the latest added.
 *
 * Returns: (transfer none) (element-type GstPbutils.EncodingProfile) (allow-none): The
 * list of #GstEncodingProfile used in @project
 */
const GList *
ges_project_list_encoding_profiles (GESProject * project)
{
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  return project->priv->encoding_profiles;
}

/**
 * ges_project_get_loading_assets:
 * @project: A #GESProject
 *
 * Get the assets that are being loaded
 *
 * Returns: (transfer full) (element-type GES.Asset): A set of loading asset
 * that will be added to @project. Note that those Asset are *not* loaded yet,
 * and thus can not be used
 */
GList *
ges_project_get_loading_assets (GESProject * project)
{
  GHashTableIter iter;
  gpointer key, value;

  GList *ret = NULL;

  g_return_val_if_fail (GES_IS_PROJECT (project), NULL);

  g_hash_table_iter_init (&iter, project->priv->loading_assets);
  while (g_hash_table_iter_next (&iter, &key, &value))
    ret = g_list_prepend (ret, gst_object_ref (value));

  return ret;
}

/**
 * ges_project_set_proxy_profile:
 * @project: (transfer none) The #GESProject to set.
 * @profile: The #GstEncodingProfile for proxy editing in @project.
 * @asset: (allow-none) The #GESUriClipAsset to set.
 * Method to set proxy editing profile for assets in project. If we set an encoding @profile on a @project and don't set on a @asset, then it means it's automatic proxy editing mode. If we set and encoding @profile on a @project and set on a @asset, then it means it's manual proxy editing mode.
 * Returns: %TRUE if the @profile was setted, else %FALSE.
 */
gboolean
ges_project_set_proxy_profile (GESProject * project,
    GstEncodingProfile * profile, GESUriClipAsset * asset)
{
  GESProjectPrivate *priv;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (GST_IS_ENCODING_PROFILE (profile), FALSE);

  priv = project->priv;

  if (asset == NULL) {
    GstEncodingProfile *tmpprofile = GST_ENCODING_PROFILE (priv->proxy_profile);

    if (GST_IS_ENCODING_PROFILE (tmpprofile)) {
      if (gst_encoding_profile_is_equal (profile, tmpprofile)) {
        GST_INFO_OBJECT (project,
            "Already have proxy profile: %s, replacing it",
            gst_encoding_profile_get_name (profile));
        gst_object_unref (priv->proxy_profile);
      }
    }

    priv->proxy_profile = gst_object_ref (profile);
  } else {
    GESAsset *tmpasset;

    g_return_val_if_fail (GES_IS_URI_CLIP_ASSET (asset), FALSE);

    tmpasset = GES_ASSET (asset);

    if (g_hash_table_lookup (priv->proxied_assets, ges_asset_get_id (tmpasset))) {
      GST_INFO_OBJECT (project,
          "Already have proxy profile %s for asset: %s, replacing it",
          gst_encoding_profile_get_name (profile), ges_asset_get_id (tmpasset));
    }
    g_hash_table_insert (priv->proxied_assets,
        g_strdup (ges_asset_get_id (tmpasset)), gst_object_ref (profile));
  }

  return TRUE;
}

/**
 * ges_project_get_proxy_profile:
 * @project: (transfer none) The #GESProject to get.
 * @asset: (allow-none) The #GESUriClipAsset to get.
 * Method to get proxy editing profile from @asset, used in @project. If we don't set an @asset, then it means it's get proxy editing profile from @project. 
 * Returns: (transfer none) The #GstEncodingProfile used for proxy edition in @project or in @asset or %NULL if not used.
 */
GstEncodingProfile *
ges_project_get_proxy_profile (GESProject * project, GESUriClipAsset * asset)
{
  GESProjectPrivate *priv;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;

  if (asset) {
    g_return_val_if_fail (GES_IS_URI_CLIP_ASSET (asset), FALSE);

    return g_hash_table_lookup (priv->proxied_assets,
        ges_asset_get_id (GES_ASSET (asset)));
  }

  return priv->proxy_profile;
}

static gboolean
project_start_proxies_cancalled_cb (GCancellable * cancellable,
    GESProject * project)
{
  GESProjectPrivate *priv;

  priv = project->priv;

  if (!GST_IS_ELEMENT (priv->proxy_pipeline)) {
    GST_DEBUG_OBJECT (project, "Project haven't pipeline");
    return FALSE;
  }

  gst_element_set_state (priv->proxy_pipeline, GST_STATE_NULL);
  gst_object_unref (priv->proxy_pipeline);

  g_signal_emit (project, _signals[PROXIES_CREATION_CANCELLED_SIGNAL], 0, NULL);

  return TRUE;
}

/**
 * ges_project_start_proxy_creation:
 * @project: (transfer none) The #GESProject.
 * @asset: (allow-none) The #GESUriClipAsset.
 * @cancellable: (allow-none) optional #GCancellable object, NULL to ignore. 
 * Method to start create proxies for proxy editing. If asset is NULL, it means start creation of all proxies.
 * Returns: %TRUE if the creation was started, else %FALSE.
 */
gboolean
ges_project_start_proxy_creation (GESProject * project, GESUriClipAsset * asset,
    GCancellable * cancellable)
{
  GESProjectPrivate *priv;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;

  if (cancellable) {
    g_return_val_if_fail (G_IS_CANCELLABLE (cancellable), FALSE);
    g_cancellable_connect (cancellable,
        (GCallback) project_start_proxies_cancalled_cb, project, NULL);
  }

  if (GST_IS_ELEMENT (priv->proxy_pipeline)) {
    if (gst_element_set_state (priv->proxy_pipeline,
            GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
      return FALSE;
    }
    return TRUE;
  }

  if (asset) {
    GstEncodingProfile *profile;

    g_return_val_if_fail (GES_IS_URI_CLIP_ASSET (asset), FALSE);

    profile = ges_project_get_proxy_profile (project, asset);
    if (profile == NULL) {
      GST_DEBUG_OBJECT (project, "Project haven't asset: %s",
          ges_asset_get_id (GES_ASSET (asset)));
      return FALSE;
    }

    if (GES_IS_URI_CLIP_ASSET (asset)) {
      _transcode (project, GES_ASSET (asset));
    }

    gst_object_unref (profile);
    if (cancellable) {
      g_cancellable_disconnect (cancellable, 0);
    }

    return TRUE;
  }

  if (ges_project_get_loading_assets (project) == NULL) {
    if (priv->proxies_creation_started == FALSE) {
      _create_proxies (project);
    } else {
      GST_DEBUG_OBJECT (project, "Proxy creation already started");
    }
  } else {
    GST_DEBUG_OBJECT (project,
        "Can't start proxy creation. Project loading assets");
  }

  if (cancellable) {
    g_cancellable_disconnect (cancellable, 0);
  }

  return TRUE;
}

/**
 * ges_project_start_proxy_creation_async:
 * @project: (transfer none) The #GESProject.
 * @asset: (allow-none) The #GESUriClipAsset.
 * @cancellable: (allow-none) optional #GCancellable object, NULL to ignore.
 * @callback: a #GAsyncReadyCallback to call when the initialization is finished.
 * @user_data: The user data to pass when callback is called.
 * Method to start create proxies for proxy editing asyncronously, callback will be called when the assets is ready to be used or if an error occured. If asset is NULL, it means start creation of all proxies.
 */
void
ges_project_start_proxy_creation_async (GESProject * project,
    GESUriClipAsset * asset, GCancellable * cancellable,
    GAsyncReadyCallback callback, gpointer user_data)
{
  GSimpleAsyncResult *simple;
  GObject *object;

  object = G_OBJECT (project);
  if (asset) {
    g_object_unref (object);
    object = G_OBJECT (asset);
  }

  /* FIXME: add support GCancellable for async */
  simple =
      g_simple_async_result_new (object, callback, user_data,
      ges_project_start_proxy_creation);
  g_simple_async_result_complete_in_idle (simple);

  g_object_unref (simple);
}

/**
 * ges_project_pause_proxy_creation:
 * @project: (transfer none) The #GESProject.
 * Method to pause create proxies for proxy editing.
 * Returns: %TRUE if the creation was paused, else %FALSE.
 */
gboolean
ges_project_pause_proxy_creation (GESProject * project)
{
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  if (gst_element_set_state (project->priv->proxy_pipeline,
          GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    return FALSE;
  }

  g_signal_emit (project, _signals[PROXIES_CREATION_PAUSED_SIGNAL], 0, NULL);

  return TRUE;
}

/**
 * ges_project_pause_proxy_creation_async:
 * @project: (transfer none) The #GESProject.
 * @callback: a #GAsyncReadyCallback to call when the initialization is finished.
 * @user_data: The user data to pass when callback is called.
 * Method to pause create proxies for proxy editing asyncronously, callback will be called when the assets is ready to be used or if an error occured.
 */
void
ges_project_pause_proxy_creation_async (GESProject * project,
    GAsyncReadyCallback callback, gpointer user_data)
{
  GSimpleAsyncResult *simple;

  simple =
      g_simple_async_result_new (G_OBJECT (project), callback, user_data,
      ges_project_pause_proxy_creation);
  g_simple_async_result_complete_in_idle (simple);
}

/**
 * ges_project_get_proxy_state:
 * @project: (transfer none) The #GESProject to get.
 * Method to get #GstState for proxy editing.
 * Returns: (transfer full) The #GstState used for proxy edition in project.
 */
GstState
ges_project_get_proxy_state (GESProject * project)
{
  GstState state;
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  gst_element_get_state (project->priv->proxy_pipeline, &state, NULL,
      GST_CLOCK_TIME_NONE);

  return state;
}

/**
 * ges_project_set_proxies_location:
 * @project: (transfer none) The #GESProject to set.
 * @location: New location.
 * Method to set user specific location of created proxies for proxy editing.
 * Returns: %TRUE if the location was setted, else %FALSE.
 */
gboolean
ges_project_set_proxies_location (GESProject * project, const gchar * location)
{
  GESProjectPrivate *priv;
  const gchar *uri;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;

  if (location == NULL) {
    GST_LOG_OBJECT (project, "Invalid location: %s", location);
    return FALSE;
  }

  if (gst_uri_is_valid (location)) {
    uri = location;
  } else {
    uri = (const gchar *) gst_filename_to_uri (location, NULL);
  }

  if (priv->proxies_location) {
    GST_INFO_OBJECT (project, "Already have proxies location: %s, replacing it",
        priv->proxies_location);
    g_free (priv->proxies_location);
  }

  priv->proxies_location = g_strdup (uri);

  return TRUE;
}

/**
 * ges_project_get_proxies_location:
 * @project: (transfer none) The #GESProject to get.
 * Method to get user specific location of created proxies for proxy editing.
 * Returns: (transfer none) The location used for proxy edition in project.
 */
const gchar *
ges_project_get_proxies_location (GESProject * project)
{
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  return (const gchar *) project->priv->proxies_location;
}

/**
 * ges_project_get_proxies_location:
 * @project: (transfer none) The #GESProject to set.
 * @timeline: (transfer none) A @project timeline.
 * Method to set proxy editing for timeline. If @use_proxies %TRUE, then set proxies for @timeline, else set @use_proxies to %FALSE.
 */
gboolean
ges_project_use_proxies_for_timeline (GESProject * project,
    GESTimeline * timeline, gboolean use_proxies)
{
  GESProjectPrivate *priv;

  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);
  g_return_val_if_fail (GES_IS_PROJECT (project), FALSE);

  priv = project->priv;

  if (use_proxies == TRUE) {
    if (g_list_find (priv->timeline_proxies, timeline) == NULL) {
      priv->timeline_proxies = g_list_append (priv->timeline_proxies, timeline);
    } else {
      /* Already in list */
      return FALSE;
    }
  } else {
    if (g_list_find (priv->timeline_proxies, timeline)) {
      priv->timeline_proxies = g_list_remove (priv->timeline_proxies, timeline);
    } else {
      /* Not founded at list */
      return FALSE;
    }
  }

  return TRUE;
}

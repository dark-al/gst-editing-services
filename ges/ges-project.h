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
#ifndef _GES_PROJECT_
#define _GES_PROJECT_

#include <glib-object.h>
#include <gio/gio.h>
#include <ges/ges-types.h>
#include <ges/ges-asset.h>
#include <gst/pbutils/encoding-profile.h>

G_BEGIN_DECLS

#define GES_TYPE_PROJECT            ges_project_get_type()
#define GES_PROJECT(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), GES_TYPE_PROJECT, GESProject))
#define GES_PROJECT_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), GES_TYPE_PROJECT, GESProjectClass))
#define GES_IS_PROJECT(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GES_TYPE_PROJECT))
#define GES_IS_PROJECT_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), GES_TYPE_PROJECT))
#define GES_PROJECT_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj), GES_TYPE_PROJECT, GESProjectClass))

typedef struct _GESProjectPrivate GESProjectPrivate;

GType ges_project_get_type (void);

struct _GESProject
{
  GESAsset parent;

  /* <private> */
  GESProjectPrivate *priv;

  /* Padding for API extension */
  gpointer __ges_reserved[GES_PADDING_LARGE];
};

struct _GESProjectClass
{
  GESAssetClass parent_class;

  /* Signals */
  void     (*asset_added)    (GESProject * self,
                              GESAsset   * asset);
  void     (*asset_removed)  (GESProject * self,
                              GESAsset   * asset);
  gboolean (*missing_uri)    (GESProject * self,
                              GError     * error,
                              GESAsset   * wrong_asset);
  gboolean (*loading_error)  (GESProject * self,
                              GError     * error,
                              gchar      * id,
                              GType extractable_type);
  gboolean (*loaded)         (GESProject  * self,
                              GESTimeline * timeline);
  void     (*proxies_created) (GESProject * self);
  void     (*proxies_creation_started) (GESProject * self);
  void     (*proxies_creation_paused) (GESProject * self);
  void     (*proxies_creation_cancelled) (GESProject * self);

  gpointer _ges_reserved[GES_PADDING];
};

gboolean  ges_project_add_asset    (GESProject* project,
                                    GESAsset *asset);
gboolean  ges_project_remove_asset (GESProject *project,
                                    GESAsset * asset);
GList   * ges_project_list_assets  (GESProject * project,
                                    GType filter);
GList   * ges_project_list_proxies  (GESProject * project,
                                    GType filter);
gboolean  ges_project_save         (GESProject * project,
                                    GESTimeline * timeline,
                                    const gchar *uri,
                                    GESAsset * formatter_asset,
                                    gboolean overwrite,
                                    GError **error);
gboolean  ges_project_load         (GESProject * project,
                                    GESTimeline * timeline,
                                    GError **error);
GESProject * ges_project_new       (const gchar *uri);
gchar      * ges_project_get_uri   (GESProject *project);
GESAsset   * ges_project_get_asset (GESProject * project,
                                    const gchar *id,
                                    GType extractable_type);
gboolean ges_project_create_asset  (GESProject * project,
                                    const gchar *id,
                                    GType extractable_type);

GList * ges_project_get_loading_assets          (GESProject * project);

gboolean ges_project_add_encoding_profile       (GESProject *project,
                                                 GstEncodingProfile *profile);
const GList *ges_project_list_encoding_profiles (GESProject *project);

gboolean ges_project_set_proxy_profile (GESProject * project, GstEncodingProfile * profile, GESUriClipAsset * asset);
GstEncodingProfile * ges_project_get_proxy_profile (GESProject * project, GESUriClipAsset * asset);
gboolean ges_project_start_proxy_creation (GESProject * project, GESUriClipAsset * asset, GCancellable * cancellable);
void ges_project_start_proxy_creation_async (GESProject * project, GESUriClipAsset * asset, GCancellable * cancellable, GAsyncReadyCallback callback, gpointer user_data);
gboolean ges_project_pause_proxy_creation (GESProject * project);
void ges_project_pause_proxy_creation_async (GESProject * project, GAsyncReadyCallback callback, gpointer user_data);
GstState ges_project_get_proxy_state (GESProject * project);
gboolean ges_project_set_proxies_location (GESProject * project, const gchar * uri);
const gchar * ges_project_get_proxies_location (GESProject * project);
gboolean ges_project_use_proxies_for_timeline (GESProject *project, GESTimeline *timeline, gboolean use_proxies);

G_END_DECLS

#endif  /* _GES_PROJECT */

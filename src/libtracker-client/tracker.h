/* Tracker - indexer and metadata database engine
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

#ifndef TRACKER_H
#define TRACKER_H

#include <dbus/dbus-glib-bindings.h>

G_BEGIN_DECLS

typedef struct {
	DBusGProxy	*proxy_statistics;
	DBusGProxy	*proxy_resources;

        GHashTable      *pending_calls;
        guint            last_call;
} TrackerClient;

typedef void (*TrackerReplyArray) (gchar    **result,
                                   GError    *error,
                                   gpointer   user_data);
typedef void (*TrackerReplyGPtrArray) (GPtrArray *result, 
                                       GError    *error, 
                                       gpointer   user_data);
typedef void (*TrackerReplyVoid)      (GError    *error, 
                                       gpointer   user_data);

gboolean       tracker_cancel_call                         (TrackerClient          *client,
                                                            guint                   call_id);
void           tracker_cancel_last_call                    (TrackerClient          *client);

gchar *        tracker_sparql_escape                       (const gchar            *str);

TrackerClient *tracker_connect                             (gboolean                enable_warnings,
                                                            gint                    timeout);
void           tracker_disconnect                          (TrackerClient          *client);

/* Synchronous API */
GPtrArray *    tracker_statistics_get                      (TrackerClient          *client,
                                                            GError                **error);
void           tracker_resources_load                      (TrackerClient          *client,
                                                            const gchar            *uri,
                                                            GError                **error);
GPtrArray *    tracker_resources_sparql_query              (TrackerClient          *client,
                                                            const gchar            *query,
                                                            GError                **error);
void           tracker_resources_sparql_update             (TrackerClient          *client,
                                                            const gchar            *query,
                                                            GError                **error);
void           tracker_resources_batch_sparql_update       (TrackerClient          *client,
                                                            const gchar            *query,
                                                            GError                **error);
void           tracker_resources_batch_commit              (TrackerClient          *client,
                                                            GError                **error);
/* Asynchronous API */
guint          tracker_statistics_get_async                (TrackerClient          *client,
                                                            TrackerReplyGPtrArray   callback,
                                                            gpointer                user_data);
guint          tracker_resources_load_async                (TrackerClient          *client,
                                                            const gchar            *uri,
                                                            TrackerReplyVoid        callback,
                                                            gpointer                user_data);
guint          tracker_resources_sparql_query_async        (TrackerClient          *client,
                                                            const gchar            *query,
                                                            TrackerReplyGPtrArray   callback,
                                                            gpointer                user_data);
guint          tracker_resources_sparql_update_async       (TrackerClient          *client,
                                                            const gchar            *query,
                                                            TrackerReplyVoid        callback,
                                                            gpointer                user_data);
guint          tracker_resources_batch_sparql_update_async (TrackerClient          *client,
                                                            const gchar            *query,
                                                            TrackerReplyVoid        callback,
                                                            gpointer                user_data);
guint          tracker_resources_batch_commit_async        (TrackerClient          *client,
                                                            TrackerReplyVoid        callback,
                                                            gpointer                user_data);
guint          tracker_search_metadata_by_text_async       (TrackerClient          *client,
                                                            const gchar            *query,
                                                            TrackerReplyArray       callback,
                                                            gpointer                user_data);
guint          tracker_search_metadata_by_text_and_location_async (TrackerClient   *client,
                                                            const gchar            *query,
                                                            const gchar            *location,
                                                            TrackerReplyArray       callback,
                                                            gpointer                user_data);
guint          tracker_search_metadata_by_text_and_mime_async (TrackerClient   *client,
                                                            const gchar            *query,
                                                            const gchar           **mimes,
                                                            TrackerReplyArray       callback,
                                                            gpointer                user_data);
guint          tracker_search_metadata_by_text_and_mime_and_location_async (TrackerClient   *client,
                                                            const gchar            *query,
                                                            const gchar           **mimes,
                                                            const gchar            *location,
                                                            TrackerReplyArray       callback,
                                                            gpointer                user_data);

G_END_DECLS

#endif /* TRACKER_H */

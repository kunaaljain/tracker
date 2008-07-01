/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006, Mr Jamie McCracken (jamiemcc@gnome.org)
 * Copyright (C) 2008, Nokia

 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 */

/* The indexer works as a state machine, there are 3 different queues:
 *
 * * The files queue: the highest priority one, individual files are
 *   stored here, waiting for metadata extraction, etc... files are
 *   taken one by one in order to be processed, when this queue is
 *   empty, a single token from the next queue is processed.
 *
 * * The directories queue: directories are stored here, waiting for
 *   being inspected. When a directory is inspected, contained files
 *   and directories will be prepended in their respective queues.
 *   When this queue is empty, a single token from the next queue
 *   is processed.
 *
 * * The modules list: indexing modules are stored here, these modules
 *   can either prepend the files or directories to be inspected in
 *   their respective queues.
 *
 * Once all queues are empty, all elements have been inspected, and the
 * indexer will emit the ::finished signal, this behavior can be observed
 * in the indexing_func() function.
 *
 * NOTE: Normally all indexing petitions will be sent over DBus, being
 *       everything just pushed in the files queue.
 */

#include <stdlib.h>
#include <string.h>

#include <gmodule.h>
#include <glib/gstdio.h>

#include <libtracker-common/tracker-config.h>
#include <libtracker-common/tracker-dbus.h>
#include <libtracker-common/tracker-file-utils.h>
#include <libtracker-common/tracker-language.h>
#include <libtracker-common/tracker-parser.h>
#include <libtracker-common/tracker-ontology.h>

#include <libtracker-db/tracker-db-manager.h>
#include <libtracker-db/tracker-db-interface-sqlite.h>

#include "tracker-indexer.h"
#include "tracker-indexer-module.h"
#include "tracker-indexer-db.h"
#include "tracker-index.h"
#include "tracker-module.h"

#define TRACKER_INDEXER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), TRACKER_TYPE_INDEXER, TrackerIndexerPrivate))

/* Flush every 'x' seconds */
#define FLUSH_FREQUENCY 10

typedef struct TrackerIndexerPrivate TrackerIndexerPrivate;
typedef struct PathInfo PathInfo;
typedef struct MetadataForeachData MetadataForeachData;

struct TrackerIndexerPrivate {
	GQueue *dir_queue;
	GQueue *file_process_queue;
	GQueue *modules_queue;

	GSList *module_names;
	GHashTable *indexer_modules;

	gchar *db_dir;

	TrackerIndex *index;
	TrackerDBInterface *metadata;
	TrackerDBInterface *contents;
	TrackerDBInterface *common;
	TrackerDBInterface *cache;

	TrackerConfig *config;
	TrackerLanguage *language;

	GTimer *timer;
	guint   items_indexed;

	guint idle_id;
	guint flush_id;
};

struct PathInfo {
	GModule *module;
	TrackerFile *file;
};

struct MetadataForeachData {
	TrackerIndex *index;
	TrackerDBInterface *db;

	TrackerLanguage *language;
	TrackerConfig *config;
	TrackerService *service;
	guint32 id;
};

enum {
	PROP_0,
	PROP_RUNNING,
};

enum {
	FINISHED,
	INDEX_UPDATED,
	LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0, };

G_DEFINE_TYPE (TrackerIndexer, tracker_indexer, G_TYPE_OBJECT)

static PathInfo *
path_info_new (GModule     *module,
	       const gchar *path)
{
	PathInfo *info;

	info = g_slice_new (PathInfo);
	info->module = module;
	info->file = tracker_indexer_module_file_new (module, path);

	return info;
}

static void
path_info_free (PathInfo *info)
{
	tracker_indexer_module_file_free (info->module, info->file);
	g_slice_free (PathInfo, info);
}

static gboolean
schedule_flush_cb (gpointer data)
{
	TrackerIndexer        *indexer;
	TrackerIndexerPrivate *priv;

	indexer = TRACKER_INDEXER (data);
	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	priv->flush_id = 0;

	priv->items_indexed += tracker_index_flush (priv->index);

	return FALSE;
}

static void
schedule_flush (TrackerIndexer *indexer,
		gboolean        immediately)
{
	TrackerIndexerPrivate *priv;

        priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	if (immediately) {
		priv->items_indexed += tracker_index_flush (priv->index);
		return;
	}

	priv->flush_id = g_timeout_add_seconds (FLUSH_FREQUENCY, 
						schedule_flush_cb, 
						indexer);
}

static void
tracker_indexer_finalize (GObject *object)
{
	TrackerIndexerPrivate *priv;

	priv = TRACKER_INDEXER_GET_PRIVATE (object);

	/* Important! Make sure we flush if we are scheduled to do so,
	 * and do that first.
	 */
	if (priv->flush_id) {
		g_source_remove (priv->flush_id);
		schedule_flush (TRACKER_INDEXER (object), TRUE);
	}	

	if (priv->timer) {
		g_timer_destroy (priv->timer);
	}

	g_free (priv->db_dir);

	g_queue_foreach (priv->dir_queue, (GFunc) path_info_free, NULL);
	g_queue_free (priv->dir_queue);

	g_queue_foreach (priv->file_process_queue, (GFunc) path_info_free, NULL);
	g_queue_free (priv->file_process_queue);

	/* The queue doesn't own the module names */
	g_queue_free (priv->modules_queue);

	g_hash_table_destroy (priv->indexer_modules);

	g_object_unref (priv->config);
	g_object_unref (priv->language);

	if (priv->index) {
		tracker_index_free (priv->index);
	}

	G_OBJECT_CLASS (tracker_indexer_parent_class)->finalize (object);
}

static void
tracker_indexer_set_property (GObject      *object,
			      guint         prop_id,
			      const GValue *value,
			      GParamSpec   *pspec)
{
	TrackerIndexer *indexer;
	TrackerIndexerPrivate *priv;

	indexer = TRACKER_INDEXER (object);
	priv = TRACKER_INDEXER_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_RUNNING:
		tracker_indexer_set_running (indexer, 
					     g_value_get_boolean (value), 
					     NULL);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
tracker_indexer_get_property (GObject    *object,
			      guint       prop_id,
			      GValue     *value,
			      GParamSpec *pspec)
{
	TrackerIndexerPrivate *priv;

	priv = TRACKER_INDEXER_GET_PRIVATE (object);

	switch (prop_id) {
	case PROP_RUNNING:
		g_value_set_boolean (value, (priv->idle_id != 0));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
	}
}

static void
tracker_indexer_class_init (TrackerIndexerClass *class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (class);

	object_class->finalize = tracker_indexer_finalize;
	object_class->set_property = tracker_indexer_set_property;
	object_class->get_property = tracker_indexer_get_property;

	signals [FINISHED] = 
		g_signal_new ("finished",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, finished),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__UINT,
			      G_TYPE_NONE, 
			      1,
			      G_TYPE_UINT);
	signals [INDEX_UPDATED] = 
		g_signal_new ("index-updated",
			      G_OBJECT_CLASS_TYPE (object_class),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (TrackerIndexerClass, index_updated),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	g_object_class_install_property (object_class,
					 PROP_RUNNING,
					 g_param_spec_boolean ("running",
							       "Running",
							       "Whether the indexer is running",
							       TRUE,
							       G_PARAM_READWRITE));

	g_type_class_add_private (object_class, sizeof (TrackerIndexerPrivate));
}

static void
tracker_indexer_init (TrackerIndexer *indexer)
{
	TrackerIndexerPrivate *priv;
	gchar *index_file;
	GSList *m;

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	priv->dir_queue = g_queue_new ();
	priv->file_process_queue = g_queue_new ();
	priv->modules_queue = g_queue_new ();
	priv->config = tracker_config_new ();
	priv->language = tracker_language_new (priv->config);

	priv->db_dir = g_build_filename (g_get_user_cache_dir (),
					 "tracker", 
					 NULL);

	priv->module_names = tracker_config_get_index_modules (priv->config);

	priv->indexer_modules = g_hash_table_new_full (g_str_hash,
						       g_str_equal,
						       NULL,
						       (GDestroyNotify) g_module_close);

	for (m = priv->module_names; m; m = m->next) {
		GModule *module;

		module = tracker_indexer_module_load (m->data);

		if (module) {
			g_hash_table_insert (priv->indexer_modules,
					     m->data, module);
		}
	}
	
	index_file = g_build_filename (priv->db_dir, "file-index.db", NULL);

	priv->index = tracker_index_new (index_file,
					 tracker_config_get_max_bucket_count (priv->config));

	priv->cache = tracker_db_manager_get_db_interface (TRACKER_DB_CACHE);
	priv->common = tracker_db_manager_get_db_interface (TRACKER_DB_COMMON);
	priv->metadata = tracker_db_manager_get_db_interfaces (3, 
							       TRACKER_DB_COMMON,
							       TRACKER_DB_CACHE, 
							       TRACKER_DB_FILE_METADATA);
	priv->contents = tracker_db_manager_get_db_interface (TRACKER_DB_FILE_CONTENTS);

	priv->timer = g_timer_new ();

	tracker_indexer_set_running (indexer, TRUE, NULL);

	g_free (index_file);
}

static void
tracker_indexer_add_file (TrackerIndexer *indexer,
			  PathInfo       *info)
{
	TrackerIndexerPrivate *priv;

	g_return_if_fail (info != NULL);

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	g_queue_push_tail (priv->file_process_queue, info);
}

static void
tracker_indexer_add_directory (TrackerIndexer *indexer,
			       PathInfo       *info)
{
	TrackerIndexerPrivate *priv;
	gboolean ignore = FALSE;
	gchar **ignore_dirs;
	gint i;

	g_return_if_fail (info != NULL);

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	ignore_dirs = tracker_indexer_module_get_ignore_directories (info->module);

	if (ignore_dirs) {
		for (i = 0; ignore_dirs[i]; i++) {
			if (strcmp (info->file->path, ignore_dirs[i]) == 0) {
				ignore = TRUE;
				break;
			}
		}
	}

	if (!ignore) {
		g_queue_push_tail (priv->dir_queue, info);
	} else {
		g_message ("Ignoring directory:'%s'", info->file->path);
		path_info_free (info);
	}

	g_strfreev (ignore_dirs);
}

static void
index_metadata_foreach (gpointer key,
			gpointer value,
			gpointer user_data)
{
	TrackerField *field;
	MetadataForeachData *data;
	gchar *parsed_value;
	gchar **arr;
	gint i;

	if (!value) {
		return;
	}

	field = tracker_ontology_get_field_def ((gchar *) key);

	data = (MetadataForeachData *) user_data;
	parsed_value = tracker_parser_text_to_string ((gchar *) value,
						      data->language,
						      tracker_config_get_max_word_length (data->config),
						      tracker_config_get_min_word_length (data->config),
						      tracker_field_get_filtered (field),
						      tracker_field_get_filtered (field),
						      tracker_field_get_delimited (field));
	arr = g_strsplit (parsed_value, " ", -1);

	for (i = 0; arr[i]; i++) {
		tracker_index_add_word (data->index,
					arr[i],
					data->id,
					tracker_service_get_id (data->service),
					tracker_field_get_weight (field));
	}

	tracker_db_set_metadata (data->db, data->id, field, (gchar *) value, parsed_value);

	g_free (parsed_value);
	g_strfreev (arr);
}

static void
index_metadata (TrackerIndexer *indexer,
		guint32         id,
		TrackerService *service,
		GHashTable     *metadata)
{
	TrackerIndexerPrivate *priv;
	MetadataForeachData data;

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	data.index = priv->index;
	data.db = priv->metadata;
	data.language = priv->language;
	data.config = priv->config;
	data.service = service;
	data.id = id;

	g_hash_table_foreach (metadata, index_metadata_foreach, &data);

	if (!priv->flush_id) {
		schedule_flush (indexer, FALSE);
	}
}

static gboolean
process_file (TrackerIndexer *indexer,
	      PathInfo       *info)
{
	GHashTable *metadata;

	g_message ("Processing file:'%s'", info->file->path);

	metadata = tracker_indexer_module_file_get_metadata (info->module, info->file);

	if (metadata) {
		TrackerService *service;
		TrackerIndexerPrivate *priv;
		const gchar *service_type;
		guint32 id;

		priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

		service_type = tracker_indexer_module_get_name (info->module);
		service = tracker_ontology_get_service_type_by_name (service_type);
		id = tracker_db_get_new_service_id (priv->common);

		/* Begin of transaction point X */

		/* If you ever need to remove this transaction, because it gets
		 * wrapped into a larger one, that's fine IF you indeed have a
		 * larger one in place that spans cache,common and the selected
		 * metadata database file */

		tracker_db_interface_start_transaction (priv->metadata);

		if (tracker_db_create_service (priv->metadata, id, service, info->file->path, metadata)) {
			gchar *text;
			guint32 eid;

			eid = tracker_db_get_new_event_id (priv->metadata);

			tracker_db_create_event (priv->metadata, eid, id, "Create");

			tracker_db_increment_stats (priv->metadata, service);

			index_metadata (indexer, id, service, metadata);

			text = tracker_indexer_module_file_get_text (info->module, info->file);

			if (text) {
				tracker_db_set_text (priv->contents, id, text);
				g_free (text);
			}
		}
		
		tracker_db_interface_end_transaction (priv->metadata); 
		
		/* End of transaction point X */

		g_hash_table_destroy (metadata);
	}

	return !tracker_indexer_module_file_iter_contents (info->module, info->file);
}

static void
process_directory (TrackerIndexer *indexer,
		   PathInfo       *info,
		   gboolean        recurse)
{
	const gchar *name;
	GDir *dir;

	g_message ("Processing directory:'%s'", info->file->path);

	dir = g_dir_open (info->file->path, 0, NULL);

	if (!dir) {
		return;
	}

	while ((name = g_dir_read_name (dir)) != NULL) {
		PathInfo *new_info;
		gchar *path;

		path = g_build_filename (info->file->path, name, NULL);

		new_info = path_info_new (info->module, path);
		tracker_indexer_add_file (indexer, new_info);

		if (recurse && g_file_test (path, G_FILE_TEST_IS_DIR)) {
			new_info = path_info_new (info->module, path);
			tracker_indexer_add_directory (indexer, new_info);
		}

		g_free (path);
	}

	g_dir_close (dir);
}

static void
process_module (TrackerIndexer *indexer,
		const gchar    *module_name)
{
	TrackerIndexerPrivate *priv;
	GModule *module;
	gchar **dirs;
	gint i;

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);
	module = g_hash_table_lookup (priv->indexer_modules, module_name);

	if (!module) {
		g_message ("No module for:'%s'", module_name);
		return;
	}

	g_message ("Starting module:'%s'", module_name);

	dirs = tracker_indexer_module_get_directories (module);
	g_return_if_fail (dirs != NULL);

	for (i = 0; dirs[i]; i++) {
		PathInfo *info;

		info = path_info_new (module, dirs[i]);
		tracker_indexer_add_directory (indexer, info);

		g_free (dirs[i]);
	}

	g_free (dirs);
}

static gboolean
indexing_func (gpointer data)
{
	TrackerIndexer *indexer;
	TrackerIndexerPrivate *priv;
	PathInfo *path;
	gchar *module;

	indexer = (TrackerIndexer *) data;
	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	if ((path = g_queue_peek_head (priv->file_process_queue)) != NULL) {
		/* Process file */
		if (process_file (indexer, path)) {
			path = g_queue_pop_head (priv->file_process_queue);
			path_info_free (path);
		}
	} else if ((path = g_queue_pop_head (priv->dir_queue)) != NULL) {
		/* Process directory contents */
		process_directory (indexer, path, TRUE);
		path_info_free (path);
	} else {
		/* Dirs/files queues are empty, process the next module */
		module = g_queue_pop_head (priv->modules_queue);

		if (!module) {
			/* Flush remaining items */
			schedule_flush (indexer, TRUE);

			/* No more modules to query, we're done */
			g_timer_stop (priv->timer);

			g_message ("Indexer finished in %4.4f seconds, %d items indexed in total",
				   g_timer_elapsed (priv->timer, NULL),
				   priv->items_indexed);

			g_signal_emit (indexer, signals[FINISHED], 0, priv->items_indexed);
			return FALSE;
		}

		process_module (indexer, module);

		g_signal_emit (indexer, signals[INDEX_UPDATED], 0);
	}

	return TRUE;
}

TrackerIndexer *
tracker_indexer_new (void)
{
	return g_object_new (TRACKER_TYPE_INDEXER, NULL);
}

gboolean
tracker_indexer_set_running (TrackerIndexer  *indexer,
			     gboolean         should_be_running,
			     GError         **error)
{
	TrackerIndexerPrivate *priv;
	guint                  request_id;
	gboolean               changed = FALSE;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_return_val_if_fail (TRACKER_IS_INDEXER (indexer), FALSE, error);

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

        tracker_dbus_request_new (request_id,
                                  "DBus request to %s indexer", 
                                  should_be_running ? "start" : "stop");

	if (should_be_running && priv->idle_id == 0) {
		priv->idle_id = g_idle_add ((GSourceFunc) indexing_func, indexer);
		changed = TRUE;
	} else if (!should_be_running && priv->idle_id != 0) {
		g_source_remove (priv->idle_id);
		priv->idle_id = 0;
		changed = TRUE;
	}

	if (changed) {
		g_object_notify (G_OBJECT (indexer), "running");
	}

	tracker_dbus_request_success (request_id);

	return TRUE;
}

gboolean
tracker_indexer_get_running (TrackerIndexer  *indexer,
			     gboolean        *is_running,
			     GError         **error)
{
	TrackerIndexerPrivate *priv;
	guint                  request_id;

	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_return_val_if_fail (TRACKER_IS_INDEXER (indexer), FALSE, error);
	tracker_dbus_return_val_if_fail (is_running != NULL, FALSE, error);

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	tracker_dbus_request_new (request_id,
                                  "DBus request to get running status");

	*is_running = priv->idle_id != 0;

	tracker_dbus_request_success (request_id);

	return TRUE;
}

void
tracker_indexer_process_all (TrackerIndexer *indexer)
{
	TrackerIndexerPrivate *priv;
	GSList *m;

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);

	for (m = priv->module_names; m; m = m->next) {
		g_queue_push_tail (priv->modules_queue, m->data);
	}
}

gboolean
tracker_indexer_files_check (TrackerIndexer  *indexer,
			     const gchar     *module_name,
			     GStrv            files,
			     GError         **error)
{
	TrackerIndexerPrivate *priv;
	GModule               *module;
	guint                  request_id;
	gint                   i;

	tracker_dbus_return_val_if_fail (TRACKER_IS_INDEXER (indexer), FALSE, error);
	tracker_dbus_return_val_if_fail (files != NULL, FALSE, error);

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);
	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
                                  "DBus request to check %d files",
				  g_strv_length (files));

	module = g_hash_table_lookup (priv->indexer_modules, module_name);

	if (!module) {
		tracker_dbus_request_failed (request_id,
					     error,
					     "The module is not loaded");
		return FALSE;
	}

	/* Add files to the queue */
	for (i = 0; files[i]; i++) {
		PathInfo *info;

		info = path_info_new (module, files[i]);
		tracker_indexer_add_file (indexer, info);
	}

	tracker_dbus_request_success (request_id);

	return TRUE;
}

gboolean
tracker_indexer_files_update (TrackerIndexer  *indexer,
			      const gchar     *module_name,
			      GStrv            files,
			      GError         **error)
{
	TrackerIndexerPrivate *priv;
	GModule               *module;
	guint                  request_id;
	gint                   i;

	tracker_dbus_return_val_if_fail (TRACKER_IS_INDEXER (indexer), FALSE, error);
	tracker_dbus_return_val_if_fail (files != NULL, FALSE, error);

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);
	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
                                  "DBus request to update %d files",
				  g_strv_length (files));

	module = g_hash_table_lookup (priv->indexer_modules, module_name);

	if (!module) {
		tracker_dbus_request_failed (request_id,
					     error,
					     "The module is not loaded");
		return FALSE;
	}

	/* Add files to the queue */
	for (i = 0; files[i]; i++) {
		PathInfo *info;

		info = path_info_new (module, files[i]);
		tracker_indexer_add_file (indexer, info);
	}

	tracker_dbus_request_success (request_id);

	return TRUE;
}

gboolean
tracker_indexer_files_delete (TrackerIndexer  *indexer,
			      const gchar     *module_name,
			      GStrv            files,
			      GError         **error)
{
	TrackerIndexerPrivate *priv;
	GModule               *module;
	guint                  request_id;
	gint                   i;

	tracker_dbus_return_val_if_fail (TRACKER_IS_INDEXER (indexer), FALSE, error);
	tracker_dbus_return_val_if_fail (files != NULL, FALSE, error);

	priv = TRACKER_INDEXER_GET_PRIVATE (indexer);
	request_id = tracker_dbus_get_next_request_id ();

	tracker_dbus_request_new (request_id,
                                  "DBus request to delete %d files",
				  g_strv_length (files));

	module = g_hash_table_lookup (priv->indexer_modules, module_name);

	if (!module) {
		tracker_dbus_request_failed (request_id,
					     error,
					     "The module is not loaded");
		return FALSE;
	}

	/* Add files to the queue */
	for (i = 0; files[i]; i++) {
		PathInfo *info;

		info = path_info_new (module, files[i]);
		tracker_indexer_add_file (indexer, info);
	}

	tracker_dbus_request_success (request_id);

	return TRUE;
}


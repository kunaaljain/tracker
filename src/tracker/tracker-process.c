/*
 * Copyright (C) 2010, Nokia <ivan.frade@nokia.com>
 * Copyright (C) 2014, Lanedo <martyn@lanedo.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "config.h"

#include <stdlib.h>
#include <errno.h>

#include <glib.h>
#include <glib/gi18n.h>

#include "tracker-process.h"

static TrackerProcessData *
process_data_new (gchar *cmd, pid_t pid)
{
	TrackerProcessData *pd;

	pd = g_slice_new0 (TrackerProcessData);
	pd->cmd = cmd;
	pd->pid = pid;

	return pd;
}

void
tracker_process_data_free (TrackerProcessData *pd)
{
	if (!pd) {
		return;
	}

	g_free (pd->cmd);
	g_slice_free (TrackerProcessData, pd);
}

GSList *
tracker_process_get_pids (void)
{
	GError *error = NULL;
	GDir *dir;
	GSList *pids = NULL;
	const gchar *name;

	dir = g_dir_open ("/proc", 0, &error);
	if (error) {
		g_printerr ("%s: %s\n",
		            _("Could not open /proc"),
		            error ? error->message : _("No error given"));
		g_clear_error (&error);
		return NULL;
	}

	while ((name = g_dir_read_name (dir)) != NULL) {
		gchar c;
		gboolean is_pid = TRUE;

		for (c = *name; c && c != ':' && is_pid; c++) {
			is_pid &= g_ascii_isdigit (c);
		}

		if (!is_pid) {
			continue;
		}

		pids = g_slist_prepend (pids, g_strdup (name));
	}

	g_dir_close (dir);

	return g_slist_reverse (pids);
}

guint32
tracker_process_get_uid_for_pid (const gchar  *pid_as_string,
                                 gchar       **filename)
{
	GFile *f;
	GFileInfo *info;
	GError *error = NULL;
	gchar *fn;
	guint uid;

#ifdef __sun /* Solaris */
	fn = g_build_filename ("/proc", pid_as_string, "psinfo", NULL);
#else
	fn = g_build_filename ("/proc", pid_as_string, "cmdline", NULL);
#endif

	f = g_file_new_for_path (fn);
	info = g_file_query_info (f,
	                          G_FILE_ATTRIBUTE_UNIX_UID,
	                          G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
	                          NULL,
	                          &error);

	if (error) {
		g_printerr ("%s '%s', %s", _("Could not stat() file"), fn, error->message);
		g_error_free (error);
		uid = 0;
	} else {
		uid = g_file_info_get_attribute_uint32 (info, G_FILE_ATTRIBUTE_UNIX_UID);
		g_object_unref (info);
	}

	if (filename) {
		*filename = fn;
	} else {
		g_free (fn);
	}

	g_object_unref (f);

	return uid;
}

GSList *
tracker_process_find_all (void)
{
	GSList *pids, *l;
	GSList *found_pids = NULL;
	guint32 own_pid;
	guint32 own_uid;
	gchar *own_pid_str;

	/* Unless we are stopping processes or listing processes,
	 * don't iterate them.
	 */
	pids = tracker_process_get_pids ();

	/* Establish own uid/pid */
	own_pid = (guint32) getpid ();
	own_pid_str = g_strdup_printf ("%d", own_pid);
	own_uid = tracker_process_get_uid_for_pid (own_pid_str, NULL);
	g_free (own_pid_str);

	for (l = pids; l; l = l->next) {
		GError *error = NULL;
		gchar *filename;
#ifdef __sun /* Solaris */
		psinfo_t psinfo = { 0 };
#endif
		gchar *contents = NULL;
		gchar **strv;
		guint uid;
		pid_t pid;

		uid = tracker_process_get_uid_for_pid (l->data, &filename);

		/* Stat the file and make sure current user == file owner */
		if (uid != own_uid) {
			continue;
		}

		pid = atoi (l->data);

		/* Don't return our own PID */
		if (pid == own_pid) {
			continue;
		}

		/* Get contents to determine basename */
		if (!g_file_get_contents (filename, &contents, NULL, &error)) {
			gchar *str;

			str = g_strdup_printf (_("Could not open '%s'"), filename);
			g_printerr ("%s: %s\n",
			            str,
			            error ? error->message : _("No error given"));
			g_free (str);
			g_clear_error (&error);
			g_free (contents);
			g_free (filename);

			continue;
		}
#ifdef __sun /* Solaris */
		memcpy (&psinfo, contents, sizeof (psinfo));

		/* won't work with paths containing spaces :( */
		strv = g_strsplit (psinfo.pr_psargs, " ", 2);
#else
		strv = g_strsplit (contents, "^@", 2);
#endif
		if (strv && strv[0]) {
			gchar *basename;

			basename = g_path_get_basename (strv[0]);

			if ((g_str_has_prefix (basename, "tracker") ||
			     g_str_has_prefix (basename, "lt-tracker"))) {
				found_pids = g_slist_prepend (found_pids, process_data_new (basename, pid));
			} else {
				g_free (basename);
			}
		}

		g_strfreev (strv);
		g_free (contents);
		g_free (filename);
	}

	g_slist_foreach (pids, (GFunc) g_free, NULL);
	g_slist_free (pids);

	return g_slist_reverse (found_pids);
}

gint
tracker_process_stop (TrackerProcessTypes daemons_to_term,
                      TrackerProcessTypes daemons_to_kill)
{
	GSList *pids, *l;
	gchar *str;

	if (daemons_to_kill == TRACKER_PROCESS_TYPE_NONE &&
	    daemons_to_term == TRACKER_PROCESS_TYPE_NONE) {
		return 0;
	}

	pids = tracker_process_find_all ();

	str = g_strdup_printf (g_dngettext (NULL,
	                                    "Found %d PID…",
	                                    "Found %d PIDs…",
	                                    g_slist_length (pids)),
	                       g_slist_length (pids));
	g_print ("%s\n", str);
	g_free (str);

	for (l = pids; l; l = l->next) {
		TrackerProcessData *pd;
		const gchar *basename;
		pid_t pid;

		pd = l->data;
		basename = pd->cmd;
		pid = pd->pid;
		
		if (daemons_to_term != TRACKER_PROCESS_TYPE_NONE) {
			if ((daemons_to_term == TRACKER_PROCESS_TYPE_STORE &&
			     !g_str_has_suffix (basename, "tracker-store")) ||
			    (daemons_to_term == TRACKER_PROCESS_TYPE_MINERS &&
			     !strstr (basename, "tracker-miner"))) {
				continue;
			}

			if (kill (pid, SIGTERM) == -1) {
				const gchar *errstr = g_strerror (errno);
						
				str = g_strdup_printf (_("Could not terminate process %d - '%s'"), pid, basename);
				g_printerr ("  %s: %s\n",
				            str,
				            errstr ? errstr : _("No error given"));
				g_free (str);
			} else {
				str = g_strdup_printf (_("Terminated process %d - '%s'"), pid, basename);
				g_print ("  %s\n", str);
				g_free (str);
			}
		} else if (daemons_to_kill != TRACKER_PROCESS_TYPE_NONE) {
			if ((daemons_to_kill == TRACKER_PROCESS_TYPE_STORE &&
			     !g_str_has_suffix (basename, "tracker-store")) ||
			    (daemons_to_kill == TRACKER_PROCESS_TYPE_MINERS &&
			     !strstr (basename, "tracker-miner"))) {
				continue;
			}

			if (kill (pid, SIGKILL) == -1) {
				const gchar *errstr = g_strerror (errno);

				str = g_strdup_printf (_("Could not kill process %d - '%s'"), pid, basename);
				g_printerr ("  %s: %s\n",
				            str,
				            errstr ? errstr : _("No error given"));
				g_free (str);
			} else {
				str = g_strdup_printf (_("Killed process %d - '%s'"), pid, basename);
				g_print ("  %s\n", str);
				g_free (str);
			}
		}
	}

	g_slist_foreach (pids, (GFunc) tracker_process_data_free, NULL);
	g_slist_free (pids);

	return 0;
}

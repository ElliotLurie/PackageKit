/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2025 Elliot Lurie <ElliotLurie@mailo.com>
 *
 * Licensed under the GNU General Public License Version 2
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <gmodule.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>

#include <pk-backend.h>
#include <pk-backend-job.h>

#include <xbps.h>


void
pk_backend_initialize (GKeyFile *conf, PkBackend *backend)
{
	struct xbps_handle *xbps = malloc (sizeof (struct xbps_handle));
	memset (xbps, 0, sizeof (struct xbps_handle));

	xbps_init (xbps);
	pk_backend_set_user_data (backend, xbps);
}

void
pk_backend_destroy (PkBackend *backend)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	xbps_end (xbps);
	free (xbps);
}

PkBitfield
pk_backend_get_groups (PkBackend *backend)
{
	return pk_bitfield_from_enums (PK_GROUP_ENUM_UNKNOWN, -1);
}

PkBitfield
pk_backend_get_filters (PkBackend *backend)
{
	return pk_bitfield_from_enums (PK_FILTER_ENUM_INSTALLED, PK_FILTER_ENUM_NOT_INSTALLED,
			PK_FILTER_ENUM_ARCH, PK_FILTER_ENUM_NOT_ARCH, -1);
}

gboolean
pk_backend_supports_parallelization (PkBackend *backend)
{
	return FALSE;
}

static const gchar*
format_repo (const gchar *repository)
{
	/* Truncate repo to base page */
	const gchar *repo;

	if (repository == NULL)
		return "";

	repo = strrchr (repository, '/');
	if (repo == NULL)
		return "";

	return repo + 1; /* Skip the "/" */
}

struct query_data {
	PkBitfield filters;
	_Bool (*filter_func)(xbps_dictionary_t pkg, const char *name, void *data);
	void *filter_data;

	PkInfoEnum info;
	PkBackendJob *job;

	GSList *prev_pkgs;
	const gchar *repo;
};


static int
query_packages_cb (struct xbps_handle *xbps, xbps_object_t obj, const char *key, void *data, _Bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_dictionary_t pkg = (xbps_dictionary_t) obj;
	gchar *id;

	/* Package properties */
	const gchar *arch, *name = key, *pkgver, *repository = qd->repo, *short_desc, *version;

	/* Filter packages */
	xbps_dictionary_get_cstring_nocopy (pkg, "architecture", &arch);
	if (pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_ARCH, -1) > 0
			&& g_strcmp0 (xbps->native_arch + 1, arch) != 0)
		return 0;

	if (pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_NOT_ARCH, -1) > 0
			&& g_strcmp0 (xbps->native_arch + 1, arch) == 0)
		return 0;
	
	if (qd->filter_func != NULL && !qd->filter_func (pkg, name, qd->filter_data))
		return 0;

	xbps_dictionary_get_cstring_nocopy (pkg, "pkgver", &pkgver);
	version = xbps_pkg_version (pkgver);

	if (repository == NULL) {
		xbps_dictionary_get_cstring_nocopy (pkg, "repository", &repository);
		repository = format_repo (repository);
	}

	id = pk_package_id_build (name, version, arch, repository);

	for (GSList *prev_pkg = qd->prev_pkgs; prev_pkg != NULL; prev_pkg = prev_pkg->next) {
		gchar *prev_id = (gchar *) prev_pkg->data;

		if (g_strcmp0 (id, prev_id) == 0) {
			g_free (id);
			return 0;
		}
	}

	qd->prev_pkgs = g_slist_append (qd->prev_pkgs, id);
	xbps_dictionary_get_cstring_nocopy (pkg, "short_desc", &short_desc);

	pk_backend_job_package (qd->job, qd->info, id, short_desc);
	return 0;
}

static int
query_repos_cb (struct xbps_repo *repo, void *data, _Bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_array_t keys;
	int ret;

	if (repo->idx == NULL) return 0;
	qd->repo = format_repo (repo->uri);
	
	keys = xbps_dictionary_all_keys (repo->idx);

	ret = xbps_array_foreach_cb (repo->xhp, keys, repo->idx, query_packages_cb, data);
	xbps_object_release (keys);
	return ret;
}

static void
query_packages (PkBackend *backend, PkBackendJob *job, struct query_data *qd, PkBitfield filters)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	qd->filters = filters;
	qd->job = job;
	qd->repo = NULL;
	qd->prev_pkgs = NULL;

	pk_backend_job_set_status (job, PK_STATUS_ENUM_QUERY);

	if (filters == PK_FILTER_ENUM_UNKNOWN || pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_INSTALLED, -1) >= 0) {
		qd->info = PK_INFO_ENUM_INSTALLED;
		xbps_pkgdb_foreach_cb (xbps, query_packages_cb, qd);
	}

	if (filters == PK_FILTER_ENUM_UNKNOWN || pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_NOT_INSTALLED, -1) >= 0) {
		qd->info = PK_INFO_ENUM_AVAILABLE;
		xbps_rpool_foreach (xbps, query_repos_cb, qd);
	}

	g_slist_free_full (qd->prev_pkgs, g_free);
}

void
pk_backend_get_packages (PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
	struct query_data qd;
	qd.filter_func = NULL;
	qd.filter_data = NULL;

	query_packages (backend, job, &qd, filters);
	pk_backend_job_finished (job);
}

static _Bool
search_names_filter (xbps_dictionary_t pkg, const char *name, void *data)
{
	gchar **values = (gchar **) data;

	for (guint i = 0; i < g_strv_length (values); i++) {
		if (strstr (name, values[i]))
			return true;
	}

	return false;
}

void
pk_backend_search_names (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
	struct query_data qd;
	qd.filter_func = search_names_filter;
	qd.filter_data = values;

	query_packages (backend, job, &qd, filters);
	pk_backend_job_finished (job);
}

const gchar *
pk_backend_get_description (PkBackend *backend)
{
	return "The X Binary Package System";
}

const gchar *
pk_backend_get_author (PkBackend *backend)
{
	return "Elliot Lurie <ElliotLurie@mailo.com>";
}

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

static void
fetch_cb (const struct xbps_fetch_cb_data *, void *)
{

}

static int
state_cb (const struct xbps_state_cb_data *, void *)
{
	return 0;
}

static void
unpack_cb (const struct xbps_unpack_cb_data *, void *)
{

}

void
pk_backend_initialize (GKeyFile *conf, PkBackend *backend)
{
	struct xbps_handle *xbps = malloc (sizeof (struct xbps_handle));
	memset (xbps, 0, sizeof (struct xbps_handle));

	xbps->fetch_cb = fetch_cb;
	xbps->state_cb = state_cb;
	xbps->unpack_cb = unpack_cb;

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
	return false;
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

struct package_data {
	const gchar *arch, *name, *repo, *pkgver, *version;
	xbps_dictionary_t pkg;
};

static void
load_package_data (struct package_data *pd, xbps_dictionary_t pkg, const gchar *name)
{
	xbps_dictionary_get_cstring_nocopy (pkg, "architecture", &pd->arch);
	xbps_dictionary_get_cstring_nocopy (pkg, "pkgver", &pd->pkgver);

	pd->name = name;
	pd->version = xbps_pkg_version (pd->pkgver);

	pd->pkg = pkg;
}

static const gchar *
get_repository_from_package (xbps_dictionary_t pkg)
{
	const gchar *repo;
	xbps_dictionary_get_cstring_nocopy (pkg, "repository", &repo);

	return format_repo (repo);
}

static gchar *
build_id_from_package (struct package_data *pd)
{
	return pk_package_id_build (pd->name, pd->version, pd->arch, pd->repo);
}

struct query_data;

typedef bool (*filter)(struct package_data *pd, void *filter_data);

struct query_data {
	PkBitfield filters;

	PkInfoEnum info;
	PkBackendJob *job;

	GSList *prev_pkgs;
	const gchar *repo;

	filter filter_cb;
	void *filter_data;
};

static bool
filter_package (struct xbps_handle *xbps, struct package_data *pd, PkBitfield filters)
{
	if (pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_ARCH, -1) > 0
			&& g_strcmp0 (xbps->native_arch + 1, pd->arch) != 0)
		return false;

	if (pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_NOT_ARCH, -1) > 0
			&& g_strcmp0 (xbps->native_arch + 1, pd->arch) == 0)
		return false;
	
	return true;
}

static void
get_add_package (struct query_data *qd, struct package_data *pd)
{
	const gchar *short_desc;

	gchar *id = build_id_from_package (pd);
	for (GSList *prev_pkg = qd->prev_pkgs; prev_pkg != NULL; prev_pkg = prev_pkg->next) {
		gchar *prev_id = (gchar *) prev_pkg->data;

		if (g_strcmp0 (id, prev_id) == 0) {
			g_free (id);
			return;
		}
	}

	qd->prev_pkgs = g_slist_append (qd->prev_pkgs, id);
	xbps_dictionary_get_cstring_nocopy (pd->pkg, "short_desc", &short_desc);

	pk_backend_job_package (qd->job, qd->info, id, short_desc);
}

static int
get_installed_cb (struct xbps_handle *xbps, xbps_object_t obj, const char *key, void *data, bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_dictionary_t pkg = (xbps_dictionary_t) obj;

	struct package_data pd;
	load_package_data (&pd, pkg, key);

	/* Filter packages */
	if (!filter_package (xbps, &pd, qd->filters) ||
			(qd->filter_cb != NULL && qd->filter_cb (&pd, qd->filter_data)))
		return 0;

	pd.repo = get_repository_from_package (pkg);

	get_add_package (qd, &pd);
	return 0;
}

static int
get_available_cb (struct xbps_handle *xbps, xbps_object_t obj, const char *key, void *data, bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_dictionary_t pkg = (xbps_dictionary_t) obj;

	struct package_data pd;
	load_package_data (&pd, pkg, key);

	/* Filter packages */
	if (!filter_package (xbps, &pd, qd->filters) ||
			(qd->filter_cb != NULL && qd->filter_cb (&pd, qd->filter_data)))
		return 0;

	/* Don't show "available" packages if they are installed on the system */
	if (!(pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_INSTALLED, -1) > 0) &&
			xbps_pkg_is_installed (xbps, pd.pkgver))
		return 0;

	pd.repo = qd->repo;
	get_add_package (qd, &pd);
	return 0;
}

static int
get_repos_cb (struct xbps_repo *repo, void *data, bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_array_t keys;
	int ret;

	if (repo->idx == NULL) return 0;
	qd->repo = format_repo (repo->uri);
	
	keys = xbps_dictionary_all_keys (repo->idx);

	ret = xbps_array_foreach_cb (repo->xhp, keys, repo->idx, get_available_cb, data);
	xbps_object_release (keys);
	return ret;
}

static void
begin_query (struct query_data *qd, PkBackendJob *job, PkBitfield filters)
{
	qd->filters = filters;
	qd->job = job;
	qd->repo = NULL;
	qd->prev_pkgs = NULL;
	qd->filter_cb = NULL;
	qd->filter_data = NULL;
}

typedef int (*query_installed)(struct xbps_handle *xbps, xbps_object_t obj, const char *key, void *data, bool *done);
typedef int (*query_repo)(struct xbps_repo *repo, void *data, bool *done);

static void
query (PkBackend *backend, struct query_data *qd, query_installed query_installed_cb, query_repo query_repo_cb)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	bool installed = pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_INSTALLED, -1) >= 0;
	bool not_installed = pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_NOT_INSTALLED, -1) >= 0;

	pk_backend_job_set_status (qd->job, PK_STATUS_ENUM_QUERY);

	if (installed || !not_installed) {
		qd->info = PK_INFO_ENUM_INSTALLED;
		xbps_pkgdb_foreach_cb (xbps, query_installed_cb, qd);
	}

	if (not_installed || !installed) {
		qd->info = PK_INFO_ENUM_AVAILABLE;
		xbps_rpool_foreach (xbps, query_repo_cb, qd);
	}
}

static void
finish_query (struct query_data *qd)
{
	g_slist_free_full (qd->prev_pkgs, g_free);
	pk_backend_job_finished (qd->job);
}

void
pk_backend_get_packages (PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
	struct query_data qd;
	begin_query (&qd, job, filters);

	query (backend, &qd, get_installed_cb, get_repos_cb);
	finish_query (&qd);
}

void
pk_backend_install_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids)
{
	/* TODO:
	 * List packages that caused errors
	 * Specify commit errors */
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);
	int rv = xbps_pkgdb_lock (xbps);

	if (rv != 0)
		{
			pk_backend_job_error_code (job, PK_ERROR_ENUM_CANNOT_GET_LOCK, "Failed to lock XBPS database: %s\n", strerror (rv));	
			pk_backend_job_finished (job);
			return;
		}

	for (guint i = 0; i < g_strv_length (package_ids); i++) {
		gchar **id_values = pk_package_id_split (package_ids [i]);
		g_autofree gchar *pkgver = g_strdup_printf ("%s-%s", id_values [PK_PACKAGE_ID_NAME], id_values [PK_PACKAGE_ID_VERSION]);

		g_strfreev (id_values);

		rv = xbps_transaction_install_pkg (xbps, pkgver, false);

		switch (rv) {
			case 0:
				continue;
			case EBUSY:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_INSTALL_BLOCKED, "The xbps package must be updated first\n");	
				break;
			case EEXIST:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_ALREADY_INSTALLED, "%s is already installed\n", pkgver);	
				break;
			case ENOENT:
			case ENOTSUP:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_NOT_FOUND, "%s not found in repository pool\n", pkgver);	
				break;
			case ENXIO:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_CANNOT_GET_REQUIRES, "%s has invalid dependencies\n", pkgver);	
				break;
			default:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_UNKNOWN, "%s failed to be queued for installation\n", pkgver);
		}

		xbps_pkgdb_unlock (xbps);
		pk_backend_job_finished (job);
		return;
	}

	rv = xbps_transaction_prepare (xbps);
	if (rv != 0) switch (rv) {
		case EAGAIN:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_CONFLICTS, "Packages conflict\n");
			break;
		case EINVAL:
		case ENXIO:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "An internal error occurred\n");
			break;
		case ENODEV:
		case ENOEXEC:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_DEP_RESOLUTION_FAILED, "Dependencies missing\n");
			break;
		case ENOSPC:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_NO_SPACE_ON_DEVICE, "No space left on root filesystem\n");
			break;
		default:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_UNKNOWN, "Failed to prepare transaction\n");

		pk_backend_job_finished (job);
		xbps_pkgdb_unlock (xbps);
		return;
	}

	xbps_transaction_commit (xbps);
	if (rv != 0)
		pk_backend_job_error_code (job, PK_ERROR_ENUM_TRANSACTION_ERROR, "Failed to commit transaction\n");

	xbps_pkgdb_unlock (xbps);
	pk_backend_job_finished (job);
}

void
pk_backend_resolve (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **packages)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);
	guint len = g_strv_length (packages);
	
	bool installed = pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_INSTALLED, -1) >= 0;
	bool not_installed = pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_NOT_INSTALLED, -1) >= 0;

	struct query_data qd;
	begin_query (&qd, job, filters);

	for (guint i = 0; i < len; i++) {
		const gchar *name;
		struct package_data pd;
		xbps_dictionary_t pkg = NULL; 

		bool pkg_is_installed = xbps_pkg_is_installed (xbps, packages [i]);
		PkInfoEnum info;

		if (installed || (!not_installed && pkg_is_installed)) {
			info = PK_INFO_ENUM_INSTALLED;
			pkg = xbps_pkgdb_get_pkg (xbps, packages [i]);
		} else if (not_installed || !pkg_is_installed) {
			info = PK_INFO_ENUM_AVAILABLE;
			pkg = xbps_rpool_get_pkg (xbps, packages [i]);
		}

		if (pkg == NULL)
			continue;

		xbps_dictionary_get_cstring_nocopy (pkg, "pkgname", &name);
		load_package_data (&pd, pkg, name);
		pd.repo = get_repository_from_package (pkg);

		if (filter_package (xbps, &pd, filters)) {
			const gchar *short_desc;

			g_autofree gchar *id = build_id_from_package (&pd);
			xbps_dictionary_get_cstring_nocopy (pd.pkg, "short_desc", &short_desc);
			pk_backend_job_package (job, info, id, short_desc);
		}

		xbps_object_release (pkg);
	}

	pk_backend_job_finished (job);
}

static void
begin_search (struct query_data *qd, gchar **values)
{
	guint len = g_strv_length (values);
	gchar **tokens = g_malloc (sizeof (gchar *) * (len + 1));

	for (guint i = 0; i < len; i++) {
		tokens[i] = g_utf8_casefold (values [i], -1);
	}
	tokens [len] = NULL;

	qd->filter_data = tokens;
}

static void
end_search (struct query_data *qd)
{
	gchar **tokens = (gchar **) qd->filter_data;

	for (guint i = 0; i < g_strv_length (tokens); i++) {
		g_free (tokens [i]);
	}

	g_free (tokens);
}

static bool
search_names_filter_cb (struct package_data *pd, void *filter_data)
{
	gchar **tokens = (gchar **) filter_data;
	g_autofree gchar *name = g_utf8_casefold (pd->name, -1);

	for (guint i = 0; i < g_strv_length (tokens); i++)
		if (!strstr (name, tokens[i]))
			return false;

	return true;
}

void
pk_backend_search_names (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
	struct query_data qd;

	begin_query (&qd, job, filters);
	begin_search (&qd, values);
	qd.filter_cb = search_names_filter_cb;

	query (backend, &qd, get_installed_cb, get_repos_cb);
	end_search (&qd);
	finish_query (&qd);
}

static bool
search_details_filter_cb (struct package_data *pd, void *filter_data)
{
	gchar **tokens = (gchar **) filter_data;
	g_autofree gchar *name = g_utf8_casefold (pd->name, -1);
	g_autofree gchar* short_desc;
	const gchar* _short_desc;

	xbps_dictionary_get_cstring_nocopy (pd->pkg, "short_desc", &_short_desc);
	short_desc = g_utf8_casefold (_short_desc, -1);

	for (guint i = 0; i < g_strv_length (tokens); i++)
		if (!strstr (name, tokens[i]) && !strstr (short_desc, tokens[i]))
			return false;

	return true;
}

void
pk_backend_search_details (PkBackend *backend, PkBackendJob *job, PkBitfield filters, gchar **values)
{
	struct query_data qd;

	begin_query (&qd, job, filters);
	begin_search (&qd, values);
	qd.filter_cb = search_details_filter_cb;

	query (backend, &qd, get_installed_cb, get_repos_cb);
	end_search (&qd);
	finish_query (&qd);
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

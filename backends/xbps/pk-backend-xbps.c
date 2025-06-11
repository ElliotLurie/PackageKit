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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
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

static struct xbps_handle xbps;

void
pk_backend_initialize (GKeyFile *conf, PkBackend *backend)
{
	xbps_init (&xbps);
}

void
pk_backend_destroy (PkBackend *backend)
{
	xbps_end (&xbps);
}

static char*
id_from_pkg (xbps_dictionary_t pkg)
{
  gchar *name, *version, *id;
	const gchar *arch, *repo;

	xbps_dictionary_get_cstring (pkg, "pkgver", &name);

  version = strrchr (name, '-');
  version[0] = '\0';
  version++;

	xbps_dictionary_get_cstring_nocopy (pkg, "architecture", &arch);
	xbps_dictionary_get_cstring_nocopy (pkg, "repository", &repo);

	id = pk_package_id_build (name, version, arch, strrchr (repo, '/') + 1);
  free (name);
  return id;
}

struct getpkg_data {
	PkBackendJob *job;
	PkInfoEnum info;
};

static int
getpkg_foreach_cb (struct xbps_handle *xbps_p, xbps_object_t obj, const char *key, void *data, _Bool *done)
{
	struct getpkg_data *gp = (struct getpkg_data *) data;
	xbps_dictionary_t pkg = (xbps_dictionary_t) obj;

	g_autofree gchar *id = id_from_pkg (pkg);

	const gchar* summary;
	xbps_dictionary_get_cstring_nocopy (pkg, "short_desc", &summary);

	pk_backend_job_package (gp->job, gp->info, id, summary);

	return 0;
}

void
pk_backend_get_packages (PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
	struct getpkg_data data;
	data.job = job;

	pk_backend_job_set_status (job, PK_STATUS_ENUM_REQUEST);

	if (pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_INSTALLED, -1)) {
		data.info = PK_INFO_ENUM_INSTALLED;
		xbps_pkgdb_foreach_cb (&xbps, getpkg_foreach_cb, &data);
	}

	pk_backend_job_finished (job);
}

void
pk_backend_get_details_local (PkBackend *backend, PkBackendJob *job, gchar **files)
{
	int pkgs;
  pk_backend_job_set_status (job, PK_STATUS_ENUM_SETUP);
  pk_backend_job_set_percentage (job, 0);

	pkgs = 0;
	while (files [pkgs] != NULL)
		pkgs++;

  pk_backend_job_set_status (job, PK_STATUS_ENUM_QUERY);
  for (int i = 0; i < pkgs; i++) {
		xbps_dictionary_t pkg = xbps_pkgdb_get_pkg (&xbps, files [i]);

		g_autofree gchar* id = id_from_pkg (pkg);
		gulong size = xbps_number_unsigned_integer_value (xbps_dictionary_get (pkg, "installed_size"));
		const gchar* summary, *license, *url;


		xbps_dictionary_get_cstring_nocopy (pkg, "short_desc", &summary);
		xbps_dictionary_get_cstring_nocopy (pkg, "license", &license);
		xbps_dictionary_get_cstring_nocopy (pkg, "homepage", &url);

		pk_backend_job_details (job, id, summary, license, PK_GROUP_ENUM_UNKNOWN, NULL, url, size);
		pk_backend_job_set_percentage (job, (int) ((float) i / pkgs * 100));
	}

  pk_backend_job_finished (job);
}

PkBitfield
pk_backend_get_groups (PkBackend *backend)
{
	return pk_bitfield_from_enums (PK_GROUP_ENUM_UNKNOWN, -1);
}

PkBitfield
pk_backend_get_filters (PkBackend *backend)
{
	return pk_bitfield_from_enums (PK_FILTER_ENUM_INSTALLED, -1);
}

gboolean
pk_backend_supports_parallelization (PkBackend *backend)
{
	return TRUE;
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

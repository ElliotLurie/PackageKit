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

#include <glib.h>
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

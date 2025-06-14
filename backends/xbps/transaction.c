#include <pk-backend.h>
#include <pk-backend-job.h>
#include <xbps.h>

static bool
begin_transaction (PkBackendJob *job, struct xbps_handle *xbps)
{
	int rv = xbps_pkgdb_lock (xbps);
	if (rv != 0) {
		pk_backend_job_error_code (job, PK_ERROR_ENUM_CANNOT_GET_LOCK, "Failed to lock XBPS database: %s\n", strerror (rv));	
		pk_backend_job_finished (job);

		return false;
	}
	pk_backend_job_set_locked (job, true);

	return true;
}

static void
run_transaction (PkBackendJob *job, struct xbps_handle *xbps)
{
	int rv = xbps_transaction_prepare (xbps);
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
			pk_backend_job_error_code (job, PK_ERROR_ENUM_CANNOT_GET_REQUIRES, "Could not satisfy dependencies\n");
			break;
		case ENOSPC:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_NO_SPACE_ON_DEVICE, "No space left on root filesystem\n");
			break;
		default:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_UNKNOWN, "Failed to prepare transaction\n");

		return;
	}

	rv = xbps_transaction_commit (xbps);
	if (rv != 0)
		pk_backend_job_error_code (job, PK_ERROR_ENUM_TRANSACTION_ERROR, "Failed to commit transaction\n");
}

static void
finish_transaction (PkBackendJob *job, struct xbps_handle *xbps)
{
	if (xbps->transd != NULL)
		{
			xbps_object_release (xbps->transd);
			xbps->transd = NULL;
		}

	xbps_pkgdb_unlock (xbps);
	pk_backend_job_set_locked (job, false);
}

static void
install_packages_thread (PkBackendJob *job, GVariant *params, gpointer data)
{
	struct xbps_handle *xbps = (struct xbps_handle *) data;
	PkBitfield transaction_flags;
	g_autofree gchar **package_ids;

	g_variant_get (params, "(t^a&s)", &transaction_flags, &package_ids);

	for (guint i = 0; i < g_strv_length (package_ids); i++) {
		gchar **id_values = pk_package_id_split (package_ids [i]);
		g_autofree gchar *pkgver = g_strdup_printf ("%s-%s", id_values [PK_PACKAGE_ID_NAME], id_values [PK_PACKAGE_ID_VERSION]);
		int rv;

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
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_NOT_FOUND, "%s not found in repository pool\n", pkgver);	
				break;
			case ENOTSUP:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_REPO_NOT_FOUND, "No repositories set up\n");	
				break;
			case ENXIO:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_DEP_RESOLUTION_FAILED, "%s has invalid dependencies\n", pkgver);	
				break;
			default:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_UNKNOWN, "%s failed to be queued for installation\n", pkgver);
		}

		goto END;
	}

	run_transaction (job, xbps);
	
END:
	finish_transaction (job, xbps);
}

void
pk_backend_install_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	if (!begin_transaction (job, xbps))
		return;

	pk_backend_job_thread_create (job, install_packages_thread, xbps, NULL);
}

static gchar *
package_name_from_id (const gchar *id)
{
	g_auto (GStrv) id_values = pk_package_id_split (id);
	return g_strdup (id_values [PK_PACKAGE_ID_NAME]);
}

static bool
remove_dependent (PkBackendJob *job, struct xbps_handle *xbps, gchar *pkg, bool autoremove)
{
	xbps_array_t dependents = xbps_pkgdb_get_pkg_revdeps (xbps, pkg);
	int rv;
	for (guint i = 0; i < xbps_array_count (dependents); i++) {
		const gchar *dep;
		if (!xbps_array_get_cstring_nocopy (dependents, i, &dep))
			return false;

		if (!remove_dependent (job, xbps, (gchar *) dep, autoremove))
			return false;
	}

	rv = xbps_transaction_remove_pkg (xbps, pkg, autoremove);
	switch (rv) {
		case 0:
			break;
		case ENOENT:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_NOT_INSTALLED, "%s is not installed\n", pkg);
			return false;
		default:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_INTERNAL_ERROR, "%s could not be queued for remvoal\n", pkg);
			return false;
	}

	return true;
}

static void
remove_packages_thread (PkBackendJob *job, GVariant *params, gpointer data)
{
	struct xbps_handle *xbps = (struct xbps_handle *) data;
	PkBitfield transaction_flags;
	g_autofree gchar **package_ids;
	bool allow_deps, autoremove;

	g_variant_get (params, "(t^a&sbb)", &transaction_flags, &package_ids, &allow_deps, &autoremove);

	for (guint i = 0; i < g_strv_length (package_ids); i++) {
		g_autofree gchar *pkg = package_name_from_id (package_ids [i]);
		int rv;

		if (allow_deps) {
			if (!remove_dependent (job, xbps, pkg, autoremove))
				goto END;

			continue;
		}
		
		rv = xbps_transaction_remove_pkg (xbps, pkg, autoremove);
		switch (rv) {
			case 0:
				continue;
			case EEXIST:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_DEP_RESOLUTION_FAILED, "%s is a dependency of another package\n", pkg);
				break;
			case ENOENT:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_NOT_INSTALLED, "%s is not installed\n", pkg);
				break;
			default:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_UNKNOWN, "%s could not be queed for remvoal\n", pkg);
		}

		goto END;
	}

	run_transaction (job, xbps);

END:
	finish_transaction (job, xbps);
}

void
pk_backend_remove_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids, gboolean allow_deps, gboolean autoremove)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	if (!begin_transaction (job, xbps))
		return;

	pk_backend_job_thread_create (job, remove_packages_thread, xbps, NULL);
}

static void
update_packages_thread (PkBackendJob *job, GVariant *params, gpointer data)
{
	struct xbps_handle *xbps = (struct xbps_handle *) data;
	PkBitfield transaction_flags;
	g_autofree gchar **package_ids;

	g_variant_get (params, "(t^a&s)", &transaction_flags, &package_ids);

	for (guint i = 0; i < g_strv_length (package_ids); i++) {
		g_autofree gchar *pkg = package_name_from_id (package_ids [i]);
		int rv = xbps_transaction_update_pkg (xbps, pkg, false);

		switch (rv) {
			case 0:
			case EEXIST:
				continue;
			case EBUSY:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_INSTALL_BLOCKED, "The xbps package must be updated first\n");
				break;
			case ENOENT:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_PACKAGE_NOT_FOUND, "%s not found in repository pool\n", pkg);
				break;
			case ENOTSUP:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_REPO_NOT_FOUND, "No repositories are available\n");
				break;
			case ENXIO:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_DEP_RESOLUTION_FAILED, "%s has invalid dependencies\n", pkg);
				break;
			default:
				pk_backend_job_error_code (job, PK_ERROR_ENUM_UNKNOWN, "%s failed to be queued to update\n", pkg);
		}

		goto END;
	}

	run_transaction (job, xbps);

END:
	finish_transaction (job, xbps);
}

void
pk_backend_update_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	if (!begin_transaction (job, xbps))
		return;

	pk_backend_job_thread_create (job, update_packages_thread, xbps, NULL);
}

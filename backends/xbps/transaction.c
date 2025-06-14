#include <pk-backend.h>
#include <pk-backend-job.h>
#include <xbps.h>

void
pk_backend_install_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids)
{
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
			pk_backend_job_error_code (job, PK_ERROR_ENUM_CANNOT_GET_REQUIRES, "Could not satisfy dependencies\n");
			break;
		case ENOSPC:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_NO_SPACE_ON_DEVICE, "No space left on root filesystem\n");
			break;
		default:
			pk_backend_job_error_code (job, PK_ERROR_ENUM_UNKNOWN, "Failed to prepare transaction\n");

		goto END;
	}

	rv = xbps_transaction_commit (xbps);
	if (rv != 0)
		pk_backend_job_error_code (job, PK_ERROR_ENUM_TRANSACTION_ERROR, "Failed to commit transaction\n");

END:
	xbps_pkgdb_unlock (xbps);
	pk_backend_job_finished (job);
}

void
pk_backend_update_packages (PkBackend *backend, PkBackendJob *job, PkBitfield transaction_flags, gchar **package_ids)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);
	guint len = g_strv_length (package_ids);

	pk_backend_job_finished (job);
}

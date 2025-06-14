#include <pk-backend.h>
#include <pk-backend-job.h>
#include <xbps.h>

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
			&& (g_strcmp0 (xbps->native_arch, pd->arch) != 0))
		return false;

	if (pk_bitfield_contain_priority (filters, PK_FILTER_ENUM_NOT_ARCH, -1) > 0
			&& (g_strcmp0 (xbps->native_arch, pd->arch) == 0))
		return false;
	
	return true;
}

static void
query_add_package (struct query_data *qd, struct package_data *pd)
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
query_installed_cb (struct xbps_handle *xbps, xbps_object_t obj, const char *key, void *data, bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_dictionary_t pkg = (xbps_dictionary_t) obj;

	struct package_data pd;
	load_package_data (&pd, pkg, key);

	/* Filter packages */
	if (!filter_package (xbps, &pd, qd->filters) ||
			(qd->filter_cb != NULL && !qd->filter_cb (&pd, qd->filter_data)))
		return 0;

	pd.repo = get_repository_from_package (pkg);

	query_add_package (qd, &pd);
	return 0;
}

static int
query_available_cb (struct xbps_handle *xbps, xbps_object_t obj, const char *key, void *data, bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_dictionary_t pkg = (xbps_dictionary_t) obj;

	struct package_data pd;
	load_package_data (&pd, pkg, key);

	/* Filter packages */
	if (!filter_package (xbps, &pd, qd->filters) ||
			(qd->filter_cb != NULL && !qd->filter_cb (&pd, qd->filter_data)))
		return 0;

	/* Don't show "available" packages if they are installed on the system */
	if (!(pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_INSTALLED, -1) > 0) &&
			xbps_pkg_is_installed (xbps, pd.pkgver))
		return 0;

	pd.repo = qd->repo;
	query_add_package (qd, &pd);
	return 0;
}

static int
query_repos_cb (struct xbps_repo *repo, void *data, bool *done)
{
	struct query_data *qd = (struct query_data *) data;
	xbps_array_t keys;
	int ret;

	if (repo->idx == NULL) return 0;
	qd->repo = format_repo (repo->uri);
	
	keys = xbps_dictionary_all_keys (repo->idx);

	ret = xbps_array_foreach_cb (repo->xhp, keys, repo->idx, query_available_cb, data);
	xbps_object_release (keys);

	return ret;
}

static bool
begin_query (struct query_data *qd, PkBackendJob *job, struct xbps_handle *xbps, PkBitfield filters)
{
	/*
	if (!xbps_pkgdb_update (xbps, false, true)) {
		pk_backend_job_error_code (job, PK_ERROR_ENUM_CANNOT_FETCH_SOURCES, "Failed to load package database\n");	
		pk_backend_job_finished (job);	
		return false;
	}
	*/

	qd->filters = filters;
	qd->job = job;
	qd->repo = NULL;
	qd->prev_pkgs = NULL;
	qd->filter_cb = NULL;
	qd->filter_data = NULL;

	return true;
}

static void
query (struct query_data *qd, struct xbps_handle *xbps)
{
	bool installed = pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_INSTALLED, -1) >= 0;
	bool not_installed = pk_bitfield_contain_priority (qd->filters, PK_FILTER_ENUM_NOT_INSTALLED, -1) >= 0;

	pk_backend_job_set_status (qd->job, PK_STATUS_ENUM_QUERY);

	if (installed || !not_installed) {
		qd->info = PK_INFO_ENUM_INSTALLED;
		xbps_pkgdb_foreach_cb (xbps, query_installed_cb, qd);
	}

	if (not_installed || !installed) {
		qd->info = PK_INFO_ENUM_AVAILABLE;
		xbps_rpool_foreach (xbps, query_repos_cb, qd);
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
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);
	struct query_data qd;
	if (!begin_query (&qd, job, xbps, filters))
		return;

	query (&qd, xbps);
	finish_query (&qd);
}


static void
add_package (PkBackendJob *job, PkInfoEnum info, struct package_data *pd)
{
	const gchar *short_desc;

	g_autofree gchar *id = build_id_from_package (pd);
	xbps_dictionary_get_cstring_nocopy (pd->pkg, "short_desc", &short_desc);
	pk_backend_job_package (job, info, id, short_desc);
}

static int
get_update_cb (struct xbps_handle *xbps, xbps_object_t obj, const char *key, void *data, bool *done)
{
	const gchar *ver, *remote_ver;
	struct query_data *qd = (struct query_data *) data;

	xbps_dictionary_t pkg = (xbps_dictionary_t) obj, remote_pkg = xbps_rpool_get_pkg (xbps, key);

	if (remote_pkg == NULL)
		return 0;

	xbps_dictionary_get_cstring_nocopy (pkg, "pkgver", &ver);
	ver = xbps_pkg_version (ver);

	xbps_dictionary_get_cstring_nocopy (remote_pkg, "pkgver", &remote_ver);
	remote_ver = xbps_pkg_version (remote_ver);

	if (xbps_cmpver (ver, remote_ver) == -1) {
		struct package_data pd;
		
		load_package_data (&pd, remote_pkg, key);
		pd.repo = get_repository_from_package (pkg);

		add_package (qd->job, PK_INFO_ENUM_NORMAL, &pd);
	}

	return 0;
}

void
pk_backend_get_updates (PkBackend *backend, PkBackendJob *job, PkBitfield filters)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	struct query_data qd;
	if (!begin_query (&qd, job, xbps, filters))
		return;

	xbps_pkgdb_foreach_cb (xbps, get_update_cb, &qd);

	finish_query (&qd);
}

void
pk_backend_refresh_cache (PkBackend *backend, PkBackendJob *job, gboolean force)
{
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	if (xbps_rpool_sync (xbps, NULL) != 0)
		pk_backend_job_error_code (job, PK_ERROR_ENUM_REPO_NOT_FOUND, "No repositories set up\n");	

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
	if (!begin_query (&qd, job, xbps, filters))
		return;

	for (guint i = 0; i < len; i++) {
		const gchar *pkgver;
		gchar *name;
		struct package_data pd;
		size_t name_size;
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

		xbps_dictionary_get_cstring_nocopy (pkg, "pkgver", &pkgver);
		name_size = (strrchr (pkgver, '-') - pkgver + 1) * sizeof (char);
		name = g_malloc (name_size);
		xbps_pkg_name (name, name_size, pkgver);

		load_package_data (&pd, pkg, (gchar *) name);
		pd.repo = get_repository_from_package (pkg);

		if (filter_package (xbps, &pd, filters))
			add_package (job, info, &pd);

		g_free (name);
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
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);
	struct query_data qd;

	if (!begin_query (&qd, job, xbps, filters))
		return;

	begin_search (&qd, values);
	qd.filter_cb = search_names_filter_cb;

	query (&qd, xbps);
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
	struct xbps_handle *xbps = (struct xbps_handle *) pk_backend_get_user_data (backend);

	if (!begin_query (&qd, job, xbps, filters))
		return;

	begin_search (&qd, values);
	qd.filter_cb = search_details_filter_cb;

	query (&qd, xbps);
	end_search (&qd);
	finish_query (&qd);
}


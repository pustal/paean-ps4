/*
 * sync_cloud.c - sync saves through a central "cloud" store.
 *
 * Supported stores:
 *   - FTP / FTPS servers           (ftp://user:pass@host:port/path/)
 *   - WebDAV shares (NAS, Nextcloud, ...)  (http[s]://user:pass@host/path/)
 *   - Google Drive                 (see gdrive.c)
 *
 * Layout inside the store (one folder per sync profile, so two consoles with
 * different users / account ids can share saves by using the same profile):
 *
 *   <store>/paean/<profile>/manifest.txt               index of all saves
 *   <store>/paean/<profile>/<TITLE_ID>_<DIR_NAME>.zip  one zip per save
 *
 * manifest.txt uses the same tab separated format as the LAN save lists, and
 * its mtime column is the mtime the save had on the console that uploaded it.
 * The sync logic itself (what to push / pull) lives in sync_common.c.
 *
 * There is no locking: running a cloud sync on both consoles at the very same
 * time may lose a manifest update (the zips are still there and the next sync
 * re-uploads them). The manifest is re-read and merged right before it is
 * written to make that window as small as possible.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <mini18n.h>

#include "saves.h"
#include "menu.h"
#include "common.h"
#include "util.h"
#include "paean_sync.h"

#define CLOUD_MANIFEST_TMP  SYNC_TMP_PATH "manifest.txt"
#define CLOUD_ARCHIVE_TMP   SYNC_TMP_PATH "cloud.zip"

/* A cloud backend: open a session, move files by name, close. */
typedef struct
{
	const char* name;
	int  (*open)(void);
	int  (*get)(const char* name, const char* local_path);  /* 1 ok, 0 missing, -1 error */
	int  (*put)(const char* local_path, const char* name);  /* 1 ok, 0 error */
	void (*close)(void);
} cloud_store_t;

/* ---------------------------------------------------------------------- */
/* FTP / WebDAV backend (libcurl)                                          */
/* ---------------------------------------------------------------------- */

static char url_base[700];          /* <cloud_url>paean/<profile>/ */
static int  url_dirs_ready = 0;     /* WebDAV collections created this session */

static int url_is_http(void)
{
	return (strncasecmp(apollo_config.cloud_url, "http", 4) == 0);
}

static int url_open(void)
{
	snprintf(url_base, sizeof(url_base), "%spaean/%s/", apollo_config.cloud_url, apollo_config.sync_profile);
	url_dirs_ready = 0;
	return 1;
}

static void url_close(void)
{
	url_base[0] = 0;
}

static CURL* url_new_handle(const char* name)
{
	char url[1024];
	char* esc = name ? curl_easy_escape(NULL, name, 0) : NULL;
	CURL* curl = curl_easy_init();

	if (curl)
	{
		http_setup_curl(curl);
		snprintf(url, sizeof(url), "%s%s", url_base, esc ? esc : "");
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FTP_SKIP_PASV_IP, 1L);
		curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
	}

	curl_free(esc);
	return curl;
}

static int url_get(const char* name, const char* local_path)
{
	CURL* curl;
	CURLcode res;
	long status = 0;
	FILE* fp;

	if ((fp = fopen(local_path, "wb")) == NULL)
		return -1;

	if ((curl = url_new_handle(name)) == NULL)
	{
		fclose(fp);
		return -1;
	}

	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);

	res = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_cleanup(curl);
	fclose(fp);

	if (res == CURLE_OK)
		return 1;

	unlink_secure(local_path);
	LOG("cloud: GET %s failed: %s (%ld)", name, curl_easy_strerror(res), status);

	// "doesn't exist" is a normal answer (e.g. first sync, no manifest yet).
	// FTP reports a missing *folder* as "access denied" (CWD fails with 550).
	if (res == CURLE_REMOTE_FILE_NOT_FOUND || res == CURLE_REMOTE_ACCESS_DENIED || status == 404 || status == 550)
		return 0;

	return -1;
}

/* curl write callback that throws the response body away. */
static size_t discard_body(void* ptr, size_t size, size_t nmemb, void* userdata)
{
	return size * nmemb;
}

/* Create a WebDAV collection; "already exists" errors are ignored. */
static void url_mkcol(const char* url)
{
	CURL* curl = curl_easy_init();

	if (!curl)
		return;

	http_setup_curl(curl);
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "MKCOL");
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);
	curl_easy_perform(curl);
	curl_easy_cleanup(curl);
}

/*
 * WebDAV has no "create missing parents": MKCOL every folder of url_base,
 * from the first one after the host name down to the profile folder.
 */
static void url_make_dirs(void)
{
	char prefix[sizeof(url_base)];
	const char* p = strstr(url_base, "://");

	if (!p || (p = strchr(p + 3, '/')) == NULL)
		return;

	while ((p = strchr(p + 1, '/')) != NULL)
	{
		size_t len = (size_t)(p - url_base) + 1;

		memcpy(prefix, url_base, len);
		prefix[len] = 0;
		url_mkcol(prefix);
	}
}

static int url_put(const char* local_path, const char* name)
{
	uint64_t size = 0;
	CURL* curl;
	CURLcode res;
	FILE* fp;

	// WebDAV needs the folders to exist; FTP creates them on the fly
	if (url_is_http() && !url_dirs_ready)
	{
		url_make_dirs();
		url_dirs_ready = 1;
	}

	if (get_file_size(local_path, &size) != SUCCESS || (fp = fopen(local_path, "rb")) == NULL)
		return 0;

	if ((curl = url_new_handle(name)) == NULL)
	{
		fclose(fp);
		return 0;
	}

	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, fread);
	curl_easy_setopt(curl, CURLOPT_READDATA, fp);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t) size);
	curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS, CURLFTP_CREATE_DIR);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_body);

	res = curl_easy_perform(curl);
	curl_easy_cleanup(curl);
	fclose(fp);

	if (res != CURLE_OK)
		LOG("cloud: PUT %s failed: %s", name, curl_easy_strerror(res));

	return (res == CURLE_OK);
}

static const cloud_store_t store_url = {
	.name = "FTP/WebDAV",
	.open = url_open,
	.get = url_get,
	.put = url_put,
	.close = url_close,
};

/* ---------------------------------------------------------------------- */
/* Google Drive backend                                                    */
/* ---------------------------------------------------------------------- */

static int gd_store_open(void)
{
	char path[64];

	snprintf(path, sizeof(path), "Paean/%s", apollo_config.sync_profile);
	return gdrive_connect() && gdrive_open_folder(path);
}

static const cloud_store_t store_gdrive = {
	.name = "Google Drive",
	.open = gd_store_open,
	.get = gdrive_get,
	.put = gdrive_put,
	.close = gdrive_disconnect,
};

/* ---------------------------------------------------------------------- */
/* Sync                                                                    */
/* ---------------------------------------------------------------------- */

int cloud_is_configured(void)
{
	switch (apollo_config.cloud_type)
	{
	case SYNC_CLOUD_URL:
		return (apollo_config.cloud_url[0] != 0);

	case SYNC_CLOUD_GDRIVE:
		return (apollo_config.gdrive_token[0] != 0);

	default:
		return 0;
	}
}

static const cloud_store_t* cloud_get_store(void)
{
	return (apollo_config.cloud_type == SYNC_CLOUD_GDRIVE) ? &store_gdrive : &store_url;
}

/* Download the manifest into `list`. Returns 1 ok (or missing), 0 on error. */
static int cloud_read_manifest(const cloud_store_t* store, sync_list_t* list)
{
	int ret = store->get(SYNC_MANIFEST_FILE, CLOUD_MANIFEST_TMP);

	if (ret < 0)
		return 0;

	if (ret > 0)
		sync_list_read(list, CLOUD_MANIFEST_TMP);

	unlink_secure(CLOUD_MANIFEST_TMP);
	return 1;
}

static int cloud_push(const cloud_store_t* store, const sync_save_t* save)
{
	char name[128];
	int ok;

	if (!sync_export_save(save, CLOUD_ARCHIVE_TMP))
		return 0;

	sync_archive_name(save, name, sizeof(name));
	ok = store->put(CLOUD_ARCHIVE_TMP, name);
	unlink_secure(CLOUD_ARCHIVE_TMP);

	return ok;
}

static int cloud_pull(const cloud_store_t* store, const sync_save_t* save)
{
	char name[128];
	int ok;

	sync_archive_name(save, name, sizeof(name));
	if (store->get(name, CLOUD_ARCHIVE_TMP) <= 0)
		return 0;

	ok = sync_import_save(save, CLOUD_ARCHIVE_TMP);
	unlink_secure(CLOUD_ARCHIVE_TMP);

	return ok;
}

int cloud_sync(const char* title_id, const char* dir_name)
{
	const cloud_store_t* store;
	sync_list_t local, remote, state, pushed;
	sync_op_t* ops = NULL;
	char endpoint[64], target[96];
	int count, done = 0, failed = 0, changed = 0;

	if (!cloud_is_configured())
	{
		show_message("%s\n\n%s", _("Cloud sync is not configured."), _("Set it up in Settings first."));
		return 0;
	}

	store = cloud_get_store();
	snprintf(endpoint, sizeof(endpoint), "cloud_%s", apollo_config.sync_profile);
	snprintf(target, sizeof(target), "%s (%s)", store->name, apollo_config.sync_profile);

	sync_init_dirs();
	sync_list_init(&local);
	sync_list_init(&remote);
	sync_list_init(&state);
	sync_list_init(&pushed);

	init_loading_screen(_("Connecting to the cloud..."));
	if (!store->open() || !cloud_read_manifest(store, &remote))
	{
		stop_loading_screen();
		show_message("%s %s\n\n%s", _("Error! Can't connect to"), target, _("Check debug logs for more information"));
		goto end;
	}
	sync_list_local(&local);
	stop_loading_screen();

	sync_state_load(endpoint, &state);
	count = sync_build_plan(&local, &remote, &state, title_id, dir_name, &ops);

	if (count && !sync_confirm_plan(target, ops, count))
		goto end;

	init_progress_bar(_("Syncing saves..."));
	for (int i = 0; i < count; i++)
	{
		sync_op_t* op = &ops[i];
		int ok;

		update_progress_bar(i, count, op->save.name);
		ok = (op->action == SYNC_ACT_PUSH) ? cloud_push(store, &op->save) : cloud_pull(store, &op->save);

		if (!ok)
		{
			failed++;
			continue;
		}

		done++;
		sync_state_set(&state, &op->save, op->save.mtime);

		if (op->action == SYNC_ACT_PUSH)
			sync_list_add(&pushed, &op->save);
		else
			changed = 1;
	}
	update_progress_bar(count, count, "");
	end_progress_bar();

	// publish the new entries; re-read first to keep concurrent updates
	if (pushed.count)
	{
		sync_list_t fresh;

		init_loading_screen(_("Updating cloud index..."));
		sync_list_init(&fresh);
		if (!cloud_read_manifest(store, &fresh))
		{
			// couldn't re-read it: fall back to the copy read at the start
			sync_list_free(&fresh);
			for (int i = 0; i < remote.count; i++)
				sync_list_add(&fresh, &remote.items[i]);
		}

		for (int i = 0; i < pushed.count; i++)
			sync_list_add(&fresh, &pushed.items[i]);

		if (!sync_list_write(&fresh, CLOUD_MANIFEST_TMP) || !store->put(CLOUD_MANIFEST_TMP, SYNC_MANIFEST_FILE))
		{
			// without the index the other console can't see the new saves
			failed += pushed.count;
			done -= pushed.count;
			for (int i = 0; i < pushed.count; i++)
				sync_list_remove(&state, pushed.items[i].title_id, pushed.items[i].dir_name);
		}

		unlink_secure(CLOUD_MANIFEST_TMP);
		sync_list_free(&fresh);
		stop_loading_screen();
	}

	// remember saves that already matched the cloud copy
	for (int i = 0; i < local.count; i++)
	{
		sync_save_t* r = sync_list_find(&remote, local.items[i].title_id, local.items[i].dir_name);

		if (r && strcmp(r->mtime, local.items[i].mtime) == 0)
			sync_state_set(&state, r, r->mtime);
	}

	sync_state_save(endpoint, &state);

	if (!count)
		show_message("%s\n%s", _("Everything is already in sync with"), target);
	else
		show_message("%s %s\n\n%s: %d\n%s: %d", _("Sync finished with"), target, _("Saves synced"), done, _("Failed"), failed);

end:
	store->close();
	free(ops);
	sync_list_free(&local);
	sync_list_free(&remote);
	sync_list_free(&state);
	sync_list_free(&pushed);

	return changed;
}

/* ---------------------------------------------------------------------- */
/* Settings callbacks                                                      */
/* ---------------------------------------------------------------------- */

void cloud_set_url(void)
{
	char tmp[sizeof(apollo_config.cloud_url)];
	sync_list_t list;
	int ok;

	sync_strlcpy(tmp, apollo_config.cloud_url[0] ? apollo_config.cloud_url : "ftp://user:pass@192.168.1.10:21/saves/", sizeof(tmp));
	if (!osk_dialog_get_text(_("Enter the FTP or WebDAV URL"), tmp, sizeof(tmp)) || !tmp[0])
		return;

	if (tmp[strlen(tmp) - 1] != '/' && strlen(tmp) + 1 < sizeof(tmp))
		strcat(tmp, "/");

	sync_strlcpy(apollo_config.cloud_url, tmp, sizeof(apollo_config.cloud_url));
	apollo_config.cloud_type = SYNC_CLOUD_URL;

	// test the connection by reading (or not finding) the manifest
	sync_init_dirs();
	sync_list_init(&list);
	init_loading_screen(_("Testing connection..."));
	ok = store_url.open() && cloud_read_manifest(&store_url, &list);
	store_url.close();
	stop_loading_screen();

	if (ok)
		show_message("%s\n%s\n\n%s: %d", _("Cloud sync URL set to:"), apollo_config.cloud_url, _("Saves in the cloud"), list.count);
	else
		show_message("%s\n%s\n\n%s", _("Error! Couldn't connect to"), apollo_config.cloud_url, _("Check debug logs for more information"));

	sync_list_free(&list);
}

void cloud_set_profile(void)
{
	char tmp[sizeof(apollo_config.sync_profile)];
	size_t n = 0;

	sync_strlcpy(tmp, apollo_config.sync_profile, sizeof(tmp));
	if (!osk_dialog_get_text(_("Sync profile name (same on both PS4s)"), tmp, sizeof(tmp)))
		return;

	// the profile becomes a folder name: keep it simple
	for (char* p = tmp; *p; p++)
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_')
			apollo_config.sync_profile[n++] = *p;

	apollo_config.sync_profile[n] = 0;
	if (!n)
		strcpy(apollo_config.sync_profile, "default");

	show_message("%s\n%s", _("Sync profile set to:"), apollo_config.sync_profile);
}

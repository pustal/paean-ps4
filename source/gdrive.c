/*
 * gdrive.c - minimal Google Drive v3 client used by Paean's cloud sync.
 *
 * Authentication uses the OAuth 2.0 "device flow" for TVs and limited input
 * devices: the PS4 shows a short code, the user approves access on a phone or
 * computer, and Paean stores the refresh token in its per-user settings.
 *
 * Only the `drive.file` scope is requested, so Paean can see and change only
 * the files it created itself (everything lives in "My Drive/Paean/<profile>").
 * Both consoles must use the same OAuth client id for that to work.
 *
 * The client id / secret come from a Google Cloud project created by the user
 * (OAuth client type "TVs and Limited Input devices"). They are read from
 * SYNC_GDRIVE_CLIENT_FILE when present, otherwise typed in with the keyboard.
 *
 * JSON handling is deliberately tiny (string lookups only): the responses we
 * read are small and flat, and pulling in a JSON library is not worth it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <mini18n.h>

#include "saves.h"
#include "menu.h"
#include "common.h"
#include "paean_sync.h"

#define GD_OAUTH_DEVICE_URL "https://oauth2.googleapis.com/device/code"
#define GD_OAUTH_TOKEN_URL  "https://oauth2.googleapis.com/token"
#define GD_API_URL          "https://www.googleapis.com/drive/v3/files"
#define GD_UPLOAD_URL       "https://www.googleapis.com/upload/drive/v3/files"
#define GD_SCOPE            "https://www.googleapis.com/auth/drive.file"
#define GD_FOLDER_MIME      "application/vnd.google-apps.folder"
#define GD_BOUNDARY         "paean_sync_boundary_7d1f3a9c"

/* Growable in-memory response body. */
typedef struct
{
	char*  data;
	size_t len;
} membuf_t;

/* Session state, valid between gdrive_connect() and gdrive_disconnect(). */
static char gd_access_token[2048];
static char gd_folder_id[128];

/* ---------------------------------------------------------------------- */
/* Small JSON helpers                                                      */
/* ---------------------------------------------------------------------- */

/*
 * Copy the string value of "key" (first occurrence) into out.
 * Handles the common escapes; good enough for ids, tokens and URLs.
 */
static int json_get_str(const char* json, const char* key, char* out, size_t size)
{
	char pattern[64];
	const char* p;
	size_t n = 0;

	if (!json || !size)
		return 0;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	if ((p = strstr(json, pattern)) == NULL)
		return 0;

	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
		p++;

	if (*p++ != '"')
		return 0;

	while (*p && *p != '"' && n + 1 < size)
	{
		if (*p == '\\' && p[1])
		{
			p++;
			switch (*p)
			{
			case 'n': out[n++] = '\n'; break;
			case 't': out[n++] = '\t'; break;
			default:  out[n++] = *p;   break;   // \" \\ \/
			}
			p++;
			continue;
		}
		out[n++] = *p++;
	}

	out[n] = 0;
	return (n > 0);
}

/* Keep folder names safe for Drive queries (no quotes or backslashes). */
static void safe_name(const char* in, char* out, size_t size)
{
	size_t n = 0;

	for (; *in && n + 1 < size; in++)
		out[n++] = (*in == '\'' || *in == '\\' || *in == '"' || *in == '/') ? '_' : *in;

	out[n] = 0;
}

/* ---------------------------------------------------------------------- */
/* HTTP                                                                    */
/* ---------------------------------------------------------------------- */

static size_t mem_write(void* ptr, size_t size, size_t nmemb, void* userdata)
{
	membuf_t* mem = userdata;
	size_t len = size * nmemb;
	char* tmp = realloc(mem->data, mem->len + len + 1);

	if (!tmp)
		return 0;

	mem->data = tmp;
	memcpy(mem->data + mem->len, ptr, len);
	mem->len += len;
	mem->data[mem->len] = 0;

	return len;
}

/*
 * Perform one request.
 *   method   - "GET", "POST" or "PATCH"
 *   ctype    - Content-Type of the body (NULL for none)
 *   body     - request body (binary safe, body_len bytes)
 *   out_mem  - receives the response body (optional)
 *   out_file - receives the response body instead (optional)
 * Returns the HTTP status code, or 0 on a transport error.
 */
static long gd_request(const char* method, const char* url, const char* ctype,
                       const void* body, size_t body_len, membuf_t* out_mem, FILE* out_file)
{
	CURL* curl = curl_easy_init();
	struct curl_slist* headers = NULL;
	char header[2200];
	long status = 0;
	CURLcode res;

	if (!curl)
		return 0;

	http_setup_curl(curl);
	// we want error bodies (they explain what went wrong), not curl errors
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
	curl_easy_setopt(curl, CURLOPT_URL, url);

	if (gd_access_token[0])
	{
		snprintf(header, sizeof(header), "Authorization: Bearer %s", gd_access_token);
		headers = curl_slist_append(headers, header);
	}

	if (ctype)
	{
		snprintf(header, sizeof(header), "Content-Type: %s", ctype);
		headers = curl_slist_append(headers, header);
	}

	if (strcmp(method, "GET") != 0)
	{
		curl_easy_setopt(curl, CURLOPT_POST, 1L);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body ? body : "");
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) body_len);

		if (strcmp(method, "POST") != 0)
			curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	}

	if (headers)
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

	if (out_file)
	{
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_file);
	}
	else if (out_mem)
	{
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, mem_write);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, out_mem);
	}

	res = curl_easy_perform(curl);
	if (res == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	else
		LOG("gdrive: %s %s failed: %s", method, url, curl_easy_strerror(res));

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return status;
}

/* POST an application/x-www-form-urlencoded body to the OAuth endpoints. */
static long gd_post_form(const char* url, const char* form, membuf_t* out)
{
	char saved[sizeof(gd_access_token)];
	long status;

	// OAuth endpoints must not receive a (possibly stale) bearer token
	strcpy(saved, gd_access_token);
	gd_access_token[0] = 0;
	status = gd_request("POST", url, "application/x-www-form-urlencoded", form, strlen(form), out, NULL);
	strcpy(gd_access_token, saved);

	return status;
}

/* URL-encode `in` into a newly allocated string (free with curl_free). */
static char* url_escape(const char* in)
{
	return curl_easy_escape(NULL, in, 0);
}

/* ---------------------------------------------------------------------- */
/* Credentials                                                             */
/* ---------------------------------------------------------------------- */

/* Load client_id / client_secret from the optional text file. */
static void load_client_file(void)
{
	char line[256];
	FILE* fp = fopen(SYNC_GDRIVE_CLIENT_FILE, "r");

	if (!fp)
		return;

	while (fgets(line, sizeof(line), fp))
	{
		line[strcspn(line, "\r\n")] = 0;

		if (strncmp(line, "client_id=", 10) == 0)
			sync_strlcpy(apollo_config.gdrive_client_id, line + 10, sizeof(apollo_config.gdrive_client_id));
		else if (strncmp(line, "client_secret=", 14) == 0)
			sync_strlcpy(apollo_config.gdrive_client_secret, line + 14, sizeof(apollo_config.gdrive_client_secret));
	}

	fclose(fp);
	LOG("gdrive: client credentials loaded from %s", SYNC_GDRIVE_CLIENT_FILE);
}

/* Make sure we have OAuth client credentials, asking the user if needed. */
static int ensure_client_credentials(void)
{
	load_client_file();

	if (!apollo_config.gdrive_client_id[0] &&
		!osk_dialog_get_text(_("Google OAuth Client ID"), apollo_config.gdrive_client_id, sizeof(apollo_config.gdrive_client_id)))
		return 0;

	if (!apollo_config.gdrive_client_secret[0] &&
		!osk_dialog_get_text(_("Google OAuth Client Secret"), apollo_config.gdrive_client_secret, sizeof(apollo_config.gdrive_client_secret)))
		return 0;

	return (apollo_config.gdrive_client_id[0] && apollo_config.gdrive_client_secret[0]);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

int gdrive_link_account(void)
{
	char form[1024], device_code[512], user_code[64], verify_url[256], error[64];
	char *cid, *csecret, *scope;
	membuf_t resp = {0};
	long status;
	int linked = 0;

	if (!ensure_client_credentials())
	{
		show_message("%s\n\n%s", _("Google Drive needs an OAuth client id and secret."), _("See the Paean README for setup steps."));
		return 0;
	}

	cid = url_escape(apollo_config.gdrive_client_id);
	csecret = url_escape(apollo_config.gdrive_client_secret);
	scope = url_escape(GD_SCOPE);

	// 1. ask Google for a user code
	init_loading_screen(_("Contacting Google..."));
	snprintf(form, sizeof(form), "client_id=%s&scope=%s", cid, scope);
	status = gd_post_form(GD_OAUTH_DEVICE_URL, form, &resp);
	stop_loading_screen();

	if (status != 200 || !json_get_str(resp.data, "device_code", device_code, sizeof(device_code)) ||
		!json_get_str(resp.data, "user_code", user_code, sizeof(user_code)))
	{
		LOG("gdrive: device code request failed (%ld): %s", status, resp.data ? resp.data : "");
		show_message("%s\n%s", _("Error! Google rejected the request."), _("Check the client id / secret."));
		goto end;
	}

	if (!json_get_str(resp.data, "verification_url", verify_url, sizeof(verify_url)))
		strcpy(verify_url, "https://www.google.com/device");

	// 2. the user approves on another device, then we poll for the token
	snprintf(form, sizeof(form), "client_id=%s&client_secret=%s&device_code=%s&grant_type=%s",
		cid, csecret, device_code, "urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code");

	do
	{
		show_message("%s\n\n%s\n\n%s\n%s\n\n%s", _("On your phone or computer open:"), verify_url,
			_("and enter this code:"), user_code, _("Press OK after you allowed access."));

		free(resp.data);
		memset(&resp, 0, sizeof(resp));
		status = gd_post_form(GD_OAUTH_TOKEN_URL, form, &resp);

		if (status == 200 && json_get_str(resp.data, "refresh_token", apollo_config.gdrive_token, sizeof(apollo_config.gdrive_token)))
		{
			linked = 1;
			break;
		}

		error[0] = 0;
		json_get_str(resp.data, "error", error, sizeof(error));
		LOG("gdrive: token poll (%ld): %s", status, error);
	}
	while ((strcmp(error, "authorization_pending") == 0 || strcmp(error, "slow_down") == 0) &&
		show_dialog(DIALOG_TYPE_YESNO, _("Access was not granted yet. Try again?")));

	if (linked)
	{
		apollo_config.cloud_type = SYNC_CLOUD_GDRIVE;
		save_app_settings(&apollo_config);
		show_message(_("Google Drive account linked!"));
	}
	else
		show_message("%s\n%s", _("Google Drive was not linked."), error);

end:
	curl_free(cid);
	curl_free(csecret);
	curl_free(scope);
	free(resp.data);
	return linked;
}

int gdrive_connect(void)
{
	char form[1024], *cid, *csecret, *token;
	membuf_t resp = {0};
	long status;

	gd_access_token[0] = 0;
	gd_folder_id[0] = 0;

	if (!apollo_config.gdrive_token[0])
		return 0;

	load_client_file();
	cid = url_escape(apollo_config.gdrive_client_id);
	csecret = url_escape(apollo_config.gdrive_client_secret);
	token = url_escape(apollo_config.gdrive_token);

	snprintf(form, sizeof(form), "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token", cid, csecret, token);
	status = gd_post_form(GD_OAUTH_TOKEN_URL, form, &resp);

	if (status != 200 || !json_get_str(resp.data, "access_token", gd_access_token, sizeof(gd_access_token)))
	{
		LOG("gdrive: token refresh failed (%ld): %s", status, resp.data ? resp.data : "");
		gd_access_token[0] = 0;
	}

	curl_free(cid);
	curl_free(csecret);
	curl_free(token);
	free(resp.data);

	return (gd_access_token[0] != 0);
}

void gdrive_disconnect(void)
{
	memset(gd_access_token, 0, sizeof(gd_access_token));
	gd_folder_id[0] = 0;
}

/*
 * Find a file or folder by name inside `parent` ("root" for My Drive).
 * Returns 1 and fills id when found, 0 when not found, -1 on error.
 */
static int gd_find(const char* name, const char* parent, int folder, char* id, size_t size)
{
	char query[512], url[1024], *q;
	membuf_t resp = {0};
	long status;
	int ret = -1;

	snprintf(query, sizeof(query), "name='%s' and '%s' in parents and trashed=false%s", name, parent,
		folder ? " and mimeType='" GD_FOLDER_MIME "'" : "");

	q = url_escape(query);
	snprintf(url, sizeof(url), GD_API_URL "?q=%s&spaces=drive&fields=files(id)&pageSize=1", q);
	curl_free(q);

	status = gd_request("GET", url, NULL, NULL, 0, &resp, NULL);
	if (status == 200)
		ret = json_get_str(resp.data, "id", id, size) ? 1 : 0;
	else
		LOG("gdrive: search '%s' failed (%ld): %s", name, status, resp.data ? resp.data : "");

	free(resp.data);
	return ret;
}

static int gd_create_folder(const char* name, const char* parent, char* id, size_t size)
{
	char meta[512];
	membuf_t resp = {0};
	long status;
	int ok;

	snprintf(meta, sizeof(meta), "{\"name\":\"%s\",\"mimeType\":\"" GD_FOLDER_MIME "\",\"parents\":[\"%s\"]}", name, parent);
	status = gd_request("POST", GD_API_URL "?fields=id", "application/json", meta, strlen(meta), &resp, NULL);
	ok = (status == 200 && json_get_str(resp.data, "id", id, size));

	if (!ok)
		LOG("gdrive: create folder '%s' failed (%ld): %s", name, status, resp.data ? resp.data : "");

	free(resp.data);
	return ok;
}

int gdrive_open_folder(const char* path)
{
	char parent[128] = "root", id[128], name[128], buf[256];
	char* tok;

	sync_strlcpy(buf, path, sizeof(buf));
	for (tok = strtok(buf, "/"); tok; tok = strtok(NULL, "/"))
	{
		safe_name(tok, name, sizeof(name));

		int found = gd_find(name, parent, 1, id, sizeof(id));
		if (found < 0 || (!found && !gd_create_folder(name, parent, id, sizeof(id))))
			return 0;

		strcpy(parent, id);
	}

	sync_strlcpy(gd_folder_id, parent, sizeof(gd_folder_id));
	LOG("gdrive: folder '%s' = %s", path, gd_folder_id);
	return 1;
}

/* Returns 1 on success, 0 if the file doesn't exist, -1 on error. */
int gdrive_get(const char* name, const char* local_path)
{
	char id[128], url[512];
	long status;
	FILE* fp;
	int found;

	if ((found = gd_find(name, gd_folder_id, 0, id, sizeof(id))) <= 0)
		return found;

	if ((fp = fopen(local_path, "wb")) == NULL)
		return -1;

	snprintf(url, sizeof(url), GD_API_URL "/%s?alt=media", id);
	status = gd_request("GET", url, NULL, NULL, 0, NULL, fp);
	fclose(fp);

	if (status != 200)
	{
		LOG("gdrive: download '%s' failed (%ld)", name, status);
		unlink_secure(local_path);
		return -1;
	}

	return 1;
}

/* Create or replace `name` in the opened folder. Returns 1 on success. */
int gdrive_put(const char* local_path, const char* name)
{
	char id[128], meta[512], url[512], head[1024];
	static const char tail[] = "\r\n--" GD_BOUNDARY "--\r\n";
	uint8_t *data, *body;
	size_t data_len, head_len, body_len;
	membuf_t resp = {0};
	long status;
	int found;

	if ((found = gd_find(name, gd_folder_id, 0, id, sizeof(id))) < 0)
		return 0;

	if (read_buffer(local_path, &data, &data_len) != SUCCESS)
		return 0;

	// multipart/related: JSON metadata part + file content part
	if (found)
		snprintf(meta, sizeof(meta), "{}");
	else
		snprintf(meta, sizeof(meta), "{\"name\":\"%s\",\"parents\":[\"%s\"]}", name, gd_folder_id);

	head_len = snprintf(head, sizeof(head),
		"--" GD_BOUNDARY "\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n%s\r\n"
		"--" GD_BOUNDARY "\r\nContent-Type: application/octet-stream\r\n\r\n", meta);

	body_len = head_len + data_len + sizeof(tail) - 1;
	if ((body = malloc(body_len)) == NULL)
	{
		free(data);
		return 0;
	}

	memcpy(body, head, head_len);
	memcpy(body + head_len, data, data_len);
	memcpy(body + head_len + data_len, tail, sizeof(tail) - 1);
	free(data);

	if (found)
		snprintf(url, sizeof(url), GD_UPLOAD_URL "/%s?uploadType=multipart&fields=id", id);
	else
		snprintf(url, sizeof(url), GD_UPLOAD_URL "?uploadType=multipart&fields=id");

	status = gd_request(found ? "PATCH" : "POST", url, "multipart/related; boundary=" GD_BOUNDARY, body, body_len, &resp, NULL);
	free(body);

	if (status != 200)
		LOG("gdrive: upload '%s' failed (%ld): %s", name, status, resp.data ? resp.data : "");

	free(resp.data);
	return (status == 200);
}

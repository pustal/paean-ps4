/*
 * sync_common.c - shared pieces of the Paean save sync engine.
 *
 * Everything here is transport agnostic: listing the local HDD saves,
 * persisting the per-endpoint sync state, deciding what has to move in which
 * direction, and turning a save into a zip (and back) for the current user.
 * The LAN (sync_lan.c) and cloud (sync_cloud.c) modules only move zips and
 * save lists around.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include <orbis/SaveData.h>
#include <orbis/SystemService.h>
#include <orbis/UserService.h>
#include <mini18n.h>

#include "saves.h"
#include "menu.h"
#include "common.h"
#include "sfo.h"
#include "util.h"
#include "paean_sync.h"

/* Mount roots are created as APOLLO_SANDBOX_PATH "<dir_name>/". */
#define SYNC_MOUNT_ROOT     APOLLO_PATH "mount"

/* Saves that must never be synced: per-console app settings. */
static const char* sync_excluded_titles[] = {
    PAEAN_TITLE_ID,         /* Paean's own settings */
    "APOL00004",            /* Apollo Save Tool settings */
    NULL
};

/* ---------------------------------------------------------------------- */
/* String helpers                                                          */
/* ---------------------------------------------------------------------- */

/* strlcpy() that also accepts NULL sources (copied as an empty string). */
void sync_strlcpy(char* dst, const char* src, size_t size)
{
	if (!size)
		return;

	strncpy(dst, src ? src : "", size - 1);
	dst[size - 1] = 0;
}

/* Replace tabs / line breaks so a value can be stored in a TSV line. */
static void sanitize_field(char* s)
{
	for (; *s; s++)
		if (*s == '\t' || *s == '\r' || *s == '\n')
			*s = ' ';
}

/* File name used for a save archive, e.g. "CUSA00001_SAVEDATA00.zip". */
void sync_archive_name(const sync_save_t* save, char* out, size_t size)
{
	snprintf(out, size, "%s_%s.zip", save->title_id, save->dir_name);
}

/* ---------------------------------------------------------------------- */
/* sync_list_t                                                             */
/* ---------------------------------------------------------------------- */

void sync_list_init(sync_list_t* list)
{
	memset(list, 0, sizeof(sync_list_t));
}

void sync_list_free(sync_list_t* list)
{
	free(list->items);
	sync_list_init(list);
}

/* Append a copy of `save`, or overwrite the entry with the same identity. */
sync_save_t* sync_list_add(sync_list_t* list, const sync_save_t* save)
{
	sync_save_t* item = sync_list_find(list, save->title_id, save->dir_name);

	if (!item)
	{
		if (list->count == list->cap)
		{
			int cap = list->cap ? list->cap * 2 : 32;
			sync_save_t* tmp = realloc(list->items, cap * sizeof(sync_save_t));

			if (!tmp)
				return NULL;

			list->items = tmp;
			list->cap = cap;
		}
		item = &list->items[list->count++];
	}

	memcpy(item, save, sizeof(sync_save_t));
	return item;
}

/* Saves are identified by title id + save folder name. */
sync_save_t* sync_list_find(const sync_list_t* list, const char* title_id, const char* dir_name)
{
	for (int i = 0; i < list->count; i++)
		if (strcmp(list->items[i].title_id, title_id) == 0 && strcmp(list->items[i].dir_name, dir_name) == 0)
			return &list->items[i];

	return NULL;
}

void sync_list_remove(sync_list_t* list, const char* title_id, const char* dir_name)
{
	sync_save_t* item = sync_list_find(list, title_id, dir_name);

	if (!item)
		return;

	// keep the array compact; order is not significant
	*item = list->items[--list->count];
}

/* "TITLE\tDIR\tMTIME\tBLOCKS\tNAME" */
void sync_list_format_line(const sync_save_t* save, char* out, size_t size)
{
	snprintf(out, size, "%s\t%s\t%s\t%u\t%s", save->title_id, save->dir_name, save->mtime, save->blocks, save->name);
}

/* Parse a line written by sync_list_format_line(). Modifies `line`. */
int sync_list_parse_line(char* line, sync_save_t* out)
{
	char* field[5] = {0};
	int n = 0;

	memset(out, 0, sizeof(sync_save_t));
	line[strcspn(line, "\r\n")] = 0;

	if (!line[0] || line[0] == ';' || line[0] == '#')
		return 0;

	for (char* p = line; n < 5; n++)
	{
		field[n] = p;
		if (n == 4 || (p = strchr(p, '\t')) == NULL)
			break;
		*p++ = 0;
	}

	// title id and folder are mandatory, everything else is optional
	if (!field[0] || !field[1] || !field[0][0] || !field[1][0])
		return 0;

	sync_strlcpy(out->title_id, field[0], sizeof(out->title_id));
	sync_strlcpy(out->dir_name, field[1], sizeof(out->dir_name));
	sync_strlcpy(out->mtime, field[2], sizeof(out->mtime));
	out->blocks = field[3] ? (uint32_t) strtoul(field[3], NULL, 10) : 0;
	sync_strlcpy(out->name, field[4], sizeof(out->name));

	return 1;
}

int sync_list_write(const sync_list_t* list, const char* path)
{
	char line[512];
	FILE* fp = fopen(path, "w");

	if (!fp)
	{
		LOG("sync: can't write %s", path);
		return 0;
	}

	fprintf(fp, "; " PAEAN_NAME " v" PAEAN_VERSION " save list\n");
	for (int i = 0; i < list->count; i++)
	{
		sync_list_format_line(&list->items[i], line, sizeof(line));
		fprintf(fp, "%s\n", line);
	}
	fclose(fp);

	return 1;
}

/* Read a list file. A missing file is not an error (empty list). */
int sync_list_read(sync_list_t* list, const char* path)
{
	char line[512];
	sync_save_t save;
	FILE* fp = fopen(path, "r");

	if (!fp)
		return 0;

	while (fgets(line, sizeof(line), fp))
		if (sync_list_parse_line(line, &save))
			sync_list_add(list, &save);

	fclose(fp);
	return list->count;
}

/* ---------------------------------------------------------------------- */
/* Local saves                                                             */
/* ---------------------------------------------------------------------- */

static int is_excluded(const char* title_id)
{
	for (int i = 0; sync_excluded_titles[i]; i++)
		if (strcmp(title_id, sync_excluded_titles[i]) == 0)
			return 1;

	return 0;
}

static const char* col_text(sqlite3_stmt* res, int col)
{
	const char* s = (const char*) sqlite3_column_text(res, col);
	return s ? s : "";
}

/* List the current user's HDD saves straight from savedata.db. */
int sync_list_local(sync_list_t* list)
{
	char path[256];
	sqlite3_stmt* res;
	sqlite3* db;
	sqlite3* appdb;
	sync_save_t save;

	snprintf(path, sizeof(path), SAVES_DB_PATH, apollo_config.user_id);
	if ((db = open_sqlite_db(path)) == NULL)
		return 0;

	if (sqlite3_prepare_v2(db, "SELECT title_id, dir_name, main_title, blocks, mtime FROM savedata", -1, &res, NULL) != SQLITE_OK)
	{
		LOG("sync: savedata query failed: %s", sqlite3_errmsg(db));
		sqlite3_close(db);
		return 0;
	}

	appdb = open_sqlite_db(APP_DB_PATH_HDD);
	while (sqlite3_step(res) == SQLITE_ROW)
	{
		memset(&save, 0, sizeof(save));
		sync_strlcpy(save.title_id, col_text(res, 0), sizeof(save.title_id));
		sync_strlcpy(save.dir_name, col_text(res, 1), sizeof(save.dir_name));
		sync_strlcpy(save.name, col_text(res, 2), sizeof(save.name));
		save.blocks = (uint32_t) sqlite3_column_int(res, 3);
		sync_strlcpy(save.mtime, col_text(res, 4), sizeof(save.mtime));

		if (!save.title_id[0] || !save.dir_name[0] || is_excluded(save.title_id))
			continue;

		// prefer the installed game's name over the save's main title
		get_appdb_title(appdb, save.title_id, save.name);
		sanitize_field(save.name);
		sync_list_add(list, &save);
	}

	if (appdb)
		sqlite3_close(appdb);
	sqlite3_finalize(res);
	sqlite3_close(db);

	LOG("sync: %d local saves", list->count);
	return list->count;
}

/* Blocks of a local save, or 0 if it doesn't exist. */
static uint32_t local_save_blocks(const char* title_id, const char* dir_name)
{
	sync_list_t list;
	sync_save_t* item;
	uint32_t blocks = 0;

	sync_list_init(&list);
	sync_list_local(&list);
	if ((item = sync_list_find(&list, title_id, dir_name)) != NULL)
		blocks = item->blocks;

	sync_list_free(&list);
	return blocks;
}

/* Write `mtime` into savedata.db so both consoles agree on the save's age. */
static int set_local_mtime(const sync_save_t* save, const char* mtime)
{
	char path[256];
	char* query;
	sqlite3* db;
	int ret;

	snprintf(path, sizeof(path), SAVES_DB_PATH, apollo_config.user_id);
	if ((db = open_sqlite_db(path)) == NULL)
		return 0;

	query = sqlite3_mprintf("UPDATE savedata SET mtime=%Q WHERE (title_id=%Q AND dir_name=%Q);",
		mtime, save->title_id, save->dir_name);

	ret = (sqlite3_exec(db, query, NULL, NULL, NULL) == SQLITE_OK);
	if (ret)
		ret = save_sqlite_db(db, path);
	else
		LOG("sync: mtime update failed: %s", sqlite3_errmsg(db));

	sqlite3_free(query);
	sqlite3_close(db);
	return ret;
}

/* ---------------------------------------------------------------------- */
/* Sync state                                                              */
/* ---------------------------------------------------------------------- */

static void state_path(const char* endpoint, char* out, size_t size)
{
	char safe[64];

	// endpoint ids end up in a file name: keep [A-Za-z0-9_-] only
	sync_strlcpy(safe, endpoint, sizeof(safe));
	for (char* p = safe; *p; p++)
		if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '_'))
			*p = '_';

	snprintf(out, size, SYNC_STATE_PATH "%08x_%s.txt", apollo_config.user_id, safe);
}

int sync_state_load(const char* endpoint, sync_list_t* state)
{
	char path[256];

	state_path(endpoint, path, sizeof(path));
	return sync_list_read(state, path);
}

int sync_state_save(const char* endpoint, const sync_list_t* state)
{
	char path[256];

	sync_init_dirs();
	state_path(endpoint, path, sizeof(path));
	return sync_list_write(state, path);
}

/* Remember that `save` was in sync at `mtime`. */
void sync_state_set(sync_list_t* state, const sync_save_t* save, const char* mtime)
{
	sync_save_t entry;

	memcpy(&entry, save, sizeof(entry));
	sync_strlcpy(entry.mtime, mtime, sizeof(entry.mtime));
	sync_list_add(state, &entry);
}

/* ---------------------------------------------------------------------- */
/* Planning                                                                */
/* ---------------------------------------------------------------------- */

int sync_decide(const sync_save_t* local, const sync_save_t* remote, const sync_save_t* state, int* conflict)
{
	int local_changed, remote_changed;

	*conflict = 0;

	if (!local && !remote)
		return SYNC_ACT_NONE;

	// Deletions are never propagated: a save missing on one side is copied.
	if (!remote)
		return SYNC_ACT_PUSH;

	if (!local)
		return SYNC_ACT_PULL;

	if (strcmp(local->mtime, remote->mtime) == 0)
		return SYNC_ACT_NONE;

	local_changed  = !state || strcmp(local->mtime, state->mtime) != 0;
	remote_changed = !state || strcmp(remote->mtime, state->mtime) != 0;

	if (local_changed && !remote_changed)
		return SYNC_ACT_PUSH;

	if (remote_changed && !local_changed)
		return SYNC_ACT_PULL;

	// Both changed (or first sync): newer wins. ISO-8601 sorts as text.
	*conflict = 1;
	return (strcmp(local->mtime, remote->mtime) > 0) ? SYNC_ACT_PUSH : SYNC_ACT_PULL;
}

static int matches_filter(const sync_save_t* s, const char* title_id, const char* dir_name)
{
	if (!title_id || !dir_name)
		return 1;

	return (strcmp(s->title_id, title_id) == 0 && strcmp(s->dir_name, dir_name) == 0);
}

static int plan_add(sync_op_t** ops, int* count, int* cap, const sync_save_t* save, int action, int conflict)
{
	if (*count == *cap)
	{
		int ncap = *cap ? *cap * 2 : 16;
		sync_op_t* tmp = realloc(*ops, ncap * sizeof(sync_op_t));

		if (!tmp)
			return 0;

		*ops = tmp;
		*cap = ncap;
	}

	memcpy(&(*ops)[*count].save, save, sizeof(sync_save_t));
	(*ops)[*count].action = action;
	(*ops)[*count].conflict = conflict;
	(*count)++;

	return 1;
}

int sync_build_plan(const sync_list_t* local, const sync_list_t* remote, const sync_list_t* state,
                    const char* title_id, const char* dir_name, sync_op_t** ops)
{
	int count = 0, cap = 0, act, conflict;
	const sync_save_t *l, *r, *st;

	*ops = NULL;

	// every local save (present remotely or not)
	for (int i = 0; i < local->count; i++)
	{
		l = &local->items[i];
		if (!matches_filter(l, title_id, dir_name))
			continue;

		r = sync_list_find(remote, l->title_id, l->dir_name);
		st = sync_list_find(state, l->title_id, l->dir_name);
		act = sync_decide(l, r, st, &conflict);

		if (act == SYNC_ACT_PUSH)
			plan_add(ops, &count, &cap, l, act, conflict);
		else if (act == SYNC_ACT_PULL)
			plan_add(ops, &count, &cap, r, act, conflict);
	}

	// remote-only saves
	for (int i = 0; i < remote->count; i++)
	{
		r = &remote->items[i];
		if (!matches_filter(r, title_id, dir_name) || sync_list_find(local, r->title_id, r->dir_name) ||
			is_excluded(r->title_id))
			continue;

		plan_add(ops, &count, &cap, r, SYNC_ACT_PULL, 0);
	}

	return count;
}

int sync_confirm_plan(const char* target, const sync_op_t* ops, int count)
{
	int push = 0, pull = 0, conflicts = 0;

	for (int i = 0; i < count; i++)
	{
		(ops[i].action == SYNC_ACT_PUSH) ? push++ : pull++;
		conflicts += ops[i].conflict;
	}

	// a single save gets a more specific question
	if (count == 1)
		return show_dialog(DIALOG_TYPE_YESNO, "%s\n%s/%s\n\n%s %s%s\n\n%s",
			ops[0].save.name, ops[0].save.title_id, ops[0].save.dir_name,
			(ops[0].action == SYNC_ACT_PUSH) ? _("Send this PS4's copy to") : _("Get the copy from"), target,
			ops[0].conflict ? _("\n(both copies changed - the newer one wins)") : "",
			_("Continue?"));

	return show_dialog(DIALOG_TYPE_YESNO, "%s %s\n\n%s: %d\n%s: %d\n%s: %d\n\n%s",
		_("Sync with"), target,
		_("Saves to send"), push,
		_("Saves to receive"), pull,
		_("Changed on both sides (newer wins)"), conflicts,
		_("Continue?"));
}

/* ---------------------------------------------------------------------- */
/* Export / import                                                         */
/* ---------------------------------------------------------------------- */

void sync_init_dirs(void)
{
	mkdirs(SYNC_STATE_PATH);
	mkdirs(SYNC_BACKUP_PATH);
	mkdirs(SYNC_TMP_PATH);
}

/* Minimal save_entry_t for the orbis_Save* helpers (HDD save of this user). */
static void to_save_entry(const sync_save_t* s, save_entry_t* e)
{
	memset(e, 0, sizeof(save_entry_t));
	e->title_id = (char*) s->title_id;
	e->dir_name = (char*) s->dir_name;
	e->name = (char*) s->name;
	e->blocks = s->blocks;
	e->type = FILE_TYPE_PS4;
	e->flags = SAVE_FLAG_PS4 | SAVE_FLAG_HDD;
}

int sync_export_save(const sync_save_t* save, const char* zip_path)
{
	save_entry_t entry;
	char mount[ORBIS_SAVE_DATA_DIRNAME_DATA_MAXSIZE];
	char path[256];
	int ret;

	to_save_entry(save, &entry);
	if (!orbis_SaveMount(&entry, ORBIS_SAVE_DATA_MOUNT_MODE_RDONLY, mount))
	{
		LOG("sync: can't mount %s/%s", save->title_id, save->dir_name);
		return 0;
	}

	// zip entries are stored as "<dir_name>/..." (relative to the mount root)
	snprintf(path, sizeof(path), APOLLO_SANDBOX_PATH, mount);
	unlink_secure(zip_path);
	ret = zip_directory(SYNC_MOUNT_ROOT, path, zip_path);

	orbis_SaveUmount(mount);
	LOG("sync: exported %s/%s -> %s (%d)", save->title_id, save->dir_name, zip_path, ret);

	return ret;
}

/* Keep the last SYNC_BACKUP_KEEP copies: name.1.zip is the newest. */
static void backup_local_save(const sync_save_t* save)
{
	char src[256], dst[256];

	sync_init_dirs();
	for (int i = SYNC_BACKUP_KEEP; i > 1; i--)
	{
		snprintf(src, sizeof(src), SYNC_BACKUP_PATH "%s_%s.%d.zip", save->title_id, save->dir_name, i - 1);
		snprintf(dst, sizeof(dst), SYNC_BACKUP_PATH "%s_%s.%d.zip", save->title_id, save->dir_name, i);
		unlink_secure(dst);
		rename(src, dst);
	}

	snprintf(dst, sizeof(dst), SYNC_BACKUP_PATH "%s_%s.1.zip", save->title_id, save->dir_name);
	if (!sync_export_save(save, dst))
		LOG("sync: backup of %s/%s failed", save->title_id, save->dir_name);
}

/* Same as exec_cmd.c:_update_save_details() - refresh XMB title and icon. */
static void update_save_details(const save_entry_t* entry, const char* sys_path)
{
	char file_path[256];
	uint8_t* icon;
	size_t icon_size;
	sfo_context_t* sfo = sfo_alloc();

	snprintf(file_path, sizeof(file_path), "%sparam.sfo", sys_path);
	if (sfo_read(sfo, file_path) >= 0)
	{
		uint32_t* list_param = (uint32_t*) sfo_get_param_value(sfo, "SAVEDATA_LIST_PARAM");

		orbis_UpdateSaveParams(entry,
			(char*) sfo_get_param_value(sfo, "MAINTITLE"),
			(char*) sfo_get_param_value(sfo, "SUBTITLE"),
			(char*) sfo_get_param_value(sfo, "DETAIL"),
			list_param ? *list_param : 0);
	}
	sfo_free(sfo);

	snprintf(file_path, sizeof(file_path), "%sicon0.png", sys_path);
	if (read_buffer(file_path, &icon, &icon_size) == SUCCESS)
	{
		snprintf(file_path, sizeof(file_path), SAVE_ICON_PATH_HDD "%s/%s_icon0.png", apollo_config.user_id, entry->title_id, entry->dir_name);
		mkdirs(file_path);
		write_buffer(file_path, icon, icon_size);
		free(icon);
	}
}

int sync_import_save(const sync_save_t* save, const char* zip_path)
{
	save_entry_t entry;
	char mount[ORBIS_SAVE_DATA_DIRNAME_DATA_MAXSIZE];
	char path[256];
	uint32_t local_blocks;
	uint32_t mode = ORBIS_SAVE_DATA_MOUNT_MODE_RDWR;
	sfo_patch_t patch = {
		.user_id = apollo_config.user_id,
		.account_id = apollo_config.account_id,
		.psid = (uint8_t*) apollo_config.psid,
	};

	to_save_entry(save, &entry);
	local_blocks = local_save_blocks(save->title_id, save->dir_name);

	if (local_blocks)
	{
		backup_local_save(save);

		// the existing image is too small for the incoming data: recreate it
		if (local_blocks < save->blocks)
		{
			LOG("sync: growing %s/%s (%u -> %u blocks)", save->title_id, save->dir_name, local_blocks, save->blocks);
			if (!orbis_SaveDelete(&entry))
				return 0;
			local_blocks = 0;
		}
	}

	if (!local_blocks)
		mode |= ORBIS_SAVE_DATA_MOUNT_MODE_CREATE2 | ORBIS_SAVE_DATA_MOUNT_MODE_COPY_ICON;

	if (!entry.blocks)
		entry.blocks = ORBIS_SAVE_DATA_BLOCKS_MIN2;

	if (!orbis_SaveMount(&entry, mode, mount))
	{
		LOG("sync: can't mount %s/%s for writing", save->title_id, save->dir_name);
		return 0;
	}

	// replace the whole contents so files removed by the game don't linger
	snprintf(path, sizeof(path), APOLLO_SANDBOX_PATH, mount);
	clean_directory(path, "");

	if (!extract_zip(zip_path, SYNC_MOUNT_ROOT "/"))
	{
		LOG("sync: extract failed for %s", zip_path);
		orbis_SaveUmount(mount);
		return 0;
	}

	// re-sign for this console's user and refresh the XMB metadata
	snprintf(path, sizeof(path), APOLLO_SANDBOX_PATH "sce_sys/param.sfo", mount);
	patch_sfo(path, &patch);

	snprintf(path, sizeof(path), APOLLO_SANDBOX_PATH "sce_sys/", mount);
	update_save_details(&entry, path);
	orbis_SaveUmount(mount);

	// adopt the sender's timestamp: equal mtimes mean "in sync"
	if (save->mtime[0])
		set_local_mtime(save, save->mtime);

	LOG("sync: imported %s/%s (%s)", save->title_id, save->dir_name, save->mtime);
	return 1;
}

/* ---------------------------------------------------------------------- */
/* Console / user identity                                                 */
/* ---------------------------------------------------------------------- */

/* The OpenPSID is unique per console and stable across reboots. */
void sync_get_console_id(char* out, size_t size)
{
	snprintf(out, size, "%016lX%016lX", ES64(apollo_config.psid[0]), ES64(apollo_config.psid[1]));
}

/* The console name set in Settings > System, or a PSID based fallback. */
void sync_get_console_name(char* out, size_t size)
{
	char name[ORBIS_SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH] = {0};

	if (sceSystemServiceParamGetString(ORBIS_SYSTEM_SERVICE_PARAM_ID_SYSTEM_NAME, name, sizeof(name)) < 0 || !name[0])
		snprintf(name, sizeof(name), "PS4-%04lX", ES64(apollo_config.psid[1]) & 0xFFFF);

	sanitize_field(name);
	for (char* p = name; *p; p++)
		if (*p == '|')
			*p = '/';

	sync_strlcpy(out, name, size);
}

void sync_get_user_name(char* out, size_t size)
{
	char name[0x20] = {0};

	if (sceUserServiceGetUserName(apollo_config.user_id, name, sizeof(name)) < 0 || !name[0])
		snprintf(name, sizeof(name), "%08x", apollo_config.user_id);

	sanitize_field(name);
	for (char* p = name; *p; p++)
		if (*p == '|')
			*p = '/';

	sync_strlcpy(out, name, size);
}

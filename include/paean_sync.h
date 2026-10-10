/*
 * paean_sync.h - Paean save synchronisation (LAN peer-to-peer and cloud).
 *
 * Paean syncs the PS4 save games of the user that is currently running the
 * app with either:
 *
 *   - another PS4 on the same network that is running Paean in
 *     "receive mode" (found automatically with a UDP broadcast), or
 *   - a central "cloud" store: an FTP/FTPS server, a WebDAV share
 *     (http/https), or a Google Drive account.
 *
 * Saves always travel *decrypted* (as a zip of the mounted save folder) and are
 * re-encrypted and re-signed for the receiving user on import, so the two
 * consoles may use different users / account IDs.
 *
 * Change detection
 * ----------------
 * Every save on the HDD has a modification time (savedata.db `mtime`). When a
 * save is imported, Paean writes the *sender's* mtime into the receiver's
 * savedata.db, so after a sync both copies carry the same mtime. For every
 * endpoint (peer console or cloud profile) Paean also remembers the mtime each
 * save had the last time it was synced ("sync state"). With that:
 *
 *   local mtime == remote mtime              -> already in sync
 *   only local differs from the sync state   -> push local to remote
 *   only remote differs from the sync state  -> pull remote to local
 *   both differ (or never synced before)     -> conflict: newer mtime wins
 *
 * Whenever a local save is about to be overwritten, its current contents are
 * zipped to SYNC_BACKUP_PATH first (the last SYNC_BACKUP_KEEP copies are kept).
 */

#ifndef _PAEAN_SYNC_H_
#define _PAEAN_SYNC_H_

#include <stdint.h>
#include <stddef.h>

/* saves.h has no include guard: include it before this header. */

/* Bump when the LAN wire protocol changes in an incompatible way. */
#define SYNC_PROTO_VERSION      1

/* UDP port used to discover peers, TCP port used to transfer saves. */
#define SYNC_UDP_PORT           47231
#define SYNC_TCP_PORT           47232

/* Local folders used by the sync engine. */
#define SYNC_PATH               APOLLO_PATH "sync/"
#define SYNC_STATE_PATH         SYNC_PATH "state/"
#define SYNC_BACKUP_PATH        SYNC_PATH "backup/"
#define SYNC_TMP_PATH           SYNC_PATH "tmp/"

/* Number of rotating backups kept per save before it is overwritten. */
#define SYNC_BACKUP_KEEP        3

/* Name of the index file kept in the cloud store. */
#define SYNC_MANIFEST_FILE      "manifest.txt"

/* Optional file with Google OAuth client credentials (see README). */
#define SYNC_GDRIVE_CLIENT_FILE APOLLO_PATH "gdrive.txt"

/* Values of apollo_config.cloud_type */
enum sync_cloud_type
{
    SYNC_CLOUD_DISABLED,
    SYNC_CLOUD_URL,         /* ftp://, ftps://, http:// or https:// (WebDAV) */
    SYNC_CLOUD_GDRIVE,      /* Google Drive (OAuth device flow) */
};

/* What to do with one save after comparing both sides. */
enum sync_action
{
    SYNC_ACT_NONE,          /* both sides already match */
    SYNC_ACT_PUSH,          /* copy local -> remote */
    SYNC_ACT_PULL,          /* copy remote -> local */
};

/*
 * Description of one PS4 save, as exchanged between endpoints.
 * Field sizes follow the PS4 limits (title id = 9 chars, dir name <= 31).
 */
typedef struct
{
    char     title_id[0x10];
    char     dir_name[0x20];
    char     mtime[0x20];   /* ISO-8601 text as stored in savedata.db */
    uint32_t blocks;        /* size of the save image, in 32 KiB blocks */
    char     name[0x80];    /* human readable title, for messages only */
} sync_save_t;

/* A growable array of sync_save_t. */
typedef struct
{
    sync_save_t* items;
    int          count;
    int          cap;
} sync_list_t;

/* One entry of a sync plan: what to do with a save and why. */
typedef struct
{
    sync_save_t save;       /* the copy that will be transferred */
    int         action;     /* enum sync_action */
    int         conflict;   /* 1 if both sides changed since the last sync */
} sync_op_t;

/* ---------------------------------------------------------------------- */
/* sync_common.c - lists, state, plan and save import/export               */
/* ---------------------------------------------------------------------- */

void         sync_list_init(sync_list_t* list);
void         sync_list_free(sync_list_t* list);
sync_save_t* sync_list_add(sync_list_t* list, const sync_save_t* save);
sync_save_t* sync_list_find(const sync_list_t* list, const char* title_id, const char* dir_name);
void         sync_list_remove(sync_list_t* list, const char* title_id, const char* dir_name);

/* Serialise / parse a list as tab separated text (one save per line). */
int          sync_list_write(const sync_list_t* list, const char* path);
int          sync_list_read(sync_list_t* list, const char* path);
int          sync_list_parse_line(char* line, sync_save_t* out);
void         sync_list_format_line(const sync_save_t* save, char* out, size_t size);

/* Fill `list` with the HDD saves of the current user. Returns the count. */
int          sync_list_local(sync_list_t* list);

/* Sync state: last synced mtime of every save, per endpoint id. */
int          sync_state_load(const char* endpoint, sync_list_t* state);
int          sync_state_save(const char* endpoint, const sync_list_t* state);
void         sync_state_set(sync_list_t* state, const sync_save_t* save, const char* mtime);

/*
 * Compare one save on both sides and decide what to do.
 * Any of the three pointers may be NULL (save missing on that side / never
 * synced). Sets *conflict when both sides changed since the last sync; the
 * newer mtime wins in that case.
 */
int          sync_decide(const sync_save_t* local, const sync_save_t* remote, const sync_save_t* state, int* conflict);

/*
 * Build the list of operations needed to sync `local` with `remote`.
 * When title_id/dir_name are given only that save is considered.
 * Returns the number of operations (NONE entries are not included).
 */
int          sync_build_plan(const sync_list_t* local, const sync_list_t* remote, const sync_list_t* state,
                             const char* title_id, const char* dir_name, sync_op_t** ops);

/* Ask the user to confirm a plan. Returns 1 to go ahead. */
int          sync_confirm_plan(const char* target, const sync_op_t* ops, int count);

/* Zip the decrypted contents of a local HDD save. Returns 1 on success. */
int          sync_export_save(const sync_save_t* save, const char* zip_path);

/*
 * Import a zip produced by sync_export_save() as a save of the current user:
 * backs up any existing copy, (re)creates the save, extracts the files,
 * re-signs param.sfo for this user and sets the save mtime to save->mtime.
 * Returns 1 on success.
 */
int          sync_import_save(const sync_save_t* save, const char* zip_path);

/* Id of this console (hex PSID) and a friendly name for messages. */
void         sync_get_console_id(char* out, size_t size);
void         sync_get_console_name(char* out, size_t size);
void         sync_get_user_name(char* out, size_t size);

/* Make sure the sync folders exist. */
void         sync_init_dirs(void);

/* Small string helpers shared by the sync modules. */
void         sync_strlcpy(char* dst, const char* src, size_t size);
void         sync_archive_name(const sync_save_t* save, char* out, size_t size);

/* ---------------------------------------------------------------------- */
/* sync_lan.c - peer discovery and transfer over the local network         */
/* ---------------------------------------------------------------------- */

/*
 * Run the LAN server until the user presses OK.
 * Returns 1 if a peer changed any local save (lists need a reload).
 */
int          lan_receive_mode(void);

/*
 * Find a peer on the network and sync with it. With title_id/dir_name set
 * only that save is synced, otherwise every save of the current user.
 * Returns 1 if any local save was changed (lists need a reload).
 */
int          lan_sync(const char* title_id, const char* dir_name);

/* ---------------------------------------------------------------------- */
/* sync_cloud.c - sync against FTP / WebDAV / Google Drive                 */
/* ---------------------------------------------------------------------- */

/* 1 when a cloud backend is configured. */
int          cloud_is_configured(void);

/* Same contract as lan_sync(), against the configured cloud store. */
int          cloud_sync(const char* title_id, const char* dir_name);

/* Settings callbacks: ask for the URL / profile and test the connection. */
void         cloud_set_url(void);
void         cloud_set_profile(void);

/* ---------------------------------------------------------------------- */
/* gdrive.c - minimal Google Drive v3 client                               */
/* ---------------------------------------------------------------------- */

/* Interactive OAuth "device flow" login. Returns 1 when linked. */
int          gdrive_link_account(void);

/* Obtain a fresh access token from the stored refresh token. */
int          gdrive_connect(void);
void         gdrive_disconnect(void);

/* Find (or create) a folder by path below "My Drive", e.g. "Paean/default". */
int          gdrive_open_folder(const char* path);

/*
 * Download / upload a file by name in the opened folder.
 * gdrive_get() returns 1 on success, 0 if the file doesn't exist, -1 on error.
 * gdrive_put() creates or replaces the file and returns 1 on success.
 */
int          gdrive_get(const char* name, const char* local_path);
int          gdrive_put(const char* local_path, const char* name);

#endif /* _PAEAN_SYNC_H_ */

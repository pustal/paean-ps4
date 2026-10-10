#define PAEAN_VERSION           "1.0.0"     //Paean version (about menu, update check)
#define PAEAN_NAME              "Paean"     //Application name shown to the user
#define APOLLO_VERSION          "2.3.2"     //Apollo PS4 version Paean is based on
#define APOLLO_PLATFORM         "PS4"       //Apollo platform

#define MENU_TITLE_OFF          45          //Offset of menu title text from menu mini icon
#define MENU_ICON_OFF           105         //X Offset to start printing menu mini icon
#define MENU_ANI_MAX            0x80        //Max animation number
#define MENU_SPLIT_OFF          400         //Offset from left of sub/split menu to start drawing
#define OPTION_ITEM_OFF         1495        //Offset from left of settings item/value

enum app_option_type
{
    APP_OPTION_NONE,
    APP_OPTION_BOOL,
    APP_OPTION_LIST,
    APP_OPTION_INC,
    APP_OPTION_CALL,
};

typedef struct
{
    const char * name;
    const char * * options;
    enum app_option_type type;
    uint8_t spacer;
    uint8_t * value;
    void(*callback)(int);
} menu_option_t;

typedef struct
{
    char app_name[8];
    char app_ver[8];
    uint8_t music;
    uint8_t doSort;
    uint8_t doAni;
    uint8_t update;
    uint8_t usb_dev;
    uint8_t online_opt;
    uint8_t dbglog;
    uint32_t user_id;
    uint64_t psid[2];
    uint64_t account_id;
    char save_db[256];
    char ftp_url[512];
    /* Paean save sync settings (see include/paean_sync.h) */
    uint8_t cloud_type;             /* enum sync_cloud_type */
    char sync_profile[32];          /* cloud folder shared by both consoles */
    char cloud_url[512];            /* ftp(s):// or http(s):// WebDAV base URL */
    char gdrive_client_id[128];     /* Google OAuth client id */
    char gdrive_client_secret[64];  /* Google OAuth client secret */
    char gdrive_token[512];         /* Google OAuth refresh token */
} app_config_t;

extern menu_option_t menu_options[];
extern app_config_t apollo_config;

void music_callback(int sel);
void update_callback(int sel);

int get_firmware_version(void);
int save_xml_owner(const char *xmlfile);
int get_xml_owners(const char *xmlfile, int cmd, char*** nam, char*** val);

/*
 * sync_lan.c - sync saves directly between two PS4s on the same network.
 *
 * One console runs Paean in "receive mode" (lan_receive_mode), which starts:
 *
 *   - a UDP responder on SYNC_UDP_PORT that answers discovery broadcasts, and
 *   - a TCP server on SYNC_TCP_PORT that serves save lists and save archives.
 *
 * The other console runs lan_sync(): it broadcasts a discovery packet, picks
 * the peer, downloads its save list, builds a sync plan (see sync_common.c)
 * and then pushes / pulls the saves that changed. The initiating console
 * drives the whole session, so the receiving console only has to sit in
 * receive mode.
 *
 * Discovery (UDP, one datagram each way):
 *   -> "PAEAN?<proto>"
 *   <- "PAEAN!<proto>|<console id>|<console name>|<user name>|<tcp port>"
 *
 * Session (TCP, '\n' terminated text lines, binary payloads after headers):
 *   -> HELLO <proto>\t<client console id>\t<client console name>
 *   <- OK <server console id>\t<console name>\t<user name>
 *   -> LIST
 *   <- COUNT <n>              followed by n save lines (see sync_list_format_line)
 *   -> GET <save line>
 *   <- DATA <size>            followed by <size> bytes of zip, or ERR <msg>
 *   -> PUT <size>\t<save line> followed by <size> bytes of zip
 *   <- OK | ERR <msg>
 *   -> DONE <save line>       record that the save is in sync at its mtime
 *   <- OK
 *   -> BYE
 *
 * The protocol is unauthenticated and meant for a trusted home network only.
 */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <orbis/NetCtl.h>
#include <orbis/SystemService.h>
#include <mini18n.h>

#include "saves.h"
#include "menu.h"
#include "common.h"
#include "util.h"
#include "paean_sync.h"

#define LAN_LINE_MAX        1024
#define LAN_IO_BUFFER       (64 * 1024)
#define LAN_DISCOVERY_MS    2500        /* how long to wait for replies */
#define LAN_MAX_PEERS       8
#define LAN_CONNECT_SECS    5
#define LAN_IO_TIMEOUT_SECS 180         /* exporting a big save can take a while */
#define LAN_THREAD_STACK    (1024 * 1024)

/* A console found on the network. */
typedef struct
{
	char     id[48];
	char     name[ORBIS_SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH];
	char     user[0x20];
	char     ip[16];
	uint16_t port;
} lan_peer_t;

/* Shared between the receive-mode threads and the UI thread. */
static volatile int lan_server_stop = 0;
static volatile int lan_server_changed = 0;

/* ---------------------------------------------------------------------- */
/* Socket helpers                                                          */
/* ---------------------------------------------------------------------- */

static void sock_set_timeout(int fd, int seconds)
{
	struct timeval tv = { .tv_sec = seconds, .tv_usec = 0 };

	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

/* Wait until `fd` is readable or `ms` milliseconds pass. 1 = readable. */
static int sock_wait_readable(int fd, int ms)
{
	fd_set set;
	struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };

	FD_ZERO(&set);
	FD_SET(fd, &set);

	return (select(fd + 1, &set, NULL, NULL, &tv) > 0);
}

static int sock_send_all(int fd, const void* data, size_t len)
{
	const uint8_t* p = data;

	while (len)
	{
		ssize_t n = send(fd, p, len, 0);
		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			return 0;
		}
		p += n;
		len -= (size_t) n;
	}

	return 1;
}

static int sock_printf(int fd, const char* fmt, ...)
{
	char line[LAN_LINE_MAX];
	va_list args;
	int len;

	va_start(args, fmt);
	len = vsnprintf(line, sizeof(line) - 1, fmt, args);
	va_end(args);

	if (len < 0 || len >= (int) sizeof(line) - 1)
		return 0;

	line[len++] = '\n';
	return sock_send_all(fd, line, len);
}

/*
 * Read one '\n' terminated line (terminator stripped). Reads byte by byte so
 * that it never consumes the binary payload that may follow a header.
 */
static int sock_recv_line(int fd, char* line, size_t size)
{
	size_t len = 0;
	char c;

	while (len + 1 < size)
	{
		ssize_t n = recv(fd, &c, 1, 0);
		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			return 0;
		}

		if (c == '\n')
			break;

		if (c != '\r')
			line[len++] = c;
	}

	line[len] = 0;
	return 1;
}

/* Receive exactly `size` bytes into a file. */
static int sock_recv_file(int fd, const char* path, uint64_t size)
{
	uint8_t* buf;
	FILE* fp;
	int ok = 1;

	if ((fp = fopen(path, "wb")) == NULL)
		return 0;

	if ((buf = malloc(LAN_IO_BUFFER)) == NULL)
	{
		fclose(fp);
		return 0;
	}

	while (size && ok)
	{
		size_t want = (size > LAN_IO_BUFFER) ? LAN_IO_BUFFER : (size_t) size;
		ssize_t n = recv(fd, buf, want, 0);

		if (n <= 0)
		{
			if (n < 0 && errno == EINTR)
				continue;
			ok = 0;
			break;
		}

		ok = (fwrite(buf, 1, n, fp) == (size_t) n);
		size -= (uint64_t) n;
	}

	free(buf);
	fclose(fp);

	if (!ok)
		unlink_secure(path);

	return ok;
}

/* Send a whole file (the size was announced in the preceding header). */
static int sock_send_file(int fd, const char* path)
{
	uint8_t* buf;
	FILE* fp;
	size_t n;
	int ok = 1;

	if ((fp = fopen(path, "rb")) == NULL)
		return 0;

	if ((buf = malloc(LAN_IO_BUFFER)) == NULL)
	{
		fclose(fp);
		return 0;
	}

	while (ok && (n = fread(buf, 1, LAN_IO_BUFFER, fp)) > 0)
		ok = sock_send_all(fd, buf, n);

	free(buf);
	fclose(fp);
	return ok;
}

static uint64_t file_size(const char* path)
{
	uint64_t size = 0;

	return (get_file_size(path, &size) == SUCCESS) ? size : 0;
}

/* Our IPv4 address and the subnet broadcast address, from NetCtl. */
static int get_local_network(struct in_addr* ip, struct in_addr* bcast)
{
	OrbisNetCtlInfo info;
	struct in_addr mask;

	memset(&info, 0, sizeof(info));
	if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_IP_ADDRESS, &info) < 0 || inet_pton(AF_INET, info.ip_address, ip) != 1)
		return 0;

	memset(&info, 0, sizeof(info));
	if (sceNetCtlGetInfo(ORBIS_NET_CTL_INFO_NETMASK, &info) < 0 || inet_pton(AF_INET, info.netmask, &mask) != 1)
		mask.s_addr = htonl(0xFFFFFF00);

	bcast->s_addr = ip->s_addr | ~mask.s_addr;
	return 1;
}

/* ---------------------------------------------------------------------- */
/* Receive mode (server side)                                              */
/* ---------------------------------------------------------------------- */

/* Answer discovery broadcasts until lan_server_stop is set. */
static int lan_discovery_thread(void* arg)
{
	int fd = (int)(intptr_t) arg;
	char buf[256], reply[256], id[48], name[ORBIS_SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH], user[0x20];
	struct sockaddr_in from;
	socklen_t from_len;

	sync_get_console_id(id, sizeof(id));
	sync_get_console_name(name, sizeof(name));
	sync_get_user_name(user, sizeof(user));
	snprintf(reply, sizeof(reply), "PAEAN!%d|%s|%s|%s|%d", SYNC_PROTO_VERSION, id, name, user, SYNC_TCP_PORT);

	while (!lan_server_stop)
	{
		if (!sock_wait_readable(fd, 500))
			continue;

		from_len = sizeof(from);
		ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0, (struct sockaddr*) &from, &from_len);
		if (n <= 0)
			continue;

		buf[n] = 0;
		if (strncmp(buf, "PAEAN?", 6) != 0)
			continue;

		LOG("lan: discovery from %s", inet_ntoa(from.sin_addr));
		sendto(fd, reply, strlen(reply), 0, (struct sockaddr*) &from, from_len);
	}

	return 0;
}

/* Serve one client session. */
static void lan_serve_client(int fd, const char* client_ip)
{
	char line[LAN_LINE_MAX], endpoint[64] = "", id[48], name[ORBIS_SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH], user[0x20];
	char* arg;
	sync_list_t local, state;
	sync_save_t save;
	int have_state = 0;

	sync_list_init(&state);
	sock_set_timeout(fd, LAN_IO_TIMEOUT_SECS);

	while (!lan_server_stop && sock_recv_line(fd, line, sizeof(line)))
	{
		// split "<COMMAND> <args>"
		arg = strchr(line, ' ');
		if (arg)
			*arg++ = 0;
		else
			arg = line + strlen(line);

		if (strcmp(line, "HELLO") == 0)
		{
			// HELLO <proto>\t<console id>\t<console name>
			char* cid = strchr(arg, '\t');
			char* cname = cid ? strchr(cid + 1, '\t') : NULL;

			if (atoi(arg) != SYNC_PROTO_VERSION || !cid)
			{
				sock_printf(fd, "ERR protocol version mismatch (server %d)", SYNC_PROTO_VERSION);
				break;
			}

			*cid++ = 0;
			if (cname)
				*cname++ = 0;

			snprintf(endpoint, sizeof(endpoint), "lan_%s", cid);
			sync_list_free(&state);
			sync_state_load(endpoint, &state);
			have_state = 1;

			sync_get_console_id(id, sizeof(id));
			sync_get_console_name(name, sizeof(name));
			sync_get_user_name(user, sizeof(user));
			sock_printf(fd, "OK %s\t%s\t%s", id, name, user);

			notify_popup(NOTIFICATION_ICON_DEFAULT, "%s\n%s (%s)", _("LAN sync started by"), cname ? cname : cid, client_ip);
		}
		else if (!have_state)
		{
			sock_printf(fd, "ERR say HELLO first");
			break;
		}
		else if (strcmp(line, "LIST") == 0)
		{
			char entry[512];

			sync_list_init(&local);
			sync_list_local(&local);

			sock_printf(fd, "COUNT %d", local.count);
			for (int i = 0; i < local.count; i++)
			{
				sync_list_format_line(&local.items[i], entry, sizeof(entry));
				sock_printf(fd, "%s", entry);
			}
			sync_list_free(&local);
		}
		else if (strcmp(line, "GET") == 0)
		{
			if (!sync_list_parse_line(arg, &save) || !sync_export_save(&save, SYNC_TMP_PATH "out.zip"))
			{
				sock_printf(fd, "ERR can't export save");
				continue;
			}

			sock_printf(fd, "DATA %" PRIu64, file_size(SYNC_TMP_PATH "out.zip"));
			if (!sock_send_file(fd, SYNC_TMP_PATH "out.zip"))
				break;

			unlink_secure(SYNC_TMP_PATH "out.zip");
		}
		else if (strcmp(line, "PUT") == 0)
		{
			// PUT <size>\t<save line>
			char* meta = strchr(arg, '\t');
			uint64_t size = strtoull(arg, NULL, 10);

			if (!meta || !sync_list_parse_line(meta + 1, &save))
			{
				sock_printf(fd, "ERR bad PUT header");
				break;
			}

			if (!sock_recv_file(fd, SYNC_TMP_PATH "in.zip", size))
				break;

			if (sync_import_save(&save, SYNC_TMP_PATH "in.zip"))
			{
				lan_server_changed = 1;
				sock_printf(fd, "OK");
				notify_popup(NOTIFICATION_ICON_DEFAULT, "%s\n%s", _("Save received:"), save.name);
			}
			else
				sock_printf(fd, "ERR import failed");

			unlink_secure(SYNC_TMP_PATH "in.zip");
		}
		else if (strcmp(line, "DONE") == 0)
		{
			if (sync_list_parse_line(arg, &save))
				sync_state_set(&state, &save, save.mtime);
			sock_printf(fd, "OK");
		}
		else if (strcmp(line, "BYE") == 0)
			break;
		else
			sock_printf(fd, "ERR unknown command");
	}

	if (have_state)
		sync_state_save(endpoint, &state);

	sync_list_free(&state);
}

/* Accept clients (one at a time) until lan_server_stop is set. */
static int lan_server_thread(void* arg)
{
	int lfd = (int)(intptr_t) arg;
	struct sockaddr_in addr;
	socklen_t len;

	while (!lan_server_stop)
	{
		if (!sock_wait_readable(lfd, 500))
			continue;

		len = sizeof(addr);
		int cfd = accept(lfd, (struct sockaddr*) &addr, &len);
		if (cfd < 0)
			continue;

		LOG("lan: client connected from %s", inet_ntoa(addr.sin_addr));
		lan_serve_client(cfd, inet_ntoa(addr.sin_addr));
		close(cfd);
		LOG("lan: client disconnected");
	}

	return 0;
}

static int open_bound_socket(int type, uint16_t port)
{
	int yes = 1;
	struct sockaddr_in addr;
	int fd = socket(AF_INET, type, (type == SOCK_DGRAM) ? IPPROTO_UDP : IPPROTO_TCP);

	if (fd < 0)
		return -1;

	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0 || (type == SOCK_STREAM && listen(fd, 2) < 0))
	{
		LOG("lan: can't bind port %d (%d)", port, errno);
		close(fd);
		return -1;
	}

	return fd;
}

int lan_receive_mode(void)
{
	struct in_addr ip, bcast;
	SDL_Thread *udp_thread, *tcp_thread;
	char name[ORBIS_SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH];
	int ufd, tfd;

	sync_init_dirs();
	if (!get_local_network(&ip, &bcast))
	{
		show_message(_("Error! This PS4 is not connected to a network."));
		return 0;
	}

	ufd = open_bound_socket(SOCK_DGRAM, SYNC_UDP_PORT);
	tfd = open_bound_socket(SOCK_STREAM, SYNC_TCP_PORT);
	if (ufd < 0 || tfd < 0)
	{
		if (ufd >= 0) close(ufd);
		if (tfd >= 0) close(tfd);
		show_message(_("Error starting the LAN sync server!"));
		return 0;
	}

	lan_server_stop = 0;
	lan_server_changed = 0;
	// the server thread mounts, zips and imports saves: give it a roomy stack
	udp_thread = SDL_CreateThreadWithStackSize(lan_discovery_thread, "lan_discovery", LAN_THREAD_STACK / 4, (void*)(intptr_t) ufd);
	tcp_thread = SDL_CreateThreadWithStackSize(lan_server_thread, "lan_server", LAN_THREAD_STACK, (void*)(intptr_t) tfd);

	// The UI thread blocks on this dialog while the threads serve the peer.
	sync_get_console_name(name, sizeof(name));
	show_message("%s\n\n%s (%s:%d)\n\n%s\n%s",
		_("Waiting for another PS4 to sync with this one."), name, inet_ntoa(ip), SYNC_TCP_PORT,
		_("On the other PS4 choose \"Sync with LAN PS4\"."), _("Press OK to stop."));

	lan_server_stop = 1;
	SDL_WaitThread(udp_thread, NULL);
	SDL_WaitThread(tcp_thread, NULL);
	close(ufd);
	close(tfd);

	return lan_server_changed;
}

/* ---------------------------------------------------------------------- */
/* Initiator (client side)                                                 */
/* ---------------------------------------------------------------------- */

/* Parse "PAEAN!<proto>|<id>|<name>|<user>|<port>". */
static int parse_discovery_reply(char* msg, lan_peer_t* peer)
{
	char* field[5];
	int n = 0;

	if (strncmp(msg, "PAEAN!", 6) != 0)
		return 0;

	for (char* p = msg + 6; n < 5 && p; n++)
	{
		field[n] = p;
		if ((p = strchr(p, '|')) != NULL)
			*p++ = 0;
	}

	if (n < 5 || atoi(field[0]) != SYNC_PROTO_VERSION)
		return 0;

	sync_strlcpy(peer->id, field[1], sizeof(peer->id));
	sync_strlcpy(peer->name, field[2], sizeof(peer->name));
	sync_strlcpy(peer->user, field[3], sizeof(peer->user));
	peer->port = (uint16_t) atoi(field[4]);

	return (peer->id[0] && peer->port);
}

/* Broadcast a discovery request and collect replies. Returns peer count. */
static int lan_discover(lan_peer_t* peers, int max)
{
	struct in_addr ip, bcast;
	struct sockaddr_in to, from;
	socklen_t from_len;
	char buf[256], my_id[48];
	int fd, yes = 1, count = 0;
	uint32_t targets[2];

	if (!get_local_network(&ip, &bcast))
		return 0;

	if ((fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0)
		return 0;

	setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
	sync_get_console_id(my_id, sizeof(my_id));

	// subnet broadcast first, limited broadcast as a fallback
	targets[0] = bcast.s_addr;
	targets[1] = htonl(INADDR_BROADCAST);
	snprintf(buf, sizeof(buf), "PAEAN?%d", SYNC_PROTO_VERSION);

	for (int round = 0; round < 3; round++)
	{
		for (int t = 0; t < 2; t++)
		{
			memset(&to, 0, sizeof(to));
			to.sin_family = AF_INET;
			to.sin_port = htons(SYNC_UDP_PORT);
			to.sin_addr.s_addr = targets[t];
			sendto(fd, buf, strlen(buf), 0, (struct sockaddr*) &to, sizeof(to));
		}

		// collect replies for a third of the discovery window
		for (int waited = 0; waited < LAN_DISCOVERY_MS / 3; waited += 100)
		{
			char msg[256];
			lan_peer_t peer;
			int dup = 0;

			if (!sock_wait_readable(fd, 100))
				continue;

			from_len = sizeof(from);
			ssize_t n = recvfrom(fd, msg, sizeof(msg) - 1, 0, (struct sockaddr*) &from, &from_len);
			if (n <= 0)
				continue;

			msg[n] = 0;
			memset(&peer, 0, sizeof(peer));
			if (!parse_discovery_reply(msg, &peer) || strcmp(peer.id, my_id) == 0)
				continue;

			sync_strlcpy(peer.ip, inet_ntoa(from.sin_addr), sizeof(peer.ip));
			for (int i = 0; i < count; i++)
				dup |= (strcmp(peers[i].id, peer.id) == 0);

			if (!dup && count < max)
			{
				LOG("lan: found %s (%s) at %s", peer.name, peer.user, peer.ip);
				peers[count++] = peer;
			}
		}
	}

	close(fd);
	return count;
}

/* TCP connect with a timeout (a plain connect() can hang for minutes). */
static int lan_connect(const char* ip, uint16_t port)
{
	struct sockaddr_in addr;
	fd_set wset;
	struct timeval tv = { .tv_sec = LAN_CONNECT_SECS, .tv_usec = 0 };
	int fd, flags, err = 0;
	socklen_t len = sizeof(err);

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1)
		return -1;

	if ((fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
		return -1;

	flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	if (connect(fd, (struct sockaddr*) &addr, sizeof(addr)) < 0 && errno != EINPROGRESS)
	{
		close(fd);
		return -1;
	}

	FD_ZERO(&wset);
	FD_SET(fd, &wset);
	if (select(fd + 1, NULL, &wset, NULL, &tv) <= 0 ||
		getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err)
	{
		close(fd);
		return -1;
	}

	fcntl(fd, F_SETFL, flags);
	sock_set_timeout(fd, LAN_IO_TIMEOUT_SECS);
	return fd;
}

/* Let the user choose the peer (or type an IP if none answered). */
static int lan_choose_peer(lan_peer_t* out)
{
	lan_peer_t peers[LAN_MAX_PEERS];
	int count;

	init_loading_screen(_("Looking for PS4s running Paean..."));
	count = lan_discover(peers, LAN_MAX_PEERS);
	stop_loading_screen();

	if (count == 1)
	{
		*out = peers[0];
		return 1;
	}

	for (int i = 0; i < count; i++)
		if (show_dialog(DIALOG_TYPE_YESNO, "%s\n\n%s\n%s: %s\n%s", _("Sync with this PS4?"),
				peers[i].name, _("User"), peers[i].user, peers[i].ip))
		{
			*out = peers[i];
			return 1;
		}

	if (count)
		return 0;

	// Broadcasts can be filtered by some routers / mesh systems.
	if (!show_dialog(DIALOG_TYPE_YESNO, "%s\n\n%s", _("No PS4 running Paean receive mode was found."),
			_("Enter the other PS4's IP address manually?")))
		return 0;

	memset(out, 0, sizeof(lan_peer_t));
	strcpy(out->ip, "192.168.1.");
	if (!osk_dialog_get_text(_("IP address of the other PS4"), out->ip, sizeof(out->ip)))
		return 0;

	sync_strlcpy(out->name, out->ip, sizeof(out->name));
	out->port = SYNC_TCP_PORT;
	return 1;
}

/* Read "OK ..." / "ERR ..." replies. */
static int expect_ok(int fd)
{
	char line[LAN_LINE_MAX];

	if (!sock_recv_line(fd, line, sizeof(line)))
		return 0;

	if (strncmp(line, "OK", 2) != 0)
	{
		LOG("lan: peer replied '%s'", line);
		return 0;
	}

	return 1;
}

static int lan_get_remote_list(int fd, sync_list_t* remote)
{
	char line[LAN_LINE_MAX];
	sync_save_t save;
	int count;

	if (!sock_printf(fd, "LIST") || !sock_recv_line(fd, line, sizeof(line)) || strncmp(line, "COUNT ", 6) != 0)
		return 0;

	count = atoi(line + 6);
	for (int i = 0; i < count; i++)
	{
		if (!sock_recv_line(fd, line, sizeof(line)))
			return 0;

		if (sync_list_parse_line(line, &save))
			sync_list_add(remote, &save);
	}

	return 1;
}

static int lan_push(int fd, const sync_save_t* save)
{
	char meta[512];
	const char* zip = SYNC_TMP_PATH "push.zip";
	int ok;

	if (!sync_export_save(save, zip))
		return 0;

	sync_list_format_line(save, meta, sizeof(meta));
	ok = sock_printf(fd, "PUT %" PRIu64 "\t%s", file_size(zip), meta) && sock_send_file(fd, zip) && expect_ok(fd);
	unlink_secure(zip);

	return ok;
}

static int lan_pull(int fd, const sync_save_t* save)
{
	char line[LAN_LINE_MAX], meta[512];
	const char* zip = SYNC_TMP_PATH "pull.zip";
	int ok;

	sync_list_format_line(save, meta, sizeof(meta));
	if (!sock_printf(fd, "GET %s", meta) || !sock_recv_line(fd, line, sizeof(line)))
		return -1;

	// a failed export on the peer is not fatal for the session
	if (strncmp(line, "DATA ", 5) != 0)
	{
		LOG("lan: GET failed: %s", line);
		return 0;
	}

	if (!sock_recv_file(fd, zip, strtoull(line + 5, NULL, 10)))
		return -1;

	ok = sync_import_save(save, zip);
	unlink_secure(zip);

	return ok;
}

int lan_sync(const char* title_id, const char* dir_name)
{
	lan_peer_t peer;
	sync_list_t local, remote, state;
	sync_op_t* ops = NULL;
	char line[LAN_LINE_MAX], endpoint[64], my_id[48], my_name[ORBIS_SYSTEM_SERVICE_MAX_SYSTEM_NAME_LENGTH], target[128];
	int fd, count, done = 0, failed = 0, changed = 0;

	sync_init_dirs();
	if (!lan_choose_peer(&peer))
		return 0;

	if ((fd = lan_connect(peer.ip, peer.port ? peer.port : SYNC_TCP_PORT)) < 0)
	{
		show_message("%s\n%s", _("Error! Can't connect to"), peer.ip);
		return 0;
	}

	// handshake: learn the peer's id/name (needed when the IP was typed in)
	sync_get_console_id(my_id, sizeof(my_id));
	sync_get_console_name(my_name, sizeof(my_name));
	if (!sock_printf(fd, "HELLO %d\t%s\t%s", SYNC_PROTO_VERSION, my_id, my_name) ||
		!sock_recv_line(fd, line, sizeof(line)) || strncmp(line, "OK ", 3) != 0)
	{
		show_message("%s\n%s", _("Error! The other PS4 refused the connection:"), line);
		close(fd);
		return 0;
	}

	{
		char* name = strchr(line + 3, '\t');
		char* user = name ? strchr(name + 1, '\t') : NULL;

		if (name) *name++ = 0;
		if (user) *user++ = 0;
		sync_strlcpy(peer.id, line + 3, sizeof(peer.id));
		if (name) sync_strlcpy(peer.name, name, sizeof(peer.name));
		if (user) sync_strlcpy(peer.user, user, sizeof(peer.user));
	}

	snprintf(target, sizeof(target), "%s (%s)", peer.name, peer.user);
	snprintf(endpoint, sizeof(endpoint), "lan_%s", peer.id);

	sync_list_init(&local);
	sync_list_init(&remote);
	sync_list_init(&state);

	init_loading_screen(_("Comparing saves..."));
	sync_list_local(&local);
	count = lan_get_remote_list(fd, &remote);
	stop_loading_screen();

	if (!count)
	{
		show_message(_("Error! Couldn't read the save list from the other PS4."));
		goto end;
	}

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
		ok = (op->action == SYNC_ACT_PUSH) ? lan_push(fd, &op->save) : lan_pull(fd, &op->save);

		if (ok < 0)
		{
			failed += count - i;
			LOG("lan: connection lost");
			break;
		}

		if (!ok)
		{
			failed++;
			continue;
		}

		done++;
		changed |= (op->action == SYNC_ACT_PULL);

		// both copies now carry op->save.mtime: record it on both consoles
		sync_state_set(&state, &op->save, op->save.mtime);
		sync_list_format_line(&op->save, line, sizeof(line));
		if (!sock_printf(fd, "DONE %s", line) || !expect_ok(fd))
			LOG("lan: peer didn't confirm DONE for %s", op->save.dir_name);
	}
	update_progress_bar(count, count, "");
	end_progress_bar();

	// also remember saves that were already identical on both sides
	for (int i = 0; i < local.count; i++)
	{
		sync_save_t* r = sync_list_find(&remote, local.items[i].title_id, local.items[i].dir_name);
		sync_save_t* st;

		if (!r || strcmp(r->mtime, local.items[i].mtime) != 0)
			continue;

		st = sync_list_find(&state, r->title_id, r->dir_name);
		if (!st || strcmp(st->mtime, r->mtime) != 0)
		{
			sync_state_set(&state, r, r->mtime);
			sync_list_format_line(r, line, sizeof(line));
			if (!sock_printf(fd, "DONE %s", line) || !expect_ok(fd))
				break;
		}
	}

	sync_state_save(endpoint, &state);

	if (!count)
		show_message("%s\n%s", _("Everything is already in sync with"), target);
	else
		show_message("%s %s\n\n%s: %d\n%s: %d", _("Sync finished with"), target, _("Saves synced"), done, _("Failed"), failed);

end:
	sock_printf(fd, "BYE");
	close(fd);
	free(ops);
	sync_list_free(&local);
	sync_list_free(&remote);
	sync_list_free(&state);

	return changed;
}

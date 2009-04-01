/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>
#include <netinet/in.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>

#include <glib.h>
#include <dbus/dbus.h>

#include "logging.h"

#include "../src/adapter.h"
#include "../src/device.h"

#include "device.h"
#include "manager.h"
#include "control.h"
#include "avdtp.h"
#include "glib-helper.h"
#include "btio.h"
#include "sink.h"

#include <bluetooth/l2cap.h>

#define AVDTP_PSM 25

#define MAX_SEID 0x3E

#define AVDTP_DISCOVER				0x01
#define AVDTP_GET_CAPABILITIES			0x02
#define AVDTP_SET_CONFIGURATION			0x03
#define AVDTP_GET_CONFIGURATION			0x04
#define AVDTP_RECONFIGURE			0x05
#define AVDTP_OPEN				0x06
#define AVDTP_START				0x07
#define AVDTP_CLOSE				0x08
#define AVDTP_SUSPEND				0x09
#define AVDTP_ABORT				0x0A
#define AVDTP_SECURITY_CONTROL			0x0B

#define AVDTP_PKT_TYPE_SINGLE			0x00
#define AVDTP_PKT_TYPE_START			0x01
#define AVDTP_PKT_TYPE_CONTINUE			0x02
#define AVDTP_PKT_TYPE_END			0x03

#define AVDTP_MSG_TYPE_COMMAND			0x00
#define AVDTP_MSG_TYPE_ACCEPT			0x02
#define AVDTP_MSG_TYPE_REJECT			0x03

#define REQ_TIMEOUT 4
#define DISCONNECT_TIMEOUT 1
#define STREAM_TIMEOUT 20

#if __BYTE_ORDER == __LITTLE_ENDIAN

struct avdtp_common_header {
	uint8_t message_type:2;
	uint8_t packet_type:2;
	uint8_t transaction:4;
} __attribute__ ((packed));

struct avdtp_single_header {
	uint8_t message_type:2;
	uint8_t packet_type:2;
	uint8_t transaction:4;
	uint8_t signal_id:6;
	uint8_t rfa0:2;
} __attribute__ ((packed));

struct avdtp_start_header {
	uint8_t message_type:2;
	uint8_t packet_type:2;
	uint8_t transaction:4;
	uint8_t no_of_packets;
	uint8_t signal_id:6;
	uint8_t rfa0:2;
} __attribute__ ((packed));

struct avdtp_continue_header {
	uint8_t message_type:2;
	uint8_t packet_type:2;
	uint8_t transaction:4;
} __attribute__ ((packed));

struct seid_info {
	uint8_t rfa0:1;
	uint8_t inuse:1;
	uint8_t seid:6;
	uint8_t rfa2:3;
	uint8_t type:1;
	uint8_t media_type:4;
} __attribute__ ((packed));

struct seid {
	uint8_t rfa0:2;
	uint8_t seid:6;
} __attribute__ ((packed));

#elif __BYTE_ORDER == __BIG_ENDIAN

struct avdtp_common_header {
	uint8_t transaction:4;
	uint8_t packet_type:2;
	uint8_t message_type:2;
} __attribute__ ((packed));

struct avdtp_single_header {
	uint8_t transaction:4;
	uint8_t packet_type:2;
	uint8_t message_type:2;
	uint8_t rfa0:2;
	uint8_t signal_id:6;
} __attribute__ ((packed));

struct avdtp_start_header {
	uint8_t transaction:4;
	uint8_t packet_type:2;
	uint8_t message_type:2;
	uint8_t no_of_packets;
	uint8_t rfa0:2;
	uint8_t signal_id:6;
} __attribute__ ((packed));

struct avdtp_continue_header {
	uint8_t transaction:4;
	uint8_t packet_type:2;
	uint8_t message_type:2;
} __attribute__ ((packed));

struct seid_info {
	uint8_t seid:6;
	uint8_t inuse:1;
	uint8_t rfa0:1;
	uint8_t media_type:4;
	uint8_t type:1;
	uint8_t rfa2:3;
} __attribute__ ((packed));

struct seid {
	uint8_t seid:6;
	uint8_t rfa0:2;
} __attribute__ ((packed));

#else
#error "Unknown byte order"
#endif

/* packets */

struct discover_resp {
	struct seid_info seps[0];
} __attribute__ ((packed));

struct getcap_resp {
	uint8_t caps[0];
} __attribute__ ((packed));

struct start_req {
	struct seid first_seid;
	struct seid other_seids[0];
} __attribute__ ((packed));

struct suspend_req {
	struct seid first_seid;
	struct seid other_seids[0];
} __attribute__ ((packed));

struct seid_rej {
	uint8_t error;
} __attribute__ ((packed));

struct conf_rej {
	uint8_t category;
	uint8_t error;
} __attribute__ ((packed));

#if __BYTE_ORDER == __LITTLE_ENDIAN

struct seid_req {
	uint8_t rfa0:2;
	uint8_t acp_seid:6;
} __attribute__ ((packed));

struct setconf_req {
	uint8_t rfa0:2;
	uint8_t acp_seid:6;
	uint8_t rfa1:2;
	uint8_t int_seid:6;

	uint8_t caps[0];
} __attribute__ ((packed));

struct stream_rej {
	uint8_t rfa0:2;
	uint8_t acp_seid:6;
	uint8_t error;
} __attribute__ ((packed));

struct reconf_req {
	uint8_t rfa0:2;
	uint8_t acp_seid:6;

	uint8_t serv_cap;
	uint8_t serv_cap_len;

	uint8_t caps[0];
} __attribute__ ((packed));

#elif __BYTE_ORDER == __BIG_ENDIAN

struct seid_req {
	uint8_t acp_seid:6;
	uint8_t rfa0:2;
} __attribute__ ((packed));

struct setconf_req {
	uint8_t acp_seid:6;
	uint8_t rfa0:2;
	uint8_t int_seid:6;
	uint8_t rfa1:2;

	uint8_t caps[0];
} __attribute__ ((packed));

struct stream_rej {
	uint8_t acp_seid:6;
	uint8_t rfa0:2;
	uint8_t error;
} __attribute__ ((packed));

struct reconf_req {
	uint8_t acp_seid:6;
	uint8_t rfa0:2;

	uint8_t serv_cap;
	uint8_t serv_cap_len;

	uint8_t caps[0];
} __attribute__ ((packed));

#else
#error "Unknown byte order"
#endif

struct in_buf {
	gboolean active;
	int no_of_packets;
	uint8_t transaction;
	uint8_t message_type;
	uint8_t signal_id;
	uint8_t buf[1024];
	uint8_t data_size;
};

struct pending_req {
	uint8_t transaction;
	uint8_t signal_id;
	void *data;
	size_t data_size;
	struct avdtp_stream *stream; /* Set if the request targeted a stream */
	guint timeout;
};

struct avdtp_remote_sep {
	uint8_t seid;
	uint8_t type;
	uint8_t media_type;
	struct avdtp_service_capability *codec;
	GSList *caps; /* of type struct avdtp_service_capability */
	struct avdtp_stream *stream;
};

struct avdtp_server {
	bdaddr_t src;
	GIOChannel *io;
	GSList *seps;
};

struct avdtp_local_sep {
	avdtp_state_t state;
	struct avdtp_stream *stream;
	struct seid_info info;
	uint8_t codec;
	GSList *caps;
	struct avdtp_sep_ind *ind;
	struct avdtp_sep_cfm *cfm;
	void *user_data;
	struct avdtp_server *server;
};

struct stream_callback {
	avdtp_stream_state_cb cb;
	void *user_data;
	unsigned int id;
};

struct avdtp_state_callback {
	avdtp_session_state_cb cb;
	void *user_data;
	unsigned int id;
};

struct avdtp_stream {
	GIOChannel *io;
	uint16_t imtu;
	uint16_t omtu;
	struct avdtp *session;
	struct avdtp_local_sep *lsep;
	uint8_t rseid;
	GSList *caps;
	GSList *callbacks;
	struct avdtp_service_capability *codec;
	guint io_id;		/* Transport GSource ID */
	guint timer;		/* Waiting for other side to close or open
				   the transport channel */
	gboolean open_acp;	/* If we are in ACT role for Open */
	gboolean close_int;	/* If we are in INT role for Close */
	guint idle_timer;
};

/* Structure describing an AVDTP connection between two devices */

struct avdtp {
	int ref;
	int free_lock;

	struct avdtp_server *server;
	bdaddr_t dst;

	avdtp_session_state_t state;

	/* True if the session should be automatically disconnected */
	gboolean auto_dc;

	GIOChannel *io;
	guint io_id;

	GSList *seps; /* Elements of type struct avdtp_remote_sep * */

	GSList *streams; /* Elements of type struct avdtp_stream * */

	GSList *req_queue; /* Elements of type struct pending_req * */
	GSList *prio_queue; /* Same as req_queue but is processed before it */

	struct avdtp_stream *pending_open;

	uint16_t imtu;
	uint16_t omtu;

	struct in_buf in;

	char *buf;

	avdtp_discover_cb_t discov_cb;
	void *user_data;

	struct pending_req *req;

	guint dc_timer;

	/* Attempt stream setup instead of disconnecting */
	gboolean stream_setup;

	DBusPendingCall *pending_auth;
};

static GSList *servers = NULL;
static GSList *sessions = NULL;

static GSList *avdtp_callbacks = NULL;

static gboolean auto_connect = TRUE;

static int send_request(struct avdtp *session, gboolean priority,
			struct avdtp_stream *stream, uint8_t signal_id,
			void *buffer, size_t size);
static gboolean avdtp_parse_resp(struct avdtp *session,
					struct avdtp_stream *stream,
					uint8_t transaction, uint8_t signal_id,
					void *buf, int size);
static gboolean avdtp_parse_rej(struct avdtp *session,
					struct avdtp_stream *stream,
					uint8_t transaction, uint8_t signal_id,
					void *buf, int size);
static int process_queue(struct avdtp *session);
static void connection_lost(struct avdtp *session, int err);
static void avdtp_sep_set_state(struct avdtp *session,
				struct avdtp_local_sep *sep,
				avdtp_state_t state);

static struct avdtp_server *find_server(GSList *list, const bdaddr_t *src)
{
	GSList *l;

	for (l = list; l; l = l->next) {
		struct avdtp_server *server = l->data;

		if (bacmp(&server->src, src) == 0)
			return server;
	}

	return NULL;
}

static const char *avdtp_statestr(avdtp_state_t state)
{
	switch (state) {
	case AVDTP_STATE_IDLE:
		return "IDLE";
	case AVDTP_STATE_CONFIGURED:
		return "CONFIGURED";
	case AVDTP_STATE_OPEN:
		return "OPEN";
	case AVDTP_STATE_STREAMING:
		return "STREAMING";
	case AVDTP_STATE_CLOSING:
		return "CLOSING";
	case AVDTP_STATE_ABORTING:
		return "ABORTING";
	default:
		return "<unknown state>";
	}
}

static gboolean try_send(int sk, void *data, size_t len)
{
	gboolean ret;

	ret = send(sk, data, len, 0);

	if (ret < 0)
		ret = -errno;
	else if ((size_t) ret != len)
		ret = -EIO;

	if (ret < 0) {
		error("try_send: %s (%d)", strerror(-ret), -ret);
		return FALSE;
	}

	return TRUE;
}

static gboolean avdtp_send(struct avdtp *session, uint8_t transaction,
				uint8_t message_type, uint8_t signal_id,
				void *data, size_t len)
{
	unsigned int cont_fragments, sent;
	struct avdtp_start_header start;
	struct avdtp_continue_header cont;
	int sock;

	if (session->io == NULL) {
		error("avdtp_send: session is closed");
		return FALSE;
	}

	sock = g_io_channel_unix_get_fd(session->io);

	/* Single packet - no fragmentation */
	if (sizeof(struct avdtp_single_header) + len <= session->omtu) {
		struct avdtp_single_header single;

		memset(&single, 0, sizeof(single));

		single.transaction = transaction;
		single.packet_type = AVDTP_PKT_TYPE_SINGLE;
		single.message_type = message_type;
		single.signal_id = signal_id;

		memcpy(session->buf, &single, sizeof(single));
		memcpy(session->buf + sizeof(single), data, len);

		return try_send(sock, session->buf, sizeof(single) + len);
	}

	/* Count the number of needed fragments */
	cont_fragments = (len - (session->omtu - sizeof(start))) /
					(session->omtu - sizeof(cont)) + 1;

	debug("avdtp_send: %u bytes split into %d fragments", len,
							cont_fragments + 1);

	/* Send the start packet */
	memset(&start, 0, sizeof(start));
	start.transaction = transaction;
	start.packet_type = AVDTP_PKT_TYPE_START;
	start.message_type = message_type;
	start.no_of_packets = cont_fragments + 1;
	start.signal_id = signal_id;

	memcpy(session->buf, &start, sizeof(start));
	memcpy(session->buf + sizeof(start), data,
					session->omtu - sizeof(start));

	if (!try_send(sock, session->buf, session->omtu))
		return FALSE;

	debug("avdtp_send: first packet with %d bytes sent",
						session->omtu - sizeof(start));

	sent = session->omtu - sizeof(start);

	/* Send the continue fragments and the end packet */
	while (sent < len) {
		int left, to_copy;

		left = len - sent;
		if (left + sizeof(cont) > session->omtu) {
			cont.packet_type = AVDTP_PKT_TYPE_CONTINUE;
			to_copy = session->omtu - sizeof(cont);
			debug("avdtp_send: sending continue with %d bytes",
								to_copy);
		} else {
			cont.packet_type = AVDTP_PKT_TYPE_END;
			to_copy = left;
			debug("avdtp_send: sending end with %d bytes",
								to_copy);
		}

		cont.transaction = transaction;
		cont.message_type = message_type;

		memcpy(session->buf, &cont, sizeof(cont));
		memcpy(session->buf + sizeof(cont), data + sent, to_copy);

		if (!try_send(sock, session->buf, to_copy + sizeof(cont)))
			return FALSE;

		sent += to_copy;
	}

	return TRUE;
}

static void pending_req_free(struct pending_req *req)
{
	if (req->timeout)
		g_source_remove(req->timeout);
	g_free(req->data);
	g_free(req);
}

static gboolean stream_close_timeout(gpointer user_data)
{
	struct avdtp_stream *stream = user_data;

	debug("Timed out waiting for peer to close the transport channel");

	stream->timer = 0;

	g_io_channel_shutdown(stream->io, FALSE, NULL);

	return FALSE;
}

static gboolean stream_open_timeout(gpointer user_data)
{
	struct avdtp_stream *stream = user_data;

	debug("Timed out waiting for peer to open the transport channel");

	stream->timer = 0;

	stream->session->pending_open = NULL;

	avdtp_abort(stream->session, stream);

	return FALSE;
}

static gboolean disconnect_timeout(gpointer user_data)
{
	struct avdtp *session = user_data;
	struct audio_device *dev;
	gboolean stream_setup;

	assert(session->ref == 1);

	session->dc_timer = 0;
	stream_setup = session->stream_setup;
	session->stream_setup = FALSE;

	dev = manager_get_device(&session->server->src, &session->dst);

	if (dev && dev->sink && stream_setup)
		sink_setup_stream(dev->sink, session);
	else
		connection_lost(session, ETIMEDOUT);

	return FALSE;
}

static void remove_disconnect_timer(struct avdtp *session)
{
	g_source_remove(session->dc_timer);
	session->dc_timer = 0;
	session->stream_setup = FALSE;
}

static void set_disconnect_timer(struct avdtp *session)
{
	if (session->dc_timer)
		remove_disconnect_timer(session);

	session->dc_timer = g_timeout_add_seconds(DISCONNECT_TIMEOUT,
						disconnect_timeout,
						session);
}

void avdtp_error_init(struct avdtp_error *err, uint8_t type, int id)
{
	err->type = type;
	switch (type) {
	case AVDTP_ERROR_ERRNO:
		err->err.posix_errno = id;
		break;
	case AVDTP_ERROR_ERROR_CODE:
		err->err.error_code = id;
		break;
	}
}

avdtp_error_type_t avdtp_error_type(struct avdtp_error *err)
{
	return err->type;
}

int avdtp_error_error_code(struct avdtp_error *err)
{
	assert(err->type == AVDTP_ERROR_ERROR_CODE);
	return err->err.error_code;
}

int avdtp_error_posix_errno(struct avdtp_error *err)
{
	assert(err->type == AVDTP_ERROR_ERRNO);
	return err->err.posix_errno;
}

static struct avdtp_stream *find_stream_by_rseid(struct avdtp *session,
							uint8_t rseid)
{
	GSList *l;

	for (l = session->streams; l != NULL; l = g_slist_next(l)) {
		struct avdtp_stream *stream = l->data;

		if (stream->rseid == rseid)
			return stream;
	}

	return NULL;
}

static struct avdtp_remote_sep *find_remote_sep(GSList *seps, uint8_t seid)
{
	GSList *l;

	for (l = seps; l != NULL; l = g_slist_next(l)) {
		struct avdtp_remote_sep *sep = l->data;

		if (sep->seid == seid)
			return sep;
	}

	return NULL;
}

static void avdtp_set_state(struct avdtp *session,
					avdtp_session_state_t new_state)
{
	GSList *l;
	struct audio_device *dev;
	bdaddr_t src, dst;
	avdtp_session_state_t old_state = session->state;

	session->state = new_state;

	avdtp_get_peers(session, &src, &dst);
	dev = manager_get_device(&src, &dst);
	if (dev == NULL) {
		error("avdtp_set_state(): no matching audio device");
		return;
	}

	for (l = avdtp_callbacks; l != NULL; l = l->next) {
		struct avdtp_state_callback *cb = l->data;
		cb->cb(dev, session, old_state, new_state, cb->user_data);
	}
}

static void stream_free(struct avdtp_stream *stream)
{
	struct avdtp_remote_sep *rsep;

	stream->lsep->info.inuse = 0;
	stream->lsep->stream = NULL;

	rsep = find_remote_sep(stream->session->seps, stream->rseid);
	if (rsep)
		rsep->stream = NULL;

	if (stream->timer)
		g_source_remove(stream->timer);

	if (stream->io)
		g_io_channel_unref(stream->io);

	if (stream->io_id)
		g_source_remove(stream->io_id);

	g_slist_foreach(stream->callbacks, (GFunc) g_free, NULL);
	g_slist_free(stream->callbacks);

	g_slist_foreach(stream->caps, (GFunc) g_free, NULL);
	g_slist_free(stream->caps);

	g_free(stream);
}

static gboolean stream_timeout(gpointer user_data)
{
	struct avdtp_stream *stream = user_data;
	struct avdtp *session = stream->session;

	avdtp_close(session, stream);

	stream->idle_timer = 0;

	return FALSE;
}

static gboolean transport_cb(GIOChannel *chan, GIOCondition cond,
				gpointer data)
{
	struct avdtp_stream *stream = data;
	struct avdtp_local_sep *sep = stream->lsep;

	if (stream->close_int && sep->cfm && sep->cfm->close)
		sep->cfm->close(stream->session, sep, stream, NULL,
				sep->user_data);

	if (!(cond & G_IO_NVAL))
		g_io_channel_shutdown(stream->io, FALSE, NULL);

	avdtp_sep_set_state(stream->session, sep, AVDTP_STATE_IDLE);

	return FALSE;
}

static void handle_transport_connect(struct avdtp *session, GIOChannel *io,
					uint16_t imtu, uint16_t omtu)
{
	struct avdtp_stream *stream = session->pending_open;
	struct avdtp_local_sep *sep = stream->lsep;

	session->pending_open = NULL;

	if (stream->timer) {
		g_source_remove(stream->timer);
		stream->timer = 0;
	}

	if (io == NULL) {
		if (!stream->open_acp && sep->cfm && sep->cfm->open) {
			struct avdtp_error err;
			avdtp_error_init(&err, AVDTP_ERROR_ERRNO, EIO);
			sep->cfm->open(session, sep, NULL, &err,
					sep->user_data);
		}
		return;
	}

	stream->io = g_io_channel_ref(io);
	stream->omtu = omtu;
	stream->imtu = imtu;

	if (!stream->open_acp && sep->cfm && sep->cfm->open)
		sep->cfm->open(session, sep, stream, NULL, sep->user_data);

	avdtp_sep_set_state(session, sep, AVDTP_STATE_OPEN);

	stream->io_id = g_io_add_watch(io, G_IO_ERR | G_IO_HUP | G_IO_NVAL,
					(GIOFunc) transport_cb, stream);
}

static int pending_req_cmp(gconstpointer a, gconstpointer b)
{
	const struct pending_req *req = a;
	const struct avdtp_stream *stream = b;

	if (req->stream == stream)
		return 0;

	return -1;
}

static void cleanup_queue(struct avdtp *session, struct avdtp_stream *stream)
{
	GSList *l;
	struct pending_req *req;

	while ((l = g_slist_find_custom(session->prio_queue, stream,
							pending_req_cmp))) {
		req = l->data;
		pending_req_free(req);
		session->prio_queue = g_slist_remove(session->prio_queue, req);
	}

	while ((l = g_slist_find_custom(session->req_queue, stream,
							pending_req_cmp))) {
		req = l->data;
		pending_req_free(req);
		session->req_queue = g_slist_remove(session->req_queue, req);
	}
}

static void avdtp_sep_set_state(struct avdtp *session,
				struct avdtp_local_sep *sep,
				avdtp_state_t state)
{
	struct avdtp_stream *stream = sep->stream;
	avdtp_state_t old_state;
	struct avdtp_error err, *err_ptr = NULL;

	if (sep->state == state) {
		avdtp_error_init(&err, AVDTP_ERROR_ERRNO, EIO);
		debug("stream state change failed: %s", avdtp_strerror(&err));
		err_ptr = &err;
	} else {
		err_ptr = NULL;
		debug("stream state changed: %s -> %s",
				avdtp_statestr(sep->state),
				avdtp_statestr(state));
	}

	old_state = sep->state;
	sep->state = state;

	if (stream) {
		GSList *l;
		for (l = stream->callbacks; l != NULL; l = g_slist_next(l)) {
			struct stream_callback *cb = l->data;
			cb->cb(stream, old_state, state, err_ptr,
					cb->user_data);
		}
	}

	switch (state) {
	case AVDTP_STATE_OPEN:
		if (old_state > AVDTP_STATE_OPEN && session->auto_dc)
			stream->idle_timer = g_timeout_add_seconds(STREAM_TIMEOUT,
								stream_timeout,
								stream);
		break;
	case AVDTP_STATE_STREAMING:
	case AVDTP_STATE_CLOSING:
	case AVDTP_STATE_ABORTING:
		if (stream->idle_timer) {
			g_source_remove(stream->idle_timer);
			stream->idle_timer = 0;
		}
		break;
	case AVDTP_STATE_IDLE:
		if (stream->idle_timer) {
			g_source_remove(stream->idle_timer);
			stream->idle_timer = 0;
		}
		session->streams = g_slist_remove(session->streams, stream);
		if (session->pending_open == stream)
			handle_transport_connect(session, NULL, 0, 0);
		if (session->req && session->req->stream == stream)
			session->req->stream = NULL;
		/* Remove pending commands for this stream from the queue */
		cleanup_queue(session, stream);
		stream_free(stream);
		if (session->ref == 1 && !session->streams)
			set_disconnect_timer(session);
		break;
	default:
		break;
	}
}

static void finalize_discovery(struct avdtp *session, int err)
{
	struct avdtp_error avdtp_err;

	avdtp_error_init(&avdtp_err, AVDTP_ERROR_ERRNO, err);

	if (!session->discov_cb)
		return;

	session->discov_cb(session, session->seps,
				err ? &avdtp_err : NULL,
				session->user_data);

	session->discov_cb = NULL;
	session->user_data = NULL;
}

static void release_stream(struct avdtp_stream *stream, struct avdtp *session)
{
	struct avdtp_local_sep *sep = stream->lsep;

	if (sep->cfm && sep->cfm->abort)
		sep->cfm->abort(session, sep, stream, NULL, sep->user_data);

	avdtp_sep_set_state(session, sep, AVDTP_STATE_IDLE);
}

static void connection_lost(struct avdtp *session, int err)
{
	char address[18];

	ba2str(&session->dst, address);
	debug("Disconnected from %s", address);

	if (session->state == AVDTP_SESSION_STATE_CONNECTING && err != EACCES)
		btd_cancel_authorization(&session->server->src, &session->dst);

	session->free_lock = 1;

	finalize_discovery(session, err);

	g_slist_foreach(session->streams, (GFunc) release_stream, session);
	session->streams = NULL;

	session->free_lock = 0;

	if (session->io) {
		g_io_channel_shutdown(session->io, FALSE, NULL);
		g_io_channel_unref(session->io);
		session->io = NULL;
	}

	avdtp_set_state(session, AVDTP_SESSION_STATE_DISCONNECTED);

	if (session->io_id) {
		g_source_remove(session->io_id);
		session->io_id = 0;
	}

	if (session->ref != 1)
		error("connection_lost: ref count not 1 after all callbacks");
	else
		avdtp_unref(session);
}

void avdtp_unref(struct avdtp *session)
{
	if (!session)
		return;

	if (!g_slist_find(sessions, session)) {
		error("avdtp_unref: trying to unref a unknown session");
		return;
	}

	session->ref--;

	debug("avdtp_unref(%p): ref=%d", session, session->ref);

	if (session->ref == 1) {
		if (session->state == AVDTP_SESSION_STATE_CONNECTING &&
								session->io) {
			g_io_channel_shutdown(session->io, TRUE, NULL);
			g_io_channel_unref(session->io);
			session->io = NULL;
		}

		if (session->io)
			set_disconnect_timer(session);
		else if (!session->free_lock) /* Drop the local ref if we
						 aren't connected */
			session->ref--;
	}

	if (session->ref > 0)
		return;

	debug("avdtp_unref(%p): freeing session and removing from list",
			session);

	if (session->dc_timer)
		remove_disconnect_timer(session);

	sessions = g_slist_remove(sessions, session);

	if (session->req)
		pending_req_free(session->req);

	g_slist_foreach(session->seps, (GFunc) g_free, NULL);
	g_slist_free(session->seps);

	g_free(session->buf);

	g_free(session);
}

struct avdtp *avdtp_ref(struct avdtp *session)
{
	session->ref++;
	debug("avdtp_ref(%p): ref=%d", session, session->ref);
	if (session->dc_timer)
		remove_disconnect_timer(session);
	return session;
}

static struct avdtp_local_sep *find_local_sep_by_seid(struct avdtp_server *server,
							uint8_t seid)
{
	GSList *l;

	for (l = server->seps; l != NULL; l = g_slist_next(l)) {
		struct avdtp_local_sep *sep = l->data;

		if (sep->info.seid == seid)
			return sep;
	}

	return NULL;
}

static struct avdtp_local_sep *find_local_sep(struct avdtp_server *server,
						uint8_t type,
						uint8_t media_type,
						uint8_t codec)
{
	GSList *l;

	for (l = server->seps; l != NULL; l = g_slist_next(l)) {
		struct avdtp_local_sep *sep = l->data;

		if (sep->info.inuse)
			continue;

		if (sep->info.type == type &&
				sep->info.media_type == media_type &&
				sep->codec == codec)
			return sep;
	}

	return NULL;
}

static GSList *caps_to_list(uint8_t *data, int size,
				struct avdtp_service_capability **codec)
{
	GSList *caps;
	int processed;

	for (processed = 0, caps = NULL; processed + 2 < size;) {
		struct avdtp_service_capability *cap;
		uint8_t length, category;

		category = data[0];
		length = data[1];

		if (processed + 2 + length > size) {
			error("Invalid capability data in getcap resp");
			break;
		}

		cap = g_malloc(sizeof(struct avdtp_service_capability) +
					length);
		memcpy(cap, data, 2 + length);

		processed += 2 + length;
		data += 2 + length;

		caps = g_slist_append(caps, cap);

		if (category == AVDTP_MEDIA_CODEC &&
				length >=
				sizeof(struct avdtp_media_codec_capability))
			*codec = cap;
	}

	return caps;
}

static gboolean avdtp_unknown_cmd(struct avdtp *session, uint8_t transaction,
							void *buf, int size)
{
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
								0, NULL, 0);
}

static gboolean avdtp_discover_cmd(struct avdtp *session, uint8_t transaction,
							void *buf, int size)
{
	GSList *l;
	unsigned int rsp_size, sep_count, i;
	struct seid_info *seps;
	gboolean ret;

	sep_count = g_slist_length(session->server->seps);
	rsp_size = sep_count * sizeof(struct seid_info);

	seps = g_new0(struct seid_info, sep_count);

	for (l = session->server->seps, i = 0; l != NULL; l = l->next, i++) {
		struct avdtp_local_sep *sep = l->data;

		memcpy(&seps[i], &sep->info, sizeof(struct seid_info));
	}

	ret = avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
				AVDTP_DISCOVER, seps, rsp_size);
	g_free(seps);

	return ret;
}

static gboolean avdtp_getcap_cmd(struct avdtp *session, uint8_t transaction,
					struct seid_req *req, unsigned int size)
{
	GSList *l, *caps;
	struct avdtp_local_sep *sep = NULL;
	unsigned int rsp_size;
	uint8_t err, buf[1024], *ptr = buf;

	if (size < sizeof(struct seid_req)) {
		err = AVDTP_BAD_LENGTH;
		goto failed;
	}

	sep = find_local_sep_by_seid(session->server, req->acp_seid);
	if (!sep) {
		err = AVDTP_BAD_ACP_SEID;
		goto failed;
	}

	if (!sep->ind->get_capability(session, sep, &caps, &err,
					sep->user_data))
		goto failed;

	for (l = caps, rsp_size = 0; l != NULL; l = g_slist_next(l)) {
		struct avdtp_service_capability *cap = l->data;

		if (rsp_size + cap->length + 2 > sizeof(buf))
			break;

		memcpy(ptr, cap, cap->length + 2);
		rsp_size += cap->length + 2;
		ptr += cap->length + 2;

		g_free(cap);
	}

	g_slist_free(caps);

	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
				AVDTP_GET_CAPABILITIES, buf, rsp_size);

failed:
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
				AVDTP_GET_CAPABILITIES, &err, sizeof(err));
}

static gboolean avdtp_setconf_cmd(struct avdtp *session, uint8_t transaction,
				struct setconf_req *req, unsigned int size)
{
	struct conf_rej rej;
	struct avdtp_local_sep *sep;
	struct avdtp_stream *stream;
	uint8_t err, category = 0x00;
	struct audio_device *dev;
	bdaddr_t src, dst;
	GSList *l;

	if (size < sizeof(struct setconf_req)) {
		error("Too short getcap request");
		return FALSE;
	}

	sep = find_local_sep_by_seid(session->server, req->acp_seid);
	if (!sep) {
		err = AVDTP_BAD_ACP_SEID;
		goto failed;
	}

	if (sep->stream) {
		err = AVDTP_SEP_IN_USE;
		goto failed;
	}

	avdtp_get_peers(session, &src, &dst);
	dev = manager_get_device(&src, &dst);
	if (!dev) {
		error("Unable to get a audio device object");
		goto failed;
	}

	switch (sep->info.type) {
	case AVDTP_SEP_TYPE_SOURCE:
		if (!dev->sink) {
			btd_device_add_uuid(dev->btd_dev, A2DP_SINK_UUID);
			if (!dev->sink) {
				error("Unable to get a audio sink object");
				goto failed;
			}
		}
		break;
	case AVDTP_SEP_TYPE_SINK:
		/* Do source_init() here when it's implemented */
		break;
	}

	stream = g_new0(struct avdtp_stream, 1);
	stream->session = session;
	stream->lsep = sep;
	stream->rseid = req->int_seid;
	stream->caps = caps_to_list(req->caps,
					size - sizeof(struct setconf_req),
					&stream->codec);

	/* Verify that the Media Transport capability's length = 0. Reject otherwise */
	for (l = stream->caps; l != NULL; l = g_slist_next(l)) {
		struct avdtp_service_capability *cap = l->data;

		if (cap->category == AVDTP_MEDIA_TRANSPORT && cap->length != 0) {
			err = AVDTP_BAD_MEDIA_TRANSPORT_FORMAT;
			goto failed;
		}
	}

	if (sep->ind && sep->ind->set_configuration) {
		if (!sep->ind->set_configuration(session, sep, stream,
							stream->caps, &err,
							&category,
							sep->user_data)) {
			stream_free(stream);
			goto failed;
		}
	}

	if (!avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
					AVDTP_SET_CONFIGURATION, NULL, 0)) {
		stream_free(stream);
		return FALSE;
	}

	sep->stream = stream;
	session->streams = g_slist_append(session->streams, stream);

	avdtp_sep_set_state(session, sep, AVDTP_STATE_CONFIGURED);

	return TRUE;

failed:
	rej.error = err;
	rej.category = category;
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
				AVDTP_SET_CONFIGURATION, &rej, sizeof(rej));
}

static gboolean avdtp_getconf_cmd(struct avdtp *session, uint8_t transaction,
					struct seid_req *req, int size)
{
	GSList *l;
	struct avdtp_local_sep *sep = NULL;
	int rsp_size;
	uint8_t err;
	uint8_t buf[1024];
	uint8_t *ptr = buf;

	if (size < (int) sizeof(struct seid_req)) {
		error("Too short getconf request");
		return FALSE;
	}

	memset(buf, 0, sizeof(buf));

	sep = find_local_sep_by_seid(session->server, req->acp_seid);
	if (!sep) {
		err = AVDTP_BAD_ACP_SEID;
		goto failed;
	}
	if (!sep->stream || !sep->stream->caps) {
		err = AVDTP_UNSUPPORTED_CONFIGURATION;
		goto failed;
	}

	for (l = sep->stream->caps, rsp_size = 0; l != NULL; l = g_slist_next(l)) {
		struct avdtp_service_capability *cap = l->data;

		if (rsp_size + cap->length + 2 > (int) sizeof(buf))
			break;

		memcpy(ptr, cap, cap->length + 2);
		rsp_size += cap->length + 2;
		ptr += cap->length + 2;
	}

	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
				AVDTP_GET_CONFIGURATION, buf, rsp_size);

failed:
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
				AVDTP_GET_CONFIGURATION, &err, sizeof(err));
}

static gboolean avdtp_reconf_cmd(struct avdtp *session, uint8_t transaction,
					struct seid_req *req, int size)
{
	return avdtp_unknown_cmd(session, transaction, (void *) req, size);
}

static gboolean avdtp_open_cmd(struct avdtp *session, uint8_t transaction,
				struct seid_req *req, unsigned int size)
{
	struct avdtp_local_sep *sep;
	struct avdtp_stream *stream;
	uint8_t err;

	if (size < sizeof(struct seid_req)) {
		error("Too short abort request");
		return FALSE;
	}

	sep = find_local_sep_by_seid(session->server, req->acp_seid);
	if (!sep) {
		err = AVDTP_BAD_ACP_SEID;
		goto failed;
	}

	if (sep->state != AVDTP_STATE_CONFIGURED) {
		err = AVDTP_BAD_STATE;
		goto failed;
	}

	stream = sep->stream;

	if (sep->ind && sep->ind->open) {
		if (!sep->ind->open(session, sep, stream, &err,
					sep->user_data))
			goto failed;
	}

	if (!avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
						AVDTP_OPEN, NULL, 0))
		return FALSE;

	stream->open_acp = TRUE;
	session->pending_open = stream;
	stream->timer = g_timeout_add_seconds(REQ_TIMEOUT,
						stream_open_timeout,
						stream);

	return TRUE;

failed:
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
				AVDTP_OPEN, &err, sizeof(err));
}

static gboolean avdtp_start_cmd(struct avdtp *session, uint8_t transaction,
				struct start_req *req, unsigned int size)
{
	struct avdtp_local_sep *sep;
	struct avdtp_stream *stream;
	struct stream_rej rej;
	struct seid *seid;
	uint8_t err, failed_seid;
	int seid_count, i;

	if (size < sizeof(struct start_req)) {
		error("Too short start request");
		return FALSE;
	}

	seid_count = 1 + size - sizeof(struct start_req);

	seid = &req->first_seid;

	for (i = 0; i < seid_count; i++, seid++) {
		failed_seid = seid->seid;

		sep = find_local_sep_by_seid(session->server,
					req->first_seid.seid);
		if (!sep || !sep->stream) {
			err = AVDTP_BAD_ACP_SEID;
			goto failed;
		}

		stream = sep->stream;

		if (sep->state != AVDTP_STATE_OPEN) {
			err = AVDTP_BAD_STATE;
			goto failed;
		}

		if (sep->ind && sep->ind->start) {
			if (!sep->ind->start(session, sep, stream, &err,
						sep->user_data))
				goto failed;
		}

		avdtp_sep_set_state(session, sep, AVDTP_STATE_STREAMING);
	}

	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
						AVDTP_START, NULL, 0);

failed:
	memset(&rej, 0, sizeof(rej));
	rej.acp_seid = failed_seid;
	rej.error = err;
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
				AVDTP_START, &rej, sizeof(rej));
}

static gboolean avdtp_close_cmd(struct avdtp *session, uint8_t transaction,
				struct seid_req *req, unsigned int size)
{
	struct avdtp_local_sep *sep;
	struct avdtp_stream *stream;
	uint8_t err;

	if (size < sizeof(struct seid_req)) {
		error("Too short close request");
		return FALSE;
	}

	sep = find_local_sep_by_seid(session->server, req->acp_seid);
	if (!sep || !sep->stream) {
		err = AVDTP_BAD_ACP_SEID;
		goto failed;
	}

	if (sep->state != AVDTP_STATE_OPEN &&
			sep->state != AVDTP_STATE_STREAMING) {
		err = AVDTP_BAD_STATE;
		goto failed;
	}

	stream = sep->stream;

	if (sep->ind && sep->ind->close) {
		if (!sep->ind->close(session, sep, stream, &err,
					sep->user_data))
			goto failed;
	}

	avdtp_sep_set_state(session, sep, AVDTP_STATE_CLOSING);

	if (!avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
						AVDTP_CLOSE, NULL, 0))
		return FALSE;

	stream->timer = g_timeout_add_seconds(REQ_TIMEOUT,
					stream_close_timeout,
					stream);

	return TRUE;

failed:
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
					AVDTP_CLOSE, &err, sizeof(err));
}

static gboolean avdtp_suspend_cmd(struct avdtp *session, uint8_t transaction,
				struct suspend_req *req, unsigned int size)
{
	struct avdtp_local_sep *sep;
	struct avdtp_stream *stream;
	struct stream_rej rej;
	struct seid *seid;
	uint8_t err, failed_seid;
	int seid_count, i;

	if (size < sizeof(struct suspend_req)) {
		error("Too short suspend request");
		return FALSE;
	}

	seid_count = 1 + size - sizeof(struct suspend_req);

	seid = &req->first_seid;

	for (i = 0; i < seid_count; i++, seid++) {
		failed_seid = seid->seid;

		sep = find_local_sep_by_seid(session->server,
					req->first_seid.seid);
		if (!sep || !sep->stream) {
			err = AVDTP_BAD_ACP_SEID;
			goto failed;
		}

		stream = sep->stream;

		if (sep->state != AVDTP_STATE_STREAMING) {
			err = AVDTP_BAD_STATE;
			goto failed;
		}

		if (sep->ind && sep->ind->suspend) {
			if (!sep->ind->suspend(session, sep, stream, &err,
						sep->user_data))
				goto failed;
		}

		avdtp_sep_set_state(session, sep, AVDTP_STATE_OPEN);
	}

	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
						AVDTP_SUSPEND, NULL, 0);

failed:
	memset(&rej, 0, sizeof(rej));
	rej.acp_seid = failed_seid;
	rej.error = err;
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
				AVDTP_SUSPEND, &rej, sizeof(rej));
}

static gboolean avdtp_abort_cmd(struct avdtp *session, uint8_t transaction,
				struct seid_req *req, unsigned int size)
{
	struct avdtp_local_sep *sep;
	uint8_t err;
	gboolean ret;

	if (size < sizeof(struct seid_req)) {
		error("Too short abort request");
		return FALSE;
	}

	sep = find_local_sep_by_seid(session->server, req->acp_seid);
	if (!sep || !sep->stream) {
		err = AVDTP_BAD_ACP_SEID;
		goto failed;
	}

	if (sep->ind && sep->ind->abort) {
		if (!sep->ind->abort(session, sep, sep->stream, &err,
					sep->user_data))
			goto failed;
	}

	ret = avdtp_send(session, transaction, AVDTP_MSG_TYPE_ACCEPT,
						AVDTP_ABORT, NULL, 0);
	if (ret)
		avdtp_sep_set_state(session, sep, AVDTP_STATE_ABORTING);

	return ret;

failed:
	return avdtp_send(session, transaction, AVDTP_MSG_TYPE_REJECT,
					AVDTP_ABORT, &err, sizeof(err));
}

static gboolean avdtp_secctl_cmd(struct avdtp *session, uint8_t transaction,
					struct seid_req *req, int size)
{
	return avdtp_unknown_cmd(session, transaction, (void *) req, size);
}

static gboolean avdtp_parse_cmd(struct avdtp *session, uint8_t transaction,
				uint8_t signal_id, void *buf, int size)
{
	switch (signal_id) {
	case AVDTP_DISCOVER:
		debug("Received DISCOVER_CMD");
		return avdtp_discover_cmd(session, transaction, buf, size);
	case AVDTP_GET_CAPABILITIES:
		debug("Received  GET_CAPABILITIES_CMD");
		return avdtp_getcap_cmd(session, transaction, buf, size);
	case AVDTP_SET_CONFIGURATION:
		debug("Received SET_CONFIGURATION_CMD");
		return avdtp_setconf_cmd(session, transaction, buf, size);
	case AVDTP_GET_CONFIGURATION:
		debug("Received GET_CONFIGURATION_CMD");
		return avdtp_getconf_cmd(session, transaction, buf, size);
	case AVDTP_RECONFIGURE:
		debug("Received RECONFIGURE_CMD");
		return avdtp_reconf_cmd(session, transaction, buf, size);
	case AVDTP_OPEN:
		debug("Received OPEN_CMD");
		return avdtp_open_cmd(session, transaction, buf, size);
	case AVDTP_START:
		debug("Received START_CMD");
		return avdtp_start_cmd(session, transaction, buf, size);
	case AVDTP_CLOSE:
		debug("Received CLOSE_CMD");
		return avdtp_close_cmd(session, transaction, buf, size);
	case AVDTP_SUSPEND:
		debug("Received SUSPEND_CMD");
		return avdtp_suspend_cmd(session, transaction, buf, size);
	case AVDTP_ABORT:
		debug("Received ABORT_CMD");
		return avdtp_abort_cmd(session, transaction, buf, size);
	case AVDTP_SECURITY_CONTROL:
		debug("Received SECURITY_CONTROL_CMD");
		return avdtp_secctl_cmd(session, transaction, buf, size);
	default:
		debug("Received unknown request id %u", signal_id);
		return avdtp_unknown_cmd(session, transaction, buf, size);
	}
}

enum avdtp_parse_result { PARSE_ERROR, PARSE_FRAGMENT, PARSE_SUCCESS };

static enum avdtp_parse_result avdtp_parse_data(struct avdtp *session,
							void *buf, size_t size)
{
	struct avdtp_common_header *header = buf;
	struct avdtp_single_header *single = (void *) session->buf;
	struct avdtp_start_header *start = (void *) session->buf;
	void *payload;
	gsize payload_size;

	switch (header->packet_type) {
	case AVDTP_PKT_TYPE_SINGLE:
		if (size < sizeof(*single)) {
			error("Received too small single packet (%d bytes)", size);
			return PARSE_ERROR;
		}
		if (session->in.active) {
			error("SINGLE: Invalid AVDTP packet fragmentation");
			return PARSE_ERROR;
		}

		payload = session->buf + sizeof(*single);
		payload_size = size - sizeof(*single);

		session->in.active = TRUE;
		session->in.data_size = 0;
		session->in.no_of_packets = 1;
		session->in.transaction = header->transaction;
		session->in.message_type = header->message_type;
		session->in.signal_id = single->signal_id;

		break;
	case AVDTP_PKT_TYPE_START:
		if (size < sizeof(*start)) {
			error("Received too small start packet (%d bytes)", size);
			return PARSE_ERROR;
		}
		if (session->in.active) {
			error("START: Invalid AVDTP packet fragmentation");
			return PARSE_ERROR;
		}

		session->in.active = TRUE;
		session->in.data_size = 0;
		session->in.transaction = header->transaction;
		session->in.message_type = header->message_type;
		session->in.no_of_packets = start->no_of_packets;
		session->in.signal_id = start->signal_id;

		payload = session->buf + sizeof(*start);
		payload_size = size - sizeof(*start);

		break;
	case AVDTP_PKT_TYPE_CONTINUE:
		if (size < sizeof(struct avdtp_continue_header)) {
			error("Received too small continue packet (%d bytes)",
									size);
			return PARSE_ERROR;
		}
		if (!session->in.active) {
			error("CONTINUE: Invalid AVDTP packet fragmentation");
			return PARSE_ERROR;
		}
		if (session->in.transaction != header->transaction) {
			error("Continue transaction id doesn't match");
			return PARSE_ERROR;
		}
		if (session->in.no_of_packets <= 1) {
			error("Too few continue packets");
			return PARSE_ERROR;
		}

		payload = session->buf + sizeof(struct avdtp_continue_header);
		payload_size = size - sizeof(struct avdtp_continue_header);

		break;
	case AVDTP_PKT_TYPE_END:
		if (size < sizeof(struct avdtp_continue_header)) {
			error("Received too small end packet (%d bytes)", size);
			return PARSE_ERROR;
		}
		if (!session->in.active) {
			error("END: Invalid AVDTP packet fragmentation");
			return PARSE_ERROR;
		}
		if (session->in.transaction != header->transaction) {
			error("End transaction id doesn't match");
			return PARSE_ERROR;
		}
		if (session->in.no_of_packets > 1) {
			error("Got an end packet too early");
			return PARSE_ERROR;
		}

		payload = session->buf + sizeof(struct avdtp_continue_header);
		payload_size = size - sizeof(struct avdtp_continue_header);

		break;
	default:
		error("Invalid AVDTP packet type 0x%02X", header->packet_type);
		return PARSE_ERROR;
	}

	if (session->in.data_size + payload_size >
					sizeof(session->in.buf)) {
		error("Not enough incoming buffer space!");
		return PARSE_ERROR;
	}

	memcpy(session->in.buf + session->in.data_size, payload, payload_size);
	session->in.data_size += payload_size;

	if (session->in.no_of_packets > 1) {
		session->in.no_of_packets--;
		debug("Received AVDTP fragment. %d to go",
						session->in.no_of_packets);
		return PARSE_FRAGMENT;
	}

	session->in.active = FALSE;

	return PARSE_SUCCESS;
}

static gboolean session_cb(GIOChannel *chan, GIOCondition cond,
				gpointer data)
{
	struct avdtp *session = data;
	struct avdtp_common_header *header;
	gsize size;

	debug("session_cb");

	if (cond & G_IO_NVAL)
		return FALSE;

	header = (void *) session->buf;

	if (cond & (G_IO_HUP | G_IO_ERR))
		goto failed;

	if (g_io_channel_read(chan, session->buf, session->imtu, &size)
							!= G_IO_ERROR_NONE) {
		error("IO Channel read error");
		goto failed;
	}

	if (size < sizeof(struct avdtp_common_header)) {
		error("Received too small packet (%d bytes)", size);
		goto failed;
	}

	switch (avdtp_parse_data(session, session->buf, size)) {
	case PARSE_ERROR:
		goto failed;
	case PARSE_FRAGMENT:
		return TRUE;
	case PARSE_SUCCESS:
		break;
	}

	if (session->in.message_type == AVDTP_MSG_TYPE_COMMAND) {
		if (!avdtp_parse_cmd(session, session->in.transaction,
					session->in.signal_id,
					session->in.buf,
					session->in.data_size)) {
			error("Unable to handle command. Disconnecting");
			goto failed;
		}

		if (session->ref == 1 && !session->streams)
			set_disconnect_timer(session);

		if (session->streams && session->dc_timer)
			remove_disconnect_timer(session);

		return TRUE;
	}

	if (session->req == NULL) {
		error("No pending request, ignoring message");
		return TRUE;
	}

	if (header->transaction != session->req->transaction) {
		error("Transaction label doesn't match");
		return TRUE;
	}

	if (session->in.signal_id != session->req->signal_id) {
		error("Reponse signal doesn't match");
		return TRUE;
	}

	g_source_remove(session->req->timeout);
	session->req->timeout = 0;

	switch (header->message_type) {
	case AVDTP_MSG_TYPE_ACCEPT:
		if (!avdtp_parse_resp(session, session->req->stream,
						session->in.transaction,
						session->in.signal_id,
						session->in.buf,
						session->in.data_size)) {
			error("Unable to parse accept response");
			goto failed;
		}
		break;
	case AVDTP_MSG_TYPE_REJECT:
		if (!avdtp_parse_rej(session, session->req->stream,
						session->in.transaction,
						session->in.signal_id,
						session->in.buf,
						session->in.data_size)) {
			error("Unable to parse reject response");
			goto failed;
		}
		break;
	default:
		error("Unknown message type 0x%02X", header->message_type);
		break;
	}

	pending_req_free(session->req);
	session->req = NULL;

	process_queue(session);

	return TRUE;

failed:
	connection_lost(session, EIO);

	return FALSE;
}

static struct avdtp *find_session(const bdaddr_t *src, const bdaddr_t *dst)
{
	GSList *l;

	for (l = sessions; l != NULL; l = g_slist_next(l)) {
		struct avdtp *s = l->data;

		if (bacmp(src, &s->server->src) || bacmp(dst, &s->dst))
			continue;

		return s;
	}

	return NULL;
}

static struct avdtp *avdtp_get_internal(const bdaddr_t *src, const bdaddr_t *dst)
{
	struct avdtp *session;

	assert(src != NULL);
	assert(dst != NULL);

	session = find_session(src, dst);
	if (session) {
		if (session->pending_auth)
			return NULL;
		else
			return session;
	}

	session = g_new0(struct avdtp, 1);

	session->server = find_server(servers, src);
	bacpy(&session->dst, dst);
	session->ref = 1;
	/* We don't use avdtp_set_state() here since this isn't a state change
	 * but just setting of the initial state */
	session->state = AVDTP_SESSION_STATE_DISCONNECTED;
	session->auto_dc = TRUE;

	sessions = g_slist_append(sessions, session);

	return session;
}

struct avdtp *avdtp_get(bdaddr_t *src, bdaddr_t *dst)
{
	struct avdtp *session;

	session = avdtp_get_internal(src, dst);

	if (!session)
		return NULL;

	return avdtp_ref(session);
}

static void avdtp_connect_cb(GIOChannel *chan, GError *err, gpointer user_data)
{
	struct avdtp *session = user_data;
	char address[18];
	GError *gerr = NULL;

	if (!g_slist_find(sessions, session)) {
		debug("avdtp_connect_cb: session got removed");
		return;
	}

	if (err) {
		error("%s", err->message);
		goto failed;
	}

	if (!session->io)
		session->io = g_io_channel_ref(chan);

	bt_io_get(chan, BT_IO_L2CAP, &gerr,
			BT_IO_OPT_OMTU, &session->omtu,
			BT_IO_OPT_IMTU, &session->imtu,
			BT_IO_OPT_INVALID);
	if (gerr) {
		error("%s", gerr->message);
		g_error_free(gerr);
		goto failed;
	}

	ba2str(&session->dst, address);
	debug("AVDTP: connected %s channel to %s",
			session->pending_open ? "transport" : "signaling",
			address);

	if (session->state == AVDTP_SESSION_STATE_CONNECTING) {
		debug("AVDTP imtu=%u, omtu=%u", session->imtu, session->omtu);

		session->buf = g_malloc0(session->imtu);
		avdtp_set_state(session, AVDTP_SESSION_STATE_CONNECTED);

		if (session->io_id)
			g_source_remove(session->io_id);

		/* This watch should be low priority since otherwise the
		 * connect callback might be dispatched before the session
		 * callback if the kernel wakes us up at the same time for
		 * them. This could happen if a headset is very quick in
		 * sending the Start command after connecting the stream
		 * transport channel.
		 */
		session->io_id = g_io_add_watch_full(chan,
						G_PRIORITY_LOW,
						G_IO_IN | G_IO_ERR | G_IO_HUP
						| G_IO_NVAL,
						(GIOFunc) session_cb, session,
						NULL);

		if (session->stream_setup) {
			set_disconnect_timer(session);
			avdtp_set_auto_disconnect(session, FALSE);
		}
	} else if (session->pending_open)
		handle_transport_connect(session, chan, session->imtu,
								session->omtu);
	else
		goto failed;

	process_queue(session);

	return;

failed:
	if (session->pending_open) {
		struct avdtp_stream *stream = session->pending_open;

		handle_transport_connect(session, NULL, 0, 0);

		if (avdtp_abort(session, stream) < 0)
			avdtp_sep_set_state(session, stream->lsep,
						AVDTP_STATE_IDLE);
	} else
		connection_lost(session, EIO);

	return;
}

static void auth_cb(DBusError *derr, void *user_data)
{
	struct avdtp *session = user_data;
	GError *err = NULL;

	if (derr && dbus_error_is_set(derr)) {
		error("Access denied: %s", derr->message);
		connection_lost(session, EACCES);
		return;
	}

	if (!bt_io_accept(session->io, avdtp_connect_cb, session, NULL,
								&err)) {
		error("bt_io_accept: %s", err->message);
		connection_lost(session, EACCES);
		g_error_free(err);
		return;
	}

	/* This is so that avdtp_connect_cb will know to do the right thing
	 * with respect to the disconnect timer */
	session->stream_setup = TRUE;
}

static void avdtp_confirm_cb(GIOChannel *chan, gpointer data)
{
	struct avdtp *session;
	struct audio_device *dev;
	char address[18];
	bdaddr_t src, dst;
	int perr;
	GError *err = NULL;

	bt_io_get(chan, BT_IO_L2CAP, &err,
			BT_IO_OPT_SOURCE_BDADDR, &src,
			BT_IO_OPT_DEST_BDADDR, &dst,
			BT_IO_OPT_DEST, address,
			BT_IO_OPT_INVALID);
	if (err) {
		error("%s", err->message);
		g_error_free(err);
		goto drop;
	}

	debug("AVDTP: incoming connect from %s", address);

	session = avdtp_get_internal(&src, &dst);

	if (session->pending_open && session->pending_open->open_acp) {
		if (!bt_io_accept(chan, avdtp_connect_cb, session, NULL, NULL))
			goto drop;
		return;
	}

	if (session->io) {
		error("Refusing unexpected connect from %s", address);
		goto drop;
	}

	dev = manager_get_device(&src, &dst);
	if (!dev) {
		error("Unable to get audio device object for %s", address);
		goto drop;
	}

	session->io = g_io_channel_ref(chan);
	avdtp_set_state(session, AVDTP_SESSION_STATE_CONNECTING);

	session->io_id = g_io_add_watch(chan, G_IO_ERR | G_IO_HUP | G_IO_NVAL,
					(GIOFunc) session_cb, session);
	perr = btd_request_authorization(&src, &dst, ADVANCED_AUDIO_UUID,
							auth_cb, session);
	if (perr < 0) {
		avdtp_unref(session);
		goto drop;
	}

	dev->auto_connect = auto_connect;

	return;

drop:
	g_io_channel_shutdown(chan, TRUE, NULL);
}

static int l2cap_connect(struct avdtp *session)
{
	GError *err = NULL;
	GIOChannel *io;

	io = bt_io_connect(BT_IO_L2CAP, avdtp_connect_cb, session,
				NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, &session->server->src,
				BT_IO_OPT_DEST_BDADDR, &session->dst,
				BT_IO_OPT_PSM, AVDTP_PSM,
				BT_IO_OPT_INVALID);
	if (!io) {
		error("%s", err->message);
		g_error_free(err);
		return -EIO;
	}

	g_io_channel_unref(io);

	return 0;
}

static void queue_request(struct avdtp *session, struct pending_req *req,
			gboolean priority)
{
	if (priority)
		session->prio_queue = g_slist_append(session->prio_queue, req);
	else
		session->req_queue = g_slist_append(session->req_queue, req);
}

static gboolean request_timeout(gpointer user_data)
{
	struct avdtp *session = user_data;
	struct pending_req *req;
	struct seid_req sreq;
	struct avdtp_local_sep *lsep;
	struct avdtp_stream *stream;
	uint8_t seid;
	struct avdtp_error err;

	req = session->req;
	session->req = NULL;

	avdtp_error_init(&err, AVDTP_ERROR_ERRNO, ETIMEDOUT);

	seid = ((struct seid_req *) (req->data))->acp_seid;

	stream = find_stream_by_rseid(session, seid);

	if (stream)
		lsep = stream->lsep;
	else
		lsep = NULL;

	switch (req->signal_id) {
	case AVDTP_RECONFIGURE:
		error("Reconfigure request timed out");
		if (lsep && lsep->cfm && lsep->cfm->reconfigure)
			lsep->cfm->reconfigure(session, lsep, stream, &err,
						lsep->user_data);
		break;
	case AVDTP_OPEN:
		error("Open request timed out");
		if (lsep && lsep->cfm && lsep->cfm->open)
			lsep->cfm->open(session, lsep, stream, &err,
					lsep->user_data);
		break;
	case AVDTP_START:
		error("Start request timed out");
		if (lsep && lsep->cfm && lsep->cfm->start)
			lsep->cfm->start(session, lsep, stream, &err,
						lsep->user_data);
		break;
	case AVDTP_SUSPEND:
		error("Suspend request timed out");
		if (lsep && lsep->cfm && lsep->cfm->suspend)
			lsep->cfm->suspend(session, lsep, stream, &err,
						lsep->user_data);
		break;
	case AVDTP_CLOSE:
		error("Close request timed out");
		if (lsep && lsep->cfm && lsep->cfm->close)
			lsep->cfm->close(session, lsep, stream, &err,
						lsep->user_data);
		break;
	case AVDTP_SET_CONFIGURATION:
		error("SetConfiguration request timed out");
		if (lsep && lsep->cfm && lsep->cfm->set_configuration)
			lsep->cfm->set_configuration(session, lsep, stream,
							&err, lsep->user_data);
		goto failed;
	case AVDTP_DISCOVER:
		error("Discover request timed out");
		goto failed;
	case AVDTP_GET_CAPABILITIES:
		error("GetCapabilities request timed out");
		goto failed;
	case AVDTP_ABORT:
		error("Abort request timed out");
		goto failed;
	}

	memset(&sreq, 0, sizeof(sreq));
	sreq.acp_seid = seid;

	if (send_request(session, TRUE, stream, AVDTP_ABORT,
						&sreq, sizeof(sreq)) < 0) {
		error("Unable to send abort request");
		goto failed;
	}

	goto done;

failed:
	connection_lost(session, ETIMEDOUT);
done:
	pending_req_free(req);
	return FALSE;
}

static int send_req(struct avdtp *session, gboolean priority,
			struct pending_req *req)
{
	static int transaction = 0;
	int err;

	if (session->state == AVDTP_SESSION_STATE_DISCONNECTED) {
		err = l2cap_connect(session);
		if (err < 0)
			goto failed;
		avdtp_set_state(session, AVDTP_SESSION_STATE_CONNECTING);
	}

	if (session->state < AVDTP_SESSION_STATE_CONNECTED ||
			session->req != NULL) {
		queue_request(session, req, priority);
		return 0;
	}

	req->transaction = transaction++;
	transaction %= 16;

	/* FIXME: Should we retry to send if the buffer
	was not totally sent or in case of EINTR? */
	if (!avdtp_send(session, req->transaction, AVDTP_MSG_TYPE_COMMAND,
				req->signal_id, req->data, req->data_size)) {
		err = -EIO;
		goto failed;
	}


	session->req = req;

	req->timeout = g_timeout_add_seconds(REQ_TIMEOUT,
					request_timeout,
					session);
	return 0;

failed:
	g_free(req->data);
	g_free(req);
	return err;
}

static int send_request(struct avdtp *session, gboolean priority,
			struct avdtp_stream *stream, uint8_t signal_id,
			void *buffer, size_t size)
{
	struct pending_req *req;

	req = g_new0(struct pending_req, 1);
	req->signal_id = signal_id;
	req->data = g_malloc(size);
	memcpy(req->data, buffer, size);
	req->data_size = size;
	req->stream = stream;

	return send_req(session, priority, req);
}

static gboolean avdtp_discover_resp(struct avdtp *session,
					struct discover_resp *resp, int size)
{
	int sep_count, i;

	sep_count = size / sizeof(struct seid_info);

	for (i = 0; i < sep_count; i++) {
		struct avdtp_remote_sep *sep;
		struct avdtp_stream *stream;
		struct seid_req req;
		int ret;

		debug("seid %d type %d media %d in use %d",
				resp->seps[i].seid, resp->seps[i].type,
				resp->seps[i].media_type, resp->seps[i].inuse);

		stream = find_stream_by_rseid(session, resp->seps[i].seid);

		sep = find_remote_sep(session->seps, resp->seps[i].seid);
		if (!sep) {
			if (resp->seps[i].inuse && !stream)
				continue;
			sep = g_new0(struct avdtp_remote_sep, 1);
			session->seps = g_slist_append(session->seps, sep);
		}

		sep->stream = stream;
		sep->seid = resp->seps[i].seid;
		sep->type = resp->seps[i].type;
		sep->media_type = resp->seps[i].media_type;

		memset(&req, 0, sizeof(req));
		req.acp_seid = sep->seid;

		ret = send_request(session, TRUE, NULL,
					AVDTP_GET_CAPABILITIES,
					&req, sizeof(req));
		if (ret < 0) {
			finalize_discovery(session, -ret);
			break;
		}
	}

	return TRUE;
}

static gboolean avdtp_get_capabilities_resp(struct avdtp *session,
						struct getcap_resp *resp,
						unsigned int size)
{
	struct avdtp_remote_sep *sep;
	uint8_t seid;

	/* Check for minimum required packet size includes:
	 *   1. getcap resp header
	 *   2. media transport capability (2 bytes)
	 *   3. media codec capability type + length (2 bytes)
	 *   4. the actual media codec elements
	 * */
	if (size < (sizeof(struct getcap_resp) + 4 +
				sizeof(struct avdtp_media_codec_capability))) {
		error("Too short getcap resp packet");
		return FALSE;
	}

	seid = ((struct seid_req *) session->req->data)->acp_seid;

	sep = find_remote_sep(session->seps, seid);

	debug("seid %d type %d media %d", sep->seid,
					sep->type, sep->media_type);

	if (sep->caps) {
		g_slist_foreach(sep->caps, (GFunc) g_free, NULL);
		g_slist_free(sep->caps);
		sep->caps = NULL;
		sep->codec = NULL;
	}

	sep->caps = caps_to_list(resp->caps, size - sizeof(struct getcap_resp),
					&sep->codec);

	return TRUE;
}

static gboolean avdtp_set_configuration_resp(struct avdtp *session,
						struct avdtp_stream *stream,
						struct avdtp_single_header *resp,
						int size)
{
	struct avdtp_local_sep *sep = stream->lsep;

	if (sep->cfm && sep->cfm->set_configuration)
		sep->cfm->set_configuration(session, sep, stream, NULL,
						sep->user_data);

	avdtp_sep_set_state(session, sep, AVDTP_STATE_CONFIGURED);

	return TRUE;
}

static gboolean avdtp_reconfigure_resp(struct avdtp *session,
					struct avdtp_stream *stream,
					struct avdtp_single_header *resp, int size)
{
	return TRUE;
}

static gboolean avdtp_open_resp(struct avdtp *session, struct avdtp_stream *stream,
				struct seid_rej *resp, int size)
{
	struct avdtp_local_sep *sep = stream->lsep;

	if (l2cap_connect(session) < 0) {
		avdtp_sep_set_state(session, sep, AVDTP_STATE_IDLE);
		return FALSE;
	}

	session->pending_open = stream;

	return TRUE;
}

static gboolean avdtp_start_resp(struct avdtp *session,
					struct avdtp_stream *stream,
					struct seid_rej *resp, int size)
{
	struct avdtp_local_sep *sep = stream->lsep;

	if (sep->cfm && sep->cfm->start)
		sep->cfm->start(session, sep, stream, NULL, sep->user_data);

	/* We might be in STREAMING already if both sides send START_CMD at the
	 * same time and the one in SNK role doesn't reject it as it should */
	if (sep->state != AVDTP_STATE_STREAMING)
		avdtp_sep_set_state(session, sep, AVDTP_STATE_STREAMING);

	return TRUE;
}

static gboolean avdtp_close_resp(struct avdtp *session,
					struct avdtp_stream *stream,
					struct seid_rej *resp, int size)
{
	struct avdtp_local_sep *sep = stream->lsep;

	avdtp_sep_set_state(session, sep, AVDTP_STATE_CLOSING);

	g_io_channel_shutdown(stream->io, TRUE, NULL);

	return TRUE;
}

static gboolean avdtp_suspend_resp(struct avdtp *session,
					struct avdtp_stream *stream,
					void *data, int size)
{
	struct avdtp_local_sep *sep = stream->lsep;

	avdtp_sep_set_state(session, sep, AVDTP_STATE_OPEN);

	if (sep->cfm && sep->cfm->suspend)
		sep->cfm->suspend(session, sep, stream, NULL, sep->user_data);

	return TRUE;
}

static gboolean avdtp_abort_resp(struct avdtp *session,
					struct avdtp_stream *stream,
					struct seid_rej *resp, int size)
{
	struct avdtp_local_sep *sep = stream->lsep;

	if (sep->cfm && sep->cfm->abort)
		sep->cfm->abort(session, sep, stream, NULL, sep->user_data);

	avdtp_sep_set_state(session, sep, AVDTP_STATE_IDLE);

	return TRUE;
}

static gboolean avdtp_parse_resp(struct avdtp *session,
					struct avdtp_stream *stream,
					uint8_t transaction, uint8_t signal_id,
					void *buf, int size)
{
	struct pending_req *next;

	if (session->prio_queue)
		next = session->prio_queue->data;
	else if (session->req_queue)
		next = session->req_queue->data;
	else
		next = NULL;

	switch (signal_id) {
	case AVDTP_DISCOVER:
		debug("DISCOVER request succeeded");
		return avdtp_discover_resp(session, buf, size);
	case AVDTP_GET_CAPABILITIES:
		debug("GET_CAPABILITIES request succeeded");
		if (!avdtp_get_capabilities_resp(session, buf, size))
			return FALSE;
		if (!(next && next->signal_id == AVDTP_GET_CAPABILITIES))
			finalize_discovery(session, 0);
		return TRUE;
	case AVDTP_SET_CONFIGURATION:
		debug("SET_CONFIGURATION request succeeded");
		return avdtp_set_configuration_resp(session, stream,
								buf, size);
	case AVDTP_RECONFIGURE:
		debug("RECONFIGURE request succeeded");
		return avdtp_reconfigure_resp(session, stream, buf, size);
	case AVDTP_OPEN:
		debug("OPEN request succeeded");
		return avdtp_open_resp(session, stream, buf, size);
	case AVDTP_SUSPEND:
		debug("SUSPEND request succeeded");
		return avdtp_suspend_resp(session, stream, buf, size);
	case AVDTP_START:
		debug("START request succeeded");
		return avdtp_start_resp(session, stream, buf, size);
	case AVDTP_CLOSE:
		debug("CLOSE request succeeded");
		return avdtp_close_resp(session, stream, buf, size);
	case AVDTP_ABORT:
		debug("ABORT request succeeded");
		return avdtp_abort_resp(session, stream, buf, size);
	}

	error("Unknown signal id in accept response: %u", signal_id);
	return TRUE;
}

static gboolean seid_rej_to_err(struct seid_rej *rej, unsigned int size,
					struct avdtp_error *err)
{
	if (size < sizeof(struct seid_rej)) {
		error("Too small packet for seid_rej");
		return FALSE;
	}

	avdtp_error_init(err, AVDTP_ERROR_ERROR_CODE, rej->error);

	return TRUE;
}

static gboolean conf_rej_to_err(struct conf_rej *rej, unsigned int size,
				struct avdtp_error *err, uint8_t *category)
{
	if (size < sizeof(struct conf_rej)) {
		error("Too small packet for conf_rej");
		return FALSE;
	}

	avdtp_error_init(err, AVDTP_ERROR_ERROR_CODE, rej->error);

	if (category)
		*category = rej->category;

	return TRUE;
}

static gboolean stream_rej_to_err(struct stream_rej *rej, unsigned int size,
					struct avdtp_error *err,
					uint8_t *acp_seid)
{
	if (size < sizeof(struct stream_rej)) {
		error("Too small packet for stream_rej");
		return FALSE;
	}

	avdtp_error_init(err, AVDTP_ERROR_ERROR_CODE, rej->error);

	if (acp_seid)
		*acp_seid = rej->acp_seid;

	return TRUE;
}

static gboolean avdtp_parse_rej(struct avdtp *session,
					struct avdtp_stream *stream,
					uint8_t transaction, uint8_t signal_id,
					void *buf, int size)
{
	struct avdtp_error err;
	uint8_t acp_seid, category;
	struct avdtp_local_sep *sep = stream ? stream->lsep : NULL;

	switch (signal_id) {
	case AVDTP_DISCOVER:
		if (!seid_rej_to_err(buf, size, &err))
			return FALSE;
		error("DISCOVER request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		return TRUE;
	case AVDTP_GET_CAPABILITIES:
		if (!seid_rej_to_err(buf, size, &err))
			return FALSE;
		error("GET_CAPABILITIES request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		return TRUE;
	case AVDTP_OPEN:
		if (!seid_rej_to_err(buf, size, &err))
			return FALSE;
		error("OPEN request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		if (sep && sep->cfm && sep->cfm->open)
			sep->cfm->open(session, sep, stream, &err,
					sep->user_data);
		return TRUE;
	case AVDTP_SET_CONFIGURATION:
		if (!conf_rej_to_err(buf, size, &err, &category))
			return FALSE;
		error("SET_CONFIGURATION request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		if (sep && sep->cfm && sep->cfm->set_configuration)
			sep->cfm->set_configuration(session, sep, stream,
							&err, sep->user_data);
		return TRUE;
	case AVDTP_RECONFIGURE:
		if (!conf_rej_to_err(buf, size, &err, &category))
			return FALSE;
		error("RECONFIGURE request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		if (sep && sep->cfm && sep->cfm->reconfigure)
			sep->cfm->reconfigure(session, sep, stream, &err,
						sep->user_data);
		return TRUE;
	case AVDTP_START:
		if (!stream_rej_to_err(buf, size, &err, &acp_seid))
			return FALSE;
		error("START request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		if (sep && sep->cfm && sep->cfm->start)
			sep->cfm->start(session, sep, stream, &err,
					sep->user_data);
		return TRUE;
	case AVDTP_SUSPEND:
		if (!stream_rej_to_err(buf, size, &err, &acp_seid))
			return FALSE;
		error("SUSPEND request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		if (sep && sep->cfm && sep->cfm->suspend)
			sep->cfm->suspend(session, sep, stream, &err,
						sep->user_data);
		return TRUE;
	case AVDTP_CLOSE:
		if (!stream_rej_to_err(buf, size, &err, &acp_seid))
			return FALSE;
		error("CLOSE request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		if (sep && sep->cfm && sep->cfm->close)
			sep->cfm->close(session, sep, stream, &err,
					sep->user_data);
		return TRUE;
	case AVDTP_ABORT:
		if (!stream_rej_to_err(buf, size, &err, &acp_seid))
			return FALSE;
		error("ABORT request rejected: %s (%d)",
				avdtp_strerror(&err), err.err.error_code);
		if (sep && sep->cfm && sep->cfm->abort)
			sep->cfm->abort(session, sep, stream, &err,
					sep->user_data);
		return TRUE;
	default:
		error("Unknown reject response signal id: %u", signal_id);
		return TRUE;
	}
}

gboolean avdtp_is_connected(const bdaddr_t *src, const bdaddr_t *dst)
{
	struct avdtp *session;

	session = find_session(src, dst);

	if (!session)
		return FALSE;

	if (session->state != AVDTP_SESSION_STATE_DISCONNECTED)
		return TRUE;

	return FALSE;
}

struct avdtp_service_capability *avdtp_stream_get_codec(
						struct avdtp_stream *stream)
{
	GSList *l;

	for (l = stream->caps; l; l = l->next) {
		struct avdtp_service_capability *cap = l->data;

		if (cap->category == AVDTP_MEDIA_CODEC)
			return cap;
	}

	return NULL;
}

gboolean avdtp_stream_has_capability(struct avdtp_stream *stream,
				struct avdtp_service_capability *cap)
{
	GSList *l;
	struct avdtp_service_capability *stream_cap;

	for (l = stream->caps; l; l = g_slist_next(l)) {
		stream_cap = l->data;

		if (stream_cap->category != cap->category ||
			stream_cap->length != cap->length)
			continue;

		if (memcmp(stream_cap->data, cap->data, cap->length) == 0)
			return TRUE;
	}

	return FALSE;
}

gboolean avdtp_stream_has_capabilities(struct avdtp_stream *stream,
					GSList *caps)
{
	GSList *l;

	for (l = caps; l; l = g_slist_next(l)) {
		struct avdtp_service_capability *cap = l->data;

		if (!avdtp_stream_has_capability(stream, cap))
			return FALSE;
	}

	return TRUE;
}

gboolean avdtp_stream_get_transport(struct avdtp_stream *stream, int *sock,
					uint16_t *imtu, uint16_t *omtu,
					GSList **caps)
{
	if (stream->io == NULL)
		return FALSE;

	if (sock)
		*sock = g_io_channel_unix_get_fd(stream->io);

	if (omtu)
		*omtu = stream->omtu;

	if (imtu)
		*imtu = stream->imtu;

	if (caps)
		*caps = stream->caps;

	return TRUE;
}

static int process_queue(struct avdtp *session)
{
	GSList **queue, *l;
	struct pending_req *req;

	if (session->req)
		return 0;

	if (session->prio_queue)
		queue = &session->prio_queue;
	else
		queue = &session->req_queue;

	if (!*queue)
		return 0;

	l = *queue;
	req = l->data;

	*queue = g_slist_remove(*queue, req);

	return send_req(session, FALSE, req);
}

struct avdtp_remote_sep *avdtp_get_remote_sep(struct avdtp *session,
						uint8_t seid)
{
	GSList *l;

	for (l = session->seps; l; l = l->next) {
		struct avdtp_remote_sep *sep = l->data;

		if (sep->seid == seid)
			return sep;
	}

	return NULL;
}

uint8_t avdtp_get_seid(struct avdtp_remote_sep *sep)
{
	return sep->seid;
}

uint8_t avdtp_get_type(struct avdtp_remote_sep *sep)
{
	return sep->type;
}

struct avdtp_service_capability *avdtp_get_codec(struct avdtp_remote_sep *sep)
{
	return sep->codec;
}

struct avdtp_stream *avdtp_get_stream(struct avdtp_remote_sep *sep)
{
	return sep->stream;
}

struct avdtp_service_capability *avdtp_service_cap_new(uint8_t category,
							void *data, int length)
{
	struct avdtp_service_capability *cap;

	if (category < AVDTP_MEDIA_TRANSPORT || category > AVDTP_MEDIA_CODEC)
		return NULL;

	cap = g_malloc(sizeof(struct avdtp_service_capability) + length);
	cap->category = category;
	cap->length = length;
	memcpy(cap->data, data, length);

	return cap;
}

static gboolean process_discover(gpointer data)
{
	struct avdtp *session = data;

	finalize_discovery(session, 0);

	return FALSE;
}

int avdtp_discover(struct avdtp *session, avdtp_discover_cb_t cb,
			void *user_data)
{
	int ret;

	if (session->discov_cb)
		return -EBUSY;

	if (session->seps) {
		session->discov_cb = cb;
		session->user_data = user_data;
		g_idle_add(process_discover, session);
		return 0;
	}

	ret = send_request(session, FALSE, NULL, AVDTP_DISCOVER, NULL, 0);
	if (ret == 0) {
		session->discov_cb = cb;
		session->user_data = user_data;
	}

	return ret;
}

int avdtp_get_seps(struct avdtp *session, uint8_t acp_type, uint8_t media_type,
			uint8_t codec, struct avdtp_local_sep **lsep,
			struct avdtp_remote_sep **rsep)
{
	GSList *l;
	uint8_t int_type;

	int_type = acp_type == AVDTP_SEP_TYPE_SINK ?
				AVDTP_SEP_TYPE_SOURCE : AVDTP_SEP_TYPE_SINK;

	*lsep = find_local_sep(session->server, int_type, media_type, codec);
	if (!*lsep)
		return -EINVAL;

	for (l = session->seps; l != NULL; l = g_slist_next(l)) {
		struct avdtp_remote_sep *sep = l->data;
		struct avdtp_service_capability *cap;
		struct avdtp_media_codec_capability *codec_data;

		if (sep->type != acp_type)
			continue;

		if (sep->media_type != media_type)
			continue;

		if (!sep->codec)
			continue;

		cap = sep->codec;
		codec_data = (void *) cap->data;

		if (codec_data->media_codec_type != codec)
			continue;

		if (!sep->stream) {
			*rsep = sep;
			return 0;
		}
	}

	return -EINVAL;
}

gboolean avdtp_stream_remove_cb(struct avdtp *session,
				struct avdtp_stream *stream,
				unsigned int id)
{
	GSList *l;
	struct stream_callback *cb;

	if (!stream)
		return FALSE;

	for (cb = NULL, l = stream->callbacks; l != NULL; l = l->next) {
		struct stream_callback *tmp = l->data;
		if (tmp && tmp->id == id) {
			cb = tmp;
			break;
		}
	}

	if (!cb)
		return FALSE;

	stream->callbacks = g_slist_remove(stream->callbacks, cb);
	g_free(cb);

	return TRUE;
}

unsigned int avdtp_stream_add_cb(struct avdtp *session,
					struct avdtp_stream *stream,
					avdtp_stream_state_cb cb, void *data)
{
	struct stream_callback *stream_cb;
	static unsigned int id = 0;

	stream_cb = g_new(struct stream_callback, 1);
	stream_cb->cb = cb;
	stream_cb->user_data = data;
	stream_cb->id = ++id;

	stream->callbacks = g_slist_append(stream->callbacks, stream_cb);;

	return stream_cb->id;
}

int avdtp_get_configuration(struct avdtp *session, struct avdtp_stream *stream)
{
	struct seid_req req;

	if (session->state < AVDTP_SESSION_STATE_CONNECTED)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.acp_seid = stream->rseid;

	return send_request(session, FALSE, stream, AVDTP_GET_CONFIGURATION,
							&req, sizeof(req));
}

static void copy_capabilities(gpointer data, gpointer user_data)
{
	struct avdtp_service_capability *src_cap = data;
	struct avdtp_service_capability *dst_cap;
	GSList **l = user_data;

	dst_cap = avdtp_service_cap_new(src_cap->category, src_cap->data,
					src_cap->length);

	*l = g_slist_append(*l, dst_cap);
}

int avdtp_set_configuration(struct avdtp *session,
				struct avdtp_remote_sep *rsep,
				struct avdtp_local_sep *lsep,
				GSList *caps,
				struct avdtp_stream **stream)
{
	struct setconf_req *req;
	struct avdtp_stream *new_stream;
	unsigned char *ptr;
	int ret, caps_len;
	struct avdtp_service_capability *cap;
	GSList *l;

	if (session->state != AVDTP_SESSION_STATE_CONNECTED)
		return -ENOTCONN;

	if (!(lsep && rsep))
		return -EINVAL;

	debug("avdtp_set_configuration(%p): int_seid=%u, acp_seid=%u",
			session, lsep->info.seid, rsep->seid);

	new_stream = g_new0(struct avdtp_stream, 1);
	new_stream->session = session;
	new_stream->lsep = lsep;
	new_stream->rseid = rsep->seid;

	g_slist_foreach(caps, copy_capabilities, &new_stream->caps);

	/* Calculate total size of request */
	for (l = caps, caps_len = 0; l != NULL; l = g_slist_next(l)) {
		cap = l->data;
		caps_len += cap->length + 2;
	}

	req = g_malloc0(sizeof(struct setconf_req) + caps_len);

	req->int_seid = lsep->info.seid;
	req->acp_seid = rsep->seid;

	/* Copy the capabilities into the request */
	for (l = caps, ptr = req->caps; l != NULL; l = g_slist_next(l)) {
		cap = l->data;
		memcpy(ptr, cap, cap->length + 2);
		ptr += cap->length + 2;
	}

	ret = send_request(session, FALSE, new_stream,
				AVDTP_SET_CONFIGURATION, req,
				sizeof(struct setconf_req) + caps_len);
	if (ret < 0)
		stream_free(new_stream);
	else {
		lsep->info.inuse = 1;
		lsep->stream = new_stream;
		rsep->stream = new_stream;
		session->streams = g_slist_append(session->streams, new_stream);
		if (stream)
			*stream = new_stream;
	}

	g_free(req);

	return ret;
}

int avdtp_reconfigure(struct avdtp *session, GSList *caps,
			struct avdtp_stream *stream)
{
	struct reconf_req *req;
	unsigned char *ptr;
	int caps_len, err;
	GSList *l;
	struct avdtp_service_capability *cap;

	if (!g_slist_find(session->streams, stream))
		return -EINVAL;

	if (stream->lsep->state != AVDTP_STATE_OPEN)
		return -EINVAL;

	/* Calculate total size of request */
	for (l = caps, caps_len = 0; l != NULL; l = g_slist_next(l)) {
		cap = l->data;
		caps_len += cap->length + 2;
	}

	req = g_malloc0(sizeof(struct reconf_req) + caps_len);

	req->acp_seid = stream->rseid;

	/* Copy the capabilities into the request */
	for (l = caps, ptr = req->caps; l != NULL; l = g_slist_next(l)) {
		cap = l->data;
		memcpy(ptr, cap, cap->length + 2);
		ptr += cap->length + 2;
	}

	err = send_request(session, FALSE, stream, AVDTP_RECONFIGURE, req,
						sizeof(*req) + caps_len);
	g_free(req);

	return err;
}

int avdtp_open(struct avdtp *session, struct avdtp_stream *stream)
{
	struct seid_req req;

	if (!g_slist_find(session->streams, stream))
		return -EINVAL;

	if (stream->lsep->state > AVDTP_STATE_CONFIGURED)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.acp_seid = stream->rseid;

	return send_request(session, FALSE, stream, AVDTP_OPEN,
							&req, sizeof(req));
}

int avdtp_start(struct avdtp *session, struct avdtp_stream *stream)
{
	struct start_req req;

	if (!g_slist_find(session->streams, stream))
		return -EINVAL;

	if (stream->lsep->state != AVDTP_STATE_OPEN)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.first_seid.seid = stream->rseid;

	return send_request(session, FALSE, stream, AVDTP_START,
							&req, sizeof(req));
}

int avdtp_close(struct avdtp *session, struct avdtp_stream *stream)
{
	struct seid_req req;
	int ret;

	if (!g_slist_find(session->streams, stream))
		return -EINVAL;

	if (stream->lsep->state < AVDTP_STATE_OPEN)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.acp_seid = stream->rseid;

	ret = send_request(session, FALSE, stream, AVDTP_CLOSE,
							&req, sizeof(req));
	if (ret == 0)
		stream->close_int = TRUE;

	return ret;
}

int avdtp_suspend(struct avdtp *session, struct avdtp_stream *stream)
{
	struct seid_req req;

	if (!g_slist_find(session->streams, stream))
		return -EINVAL;

	if (stream->lsep->state <= AVDTP_STATE_OPEN)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.acp_seid = stream->rseid;

	return send_request(session, FALSE, stream, AVDTP_SUSPEND,
							&req, sizeof(req));
}

int avdtp_abort(struct avdtp *session, struct avdtp_stream *stream)
{
	struct seid_req req;
	int ret;

	if (!g_slist_find(session->streams, stream))
		return -EINVAL;

	if (stream->lsep->state <= AVDTP_STATE_OPEN)
		return -EINVAL;

	memset(&req, 0, sizeof(req));
	req.acp_seid = stream->rseid;

	ret = send_request(session, TRUE, stream, AVDTP_ABORT,
							&req, sizeof(req));
	if (ret == 0)
		avdtp_sep_set_state(session, stream->lsep,
					AVDTP_STATE_ABORTING);

	return ret;
}

struct avdtp_local_sep *avdtp_register_sep(const bdaddr_t *src, uint8_t type,
						uint8_t media_type,
						uint8_t codec_type,
						struct avdtp_sep_ind *ind,
						struct avdtp_sep_cfm *cfm,
						void *user_data)
{
	struct avdtp_server *server;
	struct avdtp_local_sep *sep;

	server = find_server(servers, src);
	if (!server)
		return NULL;

	if (g_slist_length(server->seps) > MAX_SEID)
		return NULL;

	sep = g_new0(struct avdtp_local_sep, 1);

	sep->state = AVDTP_STATE_IDLE;
	sep->info.seid = g_slist_length(server->seps) + 1;
	sep->info.type = type;
	sep->info.media_type = media_type;
	sep->codec = codec_type;
	sep->ind = ind;
	sep->cfm = cfm;
	sep->user_data = user_data;
	sep->server = server;

	debug("SEP %p registered: type:%d codec:%d seid:%d", sep,
			sep->info.type, sep->codec, sep->info.seid);
	server->seps = g_slist_append(server->seps, sep);

	return sep;
}

int avdtp_unregister_sep(struct avdtp_local_sep *sep)
{
	struct avdtp_server *server;

	if (!sep)
		return -EINVAL;

	if (sep->info.inuse)
		return -EBUSY;

	server = sep->server;
	server->seps = g_slist_remove(server->seps, sep);

	if (sep->stream)
		avdtp_sep_set_state(sep->stream->session, sep,
							AVDTP_STATE_IDLE);

	g_free(sep);

	return 0;
}

static GIOChannel *avdtp_server_socket(const bdaddr_t *src, gboolean master)
{
	GError *err = NULL;
	GIOChannel *io;

	io = bt_io_listen(BT_IO_L2CAP, NULL, avdtp_confirm_cb,
				NULL, NULL, &err,
				BT_IO_OPT_SOURCE_BDADDR, src,
				BT_IO_OPT_PSM, AVDTP_PSM,
				BT_IO_OPT_SEC_LEVEL, BT_IO_SEC_MEDIUM,
				BT_IO_OPT_MASTER, master,
				BT_IO_OPT_INVALID);
	if (!io) {
		error("%s", err->message);
		g_error_free(err);
	}

	return io;
}

const char *avdtp_strerror(struct avdtp_error *err)
{
	if (err->type == AVDTP_ERROR_ERRNO)
		return strerror(err->err.posix_errno);

	switch(err->err.error_code) {
	case AVDTP_BAD_HEADER_FORMAT:
		return "Bad Header Format";
	case AVDTP_BAD_LENGTH:
		return "Bad Packet Lenght";
	case AVDTP_BAD_ACP_SEID:
		return "Bad Acceptor SEID";
	case AVDTP_SEP_IN_USE:
		return "Stream End Point in Use";
	case AVDTP_SEP_NOT_IN_USE:
		return "Stream End Point Not in Use";
	case AVDTP_BAD_SERV_CATEGORY:
		return "Bad Service Category";
	case AVDTP_BAD_PAYLOAD_FORMAT:
		return "Bad Payload format";
	case AVDTP_NOT_SUPPORTED_COMMAND:
		return "Command Not Supported";
	case AVDTP_INVALID_CAPABILITIES:
		return "Invalid Capabilities";
	case AVDTP_BAD_RECOVERY_TYPE:
		return "Bad Recovery Type";
	case AVDTP_BAD_MEDIA_TRANSPORT_FORMAT:
		return "Bad Media Transport Format";
	case AVDTP_BAD_RECOVERY_FORMAT:
		return "Bad Recovery Format";
	case AVDTP_BAD_ROHC_FORMAT:
		return "Bad Header Compression Format";
	case AVDTP_BAD_CP_FORMAT:
		return "Bad Content Protetion Format";
	case AVDTP_BAD_MULTIPLEXING_FORMAT:
		return "Bad Multiplexing Format";
	case AVDTP_UNSUPPORTED_CONFIGURATION:
		return "Configuration not supported";
	case AVDTP_BAD_STATE:
		return "Bad State";
	default:
		return "Unknow error";
	}
}

avdtp_state_t avdtp_sep_get_state(struct avdtp_local_sep *sep)
{
	return sep->state;
}

void avdtp_get_peers(struct avdtp *session, bdaddr_t *src, bdaddr_t *dst)
{
	if (src)
		bacpy(src, &session->server->src);
	if (dst)
		bacpy(dst, &session->dst);
}

int avdtp_init(const bdaddr_t *src, GKeyFile *config)
{
	GError *err = NULL;
	gboolean tmp, master = TRUE;
	struct avdtp_server *server;

	if (config) {
		tmp = g_key_file_get_boolean(config, "General",
							"Master", &err);
		if (err) {
			debug("audio.conf: %s", err->message);
			g_clear_error(&err);
		} else
			master = tmp;

		tmp = g_key_file_get_boolean(config, "General", "AutoConnect",
									&err);
		if (err)
			g_clear_error(&err);
		else
			auto_connect = tmp;
	}

	server = g_new0(struct avdtp_server, 1);
	if (!server)
		return -ENOMEM;

	server->io = avdtp_server_socket(src, master);
	if (!server->io) {
		g_free(server);
		return -1;
	}

	bacpy(&server->src, src);

	servers = g_slist_append(servers, server);

	return 0;
}

void avdtp_exit(const bdaddr_t *src)
{
	struct avdtp_server *server;

	server = find_server(servers, src);
	if (!server)
		return;

	servers = g_slist_remove(servers, server);

	g_io_channel_shutdown(server->io, TRUE, NULL);
	g_io_channel_unref(server->io);
	g_free(server);
}

gboolean avdtp_has_stream(struct avdtp *session, struct avdtp_stream *stream)
{
	return g_slist_find(session->streams, stream) ? TRUE : FALSE;
}

void avdtp_set_auto_disconnect(struct avdtp *session, gboolean auto_dc)
{
	session->auto_dc = auto_dc;
}

gboolean avdtp_stream_setup_active(struct avdtp *session)
{
	return session->stream_setup;
}

unsigned int avdtp_add_state_cb(avdtp_session_state_cb cb, void *user_data)
{
	struct avdtp_state_callback *state_cb;
	static unsigned int id = 0;

	state_cb = g_new(struct avdtp_state_callback, 1);
	state_cb->cb = cb;
	state_cb->user_data = user_data;
	state_cb->id = ++id;

	avdtp_callbacks = g_slist_append(avdtp_callbacks, state_cb);;

	return state_cb->id;
}

gboolean avdtp_remove_state_cb(unsigned int id)
{
	GSList *l;

	for (l = avdtp_callbacks; l != NULL; l = l->next) {
		struct avdtp_state_callback *cb = l->data;
		if (cb && cb->id == id) {
			avdtp_callbacks = g_slist_remove(avdtp_callbacks, cb);
			g_free(cb);
			return TRUE;
		}
	}

	return FALSE;
}

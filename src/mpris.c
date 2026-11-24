#include "mpris.h"

#include "report.h"

#include <dbus/dbus.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OBJECT   "/org/mpris/MediaPlayer2"
#define NAME     "org.mpris.MediaPlayer2.nusic"
#define ROOT     "org.mpris.MediaPlayer2"
#define PLAYER   "org.mpris.MediaPlayer2.Player"
#define PROPS    "org.freedesktop.DBus.Properties"
#define INTRO    "org.freedesktop.DBus.Introspectable"
#define SEEK_GAP 1500

// entry is the id of the queue entry that s shows, 0 when nothing plays, at the time s was taken in ms
static struct {
	const struct library* lib;
	DBusConnection*       conn;
	struct play_status    s;
	uint32_t              entry;
	int64_t               at;
	bool                  quit;
} m;

static const char* const root_props[] = { "CanQuit", "CanRaise", "HasTrackList", "Identity", "SupportedUriSchemes", "SupportedMimeTypes" };
static const char* const player_props[] = {
	"PlaybackStatus",
	"LoopStatus",
	"Rate",
	"Shuffle",
	"Metadata",
	"Volume",
	"Position",
	"MinimumRate",
	"MaximumRate",
	"CanGoNext",
	"CanGoPrevious",
	"CanPlay",
	"CanPause",
	"CanSeek",
	"CanControl",
};

static const char introspection[] =
        "<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\" \"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">"
        "<node>"
        "<interface name=\"org.freedesktop.DBus.Introspectable\"><method name=\"Introspect\"><arg name=\"xml\" type=\"s\" direction=\"out\"/></method></interface>"
        "<interface name=\"org.freedesktop.DBus.Properties\">"
        "<method name=\"Get\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"out\"/></method>"
        "<method name=\"GetAll\"><arg type=\"s\" direction=\"in\"/><arg type=\"a{sv}\" direction=\"out\"/></method>"
        "<method name=\"Set\"><arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/><arg type=\"v\" direction=\"in\"/></method>"
        "<signal name=\"PropertiesChanged\"><arg type=\"s\"/><arg type=\"a{sv}\"/><arg type=\"as\"/></signal>"
        "</interface>"
        "<interface name=\"org.mpris.MediaPlayer2\">"
        "<method name=\"Raise\"/><method name=\"Quit\"/>"
        "<property name=\"CanQuit\" type=\"b\" access=\"read\"/>"
        "<property name=\"CanRaise\" type=\"b\" access=\"read\"/>"
        "<property name=\"HasTrackList\" type=\"b\" access=\"read\"/>"
        "<property name=\"Identity\" type=\"s\" access=\"read\"/>"
        "<property name=\"SupportedUriSchemes\" type=\"as\" access=\"read\"/>"
        "<property name=\"SupportedMimeTypes\" type=\"as\" access=\"read\"/>"
        "</interface>"
        "<interface name=\"org.mpris.MediaPlayer2.Player\">"
        "<method name=\"Next\"/><method name=\"Previous\"/><method name=\"Pause\"/><method name=\"PlayPause\"/>"
        "<method name=\"Stop\"/><method name=\"Play\"/>"
        "<method name=\"Seek\"><arg name=\"Offset\" type=\"x\" direction=\"in\"/></method>"
        "<method name=\"SetPosition\"><arg name=\"TrackId\" type=\"o\" direction=\"in\"/><arg name=\"Position\" type=\"x\" direction=\"in\"/></method>"
        "<method name=\"OpenUri\"><arg name=\"Uri\" type=\"s\" direction=\"in\"/></method>"
        "<signal name=\"Seeked\"><arg name=\"Position\" type=\"x\"/></signal>"
        "<property name=\"PlaybackStatus\" type=\"s\" access=\"read\"/>"
        "<property name=\"LoopStatus\" type=\"s\" access=\"readwrite\"/>"
        "<property name=\"Rate\" type=\"d\" access=\"read\"/>"
        "<property name=\"Shuffle\" type=\"b\" access=\"read\"/>"
        "<property name=\"Metadata\" type=\"a{sv}\" access=\"read\"/>"
        "<property name=\"Volume\" type=\"d\" access=\"read\"/>"
        "<property name=\"Position\" type=\"x\" access=\"read\"/>"
        "<property name=\"MinimumRate\" type=\"d\" access=\"read\"/>"
        "<property name=\"MaximumRate\" type=\"d\" access=\"read\"/>"
        "<property name=\"CanGoNext\" type=\"b\" access=\"read\"/>"
        "<property name=\"CanGoPrevious\" type=\"b\" access=\"read\"/>"
        "<property name=\"CanPlay\" type=\"b\" access=\"read\"/>"
        "<property name=\"CanPause\" type=\"b\" access=\"read\"/>"
        "<property name=\"CanSeek\" type=\"b\" access=\"read\"/>"
        "<property name=\"CanControl\" type=\"b\" access=\"read\"/>"
        "</interface>"
        "</node>";

static int64_t now_ms(void) {
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);

	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// the bus closes the connection of a program that sends a string that is not utf-8
// such a string goes out with a question mark for every byte above ascii
static const char* clean(const char* s, char* buf, size_t n) {
	size_t i;

	if(dbus_validate_utf8(s, nullptr)) {
		return s;
	}

	for(i = 0; s[i] && i + 1 < n; i++) {
		buf[i] = (unsigned char)s[i] < 0x80 ? s[i] : '?';
	}

	buf[i] = 0;

	return buf;
}

static void variant(DBusMessageIter* it, int type, const char* sig, const void* value) {
	DBusMessageIter v;

	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, sig, &v);
	dbus_message_iter_append_basic(&v, type, value);
	dbus_message_iter_close_container(it, &v);
}

static void variant_string(DBusMessageIter* it, const char* s) {
	char buf[1024];

	s = clean(s, buf, sizeof(buf));
	variant(it, DBUS_TYPE_STRING, "s", &s);
}

// a variant that holds an array of strings with s in it, or with nothing
static void variant_strings(DBusMessageIter* it, const char* s) {
	char buf[1024];
	DBusMessageIter v;
	DBusMessageIter a;

	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "as", &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "s", &a);
	if(s) {
		s = clean(s, buf, sizeof(buf));
		dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &s);
	}

	dbus_message_iter_close_container(&v, &a);
	dbus_message_iter_close_container(it, &v);
}

static void entry_open(DBusMessageIter* dict, const char* key, DBusMessageIter* e) {
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, e);
	dbus_message_iter_append_basic(e, DBUS_TYPE_STRING, &key);
}

// file:// and the path, every byte that a uri does not take as it is becomes percent and two hex digits
static void file_uri(const char* path, char* buf, size_t n) {
	static const char hex[] = "0123456789ABCDEF";
	size_t len = (size_t)snprintf(buf, n, "file://");
	const unsigned char* c;

	for(c = (const unsigned char*)path; *c && len + 4 < n; c++) {
		if((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || strchr("/-._~", *c)) {
			buf[len++] = (char)*c;
		} else {
			buf[len++] = '%';
			buf[len++] = hex[*c >> 4];
			buf[len++] = hex[*c & 15];
		}
	}

	buf[len] = 0;
}

// the variant a{sv} that describes the track of m.s, an empty one when nothing plays
static void metadata(DBusMessageIter* it) {
	char buf[3 * PATH_MAX + 8];
	const struct track* t = m.s.track;
	const struct album* al;
	const char* p = buf;
	DBusMessageIter v;
	DBusMessageIter d;
	DBusMessageIter e;
	dbus_int64_t length;
	dbus_int32_t number;

	dbus_message_iter_open_container(it, DBUS_TYPE_VARIANT, "a{sv}", &v);
	dbus_message_iter_open_container(&v, DBUS_TYPE_ARRAY, "{sv}", &d);
	if(t) {
		al = &m.lib->albums[t->album];
		snprintf(buf, sizeof(buf), "/org/nusic/entry/%u", m.entry);
		entry_open(&d, "mpris:trackid", &e);
		variant(&e, DBUS_TYPE_OBJECT_PATH, "o", &p);
		dbus_message_iter_close_container(&d, &e);
		length = (dbus_int64_t)t->duration * 1000;
		entry_open(&d, "mpris:length", &e);
		variant(&e, DBUS_TYPE_INT64, "x", &length);
		dbus_message_iter_close_container(&d, &e);
		entry_open(&d, "xesam:title", &e);
		variant_string(&e, t->title);
		dbus_message_iter_close_container(&d, &e);
		entry_open(&d, "xesam:artist", &e);
		variant_strings(&e, t->artist);
		dbus_message_iter_close_container(&d, &e);
		entry_open(&d, "xesam:album", &e);
		variant_string(&e, al->title);
		dbus_message_iter_close_container(&d, &e);
		entry_open(&d, "xesam:albumArtist", &e);
		variant_strings(&e, m.lib->artists[al->artist].name);
		dbus_message_iter_close_container(&d, &e);
		if(t->number) {
			number = t->number;
			entry_open(&d, "xesam:trackNumber", &e);
			variant(&e, DBUS_TYPE_INT32, "i", &number);
			dbus_message_iter_close_container(&d, &e);
		}

		if(t->disc) {
			number = t->disc;
			entry_open(&d, "xesam:discNumber", &e);
			variant(&e, DBUS_TYPE_INT32, "i", &number);
			dbus_message_iter_close_container(&d, &e);
		}

		file_uri(t->path, buf, sizeof(buf));
		entry_open(&d, "xesam:url", &e);
		variant(&e, DBUS_TYPE_STRING, "s", &p);
		dbus_message_iter_close_container(&d, &e);
	}

	dbus_message_iter_close_container(&v, &d);
	dbus_message_iter_close_container(it, &v);
}

static const char* playback(enum play_state s) {
	if(s == PLAY_PLAYING) {
		return "Playing";
	}

	return s == PLAY_PAUSED ? "Paused" : "Stopped";
}

static const char* loop(enum play_repeat r) {
	if(r == REPEAT_TRACK) {
		return "Track";
	}

	return r == REPEAT_QUEUE ? "Playlist" : "None";
}

// appends the property as a variant, false when the interface has none of that name
// nusic has no volume and no shuffle, a caller reads that from the values
static bool property(DBusMessageIter* it, const char* iface, const char* name) {
	struct play_status s;
	dbus_bool_t yes = TRUE;
	dbus_bool_t no = FALSE;
	dbus_int64_t position;
	double value = 1.0;

	if(!strcmp(iface, ROOT)) {
		if(!strcmp(name, "Identity")) {
			variant_string(it, "nusic");
		} else if(!strcmp(name, "CanQuit")) {
			variant(it, DBUS_TYPE_BOOLEAN, "b", &yes);
		} else if(!strcmp(name, "CanRaise") || !strcmp(name, "HasTrackList")) {
			variant(it, DBUS_TYPE_BOOLEAN, "b", &no);
		} else if(!strcmp(name, "SupportedUriSchemes") || !strcmp(name, "SupportedMimeTypes")) {
			variant_strings(it, nullptr);
		} else {
			return false;
		}

		return true;
	}

	if(strcmp(iface, PLAYER)) {
		return false;
	}

	if(!strcmp(name, "PlaybackStatus")) {
		variant_string(it, playback(m.s.state));
	} else if(!strcmp(name, "LoopStatus")) {
		variant_string(it, loop(m.s.repeat));
	} else if(!strcmp(name, "Metadata")) {
		metadata(it);
	} else if(!strcmp(name, "Position")) {
		player_status(&s);
		position = (dbus_int64_t)s.position * 1000;
		variant(it, DBUS_TYPE_INT64, "x", &position);
	} else if(!strcmp(name, "Rate")) {
		value = (double)m.s.speed / SPEED_UNITY;
		variant(it, DBUS_TYPE_DOUBLE, "d", &value);
	} else if(!strcmp(name, "MinimumRate")) {
		value = (double)SPEED_MIN / SPEED_UNITY;
		variant(it, DBUS_TYPE_DOUBLE, "d", &value);
	} else if(!strcmp(name, "MaximumRate")) {
		value = (double)SPEED_MAX / SPEED_UNITY;
		variant(it, DBUS_TYPE_DOUBLE, "d", &value);
	} else if(!strcmp(name, "Volume")) {
		variant(it, DBUS_TYPE_DOUBLE, "d", &value);
	} else if(!strcmp(name, "Shuffle")) {
		variant(it, DBUS_TYPE_BOOLEAN, "b", &no);
	} else if(!strncmp(name, "Can", 3) && (!strcmp(name + 3, "GoNext") || !strcmp(name + 3, "GoPrevious") || !strcmp(name + 3, "Play") || !strcmp(name + 3, "Pause") || !strcmp(name + 3, "Seek") || !strcmp(name + 3, "Control"))) {
		variant(it, DBUS_TYPE_BOOLEAN, "b", &yes);
	} else {
		return false;
	}

	return true;
}

// appends a{sv} with the n properties named
static void properties(DBusMessageIter* it, const char* iface, const char* const* names, size_t n) {
	DBusMessageIter d;
	DBusMessageIter e;
	size_t i;

	dbus_message_iter_open_container(it, DBUS_TYPE_ARRAY, "{sv}", &d);
	for(i = 0; i < n; i++) {
		entry_open(&d, names[i], &e);
		property(&e, iface, names[i]);
		dbus_message_iter_close_container(&d, &e);
	}

	dbus_message_iter_close_container(it, &d);
}

static void emit(DBusMessage* msg) {
	if(msg) {
		dbus_connection_send(m.conn, msg, nullptr);
		dbus_message_unref(msg);
	}
}

static void signal_changed(const char* const* names, size_t n) {
	DBusMessage* msg = dbus_message_new_signal(OBJECT, PROPS, "PropertiesChanged");
	const char* iface = PLAYER;
	DBusMessageIter it;
	DBusMessageIter none;

	if(!msg) {
		return;
	}

	dbus_message_iter_init_append(msg, &it);
	dbus_message_iter_append_basic(&it, DBUS_TYPE_STRING, &iface);
	properties(&it, PLAYER, names, n);
	dbus_message_iter_open_container(&it, DBUS_TYPE_ARRAY, "s", &none);
	dbus_message_iter_close_container(&it, &none);
	emit(msg);
}

static void signal_seeked(uint32_t ms) {
	DBusMessage* msg = dbus_message_new_signal(OBJECT, PLAYER, "Seeked");
	dbus_int64_t position = (dbus_int64_t)ms * 1000;

	if(msg) {
		dbus_message_append_args(msg, DBUS_TYPE_INT64, &position, DBUS_TYPE_INVALID);
		emit(msg);
	}
}

static DBusMessage* get(DBusMessage* call) {
	const char* iface;
	const char* name;
	DBusMessage* reply;
	DBusMessageIter it;

	if(!dbus_message_get_args(call, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID)) {
		return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected an interface and a property");
	}

	reply = dbus_message_new_method_return(call);
	if(reply) {
		dbus_message_iter_init_append(reply, &it);
		if(!property(&it, iface, name)) {
			dbus_message_unref(reply);
			reply = dbus_message_new_error(call, DBUS_ERROR_UNKNOWN_PROPERTY, "no such property");
		}
	}

	return reply;
}

static DBusMessage* get_all(DBusMessage* call) {
	const char* iface;
	DBusMessage* reply;
	DBusMessageIter it;

	if(!dbus_message_get_args(call, nullptr, DBUS_TYPE_STRING, &iface, DBUS_TYPE_INVALID)) {
		return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected an interface");
	}

	reply = dbus_message_new_method_return(call);
	if(reply) {
		dbus_message_iter_init_append(reply, &it);
		if(!strcmp(iface, ROOT)) {
			properties(&it, ROOT, root_props, sizeof(root_props) / sizeof(*root_props));
		} else if(!strcmp(iface, PLAYER)) {
			properties(&it, PLAYER, player_props, sizeof(player_props) / sizeof(*player_props));
		} else {
			properties(&it, iface, nullptr, 0);
		}
	}

	return reply;
}

// the loop status is the one property that can be set
static DBusMessage* set(DBusMessage* call) {
	const char* iface;
	const char* name;
	const char* value;
	DBusMessageIter it;
	DBusMessageIter v;

	if(!dbus_message_iter_init(call, &it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
		return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected an interface, a property and a value");
	}

	dbus_message_iter_get_basic(&it, &iface);
	if(!dbus_message_iter_next(&it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_STRING) {
		return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected an interface, a property and a value");
	}

	dbus_message_iter_get_basic(&it, &name);
	if(strcmp(iface, PLAYER) || strcmp(name, "LoopStatus")) {
		return dbus_message_new_error(call, DBUS_ERROR_PROPERTY_READ_ONLY, "the property cannot be set");
	}

	if(!dbus_message_iter_next(&it) || dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT) {
		return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected an interface, a property and a value");
	}

	dbus_message_iter_recurse(&it, &v);
	if(dbus_message_iter_get_arg_type(&v) != DBUS_TYPE_STRING) {
		return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected None, Track or Playlist");
	}

	dbus_message_iter_get_basic(&v, &value);
	if(!strcmp(value, "None")) {
		player_set_repeat(REPEAT_OFF);
	} else if(!strcmp(value, "Track")) {
		player_set_repeat(REPEAT_TRACK);
	} else if(!strcmp(value, "Playlist")) {
		player_set_repeat(REPEAT_QUEUE);
	} else {
		return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected None, Track or Playlist");
	}

	return dbus_message_new_method_return(call);
}

// with nothing playing, play starts the queue at its top
static void play(bool toggle) {
	struct play_status s;

	player_status(&s);
	if(!s.track) {
		player_jump(0);
	} else if(toggle || s.state == PLAY_PAUSED) {
		player_toggle();
	}
}

// nullptr when the player interface has no such method
static DBusMessage* control(DBusMessage* call, const char* member) {
	struct play_status s;
	const char* id;
	char own[64];
	dbus_int64_t us;

	player_status(&s);
	if(!strcmp(member, "Next")) {
		player_next();
	} else if(!strcmp(member, "Previous")) {
		player_prev();
	} else if(!strcmp(member, "PlayPause")) {
		play(true);
	} else if(!strcmp(member, "Play")) {
		play(false);
	} else if(!strcmp(member, "Pause")) {
		if(s.state == PLAY_PLAYING) {
			player_toggle();
		}
	} else if(!strcmp(member, "Stop")) {
		player_stop();
	} else if(!strcmp(member, "Seek")) {
		if(!dbus_message_get_args(call, nullptr, DBUS_TYPE_INT64, &us, DBUS_TYPE_INVALID)) {
			return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected an offset in us");
		}

		player_seek_by((int32_t)(us / 1000));
	} else if(!strcmp(member, "SetPosition")) {
		if(!dbus_message_get_args(call, nullptr, DBUS_TYPE_OBJECT_PATH, &id, DBUS_TYPE_INT64, &us, DBUS_TYPE_INVALID)) {
			return dbus_message_new_error(call, DBUS_ERROR_INVALID_ARGS, "expected a track and a position in us");
		}

		// a position for a track that has since ended is dropped, as the interface asks
		snprintf(own, sizeof(own), "/org/nusic/entry/%u", m.entry);
		if(s.track && !strcmp(id, own) && us >= 0 && us / 1000 <= s.track->duration) {
			player_seek_to((uint32_t)(us / 1000));
		}
	} else if(!strcmp(member, "OpenUri")) {
		return dbus_message_new_error(call, DBUS_ERROR_NOT_SUPPORTED, "nusic plays its library only");
	} else {
		return nullptr;
	}

	return dbus_message_new_method_return(call);
}

static DBusHandlerResult on_call(DBusConnection* conn, DBusMessage* call, void* data) {
	const char* iface = dbus_message_get_interface(call);
	const char* member = dbus_message_get_member(call);
	DBusMessage* reply = nullptr;

	if(dbus_message_get_type(call) != DBUS_MESSAGE_TYPE_METHOD_CALL || !iface || !member) {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if(!strcmp(iface, INTRO) && !strcmp(member, "Introspect")) {
		const char* xml = introspection;

		reply = dbus_message_new_method_return(call);
		if(reply) {
			dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID);
		}
	} else if(!strcmp(iface, PROPS) && !strcmp(member, "Get")) {
		reply = get(call);
	} else if(!strcmp(iface, PROPS) && !strcmp(member, "GetAll")) {
		reply = get_all(call);
	} else if(!strcmp(iface, PROPS) && !strcmp(member, "Set")) {
		reply = set(call);
	} else if(!strcmp(iface, ROOT) && !strcmp(member, "Quit")) {
		m.quit = true;
		reply = dbus_message_new_method_return(call);
	} else if(!strcmp(iface, ROOT) && !strcmp(member, "Raise")) {
		reply = dbus_message_new_method_return(call);
	} else if(!strcmp(iface, PLAYER)) {
		reply = control(call, member);
	}

	if(!reply) {
		reply = dbus_message_new_error(call, DBUS_ERROR_UNKNOWN_METHOD, "no such method");
	}

	if(reply && dbus_message_get_no_reply(call)) {
		dbus_message_unref(reply);
	} else {
		emit(reply);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static const DBusObjectPathVTable vtable = { .message_function = on_call };

static void drop(void) {
	dbus_connection_close(m.conn);
	dbus_connection_unref(m.conn);
	m.conn = nullptr;
}

// a second nusic takes the name with its pid, as the interface has it for a program that runs twice
static bool take_name(DBusError* err) {
	char name[64];

	if(dbus_bus_request_name(m.conn, NAME, DBUS_NAME_FLAG_DO_NOT_QUEUE, err) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		return true;
	}

	if(dbus_error_is_set(err)) {
		return false;
	}

	snprintf(name, sizeof(name), NAME ".instance%ld", (long)getpid());

	return dbus_bus_request_name(m.conn, name, DBUS_NAME_FLAG_DO_NOT_QUEUE, err) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
}

bool mpris_init(const struct library* lib) {
	char path[PATH_MAX];
	const char* address = getenv("DBUS_SESSION_BUS_ADDRESS");
	const char* dir = getenv("XDG_RUNTIME_DIR");
	DBusError err;

	// with no bus to be found libdbus would start one, which a music player has no business doing
	if(!address || !address[0]) {
		if(!dir || (size_t)snprintf(path, sizeof(path), "%s/bus", dir) >= sizeof(path) || access(path, F_OK) < 0) {
			return false;
		}
	}

	m.lib = lib;
	m.quit = false;
	player_status(&m.s);
	dbus_error_init(&err);
	m.conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
	if(m.conn) {
		// libdbus ends the program when the bus goes away unless told otherwise
		dbus_connection_set_exit_on_disconnect(m.conn, FALSE);
		if(take_name(&err) && dbus_connection_register_object_path(m.conn, OBJECT, &vtable, nullptr)) {
			return true;
		}

		drop();
	}

	report("warning: dbus: %s, other programs do not see what plays", dbus_error_is_set(&err) ? err.message : "the name is taken");
	dbus_error_free(&err);

	return false;
}

void mpris_fini(void) {
	if(m.conn) {
		dbus_connection_flush(m.conn);
		drop();
	}
}

int mpris_fd(void) {
	int fd = -1;

	if(!m.conn || !dbus_connection_get_unix_fd(m.conn, &fd)) {
		return -1;
	}

	return fd;
}

bool mpris_dispatch(void) {
	if(!m.conn) {
		return false;
	}

	if(!dbus_connection_read_write(m.conn, 0)) {
		drop();
		return false;
	}

	while(dbus_connection_dispatch(m.conn) == DBUS_DISPATCH_DATA_REMAINS) {
	}

	dbus_connection_flush(m.conn);

	return m.quit;
}

// a position that is not where the time since the last status puts it at the speed of that status is a seek
void mpris_update(const struct play_status* s) {
	const char* names[4];
	uint32_t entry = s->track ? s->queue[s->index].id : 0;
	int64_t now = now_ms();
	int64_t expected = m.s.position + (m.s.state == PLAY_PLAYING ? (now - m.at) * m.s.speed / SPEED_UNITY : 0);
	bool same = entry == m.entry && s->track == m.s.track;
	bool seeked = same && s->track && (s->position > expected + SEEK_GAP || s->position + SEEK_GAP < expected);
	size_t n = 0;

	if(!same) {
		names[n++] = "Metadata";
	}

	if(s->state != m.s.state) {
		names[n++] = "PlaybackStatus";
	}

	if(s->repeat != m.s.repeat) {
		names[n++] = "LoopStatus";
	}

	if(s->speed != m.s.speed) {
		names[n++] = "Rate";
	}

	m.s = *s;
	m.entry = entry;
	m.at = now;
	if(!m.conn) {
		return;
	}

	if(n) {
		signal_changed(names, n);
	}

	if(seeked) {
		signal_seeked(s->position);
	}

	if(n || seeked) {
		dbus_connection_flush(m.conn);
	}
}

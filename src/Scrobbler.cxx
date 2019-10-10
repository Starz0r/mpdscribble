/* mpdscribble (MPD Client)
 * Copyright (C) 2008-2019 The Music Player Daemon Project
 * Copyright (C) 2005-2008 Kuno Woudt <kuno@frob.nl>
 * Project homepage: http://musicpd.org
 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "Scrobbler.hxx"
#include "Record.hxx"
#include "Journal.hxx"
#include "HttpClient.hxx"
#include "config.h"
#include "Log.hxx" /* for log_date() */

#include <glib.h>

#include <gcrypt.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>

#define AS_CLIENT_ID "mdc"
#define AS_CLIENT_VERSION VERSION

/* don't submit more than this amount of songs in a batch. */
#define MAX_SUBMIT_COUNT 10

static const char OK[] = "OK";
static const char BADSESSION[] = "BADSESSION";
static const char FAILED[] = "FAILED";

enum scrobbler_state {
	/**
	 * mpdscribble has started, and doesn't have a session yet.
	 * Handshake to be submitted.
	 */
	SCROBBLER_STATE_NOTHING,

	/**
	 * Handshake is in progress, waiting for the server's
	 * response.
	 */
	SCROBBLER_STATE_HANDSHAKE,

	/**
	 * We have a session, and we're ready to submit.
	 */
	SCROBBLER_STATE_READY,

	/**
	 * Submission in progress, waiting for the server's response.
	 */
	SCROBBLER_STATE_SUBMITTING,
};

typedef enum {
	AS_SUBMIT_OK,
	AS_SUBMIT_FAILED,
	AS_SUBMIT_HANDSHAKE,
} as_submitting;

struct Scrobbler {
	const ScrobblerConfig *config;

	FILE *file;

	enum scrobbler_state state;

	unsigned interval;

	guint handshake_source_id;
	guint submit_source_id;

	char *session;
	char *nowplay_url;
	char *submit_url;

	Record now_playing;

	/**
	 * A queue of #record objects.
	 */
	GQueue *queue;

	/**
	 * How many songs are we trying to submit right now?  This
	 * many will be shifted from #queue if the submit succeeds.
	 */
	unsigned pending;
};

static GSList *scrobblers;

/**
 * Creates a new scrobbler object based on the specified
 * configuration.
 */
static Scrobbler *
scrobbler_new(const ScrobblerConfig *config)
{
	Scrobbler *scrobbler = new Scrobbler;

	scrobbler->config = config;
	scrobbler->file = nullptr;
	scrobbler->state = SCROBBLER_STATE_NOTHING;
	scrobbler->interval = 1;
	scrobbler->handshake_source_id = 0;
	scrobbler->submit_source_id = 0;
	scrobbler->session = nullptr;
	scrobbler->nowplay_url = nullptr;
	scrobbler->submit_url = nullptr;

	record_clear(&scrobbler->now_playing);

	scrobbler->queue = g_queue_new();
	scrobbler->pending = 0;

	return scrobbler;
}

static void
record_free_callback(gpointer data, gpointer)
{
	auto *song = (Record *)data;
	record_free(song);
}

/**
 * Frees a scrobbler object.
 */
static void
scrobbler_free(Scrobbler *scrobbler)
{
	g_queue_foreach(scrobbler->queue, record_free_callback, nullptr);
	g_queue_free(scrobbler->queue);

	record_deinit(&scrobbler->now_playing);

	if (scrobbler->handshake_source_id != 0)
		g_source_remove(scrobbler->handshake_source_id);
	if (scrobbler->submit_source_id != 0)
		g_source_remove(scrobbler->submit_source_id);

	g_free(scrobbler->session);
	g_free(scrobbler->nowplay_url);
	g_free(scrobbler->submit_url);

	if (scrobbler->file != nullptr)
		fclose(scrobbler->file);

	delete scrobbler;
}

static void
add_var_internal(GString * s, char sep, const char *key,
		 signed char idx, const char *val)
{
	g_string_append_c(s, sep);
	g_string_append(s, key);

	if (idx >= 0)
		g_string_append_printf(s, "[%i]", idx);

	g_string_append_c(s, '=');

	if (val != nullptr) {
		char *escaped = http_client_uri_escape(val);
		g_string_append(s, escaped);
		g_free(escaped);
	}
}

static void first_var(GString * s, const char *key, const char *val)
{
	add_var_internal(s, '?', key, -1, val);
}

static void add_var(GString * s, const char *key, const char *val)
{
	add_var_internal(s, '&', key, -1, val);
}

static void
add_var_i(GString * s, const char *key, signed char idx, const char *val)
{
	add_var_internal(s, '&', key, idx, val);
}

static void
scrobbler_schedule_handshake(Scrobbler *scrobbler);

static void
scrobbler_submit(Scrobbler *scrobbler);

static void
scrobbler_schedule_submit(Scrobbler *scrobbler);

static void
scrobbler_increase_interval(Scrobbler *scrobbler)
{
	if (scrobbler->interval < 60)
		scrobbler->interval = 60;
	else
		scrobbler->interval <<= 1;

	if (scrobbler->interval > 60 * 60 * 2)
		scrobbler->interval = 60 * 60 * 2;

	g_warning("[%s] waiting %u seconds before trying again",
		  scrobbler->config->name.c_str(), scrobbler->interval);
}

static as_submitting
scrobbler_parse_submit_response(const char *scrobbler_name,
				const char *line, size_t length)
{
	if (length == sizeof(OK) - 1 && memcmp(line, OK, length) == 0) {
		g_message("[%s] OK", scrobbler_name);

		return AS_SUBMIT_OK;
	} else if (length == sizeof(BADSESSION) - 1 &&
		   memcmp(line, BADSESSION, length) == 0) {
		g_warning("[%s] invalid session", scrobbler_name);

		return AS_SUBMIT_HANDSHAKE;
	} else if (length == sizeof(FAILED) - 1 &&
		   memcmp(line, FAILED, length) == 0) {
		if (length > strlen(FAILED))
			g_warning("[%s] submission rejected: %.*s",
				  scrobbler_name,
				  (int)(length - strlen(FAILED)),
				  line + strlen(FAILED));
		else
			g_warning("[%s] submission rejected", scrobbler_name);
	} else {
		g_warning("[%s] unknown response: %.*s",
			  scrobbler_name, (int)length, line);
	}

	return AS_SUBMIT_FAILED;
}

static bool
scrobbler_parse_handshake_response(Scrobbler *scrobbler, const char *line)
{
	static const char *BANNED = "BANNED";
	static const char *BADAUTH = "BADAUTH";
	static const char *BADTIME = "BADTIME";

	/* FIXME: some code duplication between this
	   and as_parse_submit_response. */
	if (!strncmp(line, OK, strlen(OK))) {
		g_message("[%s] handshake successful",
			  scrobbler->config->name.c_str());
		return true;
	} else if (!strncmp(line, BANNED, strlen(BANNED))) {
		g_warning("[%s] handshake failed, we're banned (%s)",
			  scrobbler->config->name.c_str(), line);
	} else if (!strncmp(line, BADAUTH, strlen(BADAUTH))) {
		g_warning("[%s] handshake failed, "
			  "username or password incorrect (%s)",
			  scrobbler->config->name.c_str(), line);
	} else if (!strncmp(line, BADTIME, strlen(BADTIME))) {
		g_warning("[%s] handshake failed, clock not synchronized (%s)",
			  scrobbler->config->name.c_str(), line);
	} else if (!strncmp(line, FAILED, strlen(FAILED))) {
		g_warning("[%s] handshake failed (%s)",
			  scrobbler->config->name.c_str(), line);
	} else {
		g_warning("[%s] error parsing handshake response (%s)",
			  scrobbler->config->name.c_str(), line);
	}

	return false;
}

static char *
next_line(const char **input_r, const char *end)
{
	const char *input = *input_r;
	const char *newline = (const char *)memchr(input, '\n', end - input);
	char *line;

	if (newline == nullptr)
		return g_strdup("");

	line = g_strndup(input, newline - input);
	*input_r = newline + 1;

	return line;
}

static void
scrobbler_handshake_response(size_t length, const char *response, void *data)
{
	auto *scrobbler = (Scrobbler *)data;
	const char *end = response + length;
	char *line;
	bool ret;

	assert(scrobbler != nullptr);
	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_HANDSHAKE);

	scrobbler->state = SCROBBLER_STATE_NOTHING;

	line = next_line(&response, end);
	ret = scrobbler_parse_handshake_response(scrobbler, line);
	g_free(line);
	if (!ret) {
		scrobbler_increase_interval(scrobbler);
		scrobbler_schedule_handshake(scrobbler);
		return;
	}

	scrobbler->session = next_line(&response, end);
	g_debug("[%s] session: %s",
		scrobbler->config->name.c_str(), scrobbler->session);

	scrobbler->nowplay_url = next_line(&response, end);
	g_debug("[%s] now playing url: %s",
		scrobbler->config->name.c_str(), scrobbler->nowplay_url);

	scrobbler->submit_url = next_line(&response, end);
	g_debug("[%s] submit url: %s",
		scrobbler->config->name.c_str(), scrobbler->submit_url);

	if (*scrobbler->nowplay_url == 0 || *scrobbler->submit_url == 0) {
		g_free(scrobbler->session);
		scrobbler->session = nullptr;

		g_free(scrobbler->nowplay_url);
		scrobbler->nowplay_url = nullptr;

		g_free(scrobbler->submit_url);
		scrobbler->submit_url = nullptr;

		scrobbler_increase_interval(scrobbler);
		scrobbler_schedule_handshake(scrobbler);
		return;
	}

	scrobbler->state = SCROBBLER_STATE_READY;
	scrobbler->interval = 1;

	/* handshake was successful: see if we have songs to submit */
	scrobbler_submit(scrobbler);
}

static void
scrobbler_handshake_error(GError *error, void *data)
{
	auto *scrobbler = (Scrobbler *)data;

	assert(scrobbler != nullptr);
	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_HANDSHAKE);

	scrobbler->state = SCROBBLER_STATE_NOTHING;

	g_warning("[%s] handshake error: %s",
		  scrobbler->config->name.c_str(), error->message);
	g_error_free(error);

	scrobbler_increase_interval(scrobbler);
	scrobbler_schedule_handshake(scrobbler);
}

static const struct http_client_handler scrobbler_handshake_handler = {
	.response = scrobbler_handshake_response,
	.error = scrobbler_handshake_error,
};

static void
scrobbler_queue_remove_oldest(GQueue *queue, unsigned count)
{
	assert(count > 0);

	while (count--) {
		auto *tmp = (Record *)g_queue_pop_head(queue);
		record_free(tmp);
	}
}

static void
scrobbler_submit_response(size_t length, const char *response, void *data)
{
	auto *scrobbler = (Scrobbler *)data;

	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_SUBMITTING);
	scrobbler->state = SCROBBLER_STATE_READY;

	const char *newline = (const char *)memchr(response, '\n', length);
	if (newline != nullptr)
		length = newline - response;

	switch (scrobbler_parse_submit_response(scrobbler->config->name.c_str(),
						response, length)) {
	case AS_SUBMIT_OK:
		scrobbler->interval = 1;

		/* submission was accepted, so clean up the cache. */
		if (scrobbler->pending > 0) {
			scrobbler_queue_remove_oldest(scrobbler->queue, scrobbler->pending);
			scrobbler->pending = 0;
		} else {
			assert(record_is_defined(&scrobbler->now_playing));

			record_deinit(&scrobbler->now_playing);
			memset(&scrobbler->now_playing, 0,
			       sizeof(scrobbler->now_playing));
		}


		/* submit the next chunk (if there is some left) */
		scrobbler_submit(scrobbler);
		break;
	case AS_SUBMIT_FAILED:
		scrobbler_increase_interval(scrobbler);
		scrobbler_schedule_submit(scrobbler);
		break;
	case AS_SUBMIT_HANDSHAKE:
		scrobbler->state = SCROBBLER_STATE_NOTHING;
		scrobbler_schedule_handshake(scrobbler);
		break;
	}
}

static void
scrobbler_submit_error(GError *error, void *data)
{
	auto *scrobbler = (Scrobbler *)data;

	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_SUBMITTING);

	scrobbler->state = SCROBBLER_STATE_READY;

	g_warning("[%s] submit error: %s",
		  scrobbler->config->name.c_str(), error->message);
	g_error_free(error);

	scrobbler_increase_interval(scrobbler);
	scrobbler_schedule_submit(scrobbler);
}

static const struct http_client_handler scrobbler_submit_handler = {
	.response = scrobbler_submit_response,
	.error = scrobbler_submit_error,
};

char *as_timestamp()
{
	/* create timestamp for 1.2 protocol. */
	GTimeVal time_val;

	g_get_current_time(&time_val);
	return g_strdup_printf("%ld", (glong)time_val.tv_sec);
}

/**
 * Calculate the MD5 checksum of the specified string.  The return
 * value is a newly allocated string containing the hexadecimal
 * checksum.
 */
static char *md5_hex(const char *p, int len)
{
	gcry_md_hd_t hd;
	unsigned char *binary;
	char *result;

	if (len == -1)
		len = strlen(p);

	if (gcry_md_open(&hd, GCRY_MD_MD5, 0) != GPG_ERR_NO_ERROR)
		g_error("gcry_md_open() failed\n");
	gcry_md_write(hd, p, len);
	binary = gcry_md_read(hd, GCRY_MD_MD5);
	if (binary == nullptr)
		g_error("gcry_md_read() failed\n");
	result = (char *)g_malloc(gcry_md_get_algo_dlen(GCRY_MD_MD5) * 2 + 1);
	for (size_t i = 0; i < gcry_md_get_algo_dlen(GCRY_MD_MD5); ++i)
		snprintf(result + i * 2, 3, "%02x", binary[i]);
	gcry_md_close(hd);

	return result;
}

static char *as_md5(const char *password, const char *timestamp)
{
	char *password_md5, *cat, *result;

	if (strlen(password) != 32)
		/* assume it's not hashed yet */
		password = password_md5 = md5_hex(password, -1);
	else
		password_md5 = nullptr;

	cat = g_strconcat(password, timestamp, nullptr);
	g_free(password_md5);

	result = md5_hex(cat, -1);
	g_free(cat);

	return result;
}

static void
scrobbler_handshake(Scrobbler *scrobbler)
{
	assert(scrobbler->config->file.empty());

	GString *url;
	char *timestr, *md5;

	scrobbler->state = SCROBBLER_STATE_HANDSHAKE;

	timestr = as_timestamp();
	md5 = as_md5(scrobbler->config->password.c_str(), timestr);

	/* construct the handshake url. */
	url = g_string_new(scrobbler->config->url.c_str());
	first_var(url, "hs", "true");
	add_var(url, "p", "1.2");
	add_var(url, "c", AS_CLIENT_ID);
	add_var(url, "v", AS_CLIENT_VERSION);
	add_var(url, "u", scrobbler->config->username.c_str());
	add_var(url, "t", timestr);
	add_var(url, "a", md5);

	g_free(timestr);
	g_free(md5);

	//  notice ("handshake url:\n%s", url);

	http_client_request(url->str, nullptr,
			    &scrobbler_handshake_handler, scrobbler);

	g_string_free(url, true);
}

static gboolean
scrobbler_handshake_timer(gpointer data)
{
	auto *scrobbler = (Scrobbler *)data;

	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_NOTHING);

	scrobbler->handshake_source_id = 0;

	scrobbler_handshake(scrobbler);
	return false;
}

static void
scrobbler_schedule_handshake(Scrobbler *scrobbler)
{
	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_NOTHING);
	assert(scrobbler->handshake_source_id == 0);

	scrobbler->handshake_source_id =
		g_timeout_add_seconds(scrobbler->interval,
				      scrobbler_handshake_timer, scrobbler);
}

static void
scrobbler_send_now_playing(Scrobbler *scrobbler, const char *artist,
			   const char *track, const char *album,
			   const char *number,
			   const char *mbid, const int length)
{
	GString *post_data;
	char len[16];

	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_READY);
	assert(scrobbler->submit_source_id == 0);

	scrobbler->state = SCROBBLER_STATE_SUBMITTING;

	snprintf(len, sizeof(len), "%i", length);

	post_data = g_string_new(nullptr);
	add_var(post_data, "s", scrobbler->session);
	add_var(post_data, "a", artist);
	add_var(post_data, "t", track);
	add_var(post_data, "b", album);
	add_var(post_data, "l", len);
	add_var(post_data, "n", number);
	add_var(post_data, "m", mbid);

	g_message("[%s] sending 'now playing' notification",
		  scrobbler->config->name.c_str());

	http_client_request(scrobbler->nowplay_url,
			    post_data->str,
			    &scrobbler_submit_handler, scrobbler);

	g_string_free(post_data, true);
}

static void
scrobbler_schedule_now_playing_callback(gpointer data, gpointer user_data)
{
	auto *scrobbler = (Scrobbler *)data;
	const auto *song = (const Record *)user_data;

	if (scrobbler->file != nullptr)
		/* there's no "now playing" support for files */
		return;

	record_deinit(&scrobbler->now_playing);
	record_copy(&scrobbler->now_playing, song);

	if (scrobbler->state == SCROBBLER_STATE_READY &&
	    scrobbler->submit_source_id == 0)
		scrobbler_schedule_submit(scrobbler);
}

void
as_now_playing(const char *artist, const char *track,
	       const char *album, const char *number,
	       const char *mbid, const int length)
{
	Record record;

	record.artist = g_strdup(artist);
	record.track = g_strdup(track);
	record.album = g_strdup(album);
	record.number = g_strdup(number);
	record.mbid = g_strdup(mbid);
	record.time = nullptr;
	record.length = length;

	g_slist_foreach(scrobblers,
			scrobbler_schedule_now_playing_callback, &record);

	record_deinit(&record);
}

static void
scrobbler_submit(Scrobbler *scrobbler)
{
	//MAX_SUBMIT_COUNT
	unsigned count = 0;
	GString *post_data;

	assert(scrobbler->config->file.empty());
	assert(scrobbler->state == SCROBBLER_STATE_READY);
	assert(scrobbler->submit_source_id == 0);

	if (g_queue_is_empty(scrobbler->queue)) {
		/* the submission queue is empty.  See if a "now playing" song is
		   scheduled - these should be sent after song submissions */
		if (record_is_defined(&scrobbler->now_playing))
			scrobbler_send_now_playing(scrobbler,
						   scrobbler->now_playing.artist,
						   scrobbler->now_playing.track,
						   scrobbler->now_playing.album,
						   scrobbler->now_playing.number,
						   scrobbler->now_playing.mbid,
						   scrobbler->now_playing.length);

		return;
	}

	scrobbler->state = SCROBBLER_STATE_SUBMITTING;

	/* construct the handshake url. */
	post_data = g_string_new(nullptr);
	add_var(post_data, "s", scrobbler->session);

	for (GList *list = g_queue_peek_head_link(scrobbler->queue);
	     list != nullptr && count < MAX_SUBMIT_COUNT;
	     list = g_list_next(list)) {
		auto *song = (Record *)list->data;
		char len[16];

		snprintf(len, sizeof(len), "%i", song->length);

		add_var_i(post_data, "a", count, song->artist);
		add_var_i(post_data, "t", count, song->track);
		add_var_i(post_data, "l", count, len);
		add_var_i(post_data, "i", count, song->time);
		add_var_i(post_data, "o", count, song->source);
		add_var_i(post_data, "r", count, "");
		add_var_i(post_data, "b", count, song->album);
		add_var_i(post_data, "n", count, song->number);
		add_var_i(post_data, "m", count, song->mbid);

		if (song->love)
			add_var_i(post_data, "r", count, "L");

		count++;
	}

	g_message("[%s] submitting %i song%s",
		  scrobbler->config->name.c_str(), count, count == 1 ? "" : "s");
	g_debug("[%s] post data: %s",
		scrobbler->config->name.c_str(), post_data->str);
	g_debug("[%s] url: %s",
		scrobbler->config->name.c_str(), scrobbler->submit_url);

	scrobbler->pending = count;
	http_client_request(scrobbler->submit_url,
			    post_data->str,
			    &scrobbler_submit_handler, scrobbler);

	g_string_free(post_data, true);
}

static void
scrobbler_push_callback(gpointer data, gpointer user_data)
{
	auto *scrobbler = (Scrobbler *)data;
	const auto *record = (const Record *)user_data;

	if (scrobbler->file != nullptr) {
		fprintf(scrobbler->file, "%s %s - %s\n",
			log_date(), record->artist, record->track);
		fflush(scrobbler->file);
		return;
	}

	g_queue_push_tail(scrobbler->queue, record_dup(record));

	if (scrobbler->state == SCROBBLER_STATE_READY &&
	    scrobbler->submit_source_id == 0)
		scrobbler_schedule_submit(scrobbler);
}

void
as_songchange(const char *file, const char *artist, const char *track,
	      const char *album, const char *number,
	      const char *mbid, const int length,
	      bool love,
	      const char *time2)
{
	Record record;

	/* from the 1.2 protocol draft:

	   You may still submit if there is no album title (variable b)
	   You may still submit if there is no musicbrainz id available (variable m)

	   everything else is mandatory.
	 */
	if (!(artist && strlen(artist))) {
		g_warning("empty artist, not submitting; "
			  "please check the tags on %s\n", file);
		return;
	}

	if (!(track && strlen(track))) {
		g_warning("empty title, not submitting; "
			  "please check the tags on %s", file);
		return;
	}

	record.artist = g_strdup(artist);
	record.track = g_strdup(track);
	record.album = g_strdup(album);
	record.number = g_strdup(number);
	record.mbid = g_strdup(mbid);
	record.length = length;
	record.time = time2 ? g_strdup(time2) : as_timestamp();
	record.love = love;
	record.source = strstr(file, "://") == nullptr ? "P" : "R";

	g_message("%s, songchange: %s - %s (%i)\n",
		  record.time, record.artist,
		  record.track, record.length);

	g_slist_foreach(scrobblers, scrobbler_push_callback, &record);

	record_deinit(&record);
}

static void
AddScrobbler(const ScrobblerConfig *config)
{
	Scrobbler *scrobbler = scrobbler_new(config);

	if (!config->journal.empty()) {
		guint queue_length;

		journal_read(config->journal.c_str(), scrobbler->queue);

		queue_length = g_queue_get_length(scrobbler->queue);
		g_message("loaded %i song%s from %s",
			  queue_length, queue_length == 1 ? "" : "s",
			  config->journal.c_str());
	}

	scrobblers = g_slist_prepend(scrobblers, scrobbler);
	if (!config->file.empty()) {
		scrobbler->file = fopen(config->file.c_str(), "a");
		if (scrobbler->file == nullptr)
			g_error("Failed to open file '%s' of scrobbler '%s': %s\n",
				config->file.c_str(), config->name.c_str(),
				g_strerror(errno));
	} else
		scrobbler_schedule_handshake(scrobbler);
}

void
as_init(const std::forward_list<ScrobblerConfig> &scrobbler_configs)
{
	g_message("starting mpdscribble (" AS_CLIENT_ID " " AS_CLIENT_VERSION ")\n");

	for (const auto &i : scrobbler_configs)
		AddScrobbler(&i);
}

static gboolean
scrobbler_submit_timer(gpointer data)
{
	auto *scrobbler = (Scrobbler *)data;

	assert(scrobbler->state == SCROBBLER_STATE_READY);

	scrobbler->submit_source_id = 0;

	scrobbler_submit(scrobbler);
	return false;
}

static void
scrobbler_schedule_submit(Scrobbler *scrobbler)
{
	assert(scrobbler->submit_source_id == 0);
	assert(!g_queue_is_empty(scrobbler->queue) ||
	       record_is_defined(&scrobbler->now_playing));

	scrobbler->submit_source_id =
		g_timeout_add_seconds(scrobbler->interval,
				      scrobbler_submit_timer, scrobbler);
}

static void
scrobbler_save_callback(gpointer data, gpointer)
{
	auto *scrobbler = (Scrobbler *)data;

	if (scrobbler->file != nullptr || scrobbler->config->journal.empty())
		return;

	if (journal_write(scrobbler->config->journal.c_str(),
			  scrobbler->queue)) {
		guint queue_length = g_queue_get_length(scrobbler->queue);
		g_message("[%s] saved %i song%s to %s",
			  scrobbler->config->name.c_str(),
			  queue_length, queue_length == 1 ? "" : "s",
			  scrobbler->config->journal.c_str());
	}
}

void as_save_cache()
{
	g_slist_foreach(scrobblers, scrobbler_save_callback, nullptr);
}

static void
scrobbler_submit_now_callback(gpointer data, gpointer)
{
	auto *scrobbler = (Scrobbler *)data;

	scrobbler->interval = 1;

	if (scrobbler->handshake_source_id != 0) {
		g_source_remove(scrobbler->handshake_source_id);
		scrobbler->handshake_source_id = 0;
		scrobbler_schedule_handshake(scrobbler);
	}

	if (scrobbler->submit_source_id != 0) {
		g_source_remove(scrobbler->submit_source_id);
		scrobbler->submit_source_id = 0;
		scrobbler_schedule_submit(scrobbler);
	}
}

void as_submit_now()
{
	g_slist_foreach(scrobblers, scrobbler_submit_now_callback, nullptr);
}

static void
scrobbler_free_callback(gpointer data, gpointer)
{
	auto *scrobbler = (Scrobbler *)data;

	scrobbler_free(scrobbler);
}

void as_cleanup()
{
	g_slist_foreach(scrobblers, scrobbler_free_callback, nullptr);
	g_slist_free(scrobblers);
}

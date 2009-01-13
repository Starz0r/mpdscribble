/* mpdscribble (MPD Client)
 * Copyright (C) 2008-2009 The Music Player Daemon Project
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

#include "daemon.h"
#include "file.h"
#include "log.h"
#include "lmc.h"
#include "as.h"
#include "mbid.h"

#include <glib.h>

#include <stdbool.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

static GMainLoop *main_loop;
static guint save_source_id;

static GTimer *timer;
static char mbid[MBID_BUFFER_SIZE];

static void signal_handler(G_GNUC_UNUSED int signum)
{
	g_main_loop_quit(main_loop);
}

static void
x_sigaction(int signum, const struct sigaction *act)
{
	if (sigaction(signum, act, NULL) < 0) {
		perror("sigaction()");
		exit(EXIT_FAILURE);
	}
}

static void
setup_signals(void)
{
	struct sigaction sa;

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = signal_handler;
	x_sigaction(SIGINT, &sa);
	x_sigaction(SIGTERM, &sa);
	x_sigaction(SIGHUP, &sa);

	sa.sa_handler = SIG_IGN;
	x_sigaction(SIGPIPE, &sa);
}

static bool played_long_enough(int length)
{
	int elapsed = g_timer_elapsed(timer, NULL);

	/* http://www.lastfm.de/api/submissions "The track must have been
	   played for a duration of at least 240 seconds or half the track's
	   total length, whichever comes first. Skipping or pausing the
	   track is irrelevant as long as the appropriate amount has been
	   played."
	 */
	return elapsed > 240 || (length >= 30 && elapsed > length / 2);
}

static void song_changed(const struct mpd_song *song)
{
	g_message("new song detected (%s - %s), id: %i, pos: %i\n",
		  song->artist, song->title, song->id, song->pos);

	g_timer_start(timer);

	if (file_config.musicdir && chdir(file_config.musicdir) == 0) {
		// yeah, I know i'm being silly, but I can't be arsed to
		// concat the parts :P
		if (getMBID(song->file, mbid))
			mbid[0] = 0x00;
		else
			g_message("mbid is %s\n", mbid);
	}

	as_now_playing(song->artist, song->title, song->album, mbid,
		       song->time);
}

/**
 * Regularly save the cache.
 */
static gboolean
timer_save_cache(G_GNUC_UNUSED gpointer data)
{
	as_save_cache();
	return true;
}

/**
 * Pause mode on the current song was activated.
 */
void
song_paused(void)
{
	g_timer_stop(timer);
}

/**
 * The current song continues to play (after pause).
 */
void
song_continued(void)
{
	g_timer_continue(timer);
}

/**
 * MPD started playing this song.
 */
void
song_started(const struct mpd_song *song)
{
	song_changed(song);
}

/**
 * MPD is still playing the song.
 */
void
song_playing(G_GNUC_UNUSED const struct mpd_song *song,
	     G_GNUC_UNUSED int elapsed)
{
	int prev_elapsed = g_timer_elapsed(timer, NULL);

	if (elapsed < 60 && prev_elapsed > elapsed &&
	    prev_elapsed - elapsed >= 240) {
		/* the song is playing repeatedly: make it virtually
		   stop and re-start */
		g_debug("repeated song detected");

		song_ended(song);
		song_started(song);
	}
}

/**
 * MPD stopped playing this song.
 */
void
song_ended(const struct mpd_song *song)
{
	int q;

	if (!played_long_enough(song->time))
		return;

	/* FIXME:
	   libmpdclient doesn't have any way to fetch the musicbrainz id. */
	q = as_songchange(song->file, song->artist,
			  song->title,
			  song->album, mbid,
			  song->time >
			  0 ? song->time : (int)
			  g_timer_elapsed(timer,
					  NULL),
			  NULL);
	if (q != -1)
		g_message("added (%s - %s) to submit queue at position %i\n",
			  song->artist, song->title, q);
}

int main(int argc, char **argv)
{
	daemonize_close_stdin();

	if (!file_read_config(argc, argv))
		g_error("cannot read configuration file\n");

	log_init(file_config.log, file_config.verbose);

	if (!file_config.no_daemon)
		daemonize_detach();

	if (file_config.pidfile != NULL)
		daemonize_write_pidfile(file_config.pidfile);

#ifndef NDEBUG
	if (!file_config.no_daemon)
#endif
		daemonize_close_stdout_stderr();

	main_loop = g_main_loop_new(NULL, FALSE);

	lmc_connect(file_config.host, file_config.port);
	as_init();

	setup_signals();

	timer = g_timer_new();

	/* set up timeouts */

	save_source_id = g_timeout_add(file_config.cache_interval * 1000,
				       timer_save_cache, NULL);

	/* run the main loop */

	g_main_loop_run(main_loop);

	/* cleanup */

	g_message("shutting down\n");

	g_main_loop_unref(main_loop);

	g_timer_destroy(timer);

	as_cleanup();
	lmc_disconnect();
	file_cleanup();
	log_deinit();

	if (file_config.pidfile != NULL)
		unlink(file_config.pidfile);

	return 0;
}

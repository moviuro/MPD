/* the Music Player Daemon (MPD)
 * Copyright (C) 2003-2007 by Warren Dukes (warren.dukes@gmail.com)
 * Copyright (C) 2008 Max Kellermann <max@duempel.org>
 * This project's homepage is: http://www.musicpd.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "decoder_thread.h"
#include "decoder_control.h"
#include "decoder_internal.h"
#include "player_control.h"
#include "song.h"
#include "mapper.h"
#include "path.h"
#include "log.h"
#include "ls.h"

static void decodeStart(void)
{
	struct song *song = dc.next_song;
	struct decoder decoder;
	int ret;
	bool close_instream = true;
	struct input_stream inStream;
	struct decoder_plugin *plugin = NULL;
	char path_max_fs[MPD_PATH_MAX];

	if (song_is_file(song))
		map_song_fs(song, path_max_fs);
	else
		song_get_url(song, path_max_fs);

	dc.current_song = dc.next_song; /* NEED LOCK */
	if (!input_stream_open(&inStream, path_max_fs)) {
		dc.error = DECODE_ERROR_FILE;
		goto stop_no_close;
	}

	decoder.seeking = false;

	dc.state = DECODE_STATE_START;
	dc.command = DECODE_COMMAND_NONE;
	notify_signal(&pc.notify);

	/* wait for the input stream to become ready; its metadata
	   will be available then */

	while (!inStream.ready) {
		if (dc.command != DECODE_COMMAND_NONE)
			goto stop;

		ret = input_stream_buffer(&inStream);
		if (ret < 0)
			goto stop;
	}

	/* for http streams, seekable is determined in input_stream_buffer */
	dc.seekable = inStream.seekable;

	if (dc.command == DECODE_COMMAND_STOP)
		goto stop;

	ret = DECODE_ERROR_UNKTYPE;
	if (!song_is_file(song)) {
		unsigned int next = 0;

		/* first we try mime types: */
		while (ret && (plugin = decoder_plugin_from_mime_type(inStream.mime, next++))) {
			if (plugin->stream_decode == NULL)
				continue;
			if (!(plugin->stream_types & INPUT_PLUGIN_STREAM_URL))
				continue;
			if (plugin->try_decode != NULL
			    && !plugin->try_decode(&inStream))
				continue;
			ret = plugin->stream_decode(&decoder, &inStream);
			break;
		}

		/* if that fails, try suffix matching the URL: */
		if (plugin == NULL) {
			const char *s = getSuffix(path_max_fs);
			next = 0;
			while (ret && (plugin = decoder_plugin_from_suffix(s, next++))) {
				if (plugin->stream_decode == NULL)
					continue;
				if (!(plugin->stream_types &
				      INPUT_PLUGIN_STREAM_URL))
					continue;
				if (plugin->try_decode != NULL &&
				    !plugin->try_decode(&inStream))
					continue;
				decoder.plugin = plugin;
				ret = plugin->stream_decode(&decoder,
							    &inStream);
				break;
			}
		}
		/* fallback to mp3: */
		/* this is needed for bastard streams that don't have a suffix
		   or set the mimeType */
		if (plugin == NULL) {
			/* we already know our mp3Plugin supports streams, no
			 * need to check for stream{Types,DecodeFunc} */
			if ((plugin = decoder_plugin_from_name("mp3"))) {
				decoder.plugin = plugin;
				ret = plugin->stream_decode(&decoder,
							    &inStream);
			}
		}
	} else {
		unsigned int next = 0;
		const char *s = getSuffix(path_max_fs);
		while (ret && (plugin = decoder_plugin_from_suffix(s, next++))) {
			if (!plugin->stream_types & INPUT_PLUGIN_STREAM_FILE)
				continue;

			if (plugin->try_decode != NULL &&
			    !plugin->try_decode(&inStream))
				continue;

			if (plugin->file_decode != NULL) {
				input_stream_close(&inStream);
				close_instream = false;
				decoder.plugin = plugin;
				ret = plugin->file_decode(&decoder,
							  path_max_fs);
				break;
			} else if (plugin->stream_decode != NULL) {
				decoder.plugin = plugin;
				ret = plugin->stream_decode(&decoder,
							    &inStream);
				break;
			}
		}
	}

	if (ret < 0 || ret == DECODE_ERROR_UNKTYPE) {
		if (ret != DECODE_ERROR_UNKTYPE)
			dc.error = DECODE_ERROR_FILE;
		else
			dc.error = DECODE_ERROR_UNKTYPE;
	}

stop:
	if (close_instream)
		input_stream_close(&inStream);
stop_no_close:
	dc.state = DECODE_STATE_STOP;
	dc.command = DECODE_COMMAND_NONE;
}

static void * decoder_task(mpd_unused void *arg)
{
	while (1) {
		assert(dc.state == DECODE_STATE_STOP);

		if (dc.command == DECODE_COMMAND_START ||
		    dc.command == DECODE_COMMAND_SEEK) {
			decodeStart();
		} else if (dc.command == DECODE_COMMAND_STOP) {
			dc.command = DECODE_COMMAND_NONE;
			notify_signal(&pc.notify);
		} else {
			notify_wait(&dc.notify);
			notify_signal(&pc.notify);
		}
	}

	return NULL;
}

void decoder_thread_start(void)
{
	pthread_attr_t attr;
	pthread_t decoder_thread;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	if (pthread_create(&decoder_thread, &attr, decoder_task, NULL))
		FATAL("Failed to spawn decoder task: %s\n", strerror(errno));
}

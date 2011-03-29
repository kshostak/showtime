/*
 *  Backend
 *  Copyright (C) 2008 Andreas Öman
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef BACKEND_H__
#define BACKEND_H__

#include "prop/prop.h"

struct pixmap;
struct media_pipe;
struct navigator;
struct event;

/**
 * Kept in sync with service_status_t
 */
typedef enum {
  BACKEND_PROBE_OK,
  BACKEND_PROBE_AUTH,
  BACKEND_PROBE_NO_HANDLER,
  BACKEND_PROBE_FAIL,
} backend_probe_result_t;


LIST_HEAD(play_video_subtitle_list, play_video_subtitle);

/**
 *
 */
typedef struct play_video_subtitle {
  LIST_ENTRY(play_video_subtitle) pvs_link;
  char *pvs_url;
  char *pvs_language;
} play_video_subtitle_t;



#define BACKEND_VIDEO_PRIMARY     0x1
#define BACKEND_VIDEO_NO_AUDIO    0x2
#define BACKEND_VIDEO_NO_AUTOSTOP 0x4

/**
 *
 */
typedef struct backend {

  LIST_ENTRY(backend) be_global_link;

  int be_flags;
#define BACKEND_OPEN_CHECKS_URI 0x1

  int (*be_init)(void);

  void (*be_fini)(void);

  int (*be_canhandle)(const char *ur);

  int (*be_open)(prop_t *page, const char *url);

  struct event *(*be_play_video)(const char *url,
				 struct media_pipe *mp,
				 int flags, int priority,
				 struct play_video_subtitle_list *subtitles,
				 char *errbuf, size_t errlen);

  struct event *(*be_play_audio)(const char *url, struct media_pipe *mp,
				 char *errbuf, size_t errlen, int paused);

  struct pixmap *(*be_imageloader)(const char *url, int want_thumb,
				   const char **vpaths,
				   char *errbuf, size_t errlen);

  int (*be_normalize)(const char *url, char *dst, size_t dstlen);

  int (*be_probe)(const char *url, char *errbuf, size_t errlen);

  void (*be_search)(struct prop *model, const char *query);

  int (*be_resolve_item)(const char *url, prop_t *item);

} backend_t;



/**
 *
 */
void backend_init(void);

void backend_fini(void);

int backend_open(struct prop *page, const char *url)
     __attribute__ ((warn_unused_result));

struct event *backend_play_video(const char *url, struct media_pipe *mp,
				 int flags, int priority,
				 struct play_video_subtitle_list *subtitles,
				 char *errbuf, size_t errlen)
  __attribute__ ((warn_unused_result));


struct event *backend_play_audio(const char *url, struct media_pipe *mp,
				 char *errbuf, size_t errlen, int paused)
  __attribute__ ((warn_unused_result));


struct pixmap *backend_imageloader(const char *url, int want_thumb,
				   const char **vpaths,
				   char *errbuf, size_t errlen)
     __attribute__ ((warn_unused_result));

backend_t *backend_canhandle(const char *url)
     __attribute__ ((warn_unused_result));


backend_probe_result_t backend_probe(const char *url,
				     char *errbuf, size_t errlen)
     __attribute__ ((warn_unused_result));

void backend_register(backend_t *be);

int backend_open_video(prop_t *page, const char *url)
     __attribute__ ((warn_unused_result));

int backend_resolve_item(const char *url, prop_t *item)
     __attribute__ ((warn_unused_result));


void backend_search(prop_t *model, const char *url);

#define BE_REGISTER(name) \
  static void  __attribute__((constructor)) backend_init_ ## name(void) {\
  static int cnt;							\
  if(cnt == 0)								\
    backend_register(&be_ ## name);					\
  cnt++;								\
  }


#endif /* BACKEND_H__ */

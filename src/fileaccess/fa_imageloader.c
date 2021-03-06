/*
 *  Image loader, used by libglw
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

#include <assert.h>
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


#include "showtime.h"
#include "fileaccess.h"
#include "fa_imageloader.h"
#if ENABLE_LIBAV
#include "fa_libav.h"
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#endif
#include "misc/pixmap.h"
#include "misc/jpeg.h"
#include "backend/backend.h"
#include "blobcache.h"

static const uint8_t pngsig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
static const uint8_t gif89sig[6] = {'G', 'I', 'F', '8', '9', 'a'};
static const uint8_t gif87sig[6] = {'G', 'I', 'F', '8', '7', 'a'};

static const uint8_t svgsig1[5] = {'<', '?', 'x', 'm', 'l'};
static const uint8_t svgsig2[4] = {'<', 's', 'v', 'g'};

#if ENABLE_LIBAV
static hts_mutex_t image_from_video_mutex[2];
static AVCodecContext *thumbctx;
static AVCodec *thumbcodec;

static pixmap_t *fa_image_from_video(const char *url, const image_meta_t *im,
				     char *errbuf, size_t errlen,
				     int *cache_control,
				     fa_load_cb_t *cb, void *opaque);
#endif

/**
 *
 */
void
fa_imageloader_init(void)
{
#if ENABLE_LIBAV
  hts_mutex_init(&image_from_video_mutex[0]);
  hts_mutex_init(&image_from_video_mutex[1]);
  thumbcodec = avcodec_find_encoder(CODEC_ID_MJPEG);
#endif
}


/**
 * Load entire image into memory using fileaccess load method.
 * Faster than open+read+close.
 */
static pixmap_t *
fa_imageloader2(const char *url, const char **vpaths,
		char *errbuf, size_t errlen, int *cache_control,
		fa_load_cb_t *cb, void *opaque)
{
  buf_t *buf;
  jpeg_meminfo_t mi;
  pixmap_type_t fmt;
  int width = -1, height = -1, orientation = 0;

  buf = fa_load(url, vpaths, errbuf, errlen, cache_control, 0, cb, opaque);
  if(buf == NULL || buf == NOT_MODIFIED)
    return (pixmap_t *)buf;

  const uint8_t *p = buf_c8(buf);
  mi.data = p;
  mi.size = buf->b_size;

  if(buf->b_size < 16)
    goto bad;

  /* Probe format */

  if((p[6] == 'J' && p[7] == 'F' && p[8] == 'I' && p[9] == 'F') ||
     (p[6] == 'E' && p[7] == 'x' && p[8] == 'i' && p[9] == 'f')) {
      
    jpeginfo_t ji;
    
    if(jpeg_info(&ji, jpeginfo_mem_reader, &mi, 
		 JPEG_INFO_DIMENSIONS | JPEG_INFO_ORIENTATION,
		 p, buf->b_size, errbuf, errlen)) {
      buf_release(buf);
      return NULL;
    }

    fmt = PIXMAP_JPEG;

    width = ji.ji_width;
    height = ji.ji_height;
    orientation = ji.ji_orientation;

    jpeg_info_clear(&ji);

  } else if(!memcmp(pngsig, p, 8)) {
    fmt = PIXMAP_PNG;
  } else if(!memcmp(gif87sig, p, sizeof(gif87sig)) ||
	    !memcmp(gif89sig, p, sizeof(gif89sig))) {
    fmt = PIXMAP_GIF;
  } else if(!memcmp(svgsig1, p, sizeof(svgsig1)) ||
	    !memcmp(svgsig2, p, sizeof(svgsig2))) {
    fmt = PIXMAP_SVG;
  } else {
  bad:
    snprintf(errbuf, errlen, "Unknown format");
    buf_release(buf);
    return NULL;
  }

  pixmap_t *pm = pixmap_alloc_coded(p, buf->b_size, fmt);
  if(pm != NULL) {
    pm->pm_width = width;
    pm->pm_height = height;
    pm->pm_orientation = orientation;
  } else {
    snprintf(errbuf, errlen, "Out of memory");
  }
  buf_release(buf);
  return pm;
}


/**
 *
 */
static int
jpeginfo_reader(void *handle, void *buf, off_t offset, size_t size)
{
  if(fa_seek(handle, offset, SEEK_SET) != offset)
    return -1;
  return fa_read(handle, buf, size);
}


/**
 *
 */
pixmap_t *
fa_imageloader(const char *url, const struct image_meta *im,
	       const char **vpaths, char *errbuf, size_t errlen,
	       int *cache_control, fa_load_cb_t *cb, void *opaque)
{
  uint8_t p[16];
  int r;
  int width = -1, height = -1, orientation = 0;
  fa_handle_t *fh;
  pixmap_t *pm;
  pixmap_type_t fmt;

#if ENABLE_LIBAV
  if(strchr(url, '#'))
    return fa_image_from_video(url, im, errbuf, errlen, cache_control,
			       cb, opaque);
#endif

  if(!im->im_want_thumb)
    return fa_imageloader2(url, vpaths, errbuf, errlen, cache_control,
			   cb, opaque);

  if((fh = fa_open_vpaths(url, vpaths, errbuf, errlen,
			  FA_BUFFERED_SMALL)) == NULL)
    return NULL;

  if(ONLY_CACHED(cache_control)) {
    snprintf(errbuf, errlen, "Not cached");
    return NULL;
  }

  if(fa_read(fh, p, sizeof(p)) != sizeof(p)) {
    snprintf(errbuf, errlen, "File too short");
    fa_close(fh);
    return NULL;
  }

  /* Probe format */

  if((p[6] == 'J' && p[7] == 'F' && p[8] == 'I' && p[9] == 'F') ||
     (p[6] == 'E' && p[7] == 'x' && p[8] == 'i' && p[9] == 'f')) {
      
    jpeginfo_t ji;
    
    if(jpeg_info(&ji, jpeginfo_reader, fh,
		 JPEG_INFO_DIMENSIONS |
		 JPEG_INFO_ORIENTATION |
		 (im->im_want_thumb ? JPEG_INFO_THUMBNAIL : 0),
		 p, sizeof(p), errbuf, errlen)) {
      fa_close(fh);
      return NULL;
    }

    if(im->im_want_thumb && ji.ji_thumbnail) {
      pixmap_t *pm = pixmap_dup(ji.ji_thumbnail);
      fa_close(fh);
      jpeg_info_clear(&ji);
      return pm;
    }

    fmt = PIXMAP_JPEG;

    width = ji.ji_width;
    height = ji.ji_height;
    orientation = ji.ji_orientation;

    jpeg_info_clear(&ji);

  } else if(!memcmp(pngsig, p, 8)) {
    fmt = PIXMAP_PNG;
  } else if(!memcmp(gif87sig, p, sizeof(gif87sig)) ||
	    !memcmp(gif89sig, p, sizeof(gif89sig))) {
    fmt = PIXMAP_GIF;
  } else if(!memcmp(svgsig1, p, sizeof(svgsig1)) ||
	    !memcmp(svgsig2, p, sizeof(svgsig2))) {
    fmt = PIXMAP_SVG;
  } else {
    snprintf(errbuf, errlen, "Unknown format");
    fa_close(fh);
    return NULL;
  }

  int64_t s = fa_fsize(fh);
  if(s < 0) {
    snprintf(errbuf, errlen, "Can't read from non-seekable file");
    fa_close(fh);
    return NULL;
  }

  pm = pixmap_alloc_coded(NULL, s, fmt);

  if(pm == NULL) {
    snprintf(errbuf, errlen, "Out of memory");
    fa_close(fh);
    return NULL;
  }

  pm->pm_width = width;
  pm->pm_height = height;
  pm->pm_orientation = orientation;
  fa_seek(fh, SEEK_SET, 0);
  r = fa_read(fh, pm->pm_data, pm->pm_size);
  fa_close(fh);

  if(r != pm->pm_size) {
    pixmap_release(pm);
    snprintf(errbuf, errlen, "Read error");
    return NULL;
  }
  return pm;
}

#if ENABLE_LIBAV

static char *ifv_url;
static AVFormatContext *ifv_fctx;
static AVCodecContext *ifv_ctx;
int ifv_stream;

static void
ifv_close(void)
{
  free(ifv_url);
  ifv_url = NULL;

  if(ifv_ctx != NULL) {
    avcodec_close(ifv_ctx);
    ifv_ctx = NULL;
  }

  if(ifv_fctx != NULL) {
    fa_libav_close_format(ifv_fctx);
    ifv_fctx = NULL;
  }
}


/**
 *
 */
static void
write_thumb(const AVCodecContext *src, const AVFrame *sframe, 
            int width, int height, const char *cacheid, time_t mtime)
{
  if(thumbcodec == NULL)
    return;

  AVCodecContext *ctx = thumbctx;
  static AVFrame *oframe;

  if(ctx == NULL || ctx->width  != width || ctx->height != height) {
    
    if(ctx != NULL) {
      avcodec_close(ctx);
      free(ctx);
    }

    ctx = avcodec_alloc_context3(thumbcodec);
    ctx->pix_fmt = AV_PIX_FMT_YUVJ420P;
    ctx->time_base.den = 1;
    ctx->time_base.num = 1;
    ctx->sample_aspect_ratio.num = 1;
    ctx->sample_aspect_ratio.den = 1;
    ctx->width  = width;
    ctx->height = height;

    if(avcodec_open2(ctx, thumbcodec, NULL) < 0) {
      TRACE(TRACE_ERROR, "THUMB", "Unable to open thumb encoder");
      thumbctx = NULL;
      return;
    }
    thumbctx = ctx;

    if(oframe == NULL) {
      oframe = avcodec_alloc_frame();
      memset(oframe, 0, sizeof(AVFrame));
    }
  }

  avpicture_alloc((AVPicture *)oframe, ctx->pix_fmt, width, height);
      
  struct SwsContext *sws;
  sws = sws_getContext(src->width, src->height, src->pix_fmt,
                       width, height, ctx->pix_fmt, SWS_BILINEAR,
                       NULL, NULL, NULL);

  sws_scale(sws, (const uint8_t **)sframe->data, sframe->linesize,
            0, src->height, &oframe->data[0], &oframe->linesize[0]);
  sws_freeContext(sws);

  oframe->pts = AV_NOPTS_VALUE;
  AVPacket out;
  memset(&out, 0, sizeof(AVPacket));
  int got_packet;
  int r = avcodec_encode_video2(ctx, &out, oframe, &got_packet);
  if(r >= 0 && got_packet) {
    buf_t *b = buf_create_and_adopt(out.size, out.data, &av_free);
    blobcache_put(cacheid, "videothumb", b, INT32_MAX, NULL, mtime);
    buf_release(b);
  } else {
    assert(out.data == NULL);
  }
  avpicture_free((AVPicture *)oframe);
}






/**
 *
 */
static pixmap_t *
fa_image_from_video2(const char *url, const image_meta_t *im, 
		     const char *cacheid, char *errbuf, size_t errlen,
		     int sec, time_t mtime, fa_load_cb_t *cb, void *opaque)
{
  pixmap_t *pm = NULL;

  if(ifv_url == NULL || strcmp(url, ifv_url)) {
    // Need to open
    int i;
    AVFormatContext *fctx;
    fa_handle_t *fh = fa_open_ex(url, errbuf, errlen, FA_BUFFERED_BIG, NULL);

    if(fh == NULL)
      return NULL;

    AVIOContext *avio = fa_libav_reopen(fh);

    if((fctx = fa_libav_open_format(avio, url, NULL, 0, NULL)) == NULL) {
      fa_libav_close(avio);
      snprintf(errbuf, errlen, "Unable to open format");
      return NULL;
    }

    if(!strcmp(fctx->iformat->name, "avi"))
      fctx->flags |= AVFMT_FLAG_GENPTS;

    AVCodecContext *ctx = NULL;
    for(i = 0; i < fctx->nb_streams; i++) {
      if(fctx->streams[i]->codec != NULL && 
	 fctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
	ctx = fctx->streams[i]->codec;
	break;
      }
    }
    if(ctx == NULL) {
      fa_libav_close_format(fctx);
      return NULL;
    }

    AVCodec *codec = avcodec_find_decoder(ctx->codec_id);
    if(codec == NULL) {
      fa_libav_close_format(fctx);
      snprintf(errbuf, errlen, "Unable to find codec");
      return NULL;
    }

    if(avcodec_open2(ctx, codec, NULL) < 0) {
      fa_libav_close_format(fctx);
      snprintf(errbuf, errlen, "Unable to open codec");
      return NULL;
    }

    ifv_close();

    ifv_stream = i;
    ifv_url = strdup(url);
    ifv_fctx = fctx;
    ifv_ctx = ctx;
  }

  AVPacket pkt;
  AVFrame *frame = avcodec_alloc_frame();
  int got_pic;


  AVStream *st = ifv_fctx->streams[ifv_stream];
  int64_t ts = av_rescale(sec, st->time_base.den, st->time_base.num);

  if(av_seek_frame(ifv_fctx, ifv_stream, ts, AVSEEK_FLAG_BACKWARD) < 0) {
    ifv_close();
    snprintf(errbuf, errlen, "Unable to seek to %"PRId64, ts);
    return NULL;
  }
  
  avcodec_flush_buffers(ifv_ctx);

#define MAX_FRAME_SCAN 500
  
  int cnt = MAX_FRAME_SCAN;
  while(1) {
    int r;

    r = av_read_frame(ifv_fctx, &pkt);
    
    if(r == AVERROR(EAGAIN))
      continue;
    
    if(r == AVERROR_EOF) {
      break;
    }
    
    if(cb != NULL && cb(opaque, 0, 1)) {
      snprintf(errbuf, errlen, "Aborted");
      break;
    }

    if(r != 0) {
      ifv_close();
      break;
    }

    if(pkt.stream_index != ifv_stream) {
      av_free_packet(&pkt);
      continue;
    }
    cnt--;
    int want_pic = pkt.pts >= ts || cnt <= 0;

    ifv_ctx->skip_frame = want_pic ? AVDISCARD_DEFAULT : AVDISCARD_NONREF;
    
    avcodec_decode_video2(ifv_ctx, frame, &got_pic, &pkt);
    av_free_packet(&pkt);
    if(got_pic == 0 || !want_pic) {
      continue;
    }
    int w,h;

    if(im->im_req_width != -1 && im->im_req_height != -1) {
      w = im->im_req_width;
      h = im->im_req_height;
    } else if(im->im_req_width != -1) {
      w = im->im_req_width;
      h = im->im_req_width * ifv_ctx->height / ifv_ctx->width;

    } else if(im->im_req_height != -1) {
      w = im->im_req_height * ifv_ctx->width / ifv_ctx->height;
      h = im->im_req_height;
    } else {
      w = im->im_req_width;
      h = im->im_req_height;
    }

    pm = pixmap_create(w, h, PIXMAP_BGR32, 0);

    if(pm == NULL) {
      ifv_close();
      snprintf(errbuf, errlen, "Out of memory");
      return NULL;
    }

    struct SwsContext *sws;
    sws = sws_getContext(ifv_ctx->width, ifv_ctx->height, ifv_ctx->pix_fmt,
			 w, h, AV_PIX_FMT_BGR32, SWS_BILINEAR,
                         NULL, NULL, NULL);
    if(sws == NULL) {
      ifv_close();
      snprintf(errbuf, errlen, "Scaling failed");
      pixmap_release(pm);
      return NULL;
    }
    
    uint8_t *ptr[4] = {0,0,0,0};
    int strides[4] = {0,0,0,0};

    ptr[0] = pm->pm_pixels;
    strides[0] = pm->pm_linesize;

    sws_scale(sws, (const uint8_t **)frame->data, frame->linesize,
	      0, ifv_ctx->height, ptr, strides);

    sws_freeContext(sws);

    write_thumb(ifv_ctx, frame, w, h, cacheid, mtime);

    break;
  }

  av_free(frame);
  if(pm == NULL)
    snprintf(errbuf, errlen, "Frame not found (scanned %d)", 
	     MAX_FRAME_SCAN - cnt);
  return pm;
}


/**
 *
 */
static pixmap_t *
fa_image_from_video(const char *url0, const image_meta_t *im,
		    char *errbuf, size_t errlen, int *cache_control,
		    fa_load_cb_t *cb, void *opaque)
{
  static char *stated_url;
  static fa_stat_t fs;
  time_t stattime = 0;
  time_t mtime = 0;
  pixmap_t *pm = NULL;
  char cacheid[512];
  char *url = mystrdupa(url0);
  char *tim = strchr(url, '#');
  const char *siz;
  *tim++ = 0;
  int secs = atoi(tim);

  hts_mutex_lock(&image_from_video_mutex[0]);
  
  if(strcmp(url, stated_url ?: "")) {
    free(stated_url);
    stated_url = NULL;
    if(fa_stat(url, &fs, errbuf, errlen)) {
      hts_mutex_unlock(&image_from_video_mutex[0]);
      return NULL;
    }
    stated_url = strdup(url);
  }
  stattime = fs.fs_mtime;
  hts_mutex_unlock(&image_from_video_mutex[0]);

  if(im->im_req_width < 100 && im->im_req_height < 100) {
    siz = "min";
  } else if(im->im_req_width < 200 && im->im_req_height < 200) {
    siz = "mid";
  } else {
    siz = "max";
  }

  snprintf(cacheid, sizeof(cacheid), "%s-%s", url0, siz);
  buf_t *b = blobcache_get(cacheid, "videothumb", 0, 0, NULL, &mtime);
  if(b != NULL && mtime == stattime) {
    pm = pixmap_alloc_coded(b->b_ptr, b->b_size, PIXMAP_JPEG);
    buf_release(b);
    return pm;
  }
  buf_release(b);

  if(ONLY_CACHED(cache_control)) {
    snprintf(errbuf, errlen, "Not cached");
    return NULL;
  }

  hts_mutex_lock(&image_from_video_mutex[1]);
  pm = fa_image_from_video2(url, im, cacheid, errbuf, errlen,
			    secs, stattime, cb, opaque);
  hts_mutex_unlock(&image_from_video_mutex[1]);
  return pm;
}
#endif

/*
 *  Built-in upgrade
 *  Copyright (C) 2012 Andreas Öman
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

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

#include "showtime.h"
#include "upgrade.h"
#include "arch/arch.h"
#include "arch/halloc.h"
#include "fileaccess/fileaccess.h"
#include "htsmsg/htsmsg_json.h"
#include "htsmsg/htsmsg_store.h"
#include "misc/sha.h"
#include "misc/str.h"
#include "settings.h"
#include "notifications.h"

#if CONFIG_BSPATCH
#include "ext/bspatch/bspatch.h"
#endif

extern char *showtime_bin;

static const char *ctrlbase = "http://showtime.lonelycoder.com/upgrade/";
static const char *artifact_type;
static const char *archname;

static prop_t *upgrade_root;
static prop_t *upgrade_status;
static prop_t *upgrade_error;
static prop_t *upgrade_progress;
static char *upgrade_track;
static char *download_url;
static uint8_t download_digest[20];
static int download_size;
static int notify_upgrades;
static int inhibit_checks = 1;
static prop_t *news_ref;

/**
 *
 * global.upgrade
 *
 *   .track ("testing", "stable", etc)
 *   .availableVersion
 *   .status   upToDate
 *             checking
 *             canUpgrade
 *             prepare
 *             download
 *             patch
 *             installing
 *             upgradeError
 *             checkError
 *             countdown
 *   .errorstr
 *   .downloadSize
 *   .progress (in percent)
 *   .eventSink
 *   .changelog
 *        .version
 *        .text
 *
 */





static void
check_upgrade(int set_news)
{
  char url[1024];
  buf_t *b;
  htsmsg_t *json;
  char errbuf[1024];

  if(inhibit_checks)
    return;

  if(upgrade_track == NULL) {
    prop_set_string(upgrade_error, "No release track specified");
    goto err;
  }

  prop_set_string(upgrade_status, "checking");

  TRACE(TRACE_DEBUG, "Upgrade", "Checking upgrades for %s-%s",
	upgrade_track, archname);

  snprintf(url, sizeof(url), "%s/%s-%s.json", ctrlbase, upgrade_track,
	   archname);

  b = fa_load(url, NULL, errbuf, sizeof(errbuf),
              NULL, FA_DISABLE_AUTH, NULL, NULL);
  if(b == NULL) {
    prop_set_string(upgrade_error, errbuf);
  err:
    prop_set_string(upgrade_status, "checkError");
    return;
  }

  json = htsmsg_json_deserialize(buf_cstr(b));
  buf_release(b);

  if(json == NULL) {
    prop_set_string(upgrade_error, "Malformed JSON in repository");
    goto err;
  }

  // Find an artifact for us

  const char *dlurl = NULL;
  const char *sha1 = NULL;
  int dlsize = 0;
  const char *ver;

  htsmsg_t *artifacts = htsmsg_get_list(json, "artifacts");
  if(artifacts != NULL) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, artifacts) {
      htsmsg_t *a;
      if((a = htsmsg_get_map_by_field(f)) == NULL)
	continue;

      const char *type = htsmsg_get_str(a, "type");

      if(type == NULL || strcmp(artifact_type, type))
	continue;

      dlurl = htsmsg_get_str(a, "url");
      sha1 = htsmsg_get_str(a, "sha1");
      dlsize = htsmsg_get_u32_or_default(a, "size", 0);
      break;
    }
  }

  ver = htsmsg_get_str(json, "version");

  if(dlurl == NULL || dlsize == 0 || sha1 == NULL || ver == NULL) {
    prop_set_string(upgrade_error, "No URL or size present");
    goto err;
  }

  hex2bin(download_digest, sizeof(download_digest), sha1);

  mystrset(&download_url, dlurl);

  prop_set(upgrade_root, "track", PROP_SET_STRING, upgrade_track);
  prop_set(upgrade_root, "availableVersion", PROP_SET_STRING, ver);

  download_size = dlsize;

  prop_set(upgrade_root, "size", PROP_SET_INT, dlsize);

  int canUpgrade = gconf.enable_omnigrade;
  
  if(ver != NULL) {
    int current_ver = showtime_get_version_int();
    int available_ver = showtime_parse_version_int(ver);
    if(available_ver > current_ver) {
      canUpgrade = 1;
    }
  }

  if(canUpgrade) {
    prop_set_string(upgrade_status, "canUpgrade");
  } else {
    prop_set_string(upgrade_status, "upToDate");
  }

  prop_destroy(news_ref);
  prop_ref_dec(news_ref);

  if(set_news && canUpgrade) {
    rstr_t *r = _("Showtime version %s is available");
    rstr_t *s = _("Open download page");
    char buf[128];
    snprintf(buf, sizeof(buf), rstr_get(r), ver);
    news_ref = add_news(buf, "page:upgrade", rstr_get(s));
    rstr_release(r);
    rstr_release(s);
  }

  // Update changelog

  prop_t *changelog = prop_create(upgrade_root, "changelog");
  prop_destroy_childs(changelog);
  htsmsg_t *chlog = htsmsg_get_list(json, "changelog");

  if(chlog) {
    htsmsg_field_t *f;
    HTSMSG_FOREACH(f, chlog) {
      htsmsg_t *le;
      if((le = htsmsg_get_map_by_field(f)) == NULL)
	continue;
      const char *version = htsmsg_get_str(le, "version");
      const char *text    = htsmsg_get_str(le, "desc");
      prop_t *q = prop_create_root(NULL);
      prop_set_string(prop_create(q, "version"), version);
      prop_set_string(prop_create(q, "text"), text);
      if(prop_set_parent(q, changelog))
	abort();
    }
  }
  htsmsg_destroy(json);
}


/**
 *
 */
static int
download_callback(void *opaque, int loaded, int total)
{
  if(!total)
    total = download_size;

  prop_set(upgrade_root, "size", PROP_SET_INT, total);
  prop_set_float(upgrade_progress, (float)loaded / (float)total);
  return 0;
}


static void
install_error(const char *str)
{
  prop_set_string(upgrade_error, str);
  prop_set_string(upgrade_status, "upgradeError");
  TRACE(TRACE_ERROR, "upgrade", "Download of %s failed -- %s", 
	download_url, str);

}

/**
 *
 */
static int
attempt_upgrade(int accept_patch)
{
  const char *fname = gconf.binary;
  uint8_t digest[20];
  char digeststr[41];
  char errbuf[1024];
  int fd;
  struct http_header_list req_headers;
  struct http_header_list response_headers;

  void *current_data = NULL;
  int current_size = 0;

  sha1_decl(shactx);

  LIST_INIT(&req_headers);
  LIST_INIT(&response_headers);

  prop_set_float(upgrade_progress, 0);
  prop_set_string(upgrade_status, "prepare");

#if CONFIG_BSPATCH
  char ae[128];
  ae[0] = 0;
  if(accept_patch) {

    // Figure out SHA-1 of currently running binary

    fd = open(fname, O_RDONLY);
    if(fd == -1)
      return -1;

    struct stat st;
    if(fstat(fd, &st)) {
      close(fd);
      return -1;
    }

    current_size = st.st_size;
    current_data = halloc(current_size);
    if(current_data == NULL) {
      close(fd);
      return -1;
    }
    if(read(fd, current_data, current_size) != current_size) {
      hfree(current_data, current_size);
      close(fd);
      return -1;
    }
    close(fd);

    sha1_init(shactx);
    sha1_update(shactx, current_data, current_size);
    sha1_final(shactx, digest);
    bin2hex(digeststr, sizeof(digeststr), digest, sizeof(digest));
    snprintf(ae, sizeof(ae), "bspatch-from-%s", digeststr);
    http_header_add(&req_headers, "Accept-Encoding", ae, 0);
  }
#endif

  prop_set_string(upgrade_status, "download");
  TRACE(TRACE_INFO, "upgrade", "Starting download of %s (%d bytes)", 
	download_url, download_size);
 
  buf_t *b;

  int r = http_request(download_url, NULL, &b,
		       errbuf, sizeof(errbuf), NULL, NULL,
		       FA_COMPRESSION | FA_DEBUG,
		       &response_headers, &req_headers, NULL,
		       download_callback, NULL);
  
  if(r) {
    install_error(errbuf);

    if(current_data)
      hfree(current_data, current_size);

    return -1;
  }

#if CONFIG_BSPATCH

  const char *encoding = http_header_get(&response_headers,
					 "Content-Encoding");

  int got_patch = encoding && !strcmp(encoding, ae);

  if(current_data) {

    if(got_patch) {
      TRACE(TRACE_DEBUG, "upgrade", "Received upgrade as patch (%d bytes)",
	    (int)b->b_size);

      prop_set_string(upgrade_status, "patch");
      buf_t *new = bspatch(current_data, current_size, b->b_ptr, b->b_size);
      buf_release(b);
      if(new == NULL) {
	TRACE(TRACE_DEBUG, "upgrade", "Patch is corrupt");
	hfree(current_data, current_size);
	return -1;
      }
      b = new;
    }
    hfree(current_data, current_size);
  }
#endif

  TRACE(TRACE_DEBUG, "upgrade", "Verifying SHA-1 of %d bytes",
        (int)b->b_size);

  prop_set_string(upgrade_status, "install");

  int match;

  sha1_init(shactx);
  sha1_update(shactx, b->b_ptr, b->b_size);
  sha1_final(shactx, digest);

  match = !memcmp(digest, download_digest, 20);

  bin2hex(digeststr, sizeof(digeststr), digest, sizeof(digest));
  TRACE(TRACE_DEBUG, "upgrade", "SHA-1 of downloaded file: %s (%s)", digeststr,
	match ? "match" : "no match");

  if(!match) {
    install_error("SHA-1 sum mismatch");
    buf_release(b);
    return -1;
  }

  TRACE(TRACE_INFO, "upgrade", "Replacing %s with %d bytes received",
	fname, (int)b->b_size);

  if(unlink(fname)) {
    install_error("Unlink failed");
    buf_release(b);
    return -1;
  }

  TRACE(TRACE_DEBUG, "upgrade", "Executable removed, rewriting");

  fd = open(fname, O_CREAT | O_RDWR, 0777);
  if(fd == -1) {
    install_error("Unable to open file");
    buf_release(b);
    return -1;
  }

  int fail = write(fd, b->b_ptr, b->b_size) != b->b_size;
  fail |= !!close(fd);
  buf_release(b);

  if(fail) {
    install_error("Unable to write to file");
    return -1;
  }

  TRACE(TRACE_INFO, "upgrade", "All done, restarting");
  prop_set_string(upgrade_status, "countdown");
  prop_t *cnt = prop_create(upgrade_root, "countdown");
  int i;
  for(i = 3; i > 0; i--) {
    prop_set_int(cnt, i);
    sleep(1);
  }
  showtime_shutdown(13);

  return 0;
}



static void *
install_thread(void *aux)
{
  if(gconf.enable_patched_upgrade) {
    if(!attempt_upgrade(1))
      return NULL;
  }

  attempt_upgrade(0);
  return NULL;
}


static void
install(void)
{
  hts_thread_create_detached("upgrade", install_thread, NULL,
			     THREAD_PRIO_BGTASK);
}


/**
 *
 */
static void
upgrade_cb(void *opaque, prop_event_t event, ...)
{
  va_list ap;
  event_t *e;
  va_start(ap, event);

  switch(event) {
  case PROP_EXT_EVENT:
    e = va_arg(ap, event_t *);
    if(event_is_type(e, EVENT_DYNAMIC_ACTION)) {
      if(!strcmp(e->e_payload, "checkUpdates")) 
	check_upgrade(0);
      if(!strcmp(e->e_payload, "install")) 
	install();
    }
    break;

  default:
    break;
  }
  va_end(ap);
}


/**
 *
 */
static void
set_upgrade_track(void *opaque, const char *str)
{
  mystrset(&upgrade_track, str);
  check_upgrade(0);
}



/**
 *
 */
static void
set_notify_upgrades(void *opaque, int v)
{
  notify_upgrades = v;
}

/**
 *
 */
void
upgrade_init(void)
{
  
  if(gconf.binary == NULL)
    return;

#if PS3
  artifact_type = "self";
  archname = "ps3";
#endif

  if(artifact_type == NULL || archname == NULL)
    return;

  upgrade_root     = prop_create(prop_get_global(), "upgrade");
  upgrade_status   = prop_create(upgrade_root, "status");
  upgrade_progress = prop_create(upgrade_root, "progress");
  upgrade_error    = prop_create(upgrade_root, "error");


  htsmsg_t *store;

  if((store = htsmsg_store_load("upgrade")) == NULL)
    store = htsmsg_create_map();

  setting_t *x;

  settings_create_separator(gconf.settings_general,
			  _p("Software upgrade"));

  x = settings_create_multiopt(gconf.settings_general, "track",
			       _p("Upgrade to releases from"), 0);

  settings_multiopt_add_opt(x, "stable", _p("Stable"), 1);
  settings_multiopt_add_opt(x, "testing", _p("Testing"), 0);

  settings_multiopt_initiate(x, set_upgrade_track, NULL, NULL, 
			     store, settings_generic_save_settings,
                             (void *)"upgrade");

  settings_create_bool(gconf.settings_general, "check",
		       _p("Notify about upgrades"), 1,
		       store, set_notify_upgrades, NULL, 
		       SETTINGS_INITIAL_UPDATE, NULL,
		       settings_generic_save_settings, 
		       (void *)"upgrade");

  prop_t *p = prop_create_root(NULL);
  prop_link(_p("Check for updates now"),
	    prop_create(prop_create(p, "metadata"), "title"));
  prop_set_string(prop_create(p, "type"), "load");
  prop_set_string(prop_create(p, "url"), "page:upgrade");

  if(prop_set_parent(p, prop_create(gconf.settings_general, "nodes")))
     abort();

  inhibit_checks = 0;
  check_upgrade(notify_upgrades);

  prop_subscribe(0,
		 PROP_TAG_CALLBACK, upgrade_cb, NULL,
		 PROP_TAG_NAME("global", "upgrade", "eventSink"),
		 NULL);
}

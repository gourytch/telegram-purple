/*
    This file is part of telegram-purple
 
    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
 
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
 
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02111-1301  USA
 
    Copyright Matthias Jentsch, Vitaly Valtman, Christopher Althaus, Markus Endres 2014
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <glib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>
#include <regex.h>

#include "purple.h"
#include "notify.h"
#include "plugin.h"
#include "version.h"
#include "account.h"
#include "accountopt.h"
#include "blist.h"
#include "cmds.h"
#include "conversation.h"
#include "connection.h"
#include "debug.h"
#include "privacy.h"
#include "prpl.h"
#include "roomlist.h"
#include "status.h"
#include "util.h"
#include "prpl.h"
#include "prefs.h"
#include "util.h"
#include "eventloop.h"
#include "request.h"

#include <tgl.h>
#include <tgl-binlog.h>
#include <tools.h>
#include "tgp-structs.h"
#include "tgp-2prpl.h"
#include "tgp-net.h"
#include "tgp-timers.h"
#include "telegram-base.h"
#include "telegram-purple.h"
#include "msglog.h"
#include "tgp-utils.h"
#include "tgp-chat.h"
#include "tgp-ft.h"

#define _(m) m

PurplePlugin *_telegram_protocol = NULL;
PurpleGroup *tggroup;
const char *config_dir = ".telegram-purple";
const char *pk_path = "/etc/telegram-purple/server.pub";

void on_user_get_info (struct tgl_state *TLS, void *info_data, int success, struct tgl_user *U);

static char *format_status (struct tgl_user_status *status) {
  return status->online ? "Online" : "Mobile";
}

static char *format_service_msg (struct tgl_state *TLS, struct tgl_message *M) {
  assert (M && M->service);

  char *txt_user = NULL;
  char *txt_action = NULL;
  char *txt = NULL;
  
  tgl_peer_t *peer = tgl_peer_get (TLS, M->from_id);
  if (! peer) {
    return NULL;
  }
  txt_user = p2tgl_strdup_alias (peer);
  
  switch (M->action.type) {
    case tgl_message_action_chat_create:
      txt_action = g_strdup_printf ("created chat %s", M->action.title);
      break;
    case tgl_message_action_chat_edit_title:
      txt_action = g_strdup_printf ("changed title to %s", M->action.new_title);
      break;
    case tgl_message_action_chat_edit_photo:
      txt_action = g_strdup ("changed photo");
      break;
    case tgl_message_action_chat_delete_photo:
      txt_action = g_strdup ("deleted photo");
      break;
    case tgl_message_action_chat_add_user:
      {
        tgl_peer_t *peer = tgl_peer_get (TLS, TGL_MK_USER (M->action.user));
        if (peer) {
          char *alias = p2tgl_strdup_alias (peer);
          txt_action = g_strdup_printf ("added user %s", alias);
          g_free (alias);
        }
        break;
      }
    case tgl_message_action_chat_delete_user:
      {
        tgl_peer_t *peer = tgl_peer_get (TLS, TGL_MK_USER (M->action.user));
        if (peer) {
          char *alias = p2tgl_strdup_alias (peer);
          txt_action = g_strdup_printf ("deleted user %s", alias);
          g_free (alias);
        }
        break;
      }
    case tgl_message_action_set_message_ttl:
      txt_action = g_strdup_printf ("set ttl to %d seconds", M->action.ttl);
      break;
    case tgl_message_action_read_messages:
      txt_action = g_strdup_printf ("%d messages marked read", M->action.read_cnt);
      break;
    case tgl_message_action_delete_messages:
      txt_action = g_strdup_printf ("%d messages deleted", M->action.delete_cnt);
      break;
    case tgl_message_action_screenshot_messages:
      txt_action = g_strdup_printf ("%d messages screenshoted", M->action.screenshot_cnt);
      break;
    case tgl_message_action_notify_layer:
      txt_action = g_strdup_printf ("updated layer to %d", M->action.layer);
      break;
    case tgl_message_action_request_key:
      txt_action = g_strdup_printf ("Request rekey #%016llx", M->action.exchange_id);
      break;
    case tgl_message_action_accept_key:
      txt_action = g_strdup_printf ("Accept rekey #%016llx", M->action.exchange_id);
      break;
    case tgl_message_action_commit_key:
      txt_action = g_strdup_printf ("Commit rekey #%016llx", M->action.exchange_id);
      break;
    case tgl_message_action_abort_key:
      txt_action = g_strdup_printf ("Abort rekey #%016llx", M->action.exchange_id);
      break;
    default:
      txt_action = NULL;
      break;
  }
  if (txt_action) {
    debug ("SERVICE MESSAGE: %s", txt_action);
    txt = g_strdup_printf ("%s %s.", txt_user, txt_action);
    g_free (txt_action);
  }
  g_free (txt_user);
  return txt;
}

char *format_user_status (struct tgl_user_status *status) {
  char *when;
  switch (status->online) {
    case -1:
      when = g_strdup_printf("%s", format_time (status->when));
      break;
    case -2:
      when = g_strdup_printf("recently");
      break;
    case -3:
      when = g_strdup_printf("last week");
      break;
    case -4:
      when = g_strdup_printf("last month");
      break;
    default:
      when = g_strdup ("unknown");
      break;
  }
  return when;
}

static char *format_print_name (struct tgl_state *TLS, tgl_peer_id_t id, const char *a1, const char *a2, const char *a3, const char *a4) {
  const char *d[4];
  d[0] = a1; d[1] = a2; d[2] = a3; d[3] = a4;
  static char buf[10000];
  buf[0] = 0;
  int i;
  int p = 0;
  for (i = 0; i < 4; i++) {
    if (d[i] && strlen (d[i])) {
      p += tgl_snprintf (buf + p, 9999 - p, "%s%s", p ? " " : "", d[i]);
      assert (p < 9990);
    }
  }
  char *s = buf;
  while (*s) {
    if (*s == '\n') { *s = ' '; }
    if (*s == '#') { *s = '@'; }
    s++;
  }
  s = buf;
  int fl = strlen (s);
  int cc = 0;
  while (1) {
    tgl_peer_t *P = tgl_peer_get_by_name (TLS, s);
    if (!P || !tgl_cmp_peer_id (P->id, id)) {
      break;
    }
    cc ++;
    assert (cc <= 9999);
    tgl_snprintf (s + fl, 9999 - fl, " #%d", cc);
  }
  return tgl_strdup (s);
}

static char *format_document_desc (char *type, char *caption, gint64 size) {
  char *s = tgp_g_format_size (size);
  char *msg = g_strdup_printf ("[%s] %s %s", type, caption, s);
  g_free (s);
  return msg;
}

static char *format_message (struct tgl_message *M) {

  switch (M->media.type) {
    case tgl_message_media_document:
      return format_document_desc("DOCUMENT", M->media.document.caption, M->media.document.size);
      break;
    case tgl_message_media_document_encr:
      return format_document_desc("DOCUMENT", M->media.encr_document.caption, M->media.encr_document.size);
      break;
    case tgl_message_media_photo_encr:
      return format_document_desc("PHOTO", "", M->media.encr_photo.size);
      break;
    case tgl_message_media_contact:
      return g_strdup ("[CONTACT]");
      break;
    default:
      if (M->message && *M->message != 0) {
        return purple_markup_escape_text (M->message, strlen (M->message));
      }
      return g_strdup("");
      break;
  }
}

static void picture_message_done (struct tgl_state *TLS, void *callback_extra, int success, struct tgl_message *M) {
  if (success) {
    debug ("send photo sucess!");
  } else {
    debug ("send photo fail!");
  }
}

static void tgl_do_send_unescape_message (struct tgl_state *TLS, const char *message, tgl_peer_id_t to) {
  
  debug (message);
  
  // search for outgoing embedded image tags and send them
  gchar *img = NULL;
  gchar *stripped = NULL;
  if ((img = g_strrstr (message, "<IMG")) || (img = g_strrstr (message, "<img"))) {
    debug ("img found: %s", img);
    
    gchar *id;
    if ((id = g_strrstr (img, "ID=\"")) || (id = g_strrstr (img, "id=\""))) {
      id += 4;
      debug ("id found: %s", id);
      int imgid = atoi (id);
      
      if (imgid > 0) {
        PurpleStoredImage *psi = purple_imgstore_find_by_id (imgid);
        
        gchar *tmp = g_build_filename(g_get_tmp_dir(), purple_imgstore_get_filename (psi), NULL) ;
        
        GError *err = NULL;
        gconstpointer data = purple_imgstore_get_data (psi);
        g_file_set_contents (tmp, data, purple_imgstore_get_size (psi), &err);
        if (! err) {
          tgl_do_send_document (TLS, -2, to, tmp, picture_message_done, NULL);
        } else {
          failure ("Cannot store image, temp directory not available: %s\n", err->message);
          g_error_free (err);
        }
      }
    }
    
    // send remaining text as additional plaintext message
    stripped = purple_markup_strip_html (message);
    tgl_do_send_message (TLS, to, stripped, (int)strlen (stripped), 0, 0);
    g_free (stripped);
    return;
  }
  
#ifndef __ADIUM_
  /* 
     Adium won't escape any HTML markup and just pass it through,
     while Pidgin will replace special chars with the escape chars.
     Also, Pidgin will add additional markup for RTL languages and such,
     which Adium doesn't do either. 
     
     TODO: This is ridiculous, there has to a better way to handle this across platforms.
   */
  
  /* first, we remove any HTML markup added by Pidgin, since Telegram won't handle it properly.
     User-entered HTML is still escaped and therefore won't be harmed. */
  stripped = purple_markup_strip_html (message);
  
  /* now unescape the markup, so that html special chars will still show
     up properly in Telegram */
  gchar *unescaped = purple_unescape_text (stripped);
  tgl_do_send_message (TLS, to, unescaped, (int)strlen (unescaped), 0, 0);
  
  g_free (unescaped);
  g_free (stripped);
  return;
#endif
  
  tgl_do_send_message (TLS, to, message, (int)strlen (message), 0, 0);
}

static void start_secret_chat (PurpleBlistNode *node, gpointer data) {
  PurpleBuddy *buddy = data;

  connection_data *conn = get_conn_from_buddy (buddy);
  const char *name = purple_buddy_get_name (buddy);
  tgl_do_create_secret_chat (conn->TLS, TGL_MK_USER(atoi (name)), 0, 0);
}

static void update_message_received (struct tgl_state *TLS, struct tgl_message *M);
static void update_user_handler (struct tgl_state *TLS, struct tgl_user *U, unsigned flags);
static void update_chat_handler (struct tgl_state *TLS, struct tgl_chat *C, unsigned flags);
static void update_secret_chat_handler (struct tgl_state *TLS, struct tgl_secret_chat *C, unsigned flags);
static void update_user_typing (struct tgl_state *TLS, struct tgl_user *U, enum tgl_typing_status status);
struct tgl_update_callback tgp_callback = {
  .logprintf = debug,
  .new_msg = update_message_received, 
  .msg_receive = update_message_received,
  .user_update = update_user_handler,
  .chat_update = update_chat_handler,
  .secret_chat_update = update_secret_chat_handler,
  .type_notification = update_user_typing,
  .create_print_name = format_print_name
};

void on_message_load_photo (struct tgl_state *TLS, void *extra, int success, char *filename) {
  
  connection_data *conn = TLS->ev_base;

  int imgStoreId = p2tgl_imgstore_add_with_id (filename);
  if (imgStoreId > 0) {
    used_images_add (conn, imgStoreId);
    char *image = format_img_full (imgStoreId);
    
    struct tgl_message *M = extra;
    switch (tgl_get_peer_type (M->to_id)) {
      case TGL_PEER_CHAT:
        if (!our_msg (TLS, M)) {
          chat_add_message (TLS, M, image);
        }
        break;
        
      case TGL_PEER_USER:
        if (out_msg (TLS, M)) {
          p2tgl_got_im_combo (TLS, M->to_id, image, PURPLE_MESSAGE_SEND | PURPLE_MESSAGE_IMAGES, M->date);
        } else {
          p2tgl_got_im (TLS, M->from_id, image, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES, M->date);
        }
        break;
    }
    g_free (image);
  }

}

static void update_message_received (struct tgl_state *TLS, struct tgl_message *M) {
  connection_data *conn = TLS->ev_base;
  
  /* don't write to this file on every single message, which
     can be pretty overkill on calls to tgl_do_get_difference that result in many
     updates */
  write_state_file_schedule (conn->TLS);
  
  if (M->service) {
    char *text = format_service_msg (TLS, M);
    if (text) {
      switch (tgl_get_peer_type (M->to_id)) {
        case TGL_PEER_CHAT:
          chat_add_message (TLS, M, text);
          break;
          
        case TGL_PEER_USER:
          p2tgl_got_im (TLS, M->from_id, text, PURPLE_MESSAGE_SYSTEM, M->date);
          break;
      }
      g_free (text);
    }
    return;
  }
  
  if (
      // ignore all empty or deleted messages
      (M->flags & (FLAG_MESSAGE_EMPTY | FLAG_DELETED)) ||
     
      // ignore all messages that are not created for the first time
     !(M->flags & FLAG_CREATED) ||
      
      // drop all messages that don't contain any data
      !M->message ||
      
      // drop messages that were created by *this* client and were already
      // added to the conversation
      our_msg (TLS, M) ||
      
      // drop all messages with an invalid peer id.
      // TODO: Verify if we actually need this
      !tgl_get_peer_type (M->to_id)
  ) {
    return;
  }

  if (M->media.type == tgl_message_media_photo) {
    tgl_do_load_photo (TLS, &M->media.photo, on_message_load_photo, M);
    return;
  }
  
  if (M->media.type == tgl_message_media_document) {
    char *who = g_strdup_printf("%d", tgl_get_peer_id(M->from_id));
    
    tgprpl_recv_file (conn->gc, who, &M->media.document);
    
    g_free (who);
    return;
  }

  char *text = format_message (M);
  switch (tgl_get_peer_type (M->to_id)) {
    case TGL_PEER_CHAT:
      chat_add_message (TLS, M, text);
      break;
      
    case TGL_PEER_ENCR_CHAT:
        p2tgl_got_im (TLS, M->to_id, text, PURPLE_MESSAGE_RECV, M->date);
      
        pending_reads_add (conn->pending_reads, M->to_id);
        if (p2tgl_status_is_present (purple_account_get_active_status(conn->pa))) {
          pending_reads_send_all (conn->pending_reads, conn->TLS);
        }
      break;
      
    case TGL_PEER_USER:
      
      if (out_msg (TLS, M)) {
        p2tgl_got_im_combo (TLS, M->to_id, text, PURPLE_MESSAGE_SEND, M->date);
      } else {
        p2tgl_got_im (TLS, M->from_id, text, PURPLE_MESSAGE_RECV, M->date);
        
        pending_reads_add (conn->pending_reads, M->from_id);
        if (p2tgl_status_is_present (purple_account_get_active_status(conn->pa))) {
          pending_reads_send_all (conn->pending_reads, conn->TLS);
        }
      }
      break;
  }
  
  g_free (text);
}

static void update_user_handler (struct tgl_state *TLS, struct tgl_user *user, unsigned flags) {
  if (TLS->our_id == tgl_get_peer_id (user->id)) {
    if (flags & TGL_UPDATE_NAME) {
      p2tgl_connection_set_display_name (TLS, (tgl_peer_t *)user);
    }
  } else {
    PurpleBuddy *buddy = p2tgl_buddy_find (TLS, user->id);
    if (!buddy) {
      buddy = p2tgl_buddy_new (TLS, (tgl_peer_t *)user);
      purple_blist_add_buddy (buddy, NULL, tggroup, NULL);
    }
    if (flags & TGL_UPDATE_CREATED) {
      purple_buddy_set_protocol_data (buddy, (gpointer)user);
      p2tgl_prpl_got_user_status (TLS, user->id, &user->status);
      p2tgl_buddy_update (TLS, (tgl_peer_t *)user, flags);
    }
    if (flags & TGL_UPDATE_PHOTO) {
      tgl_do_get_user_info (TLS, user->id, 0, on_user_get_info, get_user_info_data_new (0, user->id));
    }
    if (flags & TGL_UPDATE_DELETED && buddy) {
      purple_blist_remove_buddy (buddy);
    }
  }
}

static void write_secret_chat_gw (struct tgl_state *TLS, void *extra, int success, struct tgl_secret_chat *E) {
  if (!success) { return; }
  write_secret_chat_file (TLS);
}

struct accept_secret_chat_data {
  struct tgl_state *TLS;
  struct tgl_secret_chat *U;
};

static void accept_secret_chat_cb (gpointer _data, const gchar *code) {
  struct accept_secret_chat_data *data = _data;
  
  tgl_do_accept_encr_chat_request (data->TLS, data->U, write_secret_chat_gw, 0);
  
  g_free (data);
}

static void decline_secret_chat_cb (gpointer _data, const gchar *code) {
  struct accept_secret_chat_data *data = _data;
  
  bl_do_encr_chat_delete (data->TLS, data->U);
  purple_blist_remove_buddy (p2tgl_buddy_find(data->TLS, data->U->id));
  
  g_free (data);
}

static void update_secret_chat_handler (struct tgl_state *TLS, struct tgl_secret_chat *U, unsigned flags) {
  debug ("secret-chat-state: %d", U->state);
  
  if (flags & TGL_UPDATE_WORKING || flags & TGL_UPDATE_DELETED) {
    write_secret_chat_file (TLS);
  }

  PurpleBuddy *buddy = p2tgl_buddy_find (TLS, U->id);
  
  if (! (flags & TGL_UPDATE_DELETED)) {
    if (!buddy) {
      buddy = p2tgl_buddy_new (TLS, (tgl_peer_t *)U);
      purple_blist_add_buddy (buddy, NULL, tggroup, NULL);
      purple_blist_alias_buddy (buddy, U->print_name);
    }
    p2tgl_prpl_got_set_status_mobile (TLS, U->id);
  }
  
  if (flags & TGL_UPDATE_REQUESTED && buddy)  {
    connection_data *conn = TLS->ev_base;
    
    const char* choice = purple_account_get_string (conn->pa, "accept-secret-chats", "ask");
    if (! strcmp (choice, "always")) {
      tgl_do_accept_encr_chat_request (TLS, U, write_secret_chat_gw, 0);
      
    } else if (! strcmp(choice, "ask")) {
      PurpleBuddy *who = p2tgl_buddy_find (TLS, TGL_MK_USER(U->user_id));
      
      struct accept_secret_chat_data *data = g_new (struct accept_secret_chat_data, 1);
      data->TLS = TLS;
      data->U = U;
     
      gchar *message = g_strdup_printf ("Accept Secret Chat '%s'?", U->print_name);
      
      purple_request_accept_cancel (conn->gc, "Secret Chat", message, "Secret chats can only have one "
                                    "end point. If you accept a secret chat on this device, its messages will "
                                    "not be available anywhere else. If you decline, you can accept"
                                    " the chat on other devices.", 0, conn->pa, who->name, NULL, data,
                                    G_CALLBACK(accept_secret_chat_cb), G_CALLBACK(decline_secret_chat_cb));
      g_free (message);
    }
  }
  
  if (flags & TGL_UPDATE_CREATED && buddy) {
    purple_buddy_set_protocol_data (buddy, (gpointer)U);
    p2tgl_buddy_update (TLS, (tgl_peer_t *)U, flags);
  }
  
  if (flags & TGL_UPDATE_DELETED && buddy) {
    p2tgl_got_im (TLS, U->id, "Secret chat terminated.", PURPLE_MESSAGE_SYSTEM | PURPLE_MESSAGE_WHISPER, time(0));
    p2tgl_prpl_got_set_status_offline (TLS, U->id);
  }
}

static void update_chat_handler (struct tgl_state *TLS, struct tgl_chat *chat, unsigned flags) {
  PurpleChat *ch = p2tgl_chat_find (TLS, chat->id);

  if (flags & TGL_UPDATE_CREATED) {
    tgl_do_get_chat_info (TLS, chat->id, 0, on_chat_get_info, 0);
  }
  if (flags & TGL_UPDATE_TITLE && ch) {
    purple_blist_alias_chat (ch, chat->print_title);
  }
  if (flags & (TGL_UPDATE_MEMBERS | TGL_UPDATE_ADMIN) && ch) {
    chat_users_update (TLS, chat);
  }
  if (flags & TGL_UPDATE_DELETED && ch) {
    purple_blist_remove_chat (ch);
  }
}

static void update_user_typing (struct tgl_state *TLS, struct tgl_user *U, enum tgl_typing_status status) {
  if (status == tgl_typing_typing) {
    p2tgl_got_typing(TLS, U->id, 2);
  }
}

static void on_contact_added (struct tgl_state *TLS,void *callback_extra, int success, int size, struct tgl_user *users[]) {
  PurpleBuddy *buddy = callback_extra;
  
  purple_blist_remove_buddy (buddy);
  if (!success || !size) {
    purple_notify_error (_telegram_protocol, "Adding Buddy Failed", "Buddy Not Found", "No contact with this phone number was found.");
  }
}

static void on_userpic_loaded (struct tgl_state *TLS, void *extra, int success, char *filename) {
  connection_data *conn = TLS->ev_base;
  
  struct download_desc *dld = extra;
  struct tgl_user *U = dld->data;
  tgl_peer_t *P = tgl_peer_get (TLS, dld->get_user_info_data->peer);
  
  if (!success || !P) {
    warning ("Can not load userpic for user %s %s", U->first_name, U->last_name);
    goto fin;
  }
  
  int imgStoreId = p2tgl_imgstore_add_with_id (filename);
  if (imgStoreId > 0) {
    used_images_add (conn, imgStoreId);

    p2tgl_buddy_icons_set_for_user (conn->pa, &P->id, filename);
    
    if (dld->get_user_info_data->show_info == 1) {
      PurpleNotifyUserInfo *info = p2tgl_notify_peer_info_new (TLS, P);
      p2tgl_notify_userinfo (TLS, P->id, info, NULL, NULL);
    }
  }
  
fin:
  free (dld->get_user_info_data);
  free (dld);
}

void on_user_get_info (struct tgl_state *TLS, void *info_data, int success, struct tgl_user *U)
{
  get_user_info_data *user_info_data = (get_user_info_data *)info_data;
  tgl_peer_t *P = tgl_peer_get (TLS, user_info_data->peer);
  
  if (! success) {
    warning ("on_user_get_info not successfull, aborting...");
    return;
  }
  
  if (U->photo.sizes_num == 0) {
    // No profile pic to load, display it right away
    if (user_info_data->show_info) {
      PurpleNotifyUserInfo *info = p2tgl_notify_peer_info_new (TLS, P);
      p2tgl_notify_userinfo (TLS, P->id, info, NULL, NULL);
    }
    g_free (user_info_data);
  
  } else {
    struct download_desc *dld = malloc (sizeof(struct download_desc));
    dld->data = U;
    dld->get_user_info_data = info_data;
    tgl_do_load_photo (TLS, &U->photo, on_userpic_loaded, dld);
  }
}

void on_chat_get_info (struct tgl_state *TLS, void *extra, int success, struct tgl_chat *C) {
  if (!success || !chat_is_member (TLS->our_id, C)) {
    return;
  }
  
  PurpleChat *PC = p2tgl_chat_find (TLS, C->id);
  if (!PC) {
    PC = p2tgl_chat_new (TLS, C);
    purple_blist_add_chat (PC, NULL, NULL);
  }
  
  chat_users_update (TLS, C);
}

void on_ready (struct tgl_state *TLS) {
  debug ("on_ready().");
  connection_data *conn = TLS->ev_base;
  
  purple_connection_set_state (conn->gc, PURPLE_CONNECTED);
  purple_connection_set_display_name (conn->gc, purple_account_get_username(conn->pa));
  purple_blist_add_account (conn->pa);
  tggroup = purple_find_group ("Telegram");
  if (tggroup == NULL) {
    tggroup = purple_group_new ("Telegram");
    purple_blist_add_group (tggroup, NULL);
  }
  
  debug ("seq = %d, pts = %d", TLS->seq, TLS->pts);
  tgl_do_get_difference (TLS, 0, 0, 0);
  tgl_do_get_dialog_list (TLS, 0, 0);
  tgl_do_update_contact_list (TLS, 0, 0);
}

static const char *tgprpl_list_icon (PurpleAccount * acct, PurpleBuddy * buddy) {
  return "telegram";
}

static void tgprpl_tooltip_text (PurpleBuddy * buddy, PurpleNotifyUserInfo * info, gboolean full) {
  debug ("tgprpl_tooltip_text()", buddy->name);
  
  tgl_peer_id_t *peer = purple_buddy_get_protocol_data(buddy);
  if (!peer) {
    purple_notify_user_info_add_pair (info, "Status", "Offline");
    return;
  }
  tgl_peer_t *P = tgl_peer_get (get_conn_from_buddy (buddy)->TLS, *peer);
  if (!P) {
    warning ("tgprpl_tooltip_text: warning peer with id %d not found in tree.", peer->id);
    return;
  }
  purple_notify_user_info_add_pair (info, "Status", format_status(&P->user.status));
  
  gchar *status = format_user_status (&P->user.status);
  purple_notify_user_info_add_pair (info, "last online: ", status);
  g_free (status);
}

static GList *tgprpl_status_types (PurpleAccount * acct) {
  debug ("tgprpl_status_types()");
  GList *types = NULL;
  PurpleStatusType *type;
  
  type = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, NULL, NULL, FALSE, TRUE, FALSE);
  types = g_list_prepend (types, type);
  
  type = purple_status_type_new_full(PURPLE_STATUS_MOBILE, NULL, NULL, FALSE, TRUE, FALSE);
  types = g_list_prepend (types, type);

  type = purple_status_type_new_full(PURPLE_STATUS_OFFLINE, NULL, NULL, FALSE, TRUE, FALSE);
  types = g_list_prepend (types, type);
  
  /*
    The states below are only registered internally so that we get notified about 
    state changes to away and unavailable. This is useful for deciding when to send 
    No other peer should ever have those states.
   */
  type = purple_status_type_new_full(PURPLE_STATUS_AWAY, NULL, NULL, FALSE, TRUE, FALSE);
  types = g_list_prepend (types, type);
  type = purple_status_type_new_full(PURPLE_STATUS_UNAVAILABLE, NULL, NULL, FALSE, TRUE, FALSE);
  types = g_list_prepend (types, type);
  
  return g_list_reverse (types);
}

static GList* tgprpl_blist_node_menu (PurpleBlistNode *node) {
  debug ("tgprpl_blist_node_menu()");
  
  GList* menu = NULL;
  if (PURPLE_BLIST_NODE_IS_BUDDY(node)) {
    // Add encrypted chat option to the right click menu of all buddies
    PurpleBuddy* buddy = (PurpleBuddy*)node;
    PurpleMenuAction* menu_action = purple_menu_action_new("Start Secret Chat",
                                      PURPLE_CALLBACK(start_secret_chat), buddy, NULL);
    menu = g_list_append(menu, (gpointer)menu_action);
  }
  return menu;
}

static GList *tgprpl_chat_join_info (PurpleConnection * gc) {
  debug ("tgprpl_chat_join_info()");
  struct proto_chat_entry *pce;
  
  pce = g_new0(struct proto_chat_entry, 1);
  pce->label = "_Subject:";
  pce->identifier = "subject";
  pce->required = TRUE;
  return g_list_append(NULL, pce);
}

static void tgprpl_login (PurpleAccount * acct) {
  debug ("tgprpl_login()");
  PurpleConnection *gc = purple_account_get_connection (acct);
  char const *username = purple_account_get_username (acct);
  
  struct tgl_state *TLS = tgl_state_alloc ();
  
  const char *dir = config_dir;
  struct passwd *pw = getpwuid (getuid ());
  size_t len = strlen (dir) + strlen (pw->pw_dir) + 2 + strlen (username);
  TLS->base_path = malloc (len);
  snprintf (TLS->base_path, len, "%s/%s/%s", pw->pw_dir, dir, username);
  debug ("base configuration path: '%s'", TLS->base_path);
  g_mkdir_with_parents (TLS->base_path, 0700);
  
  len += strlen ("/downloads");
  char *ddir = malloc (len);
  sprintf (ddir, "%s/downloads", TLS->base_path);
  tgl_set_download_directory (TLS, ddir);
  g_mkdir_with_parents (ddir, 0700);
  free (ddir);
  
  tgl_set_verbosity (TLS, 4);
  tgl_set_rsa_key (TLS, pk_path);
  
  // create handle to store additional info for libpurple in
  // the new telegram instance
  connection_data *conn = connection_data_init (TLS, gc, acct);
  purple_connection_set_protocol_data (gc, conn);
  
  tgl_set_ev_base (TLS, conn);
  tgl_set_net_methods (TLS, &tgp_conn_methods);
  tgl_set_timer_methods (TLS, &tgp_timers);
  tgl_set_callback (TLS, &tgp_callback);
  tgl_register_app_id (TLS, TGP_APP_ID, TGP_APP_HASH); 
  
  tgl_init (TLS);
  purple_connection_set_state (conn->gc, PURPLE_CONNECTING);
  
  telegram_login (TLS);
}

static void tgprpl_close (PurpleConnection * gc) {
  debug ("tgprpl_close()");
  connection_data *conn = purple_connection_get_protocol_data (gc);
  
  connection_data_free (conn);
}

static int tgprpl_send_im (PurpleConnection * gc, const char *who, const char *message, PurpleMessageFlags flags) {
  debug ("tgprpl_send_im()");
  connection_data *conn = purple_connection_get_protocol_data(gc);
 
  // this is part of a workaround to support clients without
  // the request API (request.h), see telegram-base.c:request_code()
  if (conn->in_fallback_chat) {
    request_code_entered (conn->TLS, message);
    conn->in_fallback_chat = 0;
    return 1;
  }
  
  /* 
     Make sure that we only send messages to an existing peer by
     searching it in the peer tree. This allows us to give immediate feedback 
     by returning an error-code in case the peer doesn't exist
   */
  tgl_peer_t *peer = find_peer_by_name (conn->TLS, who);
  if (peer) {

    if (tgl_get_peer_type(peer->id) == TGL_PEER_ENCR_CHAT && peer->encr_chat.state != sc_ok) {
      // secret chat not ready for sending messages or deleted
      return -1;
    }
    
    tgl_do_send_unescape_message (conn->TLS, message, peer->id);
    return 1;
  }
  
  // peer not found
  return -1;
}

static unsigned int tgprpl_send_typing (PurpleConnection * gc, const char *who, PurpleTypingState typing) {
  debug ("tgprpl_send_typing()");
  int id = atoi (who);
  connection_data *conn = purple_connection_get_protocol_data(gc);
  tgl_peer_t *U = tgl_peer_get (conn->TLS, TGL_MK_USER (id));
  if (U) {
    if (typing == PURPLE_TYPING) {
      tgl_do_send_typing (conn->TLS, U->id, tgl_typing_typing, 0, 0);
    } else {
      tgl_do_send_typing (conn->TLS, U->id, tgl_typing_cancel, 0, 0);
    }
  }
  return 0;
}

static void tgprpl_get_info (PurpleConnection * gc, const char *who) {
  debug ("tgprpl_get_info()");
  connection_data *conn = purple_connection_get_protocol_data(gc);
  
  tgl_peer_t *peer = find_peer_by_name (conn->TLS, who);
  if (! peer) { return; }
  
  get_user_info_data* info_data = get_user_info_data_new (1, peer->id);
  
  switch (tgl_get_peer_type (peer->id)) {
    case TGL_PEER_USER:
    case TGL_PEER_CHAT:
      tgl_do_get_user_info (conn->TLS, peer->id, 0, on_user_get_info, info_data);
      break;
      
    case TGL_PEER_ENCR_CHAT: {
      tgl_peer_t *parent_peer = tgp_encr_chat_get_partner(conn->TLS, &peer->encr_chat);
      if (parent_peer) {
        tgl_do_get_user_info (conn->TLS, parent_peer->id, 0, on_user_get_info, info_data);
      }
      break;
    }
  }
}

static void tgprpl_set_status (PurpleAccount * acct, PurpleStatus * status) {
  debug ("tgprpl_set_status(%s)", purple_status_get_name (status));
  debug ("tgprpl_set_status(currstatus=%s)", purple_status_get_name(purple_account_get_active_status(acct)));

  PurpleConnection *gc = purple_account_get_connection(acct);
  if (!gc) { return; }
  connection_data *conn = purple_connection_get_protocol_data (gc);

  if (p2tgl_status_is_present(status)) {
    pending_reads_send_all (conn->pending_reads, conn->TLS);
  }
}

static void tgprpl_add_buddy (PurpleConnection * gc, PurpleBuddy * buddy, PurpleGroup * group) {
  connection_data *conn = purple_connection_get_protocol_data(gc);
  const char* first = buddy->alias ? buddy->alias : "";
  
  tgl_do_add_contact (conn->TLS, buddy->name, (int)strlen (buddy->name), first, (int)strlen (first), "", 0, 0, on_contact_added, buddy);
}

static void tgprpl_remove_buddy (PurpleConnection * gc, PurpleBuddy * buddy, PurpleGroup * group) {
  debug ("tgprpl_remove_buddy()");
  if (!buddy) { return; }

  connection_data *conn = purple_connection_get_protocol_data (gc);
  
  tgl_peer_t *peer = find_peer_by_name (conn->TLS, buddy->name);
  if (!peer) {
    // telegram peer not existing, orphaned buddy
    return;
  }
  switch (tgl_get_peer_type(peer->id)) {
    case TGL_PEER_ENCR_CHAT:
      /* TODO: implement the api call cancel secret chats. Currently the chat will only be marked as 
               deleted on our side so that it won't be added on startup 
              (when the secret chat file is loaded) */
      bl_do_encr_chat_delete (conn->TLS, &peer->encr_chat);
      break;
    case TGL_PEER_USER:
      tgl_do_del_contact (conn->TLS, peer->id, NULL, NULL);
      break;
  }
  
}

static void tgprpl_add_deny (PurpleConnection * gc, const char *name) {
  debug ("tgprpl_add_deny()");
}

static void tgprpl_rem_deny (PurpleConnection * gc, const char *name) {
  debug ("tgprpl_rem_deny()");
}

static void tgprpl_chat_join (PurpleConnection * gc, GHashTable * data) {
  debug ("tgprpl_chat_join()");
  
  connection_data *conn = purple_connection_get_protocol_data (gc);
  int id = atoi (g_hash_table_lookup (data, "id"));
  if (id) {
    chat_show (conn->gc, id);
  }
}

static char *tgprpl_get_chat_name (GHashTable * data) {
  debug ("tgprpl_get_chat_name()");
  return g_strdup(g_hash_table_lookup(data, "subject"));
}

static GHashTable *tgprpl_chat_info_deflt (PurpleConnection * gc, const char *chat_name) {
  debug ("tgprpl_chat_info_defaults()");
  return NULL;
}

static void tgprpl_chat_invite (PurpleConnection * gc, int id, const char *message, const char *name) {
  debug ("tgprpl_chat_invite()");

  connection_data *conn = purple_connection_get_protocol_data (gc);
  tgl_peer_t *chat = tgl_peer_get(conn->TLS, TGL_MK_CHAT (id));
  tgl_peer_t *user = tgl_peer_get(conn->TLS, TGL_MK_USER (atoi(name)));
  
  if (! chat || ! user) {
    purple_notify_error (_telegram_protocol, "Not found", "Cannot invite buddy to chat.", "Specified user is not existing.");
    return;
  }
  
  tgl_do_add_user_to_chat (conn->TLS, chat->id, user->id, 0, NULL, NULL);
}

static int tgprpl_send_chat (PurpleConnection * gc, int id, const char *message, PurpleMessageFlags flags) {
  debug ("tgprpl_send_chat()");
  connection_data *conn = purple_connection_get_protocol_data (gc);
  tgl_do_send_unescape_message (conn->TLS, message, TGL_MK_CHAT(id));

  p2tgl_got_chat_in(conn->TLS, TGL_MK_CHAT(id), TGL_MK_USER(conn->TLS->our_id), message,
    PURPLE_MESSAGE_RECV, time(NULL));
  return 1;
}

static void tgprpl_group_buddy (PurpleConnection * gc, const char *who, const char *old_group, const char *new_group) {
  debug ("tgprpl_group_buddy()");
}

static void tgprpl_rename_group (PurpleConnection * gc, const char *old_name, PurpleGroup * group, GList * moved_buddies) {
  debug ("tgprpl_rename_group()");
}

static void tgprpl_convo_closed (PurpleConnection * gc, const char *who) {
  debug ("tgprpl_convo_closed()");
}

static void tgprpl_set_buddy_icon (PurpleConnection * gc, PurpleStoredImage * img) {
  debug ("tgprpl_set_buddy_icon()");
  
  connection_data *conn = purple_connection_get_protocol_data (gc);
  if (purple_imgstore_get_filename (img)) {
    char* filename = g_strdup_printf ("%s/icons/%s", purple_user_dir(), purple_imgstore_get_filename (img));
    debug (filename);
    
    tgl_do_set_profile_photo (conn->TLS, filename, NULL, NULL);
    
    g_free (filename);
  }
}

static gboolean tgprpl_can_receive_file (PurpleConnection * gc, const char *who) {
  debug ("tgprpl_can_receive_file()");
  return TRUE;
}

static PurplePluginProtocolInfo prpl_info = {
  OPT_PROTO_NO_PASSWORD | OPT_PROTO_IM_IMAGE,
  NULL,                    // user_splits, initialized in tgprpl_init()
  NULL,                    // protocol_options, initialized in tgprpl_init()
  {
    "png",
    1,                     // min_width
    1,                     // min_height
    512,                   // max_width
    512,                   // max_height
    64000,                 // max_filesize
    PURPLE_ICON_SCALE_SEND,
  },
  tgprpl_list_icon,
  NULL,
  NULL,
  tgprpl_tooltip_text,
  tgprpl_status_types,
  tgprpl_blist_node_menu,  // blist_node_menu
  tgprpl_chat_join_info,
  tgprpl_chat_info_deflt,
  tgprpl_login,
  tgprpl_close,
  tgprpl_send_im,
  NULL,                    // set_info
  tgprpl_send_typing,
  tgprpl_get_info,
  tgprpl_set_status,
  NULL,                    // set_idle
  NULL,                    // change_passwd
  tgprpl_add_buddy,
  NULL,                    // add_buddies
  tgprpl_remove_buddy,
  NULL,                    // remove_buddies
  NULL,                    // add_permit
  tgprpl_add_deny,
  NULL,                    // rem_permit
  tgprpl_rem_deny,
  NULL,                    // set_permit_deny
  tgprpl_chat_join,
  NULL,                    // reject_chat
  tgprpl_get_chat_name,
  tgprpl_chat_invite,
  NULL,                    // chat_leave
  NULL,                    // chat_whisper
  tgprpl_send_chat,
  NULL,                    // keepalive
  NULL,                    // register_user
  NULL,                    // get_cb_info
  NULL,                    // get_cb_away
  NULL,                    // alias_buddy
  tgprpl_group_buddy, 
  tgprpl_rename_group,
  NULL,                    // buddy_free
  tgprpl_convo_closed,     
  NULL,                    // normalize
  tgprpl_set_buddy_icon,
  NULL,                    // remove_group
  NULL,
  NULL,                    // set_chat_topic
  NULL,                    // find_blist_chat
  NULL,                    // roomlist_get_list
  NULL,                    // roomlist_cancel
  NULL,                    // roomlist_expand_category
  tgprpl_can_receive_file,
  tgprpl_send_file,       
  tgprpl_new_xfer,        
  NULL,                    // offline_message
  NULL,                    // whiteboard_prpl_ops
  NULL,                    // send_raw
  NULL,                    // roomlist_room_serialize
  NULL,                    // unregister_user
  NULL,                    // send_attention
  NULL,                    // get_attention_types
  sizeof(PurplePluginProtocolInfo),
  NULL,           		     // get_account_text_table
  NULL,                    // initiate_media
  NULL,                    // get_media_caps
  NULL,                    // get_moods
  NULL,                    // set_public_alias
  NULL,                    // get_public_alias
  NULL,                    // add_buddy_with_invite
  NULL                     // add_buddies_with_invite
};

static void tgprpl_init (PurplePlugin *plugin) {
  
  PurpleAccountOption *opt;
  
  GList *verification_values = NULL;
  
#define ADD_VALUE(list, desc, v) { \
  PurpleKeyValuePair *kvp = g_new0(PurpleKeyValuePair, 1); \
  kvp->key = g_strdup((desc)); \
  kvp->value = g_strdup((v)); \
  list = g_list_prepend(list, kvp); \
}
    
  ADD_VALUE(verification_values, "Ask", "ask");
  ADD_VALUE(verification_values, "Always", "always");
  ADD_VALUE(verification_values, "Never", "never");
 
  opt = purple_account_option_list_new("Accept Secret Chats", "accept-secret-chats", verification_values);
  prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, opt);
  
  opt = purple_account_option_bool_new("Fallback SMS verification", "compat-verification", 0);
  prpl_info.protocol_options = g_list_append(prpl_info.protocol_options, opt);
  
  _telegram_protocol = plugin;
}

static GList *tgprpl_actions(PurplePlugin * plugin, gpointer context) {
  // return possible actions (See Libpurple doc)
  return (GList *)NULL;
}

static PurplePluginInfo plugin_info = {
  PURPLE_PLUGIN_MAGIC,
  PURPLE_MAJOR_VERSION,
  PURPLE_MINOR_VERSION,
  PURPLE_PLUGIN_PROTOCOL,
  NULL,
  0,
  NULL,
  PURPLE_PRIORITY_DEFAULT,
  PLUGIN_ID,
  "Telegram",
  PACKAGE_VERSION " libtgl: " TGL_VERSION,
  "Telegram",
  TG_DESCRIPTION,
  TG_AUTHOR,
  "https://github.com/majn/telegram-purple",
  NULL,           // on load
  NULL,           // on unload
  NULL,           // on destroy
  NULL,           // ui specific struct
  &prpl_info,
  NULL,           // prefs info
  tgprpl_actions,
  NULL,           // reserved
  NULL,           // reserved
  NULL,           // reserved
  NULL            // reserved
};

PURPLE_INIT_PLUGIN (telegram, tgprpl_init, plugin_info)

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
 
 Copyright Matthias Jentsch 2014
 */

#ifndef __telegram_adium__tgp_data__
#define __telegram_adium__tgp_data__

#include "purple.h"

#include <tgl.h>
#include <glib.h>

typedef struct {
  struct tgl_state *TLS;
  char *hash;
  PurpleAccount *pa;
  PurpleConnection *gc;
  int updated;
  GQueue *new_messages;
  GQueue *pending_reads;
  GList *used_images;
  guint write_timer;
  guint login_timer;
  int in_fallback_chat;
} connection_data;

typedef struct { 
  int show_info; 
  tgl_peer_id_t peer;
} get_user_info_data;

struct tgp_xfer_send_data {
  int timer;
  int done;
  PurpleXfer *xfer;
  connection_data *conn;
  struct tgl_document *document;
};

struct download_desc {
  get_user_info_data *get_user_info_data;
  void *data;
};

struct message_text {
  struct tgl_message *M;
  char *text;
};

void pending_reads_send_all (GQueue *queue, struct tgl_state *TLS);
void pending_reads_add (GQueue *queue, tgl_peer_id_t id);

struct message_text *message_text_init (struct tgl_message *M, gchar *text);
void message_text_free (gpointer data);

void used_images_add (connection_data *data, gint imgid);

void *connection_data_free (connection_data *conn);
connection_data *connection_data_init (struct tgl_state *TLS, PurpleConnection *gc, PurpleAccount *pa);

get_user_info_data* get_user_info_data_new (int show_info, tgl_peer_id_t peer);

#endif

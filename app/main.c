// Phase 2 host: a GTK4 window over the Hyperswarm backend worklet. On startup it
// boots the worklet and runs the typed-RPC client on bare-kit's IPC poll thread;
// the GTK main loop owns the main thread. The worklet's events drive the widgets
// - marshaled onto the main thread with g_idle_add, since they arrive on the
// poll thread - and flipping the switch sends a set-state request.

#include <gtk/gtk.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

#include <rpc.h>
#include <rpc/client.h>
#include <sync_hrpc.h>

#include "bare-kit.h"

static bare_worklet_t *worklet;
static bare_ipc_t *ipc;
static bare_ipc_poll_t *ipc_poll;
static uv_buf_t source;

static rpc_client_t client;

// Guards rpc_client_t, which is not thread-safe: the toggle handler
// (next_id/track) on the main thread and the poll thread (rpc_client_read) both
// touch it. Callbacks fire under the lock but never re-enter the client.
static uv_mutex_t client_lock;

// Widgets driven by the worklet's events. Set in activate on the main thread,
// before the main loop dispatches any g_idle_add callback.
static GtkWidget *toggle;
static GtkWidget *peers_value;
static GtkWidget *key_value;
static GtkWidget *topic_value;

// Read the packed worklet bundle into memory; bare_worklet_start takes its bytes.
static uv_buf_t
read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    perror("fopen");
    exit(1);
  }

  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);

  if (n < 0) {
    fprintf(stderr, "cannot size %s\n", path);
    exit(1);
  }

  char *buf = malloc(n);
  if (buf == NULL) {
    fprintf(stderr, "out of memory reading %s\n", path);
    exit(1);
  }

  size_t read = fread(buf, 1, n, f);
  fclose(f);

  if (read != (size_t) n) {
    fprintf(stderr, "short read of %s\n", path);
    exit(1);
  }

  return uv_buf_init(buf, (unsigned int) n);
}

static gboolean
on_toggle(GtkSwitch *sw, gboolean state, gpointer user_data);

// --- main-thread widget updates, posted from the poll thread via g_idle_add ---

// Set the switch without re-entering on_toggle, so an event-driven update does
// not echo a redundant set-state back to the worklet.
static void
set_toggle(bool on) {
  g_signal_handlers_block_by_func(toggle, G_CALLBACK(on_toggle), NULL);
  gtk_switch_set_active(GTK_SWITCH(toggle), on);
  g_signal_handlers_unblock_by_func(toggle, G_CALLBACK(on_toggle), NULL);
}

static gboolean
apply_state(gpointer data) {
  set_toggle(GPOINTER_TO_INT(data) != 0);
  return G_SOURCE_REMOVE;
}

static gboolean
apply_peers(gpointer data) {
  char buf[32];
  snprintf(buf, sizeof buf, "%u", GPOINTER_TO_UINT(data));
  gtk_label_set_text(GTK_LABEL(peers_value), buf);
  return G_SOURCE_REMOVE;
}

typedef struct {
  char *key;
  char *topic;
} info_t;

static gboolean
apply_info(gpointer data) {
  info_t *info = data;
  gtk_label_set_text(GTK_LABEL(key_value), info->key);
  gtk_label_set_text(GTK_LABEL(topic_value), info->topic);
  g_free(info->key);
  g_free(info->topic);
  g_free(info);
  return G_SOURCE_REMOVE;
}

// --- poll-thread RPC callbacks: copy out, then hand to the main thread ---

// The reply to our set-state request, routed here by id. Reconciles the switch
// with the worklet's authoritative state.
static void
on_set_state_reply(void *data, const rpc_message_t *msg) {
  sync_switch_state_t state;
  hrpc_error_t error;
  int r = sync_decode_set_state_response(msg, &state, &error);

  if (r == hrpc_ok) {
    g_idle_add(apply_state, GINT_TO_POINTER(state.on ? 1 : 0));
  } else if (r == hrpc_error_response) {
    // The backend never rejects set-state, so this only logs; a backend that
    // can reject should reconcile the switch back here.
    fprintf(stderr, "set-state error: %.*s\n", (int) error.message.len, error.message.data);
  } else {
    fprintf(stderr, "set-state reply decode failed (%d)\n", r);
  }
}

// Send one set-state request and track its reply by id. Called from the toggle
// handler while the poll thread runs, so the client touches are locked. The
// reply can only arrive after the worklet sees this write, so tracking before
// the write is enough to catch it.
static void
send_set_state(bare_ipc_t *ipc, bool on) {
  uv_mutex_lock(&client_lock);
  uint64_t id = rpc_client_next_id(&client);
  uv_mutex_unlock(&client_lock);

  sync_switch_state_t want = {.on = on};
  uint8_t *request;
  size_t request_len;
  if (sync_encode_set_state(id, &want, &request, &request_len) < 0) {
    fprintf(stderr, "encode set-state failed\n");
    return;
  }

  uv_mutex_lock(&client_lock);
  rpc_client_track(&client, id, on_set_state_reply, NULL);
  uv_mutex_unlock(&client_lock);

  // A short or failed write leaves a partial frame in the pipe, desyncing every
  // later frame, and the tracked reply never comes. Unrecoverable, so fatal.
  int written = bare_ipc_write(ipc, request, request_len);
  free(request);
  if (written < 0 || (size_t) written != request_len) {
    fprintf(stderr, "set-state write failed (%d of %zu bytes)\n", written, request_len);
    exit(1);
  }
}

static void
on_new_state(void *ctx, const sync_switch_state_t *state) {
  g_idle_add(apply_state, GINT_TO_POINTER(state->on ? 1 : 0));
}

static void
on_peers_changed(void *ctx, const sync_peers_t *peers) {
  g_idle_add(apply_peers, GUINT_TO_POINTER((guint) peers->count));
}

static void
on_info(void *ctx, const sync_identity_t *id) {
  info_t *info = g_new(info_t, 1);
  info->key = g_strndup((const char *) id->public_key.data, id->public_key.len);
  info->topic = g_strndup((const char *) id->topic.data, id->topic.len);
  g_idle_add(apply_info, info);
}

// Fallthrough for frames not matched to a pending request. Dispatch decodes the
// known events and calls the matching handler above; anything else is raw.
static void
on_event(void *data, const rpc_message_t *msg) {
  sync_hrpc_handlers_t handlers = {
    .on_new_state = on_new_state,
    .on_peers_changed = on_peers_changed,
    .on_info = on_info,
  };

  uint8_t *reply = NULL;
  size_t reply_len = 0;
  if (sync_hrpc_dispatch(&handlers, msg, &reply, &reply_len) < 0) {
    fprintf(stderr, "unhandled frame (command %llu)\n", (unsigned long long) msg->command);
  }
}

// Runs on bare-kit's IPC poll thread (its own pthread on Linux). Drains the
// readable bytes and feeds them to the client, which decodes complete frames and
// invokes the matching callback. A zero-length read means the worklet closed its
// end; a decode error means the stream is unrecoverable. Both are fatal.
static void
on_readable(bare_ipc_poll_t *poll, int events) {
  bare_ipc_t *i = bare_ipc_poll_get_ipc(poll);

  while (1) {
    void *data;
    size_t len;
    if (bare_ipc_read(i, &data, &len) != 0) break;
    if (len == 0) {
      fprintf(stderr, "[host] worklet closed\n");
      exit(1);
    }
    uv_mutex_lock(&client_lock);
    int r = rpc_client_read(&client, data, len);
    uv_mutex_unlock(&client_lock);
    if (r < 0) {
      fprintf(stderr, "rpc read failed (%d)\n", r);
      exit(1);
    }
  }
}

// Boot the worklet and start the RPC client + poll thread. Runs once, before the
// window is built.
static void
startup(GApplication *app, gpointer user_data) {
  uv_mutex_init(&client_lock);

  bare_worklet_alloc(&worklet);
  bare_worklet_options_t options = {0};
  bare_worklet_init(worklet, &options);

  source = read_file(BUNDLE_PATH);
  bare_worklet_start(worklet, "/app.bundle", &source, 0, NULL);

  bare_ipc_alloc(&ipc);
  bare_ipc_init(ipc, worklet);

  rpc_client_init(&client, on_event, NULL);

  bare_ipc_poll_alloc(&ipc_poll);
  bare_ipc_poll_init(ipc_poll, ipc);
  bare_ipc_poll_start(ipc_poll, bare_ipc_readable, on_readable);
}

// The user flipped the switch. Send a set-state and let the default handler move
// the switch (optimistic); the reply reconciles it.
static gboolean
on_toggle(GtkSwitch *sw, gboolean state, gpointer user_data) {
  send_set_state(ipc, state);
  return FALSE;
}

// One "name: value" row in the info grid; returns the value label so the caller
// can update it from events.
static GtkWidget *
add_row(GtkWidget *grid, int row, const char *name, const char *value) {
  GtkWidget *key = gtk_label_new(name);
  gtk_widget_set_halign(key, GTK_ALIGN_START);
  GtkWidget *val = gtk_label_new(value);
  gtk_widget_set_halign(val, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), key, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), val, 1, row, 1, 1);
  return val;
}

static void
activate(GtkApplication *app, gpointer user_data) {
  GtkWidget *window = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(window), "Bare <-> Linux");
  gtk_window_set_default_size(GTK_WINDOW(window), 360, -1);

  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
  gtk_widget_set_margin_top(box, 24);
  gtk_widget_set_margin_bottom(box, 24);
  gtk_widget_set_margin_start(box, 24);
  gtk_widget_set_margin_end(box, 24);

  GtkWidget *heading = gtk_label_new("Bare <-> Linux");

  GtkWidget *toggle_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *toggle_label = gtk_label_new("Shared switch");
  gtk_widget_set_hexpand(toggle_label, TRUE);
  gtk_widget_set_halign(toggle_label, GTK_ALIGN_START);
  toggle = gtk_switch_new();
  gtk_widget_set_halign(toggle, GTK_ALIGN_END);
  g_signal_connect(toggle, "state-set", G_CALLBACK(on_toggle), NULL);
  gtk_box_append(GTK_BOX(toggle_row), toggle_label);
  gtk_box_append(GTK_BOX(toggle_row), toggle);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  peers_value = add_row(grid, 0, "Peers connected", "0");
  key_value = add_row(grid, 1, "Your key", "...");
  topic_value = add_row(grid, 2, "Topic", "...");

  GtkWidget *caption = gtk_label_new(
    "Launch a second copy - flip the switch in one window and watch the other "
    "follow. No server in between."
  );
  gtk_label_set_wrap(GTK_LABEL(caption), TRUE);
  gtk_label_set_justify(GTK_LABEL(caption), GTK_JUSTIFY_CENTER);

  gtk_box_append(GTK_BOX(box), heading);
  gtk_box_append(GTK_BOX(box), toggle_row);
  gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
  gtk_box_append(GTK_BOX(box), grid);
  gtk_box_append(GTK_BOX(box), caption);

  gtk_window_set_child(GTK_WINDOW(window), box);
  gtk_window_present(GTK_WINDOW(window));
}

static void
on_shutdown(GApplication *app, gpointer user_data) {
  bare_ipc_poll_destroy(ipc_poll); // also stops the poll thread and joins it
  bare_ipc_destroy(ipc);
  rpc_client_destroy(&client);
  uv_mutex_destroy(&client_lock);
  bare_worklet_terminate(worklet);
  bare_worklet_destroy(worklet);
  free(source.base);
}

int
main(int argc, char **argv) {
  // NON_UNIQUE so each launch is its own process and peer; the default
  // single-instance behavior would refocus the first window instead.
  GtkApplication *app =
    gtk_application_new("com.holepunchto.bare_linux", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_signal_connect(app, "shutdown", G_CALLBACK(on_shutdown), NULL);

  int status = g_application_run(G_APPLICATION(app), argc, argv);

  g_object_unref(app);
  return status;
}

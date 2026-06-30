// Phase 2 host: a GTK4 window over the Hyperswarm backend worklet. On startup it
// boots the worklet and runs the typed-RPC client on bare-kit's IPC poll thread,
// the same as the headless host; the GTK main loop now owns the main thread.
// This skeleton shows a static window (toggle, peer count, key, topic). Binding
// the widgets to the worklet's events and the toggle to set-state comes next, so
// for now the events are still logged to stdout.

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

// Guards rpc_client_t, which is not thread-safe. The poll thread reads frames
// into it; once the toggle can send, the main thread will touch it too.
static uv_mutex_t client_lock;

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

// The worklet's send-only events. PR 2 marshals these to the GTK thread to drive
// the widgets; for now they are logged.
static void
on_new_state(void *ctx, const sync_switch_state_t *state) {
  printf("[host] new-state: switch is %s\n", state->on ? "on" : "off");
  fflush(stdout);
}

static void
on_peers_changed(void *ctx, const sync_peers_t *peers) {
  printf("[host] peers: %llu\n", (unsigned long long) peers->count);
  fflush(stdout);
}

static void
on_info(void *ctx, const sync_identity_t *id) {
  printf(
    "[host] info: key %.*s topic %.*s\n",
    (int) id->public_key.len,
    id->public_key.data,
    (int) id->topic.len,
    id->topic.data
  );
  fflush(stdout);
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
    printf("[host] unhandled frame (command %llu)\n", (unsigned long long) msg->command);
    fflush(stdout);
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

// One "name: value" row in the info grid.
static void
add_row(GtkWidget *grid, int row, const char *name, const char *value) {
  GtkWidget *key = gtk_label_new(name);
  gtk_widget_set_halign(key, GTK_ALIGN_START);
  GtkWidget *val = gtk_label_new(value);
  gtk_widget_set_halign(val, GTK_ALIGN_START);
  gtk_grid_attach(GTK_GRID(grid), key, 0, row, 1, 1);
  gtk_grid_attach(GTK_GRID(grid), val, 1, row, 1, 1);
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
  GtkWidget *toggle = gtk_switch_new();
  gtk_widget_set_halign(toggle, GTK_ALIGN_END);
  gtk_box_append(GTK_BOX(toggle_row), toggle_label);
  gtk_box_append(GTK_BOX(toggle_row), toggle);

  GtkWidget *grid = gtk_grid_new();
  gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
  gtk_grid_set_column_spacing(GTK_GRID(grid), 16);
  add_row(grid, 0, "Peers connected", "0");
  add_row(grid, 1, "Your key", "...");
  add_row(grid, 2, "Topic", "...");

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

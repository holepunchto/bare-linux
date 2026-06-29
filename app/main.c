// Phase 1 host: boot the Hyperswarm backend worklet and talk to it over typed
// RPC. Reads `on`/`off` from stdin and sends a `set-state` request per line,
// printing the worklet's authoritative reply decoded from the generated hrpc
// codec. librpc's rpc_client_t allocates the request id, routes the matching
// reply back to our callback, and reassembles frames off the IPC byte stream.
// Inbound events (info, peers-changed, new-state) arrive on the fallthrough and
// are decoded by sync_hrpc_dispatch.

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include <rpc.h>
#include <rpc/client.h>
#include <sync_hrpc.h>

#include "bare-kit.h"

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

static uv_sem_t running;
static rpc_client_t client;

// Guards rpc_client_t, which is not thread-safe: the stdin loop (next_id/track)
// and the poll thread (rpc_client_read) both touch it. Callbacks fire under the
// lock but never re-enter the client, so there is no re-entrancy.
static uv_mutex_t client_lock;

static void
on_signal(int sig) {
  uv_sem_post(&running); // sem_post is async-signal-safe
}

// The reply to our set-state request, routed here by id. The rpc_message_t views
// are valid only for this call, so decode (which copies the bool out) now.
static void
on_set_state_reply(void *data, const rpc_message_t *msg) {
  sync_switch_state_t state;
  hrpc_error_t error;
  int r = sync_decode_set_state_response(msg, &state, &error);

  if (r == hrpc_ok) {
    printf("[host] set-state reply: switch is %s\n", state.on ? "on" : "off");
  } else if (r == hrpc_error_response) {
    printf("[host] set-state error: %.*s\n", (int) error.message.len, error.message.data);
  } else {
    printf("[host] set-state reply decode failed (%d)\n", r);
  }
  fflush(stdout);
}

// Send one set-state request and track its reply by id. Called from the stdin
// loop while the poll thread runs, so the client touches are locked. The reply
// can only arrive after the worklet sees this write, so tracking before the
// write is enough to catch it.
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

// The worklet's send-only events, decoded by sync_hrpc_dispatch. The decoded
// views borrow from the inbound frame, so print now.
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

  // We register no request handlers, so dispatch only ever decodes an event
  // (no reply) or fails; it never writes the reply out-params here.
  uint8_t *reply = NULL;
  size_t reply_len = 0;
  if (sync_hrpc_dispatch(&handlers, msg, &reply, &reply_len) < 0) {
    printf("[host] unhandled frame (command %llu)\n", (unsigned long long) msg->command);
    fflush(stdout);
  }
}

// Runs on bare-kit's IPC poll thread (its own pthread on Linux). Drains the
// readable bytes and feeds them to the client, which decodes complete frames and
// invokes the matching callback. bare_ipc_read returns 0 on a successful read
// (a zero-length read means the worklet closed its end), or a negative
// would_block / error otherwise.
static void
on_readable(bare_ipc_poll_t *poll, int events) {
  bare_ipc_t *ipc = bare_ipc_poll_get_ipc(poll);

  while (1) {
    void *data;
    size_t len;
    if (bare_ipc_read(ipc, &data, &len) != 0) break;
    if (len == 0) {
      uv_sem_post(&running);
      break;
    }
    uv_mutex_lock(&client_lock);
    rpc_client_read(&client, data, len);
    uv_mutex_unlock(&client_lock);
  }
}

int
main(int argc, char **argv) {
  uv_sem_init(&running, 0);
  uv_mutex_init(&client_lock);

  // No SA_RESTART, so a signal interrupts the blocking fgets below instead of
  // restarting it. uv_sem_wait retries EINTR on its own, so it stays correct.
  struct sigaction sa = {0};
  sa.sa_handler = on_signal;
  sigaction(SIGINT, &sa, NULL);
  sigaction(SIGTERM, &sa, NULL);

  bare_worklet_t *worklet;
  bare_worklet_alloc(&worklet);

  bare_worklet_options_t options = {0};
  bare_worklet_init(worklet, &options);

  uv_buf_t source = read_file(BUNDLE_PATH);
  bare_worklet_start(worklet, "/app.bundle", &source, 0, NULL);

  bare_ipc_t *ipc;
  bare_ipc_alloc(&ipc);
  bare_ipc_init(ipc, worklet);

  rpc_client_init(&client, on_event, NULL);

  bare_ipc_poll_t *poll;
  bare_ipc_poll_alloc(&poll);
  bare_ipc_poll_init(poll, ipc);
  bare_ipc_poll_start(poll, bare_ipc_readable, on_readable);

  printf("[host] worklet up; type 'on' or 'off' (Ctrl-D or Ctrl-C to stop)\n");
  fflush(stdout);

  // One set-state per stdin line. fgets returns NULL on EOF or on a signal
  // (handlers above don't restart it), which ends the loop.
  char line[64];
  while (fgets(line, sizeof line, stdin) != NULL) {
    if (strcmp(line, "on\n") == 0) send_set_state(ipc, true);
    else if (strcmp(line, "off\n") == 0) send_set_state(ipc, false);
    else fprintf(stderr, "unknown command (type 'on' or 'off')\n");
  }

  // stdin closed; stay up as a peer until interrupted.
  uv_sem_wait(&running);

  bare_ipc_poll_destroy(poll); // also stops the poll thread and joins it
  bare_ipc_destroy(ipc);
  rpc_client_destroy(&client);
  uv_mutex_destroy(&client_lock);
  bare_worklet_terminate(worklet);
  bare_worklet_destroy(worklet);
  free(source.base);

  return 0;
}

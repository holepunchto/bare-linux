// Phase 1 host: boot the Hyperswarm backend worklet and talk to it over typed
// RPC. On startup it sends one `set-state` request (switch on) and prints the
// worklet's authoritative reply, decoded from the generated hrpc codec. librpc's
// rpc_client_t allocates the request id, routes the matching reply back to our
// callback, and reassembles frames off the IPC byte stream. The host then stays
// alive headless until interrupted; inbound events (info, peers-changed,
// new-state) arrive on the fallthrough and are reported but not yet decoded.

#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

// Fallthrough for frames not matched to a pending request: the worklet's
// send-only events. Decoding them is the next step; for now just report them so
// the channel stays observable.
static void
on_event(void *data, const rpc_message_t *msg) {
  printf("[host] event (command %llu)\n", (unsigned long long) msg->command);
  fflush(stdout);
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
    rpc_client_read(&client, data, len);
  }
}

int
main(int argc, char **argv) {
  uv_sem_init(&running, 0);
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

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

  // Ask the worklet to switch on, and track the reply by id. This runs before
  // the poll thread starts, so the client is only ever touched from one thread:
  // here now, and the poll thread afterwards.
  uint64_t id = rpc_client_next_id(&client);
  sync_switch_state_t want = {.on = true};
  uint8_t *request;
  size_t request_len;
  int err = sync_encode_set_state(id, &want, &request, &request_len);
  if (err < 0) {
    fprintf(stderr, "encode set-state failed (%d)\n", err);
    exit(1);
  }
  rpc_client_track(&client, id, on_set_state_reply, NULL);

  // The whole round-trip hinges on this one write landing whole; a short or
  // failed write means the worklet never sees the request and the host would
  // hang waiting for a reply that never comes, so treat it as fatal.
  int written = bare_ipc_write(ipc, request, request_len);
  free(request);
  if (written < 0 || (size_t) written != request_len) {
    fprintf(stderr, "set-state write failed (%d of %zu bytes)\n", written, request_len);
    exit(1);
  }

  bare_ipc_poll_t *poll;
  bare_ipc_poll_alloc(&poll);
  bare_ipc_poll_init(poll, ipc);
  bare_ipc_poll_start(poll, bare_ipc_readable, on_readable);

  printf("[host] worklet up; sent set-state (Ctrl-C to stop)\n");
  fflush(stdout);

  uv_sem_wait(&running);

  bare_ipc_poll_destroy(poll); // also stops the poll thread and joins it
  bare_ipc_destroy(ipc);
  rpc_client_destroy(&client);
  bare_worklet_terminate(worklet);
  bare_worklet_destroy(worklet);
  free(source.base);

  return 0;
}

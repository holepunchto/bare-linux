// Phase 0 host: boot the worklet, send one message over the IPC byte channel,
// and print the echo that comes back. This proves the native boundary - bundle
// loading and bidirectional IPC - independent of hrpc.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

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

  char *buf = malloc(n);
  size_t read = fread(buf, 1, n, f);
  fclose(f);

  if (read != (size_t) n) {
    fprintf(stderr, "short read of %s\n", path);
    exit(1);
  }

  return uv_buf_init(buf, (unsigned int) n);
}

static uv_sem_t got_reply;

// Runs on bare-kit's IPC poll thread (its own pthread on Linux), not the main
// thread.
static void
on_readable(bare_ipc_poll_t *poll, int events) {
  bare_ipc_t *ipc = bare_ipc_poll_get_ipc(poll);

  void *data;
  size_t len;
  int err = bare_ipc_read(ipc, &data, &len);
  if (err == 0) {
    printf("echo: %.*s\n", (int) len, (char *) data);
    uv_sem_post(&got_reply);
  }
}

int
main(int argc, char **argv) {
  uv_sem_init(&got_reply, 0);

  bare_worklet_t *worklet;
  bare_worklet_alloc(&worklet);

  bare_worklet_options_t options = {0};
  bare_worklet_init(worklet, &options);

  uv_buf_t source = read_file("app/app.bundle");
  bare_worklet_start(worklet, "/app.bundle", &source, 0, NULL);

  bare_ipc_t *ipc;
  bare_ipc_alloc(&ipc);
  bare_ipc_init(ipc, worklet);

  bare_ipc_poll_t *poll;
  bare_ipc_poll_alloc(&poll);
  bare_ipc_poll_init(poll, ipc);
  bare_ipc_poll_start(poll, bare_ipc_readable, on_readable);

  const char *msg = "ping";
  bare_ipc_write(ipc, msg, strlen(msg));

  uv_sem_wait(&got_reply);

  bare_ipc_poll_stop(poll);
  bare_ipc_poll_destroy(poll);
  bare_ipc_destroy(ipc);
  bare_worklet_terminate(worklet);
  bare_worklet_destroy(worklet);
  free(source.base);

  return 0;
}

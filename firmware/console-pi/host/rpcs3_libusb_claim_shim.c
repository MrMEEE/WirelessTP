#define _GNU_SOURCE
#include <dlfcn.h>
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Loaded confirmation: if this fires, LD_PRELOAD is working.
__attribute__((constructor)) static void shim_loaded(void) {
  fprintf(stderr, "[toypad-shim] loaded (pid=%d)\n", (int)getpid());
  fflush(stderr);
}

// This shim mitigates a Linux usbfs race where RPCS3 submits transfers
// before interface 0 is claimed on the handle.
//
// Strategy:
// - Hook libusb_submit_transfer.
// - Hook libusb_open and libusb_set_configuration to claim earlier.
// - For each libusb_device_handle, attempt libusb_claim_interface(handle, 0)
//   once globally before first submit.
// - Retry submit once if the first submit returns LIBUSB_ERROR_BUSY.

typedef int (*libusb_submit_transfer_fn)(struct libusb_transfer* transfer);
typedef int (*libusb_claim_interface_fn)(libusb_device_handle* dev_handle, int interface_number);
typedef int (*libusb_open_fn)(libusb_device* dev, libusb_device_handle** dev_handle);
typedef int (*libusb_set_configuration_fn)(libusb_device_handle* dev_handle, int configuration);
typedef int (*libusb_interrupt_transfer_fn)(libusb_device_handle* dev_handle,
                                            unsigned char endpoint,
                                            unsigned char* data,
                                            int length,
                                            int* actual_length,
                                            unsigned int timeout);
typedef int (*libusb_bulk_transfer_fn)(libusb_device_handle* dev_handle,
                                       unsigned char endpoint,
                                       unsigned char* data,
                                       int length,
                                       int* actual_length,
                                       unsigned int timeout);
typedef int (*libusb_control_transfer_fn)(libusb_device_handle* dev_handle,
                                          uint8_t bmRequestType,
                                          uint8_t bRequest,
                                          uint16_t wValue,
                                          uint16_t wIndex,
                                          unsigned char* data,
                                          uint16_t wLength,
                                          unsigned int timeout);

typedef struct claimed_handle_node {
  libusb_device_handle* handle;
  struct claimed_handle_node* next;
} claimed_handle_node_t;

static libusb_submit_transfer_fn real_submit = NULL;
static libusb_claim_interface_fn real_claim = NULL;
static libusb_open_fn real_open = NULL;
static libusb_set_configuration_fn real_set_configuration = NULL;
static libusb_interrupt_transfer_fn real_interrupt_transfer = NULL;
static libusb_bulk_transfer_fn real_bulk_transfer = NULL;
static libusb_control_transfer_fn real_control_transfer = NULL;

static pthread_mutex_t claimed_mu = PTHREAD_MUTEX_INITIALIZER;
static claimed_handle_node_t* claimed_head = NULL;

static bool is_handle_marked(libusb_device_handle* handle) {
  claimed_handle_node_t* n = claimed_head;
  while (n) {
    if (n->handle == handle) {
      return true;
    }
    n = n->next;
  }
  return false;
}

static void mark_handle(libusb_device_handle* handle) {
  claimed_handle_node_t* n = (claimed_handle_node_t*)calloc(1, sizeof(*n));
  if (!n) {
    return;
  }
  n->handle = handle;
  n->next = claimed_head;
  claimed_head = n;
}

static void resolve_symbols(void) {
  if (!real_submit) {
    real_submit = (libusb_submit_transfer_fn)dlsym(RTLD_NEXT, "libusb_submit_transfer");
  }
  if (!real_claim) {
    real_claim = (libusb_claim_interface_fn)dlsym(RTLD_NEXT, "libusb_claim_interface");
  }
  if (!real_open) {
    real_open = (libusb_open_fn)dlsym(RTLD_NEXT, "libusb_open");
  }
  if (!real_set_configuration) {
    real_set_configuration =
        (libusb_set_configuration_fn)dlsym(RTLD_NEXT, "libusb_set_configuration");
  }
  if (!real_interrupt_transfer) {
    real_interrupt_transfer =
        (libusb_interrupt_transfer_fn)dlsym(RTLD_NEXT, "libusb_interrupt_transfer");
  }
  if (!real_bulk_transfer) {
    real_bulk_transfer = (libusb_bulk_transfer_fn)dlsym(RTLD_NEXT, "libusb_bulk_transfer");
  }
  if (!real_control_transfer) {
    real_control_transfer =
        (libusb_control_transfer_fn)dlsym(RTLD_NEXT, "libusb_control_transfer");
  }
}

static void ensure_thread_claim(libusb_device_handle* handle) {
  if (!handle) {
    return;
  }
  pthread_mutex_lock(&claimed_mu);
  if (!is_handle_marked(handle)) {
    // Best-effort claim on interface 0.
    // Treat SUCCESS and BUSY as acceptable and mark to avoid repeated calls.
    int rc = real_claim(handle, 0);
    if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_BUSY) {
      mark_handle(handle);
    }
  }
  pthread_mutex_unlock(&claimed_mu);
}

int libusb_submit_transfer(struct libusb_transfer* transfer) {
  resolve_symbols();

  static int submit_count = 0;
  if (++submit_count == 1) {
    fprintf(stderr, "[toypad-shim] libusb_submit_transfer hooked (pid=%d)\n", (int)getpid());
    fflush(stderr);
  }

  if (!real_submit || !real_claim || !transfer) {
    return LIBUSB_ERROR_OTHER;
  }

  ensure_thread_claim(transfer->dev_handle);

  int rc = real_submit(transfer);
  if (rc == LIBUSB_ERROR_BUSY) {
    // One additional claim attempt in case set_configuration just happened.
    ensure_thread_claim(transfer->dev_handle);
    rc = real_submit(transfer);
  }

  return rc;
}

int libusb_open(libusb_device* dev, libusb_device_handle** dev_handle) {
  resolve_symbols();

  if (!real_open || !real_claim || !dev_handle) {
    return LIBUSB_ERROR_OTHER;
  }

  int rc = real_open(dev, dev_handle);
  if (rc == LIBUSB_SUCCESS && *dev_handle) {
    ensure_thread_claim(*dev_handle);
  }

  return rc;
}

int libusb_set_configuration(libusb_device_handle* dev_handle, int configuration) {
  resolve_symbols();

  if (!real_set_configuration || !real_claim || !dev_handle) {
    return LIBUSB_ERROR_OTHER;
  }

  int rc = real_set_configuration(dev_handle, configuration);
  if (rc == LIBUSB_SUCCESS) {
    // set_configuration can reset interface state; re-claim proactively.
    ensure_thread_claim(dev_handle);
  }

  return rc;
}

int libusb_interrupt_transfer(libusb_device_handle* dev_handle,
                              unsigned char endpoint,
                              unsigned char* data,
                              int length,
                              int* actual_length,
                              unsigned int timeout) {
  resolve_symbols();

  if (!real_interrupt_transfer || !real_claim || !dev_handle) {
    return LIBUSB_ERROR_OTHER;
  }

  ensure_thread_claim(dev_handle);
  return real_interrupt_transfer(dev_handle, endpoint, data, length, actual_length, timeout);
}

int libusb_bulk_transfer(libusb_device_handle* dev_handle,
                         unsigned char endpoint,
                         unsigned char* data,
                         int length,
                         int* actual_length,
                         unsigned int timeout) {
  resolve_symbols();

  if (!real_bulk_transfer || !real_claim || !dev_handle) {
    return LIBUSB_ERROR_OTHER;
  }

  ensure_thread_claim(dev_handle);
  return real_bulk_transfer(dev_handle, endpoint, data, length, actual_length, timeout);
}

int libusb_control_transfer(libusb_device_handle* dev_handle,
                            uint8_t bmRequestType,
                            uint8_t bRequest,
                            uint16_t wValue,
                            uint16_t wIndex,
                            unsigned char* data,
                            uint16_t wLength,
                            unsigned int timeout) {
  resolve_symbols();

  if (!real_control_transfer || !real_claim || !dev_handle) {
    return LIBUSB_ERROR_OTHER;
  }

  ensure_thread_claim(dev_handle);
  return real_control_transfer(
      dev_handle, bmRequestType, bRequest, wValue, wIndex, data, wLength, timeout);
}

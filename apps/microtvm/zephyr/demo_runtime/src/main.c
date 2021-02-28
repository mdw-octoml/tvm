/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 *
 * SPDX-License-Identifier: Apache-2.0
 */


/*
 * This is a sample Zephyr-based application that contains the logic
 * needed to upload and control a uTVM-based model via the UART.
 * This is only intended to be a demonstration, since typically you
 * will want to incorporate this logic into your own application.
 */


#include <drivers/gpio.h>
#include <drivers/uart.h>
#include <fatal.h>
#include <kernel.h>
#include <power/reboot.h>
#include <stdio.h>
#include <sys/printk.h>
#include <sys/ring_buffer.h>
#include <tvm/runtime/crt/logging.h>
#include <tvm/runtime/crt/utvm_rpc_server.h>
#include <unistd.h>
#include <zephyr.h>
#include <random/rand32.h>

#ifdef CONFIG_ARCH_POSIX
#include "posix_board_if.h"
#endif

#include "crt_config.h"

static const struct device* tvm_uart;

// XXX MDW - TODO: Clean up the LED stuff in here to remove the 4 LEDs.

#ifdef CONFIG_LED
#define LED0_NODE DT_ALIAS(led0)
#define LED0 DT_GPIO_LABEL(LED0_NODE, gpios)
#define LED0_PIN DT_GPIO_PIN(LED0_NODE, gpios)
#define LED0_FLAGS DT_GPIO_FLAGS(LED0_NODE, gpios)
static const struct device* led0_pin;

#define LED1_NODE DT_ALIAS(led1)
#define LED1 DT_GPIO_LABEL(LED1_NODE, gpios)
#define LED1_PIN DT_GPIO_PIN(LED1_NODE, gpios)
#define LED1_FLAGS DT_GPIO_FLAGS(LED1_NODE, gpios)
static const struct device* led1_pin;

#define LED2_NODE DT_ALIAS(led2)
#define LED2 DT_GPIO_LABEL(LED2_NODE, gpios)
#define LED2_PIN DT_GPIO_PIN(LED2_NODE, gpios)
#define LED2_FLAGS DT_GPIO_FLAGS(LED2_NODE, gpios)
static const struct device* led2_pin;

#define LED3_NODE DT_ALIAS(led3)
#define LED3 DT_GPIO_LABEL(LED3_NODE, gpios)
#define LED3_PIN DT_GPIO_PIN(LED3_NODE, gpios)
#define LED3_FLAGS DT_GPIO_FLAGS(LED3_NODE, gpios)
static const struct device* led3_pin;

#endif  // CONFIG_LED

static size_t g_num_bytes_requested = 0;
static size_t g_num_bytes_written = 0;

static const uint8_t* g_transmit_data = NULL;
static size_t g_transmit_data_size = 0;
static volatile size_t g_transmitted_bytes = 0;
static volatile bool g_transmit_complete = true;

// Used by TVM to write serial data to the UART.
ssize_t write_serial(void* unused_context, const uint8_t* data, size_t size) {
#ifdef CONFIG_LED
//  gpio_pin_set(led0_pin, LED0_PIN, 1);
#endif
  g_num_bytes_requested += size;

  for (size_t i = 0; i < size; i++) {
    uart_poll_out(tvm_uart, data[i]);
    g_num_bytes_written++;
  }

#ifdef CONFIG_LED
//  gpio_pin_set(led0_pin, LED0_PIN, 0);
#endif

  return size;
}

// This is an error handler which will be invoked if the device crashes.
// Here, we turn on the LED and spin.
void k_sys_fatal_error_handler(unsigned int reason, const z_arch_esf_t *esf) {
#ifdef CONFIG_LED
  gpio_pin_set(led0_pin, LED0_PIN, 1);
  gpio_pin_set(led2_pin, LED2_PIN, 1);
#endif
  for (;;) ;
}

// Used by TVM when a message needs to be formatted.
size_t TVMPlatformFormatMessage(char* out_buf, size_t out_buf_size_bytes,
                                const char* fmt, va_list args) {
  return vsnprintk(out_buf, out_buf_size_bytes, fmt, args);
}

static volatile tvm_crt_error_t fatal_error;
// Used by TVM when an abort operation occurs.
void TVMPlatformAbort(tvm_crt_error_t error) {
  fatal_error = error;   // Save it in a global for debugging.
  //sys_reboot(SYS_REBOOT_COLD);
#ifdef CONFIG_LED
  gpio_pin_set(led3_pin, LED3_PIN, 1);
#endif
  for (;;) ;
}

// Used by TVM to generate random data.
tvm_crt_error_t TVMPlatformGenerateRandom(uint8_t* buffer, size_t num_bytes) {
  uint32_t random;  // one unit of random data.

  // Fill parts of `buffer` which are as large as `random`.
  size_t num_full_blocks = num_bytes / sizeof(random);
  for (int i = 0; i < num_full_blocks; ++i) {
    random = sys_rand32_get();
    memcpy(&buffer[i * sizeof(random)], &random, sizeof(random));
  }

  // Fill any leftover tail which is smaller than `random`.
  size_t num_tail_bytes = num_bytes % sizeof(random);
  if (num_tail_bytes > 0) {
    random = sys_rand32_get();
    memcpy(&buffer[num_bytes - num_tail_bytes], &random, num_tail_bytes);
  }

  return kTvmErrorNoError;
}

#define MILLIS_TIL_EXPIRY 200
#define TIME_TIL_EXPIRY (K_MSEC(MILLIS_TIL_EXPIRY))
K_TIMER_DEFINE(g_utvm_timer, /* expiry func */ NULL, /* stop func */ NULL);

uint32_t g_utvm_start_time;
int g_utvm_timer_running = 0;

// Used to start system timer.
tvm_crt_error_t TVMPlatformTimerStart() {
  if (g_utvm_timer_running) {
    TVMLogf("timer already running");
    return kTvmErrorSystemErrorMask | 1;
  }

#ifdef CONFIG_LED
//  gpio_pin_set(led0_pin, LED0_PIN, 1);
#endif
  k_timer_start(&g_utvm_timer, TIME_TIL_EXPIRY, TIME_TIL_EXPIRY);
  g_utvm_start_time = k_cycle_get_32();
  g_utvm_timer_running = 1;
  return kTvmErrorNoError;
}

// Used to stop system timer.
tvm_crt_error_t TVMPlatformTimerStop(double* res_us) {
  if (!g_utvm_timer_running) {
    TVMLogf("timer not running");
    return kTvmErrorSystemErrorMask | 2;
  }

  uint32_t stop_time = k_cycle_get_32();
#ifdef CONFIG_LED
//  gpio_pin_set(led0_pin, LED0_PIN, 0);
#endif

  // compute how long the work took
  uint32_t cycles_spent = stop_time - g_utvm_start_time;
  if (stop_time < g_utvm_start_time) {
    // we rolled over *at least* once, so correct the rollover it was *only*
    // once, because we might still use this result
    cycles_spent = ~((uint32_t)0) - (g_utvm_start_time - stop_time);
  }

  uint32_t ns_spent = (uint32_t)k_cyc_to_ns_floor64(cycles_spent);
  double hw_clock_res_us = ns_spent / 1000.0;

  // need to grab time remaining *before* stopping. when stopped, this function
  // always returns 0.
  int32_t time_remaining_ms = k_timer_remaining_get(&g_utvm_timer);
  k_timer_stop(&g_utvm_timer);
  // check *after* stopping to prevent extra expiries on the happy path
  if (time_remaining_ms < 0) {
    TVMLogf("negative time remaining");
    return kTvmErrorSystemErrorMask | 3;
  }
  uint32_t num_expiries = k_timer_status_get(&g_utvm_timer);
  uint32_t timer_res_ms = ((num_expiries * MILLIS_TIL_EXPIRY) + time_remaining_ms);
  double approx_num_cycles =
      (double)k_ticks_to_cyc_floor32(1) * (double)k_ms_to_ticks_ceil32(timer_res_ms);
  // if we approach the limits of the HW clock datatype (uint32_t), use the
  // coarse-grained timer result instead
  if (approx_num_cycles > (0.5 * (~((uint32_t)0)))) {
    *res_us = timer_res_ms * 1000.0;
  } else {
    *res_us = hw_clock_res_us;
  }

  g_utvm_timer_running = 0;
  return kTvmErrorNoError;
}

// Memory pool for use by TVMPlatformMemoryAllocate.
K_MEM_POOL_DEFINE(tvm_memory_pool, 64, 1024, 216, 4);

// Used by TVM to allocate memory.
tvm_crt_error_t TVMPlatformMemoryAllocate(size_t num_bytes, DLContext ctx, void** out_ptr) {
  *out_ptr = k_mem_pool_malloc(&tvm_memory_pool, num_bytes);
  return (*out_ptr == NULL) ? kTvmErrorPlatformNoMemory : kTvmErrorNoError;
}

// Used by TVM to deallocate memory.
tvm_crt_error_t TVMPlatformMemoryFree(void* ptr, DLContext ctx) {
  k_free(ptr);
  return kTvmErrorNoError;
}

// Ring buffer used to store data read from the UART on rx interrupt.
#define RING_BUF_SIZE 20 * 1024
struct uart_rx_buf_t {
  struct ring_buf buf;
  uint32_t buffer[RING_BUF_SIZE];
};
struct uart_rx_buf_t uart_rx_buf;

// Small buffer used to read data from the UART into the ring buffer.
static uint8_t uart_data[32];

// UART interrupt callback.
void uart_irq_cb(const struct device* dev, void* user_data) {
  while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
    struct uart_rx_buf_t* buf = (struct uart_rx_buf_t*)user_data;
    if (uart_irq_rx_ready(dev) != 0) {
      for (;;) {
        // Read a small chunk of data from the UART.
        int bytes_read = uart_fifo_read(dev, uart_data, sizeof(uart_data));
        if (bytes_read < 0) {
#ifdef CONFIG_LED
          gpio_pin_set(led2_pin, LED2_PIN, 1);
#endif
          TVMPlatformAbort((tvm_crt_error_t)0xbeef1);
        } else if (bytes_read == 0) {
          break;
        }
        // Write it into the ring buffer.
        int bytes_written = ring_buf_put(&buf->buf, uart_data, bytes_read);
        if (bytes_read != bytes_written) {
#ifdef CONFIG_LED
          gpio_pin_set(led1_pin, LED1_PIN, 1);
          gpio_pin_set(led2_pin, LED2_PIN, 1);
#endif
          TVMPlatformAbort((tvm_crt_error_t)0xbeef2);
        }
        //CHECK_EQ(bytes_read, bytes_written, "bytes_read: %d; bytes_written: %d", bytes_read,
        //         bytes_written);
      }
    }
  }
}

// Used to initialize the UART receiver.
void uart_rx_init(struct uart_rx_buf_t* buf, const struct device* dev) {
  ring_buf_init(&buf->buf, RING_BUF_SIZE, buf->buffer);
  uart_irq_callback_user_data_set(dev, uart_irq_cb, (void*)buf);
  uart_irq_rx_enable(dev);
}

// Used to read data from the UART.
int uart_rx_buf_read(struct uart_rx_buf_t* buf, uint8_t* data, size_t data_size_bytes) {
  unsigned int key = irq_lock();
  int bytes_read = ring_buf_get(&buf->buf, data, data_size_bytes);
  irq_unlock(key);
  return bytes_read;
}

// Buffer used to read from the UART rx ring buffer and feed it to the UTvmRpcServerLoop.
static uint8_t main_rx_buf[RING_BUF_SIZE];

// The main function of this application.
extern void __stdout_hook_install(int (*hook)(int));
void main(void) {
#ifdef CONFIG_LED
  int ret;
  led0_pin = device_get_binding(LED0);
  if (led0_pin == NULL) {
    for (;;) ;
  }
  ret = gpio_pin_configure(led0_pin, LED0_PIN, GPIO_OUTPUT_ACTIVE | LED0_FLAGS);
  if (ret < 0) {
    TVMPlatformAbort((tvm_crt_error_t)0xbeef4);
  }
  gpio_pin_set(led0_pin, LED0_PIN, 1);

  led1_pin = device_get_binding(LED1);
  if (led1_pin == NULL) {
    for (;;) ;
  }
  ret = gpio_pin_configure(led1_pin, LED1_PIN, GPIO_OUTPUT_ACTIVE | LED1_FLAGS);
  if (ret < 0) {
    TVMPlatformAbort((tvm_crt_error_t)0xbeef4);
  }
  gpio_pin_set(led1_pin, LED1_PIN, 1);

  led2_pin = device_get_binding(LED2);
  if (led2_pin == NULL) {
    for (;;) ;
  }
  ret = gpio_pin_configure(led2_pin, LED2_PIN, GPIO_OUTPUT_ACTIVE | LED2_FLAGS);
  if (ret < 0) {
    TVMPlatformAbort((tvm_crt_error_t)0xbeef4);
  }
  gpio_pin_set(led2_pin, LED2_PIN, 1);

  led3_pin = device_get_binding(LED3);
  if (led3_pin == NULL) {
    for (;;) ;
  }
  ret = gpio_pin_configure(led3_pin, LED3_PIN, GPIO_OUTPUT_ACTIVE | LED3_FLAGS);
  if (ret < 0) {
    TVMPlatformAbort((tvm_crt_error_t)0xbeef4);
  }
  gpio_pin_set(led3_pin, LED3_PIN, 1);
#endif

  // Claim console device.
  tvm_uart = device_get_binding(DT_LABEL(DT_CHOSEN(zephyr_console)));
  const struct device* shadow_tvm_uart = tvm_uart;
  uart_rx_init(&uart_rx_buf, tvm_uart);

  // Initialize uTVM RPC server, which will receive commands from the UART and execute them.
  utvm_rpc_server_t server = UTvmRpcServerInit(write_serial, NULL);
  TVMLogf("uTVM Zephyr runtime - running");
#ifdef CONFIG_LED
  gpio_pin_set(led0_pin, LED0_PIN, 0);
  gpio_pin_set(led1_pin, LED1_PIN, 0);
  gpio_pin_set(led2_pin, LED2_PIN, 0);
  gpio_pin_set(led3_pin, LED3_PIN, 0);
#endif

  // The main application loop. We continuously read commands from the UART
  // and dispatch them to UTvmRpcServerLoop().
  while (true) {
    //uint8_t buf[256];
    int bytes_read = uart_rx_buf_read(&uart_rx_buf, main_rx_buf, sizeof(main_rx_buf));
    if (bytes_read > 0) {
      size_t bytes_remaining = bytes_read;
      uint8_t* cursor = main_rx_buf;
      while (bytes_remaining > 0) {
        // Pass the received bytes to the RPC server.
        tvm_crt_error_t err = UTvmRpcServerLoop(server, &cursor, &bytes_remaining);
        if (err != kTvmErrorNoError && err != kTvmErrorFramingShortPacket) {
#ifdef CONFIG_LED
          gpio_pin_set(led0_pin, LED0_PIN, 1);
          gpio_pin_set(led2_pin, LED2_PIN, 1);
#endif
          TVMPlatformAbort(err);
        }
       if (g_num_bytes_written != 0 || g_num_bytes_requested != 0) {
          if (g_num_bytes_written != g_num_bytes_requested) {
#ifdef CONFIG_LED
            gpio_pin_set(led1_pin, LED1_PIN, 1);
#endif
            TVMPlatformAbort((tvm_crt_error_t)0xbeef5);
          }
          g_num_bytes_written = 0;
          g_num_bytes_requested = 0;
        }
      }
    }
  }

#ifdef CONFIG_ARCH_POSIX
  posix_exit(0);
#endif
}



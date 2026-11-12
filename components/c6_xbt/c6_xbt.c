#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_avrc_api.h"
#include "esp_hidh_api.h"
#include "esp_bluedroid_hci.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_core_dump.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"
#include "esp_heap_caps.h"
#if CONFIG_HEAP_TASK_TRACKING
#include "esp_heap_task_info.h"
#endif
#include "esp_hidh.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include "esp_gattc_api.h"
#include "esp_hidh_gattc.h"
#include "eh_common_interface.h"
#include "eh_cp.h"
#include "eh_cp_core.h"
#include "eh_cp_event.h"
#include "eh_frame.h"
#include "eh_transport.h"
#include "eh_transport_cp.h"
#include "c6_xbt_proto.h"

#define WORKER_STACK 3072
#define WORKER_PRIORITY 5
#define QUEUE_DEPTH 8
#define MESSAGE_MAX 600
#define ANSWER_WAIT_MS 2000
#define NAME_MAX_LEN 64
#define AUDIO_BUFFER 16384
#define CREDIT_STEP 4096
#define SENDER_STACK 1536
#define RECEIVER_STACK 2048
#define INPUT_PENDING_MAX 16
#define LOCAL_ANSWERS 4

static const char *TAG = "xbt_bt";

enum {
    HEAP_INIT,
    HEAP_OWN,
    HEAP_CTRL_BEFORE,
    HEAP_CTRL,
    HEAP_BD_INIT,
    HEAP_BLUEDROID,
    HEAP_PROFILES,
    HEAP_UP,
    HEAP_AUDIO,
    HEAP_MARKS
};
static const char *const HEAP_MARK_NAMES[HEAP_MARKS] = {"init",       "own",      "pre-ctrl", "ctrl", "bluedroid-init",
                                                        "bluedroid-enable", "profiles", "up",       "audio"};
static uint32_t s_heap_marks[HEAP_MARKS];

static void heap_mark(int mark)
{
    s_heap_marks[mark] = (uint32_t) heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

#define NVS_VALUE_MAX 1200

typedef struct {
    uint32_t msg_id;
    uint8_t len;
    uint8_t data[16];
    uint8_t *big;
    uint16_t big_len;
} command_t;

static const char *const NVS_SPACES[] = {"xbt_bt", "bt_config.conf"};
static xbt_record_t s_records[XBT_RECORDS_MAX];
static uint8_t s_record_count = 0;

static const esp_bluedroid_hci_driver_callbacks_t *s_host_cb = NULL;
static volatile bool s_send_available = true;
static volatile bool s_started = false;
static volatile bool s_ready = false;
static volatile bool s_on_dongle = false;
static volatile bool s_dongle_requested = false;
static volatile bool s_dongle_gone = false;
static char s_bt_name[33] = "XBT";
static bool s_safe = false;
static QueueHandle_t s_commands = NULL;
static uint8_t s_ble_scan_seconds = 0;
static RingbufHandle_t s_audio = NULL;
static uint32_t s_consumed = 0;
static volatile uint8_t s_volume = 127;
static volatile uint32_t s_a2d_calls = 0;
static volatile uint32_t s_a2d_max_us = 0;
static uint32_t s_command_max_us = 0;
static volatile uint32_t s_frames_in = 0;
static volatile uint32_t s_rx_dropped = 0;
static volatile uint32_t s_h4_dropped = 0;
static volatile uint32_t s_frames_out = 0;
static volatile uint32_t s_send_max_us = 0;
static volatile uint32_t s_a2d_underruns = 0;
static volatile uint8_t s_log_level = ESP_LOG_NONE;
static volatile bool s_audio_streaming;

static int32_t a2d_fill(uint8_t *buf, int32_t len);
uint8_t c6_flash_starts(void);
uint8_t c6_flash_crash_reason(void);
static void queue_upsert(const uint8_t *bda, uint8_t transport, uint8_t addr_type, uint8_t kind, const char *name,
                         size_t name_len);
static uint8_t s_connect_bda[6];
static uint8_t s_connect_addr_type = 0;

typedef uint8_t pending_frame_t;

#define TX_H4_RING_SIZE 5120
#define TX_MSG_RING_SIZE 3328
#define RX_GRANT 4960
#define RX_SYNC_ROOM 256
#define RX_RING_SIZE (RX_GRANT + XBT_FRAME_MAX + 16 + RX_SYNC_ROOM)
#define RX_CREDIT_STEP 1024
#define TX_ITEM_HEAD 3
#define TX_ITEM_RING_HEAD 12
#define H4_ACL_MAX 1021
#define H4_NEED (TX_ITEM_RING_HEAD + TX_ITEM_HEAD + 1 + 4 + H4_ACL_MAX)

static RingbufHandle_t s_tx_h4 = NULL;
static RingbufHandle_t s_tx_msg = NULL;
static RingbufHandle_t s_rx_ring = NULL;
static uint32_t s_pending_h4 = 0;
static uint32_t s_pending_messages = 0;
static uint32_t s_done_h4 = 0;
static uint32_t s_done_messages = 0;
static bool s_h4_refused = false;
static portMUX_TYPE s_pending_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_sender = NULL;
static volatile bool s_host_big = false;

static RingbufHandle_t tx_ring_of(bool h4)
{
    return h4 ? s_tx_h4 : s_tx_msg;
}

static bool h4_room(void)
{
    return s_tx_h4 != NULL && xRingbufferGetCurFreeSize(s_tx_h4) >= H4_NEED;
}

static uint8_t *frame_new_wait(uint8_t first, size_t len, pending_frame_t **slot, TickType_t wait)
{
    *slot = NULL;
    RingbufHandle_t ring = tx_ring_of((first & XBT_FRAME_MESSAGE) == 0);
    if (s_sender == NULL || ring == NULL || len + 1 > UINT16_MAX) {
        return NULL;
    }
    void *item = NULL;
    if (xRingbufferSendAcquire(ring, &item, TX_ITEM_HEAD + 1 + len, wait) != pdTRUE || item == NULL) {
        return NULL;
    }
    pending_frame_t *bytes = (pending_frame_t *) item;
    bytes[0] = (first & XBT_FRAME_MESSAGE) == 0 ? 1 : 0;
    bytes[1] = (uint8_t) ((len + 1) & 0xFF);
    bytes[2] = (uint8_t) ((len + 1) >> 8);
    bytes[3] = first;
    *slot = bytes;
    return bytes + TX_ITEM_HEAD + 1;
}

static uint8_t *frame_new(uint8_t first, size_t len, pending_frame_t **slot)
{
    return frame_new_wait(first, len, slot, 0);
}

static void frame_push(pending_frame_t *slot, size_t len)
{
    slot[1] = (uint8_t) ((len + 1) & 0xFF);
    slot[2] = (uint8_t) ((len + 1) >> 8);
    taskENTER_CRITICAL(&s_pending_lock);
    if (slot[0]) {
        s_pending_h4++;
    } else {
        s_pending_messages++;
    }
    taskEXIT_CRITICAL(&s_pending_lock);
    xRingbufferSendComplete(tx_ring_of(slot[0] != 0), slot);
    xTaskNotifyGive(s_sender);
}

static esp_err_t frame_send(uint8_t first, const uint8_t *data, size_t len)
{
    pending_frame_t *slot = NULL;
    uint8_t *payload = frame_new(first, len, &slot);
    if (payload == NULL) {
        return s_sender == NULL ? ESP_ERR_INVALID_STATE : ESP_ERR_NO_MEM;
    }
    if (len > 0) {
        memcpy(payload, data, len);
    }
    frame_push(slot, len);
    return ESP_OK;
}

static esp_err_t answer_send(uint32_t msg, const uint8_t *data, size_t len)
{
    pending_frame_t *slot = NULL;
    uint8_t *payload = frame_new_wait(XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(msg), len, &slot,
                                      pdMS_TO_TICKS(ANSWER_WAIT_MS));
    if (payload == NULL) {
        return ESP_ERR_TIMEOUT;
    }
    if (len > 0) {
        memcpy(payload, data, len);
    }
    frame_push(slot, len);
    return ESP_OK;
}

static uint8_t *message_new(uint32_t msg, size_t len, pending_frame_t **frame)
{
    return frame_new(XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(msg), len, frame);
}

static void message_push(pending_frame_t *frame, size_t len)
{
    frame_push(frame, len);
}

static void item_done(void *priv)
{
    pending_frame_t *item = (pending_frame_t *) priv;
    const bool h4 = item[0] != 0;
    vRingbufferReturnItem(tx_ring_of(h4), item);
    taskENTER_CRITICAL(&s_pending_lock);
    if (h4) {
        s_done_h4++;
    } else {
        s_done_messages++;
    }
    taskEXIT_CRITICAL(&s_pending_lock);
    if (s_sender != NULL) {
        xTaskNotifyGive(s_sender);
    }
}

static void host_send(pending_frame_t *item)
{
    interface_buffer_handle_t handle;
    memset(&handle, 0, sizeof(handle));
    handle.if_type = ESP_HCI_IF;
    handle.if_num = 0;
    handle.payload_len = (uint16_t) (item[1] | (item[2] << 8));
    handle.payload = item + TX_ITEM_HEAD;
    handle.priv_buffer_handle = item;
    handle.free_buf_handle = item_done;
    const int64_t start = esp_timer_get_time();
    const int sent = send_to_host_queue(&handle, PRIO_Q_BT);
    const uint32_t took = (uint32_t) (esp_timer_get_time() - start);
    if (took > s_send_max_us) {
        s_send_max_us = took;
    }
    s_frames_out++;
    if (sent != 0) {
        item_done(item);
    }
}

static void pending_release(uint32_t h4, uint32_t messages)
{
    taskENTER_CRITICAL(&s_pending_lock);
    s_pending_messages -= messages;
    s_pending_h4 -= h4;
    taskEXIT_CRITICAL(&s_pending_lock);
    const bool room = h4_room();
    taskENTER_CRITICAL(&s_pending_lock);
    const bool wake = s_h4_refused && s_send_available && room;
    if (wake) {
        s_h4_refused = false;
    }
    taskEXIT_CRITICAL(&s_pending_lock);
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (wake && s_on_dongle && cb != NULL && cb->notify_host_send_available != NULL) {
        cb->notify_host_send_available();
    }
}

static void log_drain(void);

static portMUX_TYPE s_credit_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_credit_free = 0;
static uint32_t s_credit_total = 0;
static volatile bool s_credit_absolute = false;

static uint32_t credit_add(uint32_t bytes)
{
    taskENTER_CRITICAL(&s_credit_lock);
    s_credit_free += bytes;
    s_credit_total += bytes;
    const uint32_t waiting = s_credit_free;
    taskEXIT_CRITICAL(&s_credit_lock);
    return waiting;
}

static uint32_t credit_total(void)
{
    taskENTER_CRITICAL(&s_credit_lock);
    const uint32_t total = s_credit_total;
    taskEXIT_CRITICAL(&s_credit_lock);
    return total;
}

static void credit_flush(void)
{
    taskENTER_CRITICAL(&s_credit_lock);
    const uint32_t bytes = s_credit_free;
    const uint32_t total = s_credit_total;
    const bool due = bytes >= RX_CREDIT_STEP;
    if (due) {
        s_credit_free = 0;
    }
    taskEXIT_CRITICAL(&s_credit_lock);
    if (!due) {
        return;
    }
    uint8_t msg[4] = {(uint8_t) (bytes & 0xFF), (uint8_t) (bytes >> 8), 0, 0};
    size_t len = 2;
    if (s_credit_absolute) {
        msg[0] = (uint8_t) (total & 0xFF);
        msg[1] = (uint8_t) (total >> 8);
        msg[2] = (uint8_t) (total >> 16);
        msg[3] = (uint8_t) (total >> 24);
        len = 4;
    }
    if (frame_send(XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(XBT_MSG_RX_CREDIT), msg, len) != ESP_OK) {
        taskENTER_CRITICAL(&s_credit_lock);
        s_credit_free += bytes;
        taskEXIT_CRITICAL(&s_credit_lock);
    }
}

static void credit_reset(void)
{
    taskENTER_CRITICAL(&s_credit_lock);
    s_credit_free = 0;
    s_credit_total = 0;
    taskEXIT_CRITICAL(&s_credit_lock);
}

static pending_frame_t *tx_next(void)
{
    size_t size = 0;
    pending_frame_t *item = (pending_frame_t *) xRingbufferReceive(s_tx_h4, &size, 0);
    return item != NULL ? item : (pending_frame_t *) xRingbufferReceive(s_tx_msg, &size, 0);
}

static void done_collect(void)
{
    taskENTER_CRITICAL(&s_pending_lock);
    const uint32_t h4 = s_done_h4;
    const uint32_t messages = s_done_messages;
    s_done_h4 = 0;
    s_done_messages = 0;
    taskEXIT_CRITICAL(&s_pending_lock);
    if (h4 != 0 || messages != 0) {
        pending_release(h4, messages);
    }
}

static void sender_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        log_drain();
        for (;;) {
            done_collect();
            credit_flush();
            pending_frame_t *item = tx_next();
            if (item == NULL) {
                break;
            }
            if ((size_t) (item[1] | (item[2] << 8)) > XBT_FRAME_MAX) {
                item_done(item);
                continue;
            }
            host_send(item);
        }
    }
}

static esp_err_t message_send(uint32_t msg, const uint8_t *data, size_t len)
{
    return frame_send(XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(msg), data, len);
}

static volatile bool s_host_ready = false;

static bool input_room(void)
{
    if (!s_host_ready) {
        return false;
    }
    taskENTER_CRITICAL(&s_pending_lock);
    const bool full = s_pending_messages >= INPUT_PENDING_MAX;
    taskEXIT_CRITICAL(&s_pending_lock);
    return !full;
}

static portMUX_TYPE s_audio_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_audio_users = 0;

static RingbufHandle_t audio_take(void)
{
    taskENTER_CRITICAL(&s_audio_lock);
    RingbufHandle_t ring = s_audio;
    if (ring != NULL) {
        s_audio_users++;
    }
    taskEXIT_CRITICAL(&s_audio_lock);
    return ring;
}

static void audio_give(void)
{
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio_users--;
    taskEXIT_CRITICAL(&s_audio_lock);
}

static volatile uint32_t s_audio_next = 0;
static volatile bool s_audio_window = false;
static bool s_audio_starved = false;
static bool s_audio_report = false;

static bool audio_window_send(RingbufHandle_t ring)
{
    const uint32_t next = s_audio_next;
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(ring, NULL, NULL, NULL, NULL, &waiting);
    const uint32_t window[2] = {next + (uint32_t) xRingbufferGetCurFreeSize(ring), (uint32_t) waiting};
    return message_send(XBT_MSG_AUDIO_WINDOW, (const uint8_t *) window, sizeof(window)) == ESP_OK;
}

static void audio_window_reset(void)
{
    s_audio_next = 0;
    s_audio_window = false;
    s_audio_starved = false;
    s_audio_report = false;
}

static uint8_t s_audio_storage[AUDIO_BUFFER];
static StaticRingbuffer_t s_audio_struct;
static RingbufHandle_t s_audio_ring = NULL;

static void audio_reserve(void)
{
    if (s_audio_ring == NULL) {
        s_audio_ring = xRingbufferCreateStatic(AUDIO_BUFFER, RINGBUF_TYPE_BYTEBUF, s_audio_storage, &s_audio_struct);
    }
}

static void audio_drain(void)
{
    if (s_audio_ring == NULL) {
        return;
    }
    size_t len = 0;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_audio_ring, &len, 0, AUDIO_BUFFER)) != NULL) {
        vRingbufferReturnItem(s_audio_ring, item);
    }
}

static void audio_close(void)
{
    taskENTER_CRITICAL(&s_audio_lock);
    RingbufHandle_t ring = s_audio;
    s_audio = NULL;
    taskEXIT_CRITICAL(&s_audio_lock);
    if (ring == NULL) {
        return;
    }
    for (;;) {
        taskENTER_CRITICAL(&s_audio_lock);
        const bool busy = s_audio_users != 0;
        taskEXIT_CRITICAL(&s_audio_lock);
        if (!busy) {
            break;
        }
        vTaskDelay(1);
    }
    audio_drain();
}

static void audio_open(void)
{
    audio_close();
    audio_reserve();
    if (s_audio_ring == NULL) {
        ESP_LOGE(TAG, "No audio ring");
        return;
    }
    audio_drain();
    taskENTER_CRITICAL(&s_audio_lock);
    s_audio = s_audio_ring;
    taskEXIT_CRITICAL(&s_audio_lock);
    heap_mark(HEAP_AUDIO);
}

static int32_t a2d_data_cb(uint8_t *buf, int32_t len)
{
    const int64_t start = esp_timer_get_time();
    const int32_t result = a2d_fill(buf, len);
    const uint32_t took = (uint32_t) (esp_timer_get_time() - start);
    s_a2d_calls++;
    if (took > s_a2d_max_us) {
        s_a2d_max_us = took;
    }
    return result;
}

static int32_t a2d_fill(uint8_t *buf, int32_t len)
{
    if (buf == NULL || len <= 0) {
        return 0;
    }
    int32_t filled = 0;
    RingbufHandle_t ring = audio_take();
    while (ring != NULL && filled < len) {
        size_t got = 0;
        uint8_t *chunk = xRingbufferReceiveUpTo(ring, &got, 0, (size_t) (len - filled));
        if (chunk == NULL) {
            break;
        }
        memcpy(buf + filled, chunk, got);
        vRingbufferReturnItem(ring, chunk);
        filled += (int32_t) got;
    }
    s_consumed += (uint32_t) filled;
    if (ring != NULL && s_audio_window) {
        const bool starved = filled == 0 && s_audio_streaming;
        if (starved && !s_audio_starved) {
            s_audio_report = true;
        }
        s_audio_starved = starved;
        if ((s_consumed >= CREDIT_STEP || s_audio_report) && audio_window_send(ring)) {
            s_consumed = 0;
            s_audio_report = false;
        }
    } else if (s_consumed >= CREDIT_STEP &&
               message_send(XBT_MSG_AUDIO_CREDIT, (const uint8_t *) &s_consumed, sizeof(s_consumed)) == ESP_OK) {
        s_consumed = 0;
    }
    if (ring != NULL) {
        audio_give();
    }
    const uint8_t volume = s_volume;
    if (volume < 127) {
        const int32_t factor = ((int32_t) volume << 15) / 127;
        int16_t *samples = (int16_t *) buf;
        for (int32_t i = 0; i < filled / 2; i++) {
            samples[i] = (int16_t) ((samples[i] * factor) >> 15);
        }
    }
    if (filled < len) {
        if (s_audio_streaming) {
            s_a2d_underruns++;
        }
        memset(buf + filled, 0, (size_t) (len - filled));
    }
    return len;
}

static uint8_t s_audio_bda[6];
static volatile bool s_audio_connected = false;
static volatile bool s_audio_streaming = false;
static volatile bool s_audio_paging = false;
static volatile bool s_audio_paused = false;
static uint32_t s_audio_interval_s = 10;
static esp_timer_handle_t s_reconnect_timer = NULL;

static uint8_t s_pair_bda[6];
static volatile bool s_pair_audio = false;
static uint8_t s_incoming_bda[6];
static volatile bool s_incoming_pair = false;
static volatile bool s_incoming_connect = false;
static volatile bool s_sighting_wanted = false;
static volatile bool s_sighted_connect = false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
static esp_a2d_conn_hdl_t s_audio_hdl = ESP_A2D_INVALID_CONN_HDL;
#endif

static void audio_state_send(void)
{
    uint8_t msg[9];
    memcpy(msg, s_audio_bda, 6);
    msg[6] = s_audio_connected ? 1 : 0;
    msg[7] = s_audio_streaming ? 1 : 0;
    msg[8] = s_audio_paging ? 1 : 0;
    message_send(XBT_MSG_AUDIO_STATE, msg, sizeof(msg));
}

static void reconnect_fire(void *arg)
{
    if (s_commands == NULL) {
        return;
    }
    command_t command = {.msg_id = XBT_MSG_AUDIO_RECONNECT};
    xQueueSend(s_commands, &command, 0);
}

static void reconnect_schedule(void)
{
    if (s_reconnect_timer == NULL || s_audio_paused || s_audio_connected) {
        return;
    }
    esp_timer_stop(s_reconnect_timer);
    esp_timer_start_once(s_reconnect_timer, (uint64_t) s_audio_interval_s * 1000000ULL);
}

static volatile bool s_a2d_deinit_done = false;
static void close_waiter_wake(void);

static void a2d_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    if (event == ESP_A2D_PROF_STATE_EVT && param->a2d_prof_stat.init_state == ESP_A2D_DEINIT_SUCCESS) {
        s_a2d_deinit_done = true;
        close_waiter_wake();
        return;
    }
    if (event == ESP_A2D_AUDIO_STATE_EVT) {
        s_audio_streaming = param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED;
        audio_state_send();
        return;
    }
    if (event != ESP_A2D_CONNECTION_STATE_EVT) {
        return;
    }
    if (s_audio_connected && memcmp(s_audio_bda, param->conn_stat.remote_bda, 6) != 0) {
        if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            ESP_LOGW(TAG, "Second audio device refused");
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
            esp_a2d_source_disconnect_by_handle(param->conn_stat.conn_hdl);
#else
            esp_a2d_source_disconnect(param->conn_stat.remote_bda);
#endif
        }
        return;
    }
    memcpy(s_audio_bda, param->conn_stat.remote_bda, 6);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
    s_audio_hdl = param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED ? ESP_A2D_INVALID_CONN_HDL
                                                                                  : param->conn_stat.conn_hdl;
#endif
    if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
        audio_open();
        s_consumed = 0;
        audio_window_reset();
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
        esp_a2d_media_ctrl_by_handle(s_audio_hdl, ESP_A2D_MEDIA_CTRL_START);
#else
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
#endif
        s_audio_connected = true;
        s_audio_paging = false;
        queue_upsert(param->conn_stat.remote_bda, XBT_TRANSPORT_CLASSIC, 0, XBT_KIND_AUDIO, NULL, 0);
    } else if (param->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
        audio_close();
        s_audio_connected = false;
        s_audio_streaming = false;
        s_audio_paging = !s_audio_paused;
        reconnect_schedule();
    } else {
        return;
    }
    audio_state_send();
}

static int split_le_recv(uint8_t *data, uint16_t len);
static void split_local_ext_features(void);

#define HCI_HOST_PRIORITY (configMAX_PRIORITIES - 3)
#define HOST_RECV_PRIORITY (configMAX_PRIORITIES - 2)

static int host_recv(const esp_bluedroid_hci_driver_callbacks_t *cb, uint8_t *data, uint16_t len)
{
    const UBaseType_t own = uxTaskPriorityGet(NULL);
    if (own >= HCI_HOST_PRIORITY) {
        return cb->notify_host_recv(data, len);
    }
    vTaskPrioritySet(NULL, HOST_RECV_PRIORITY);
    const int result = cb->notify_host_recv(data, len);
    vTaskPrioritySet(NULL, own);
    return result;
}

static void vhci_send_available(void)
{
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (cb != NULL && cb->notify_host_send_available != NULL) {
        cb->notify_host_send_available();
    }
}

static int vhci_recv(uint8_t *data, uint16_t len)
{
    if (s_on_dongle) {
        return split_le_recv(data, len);
    }
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (cb != NULL && cb->notify_host_recv != NULL) {
        return host_recv(cb, data, len);
    }
    return 0;
}

static const esp_vhci_host_callback_t VHCI_CB = {
    .notify_host_send_available = vhci_send_available,
    .notify_host_recv = vhci_recv,
};

#define H4_COMMAND 0x01
#define H4_EVENT 0x04
#define HCI_COMMAND_COMPLETE 0x0E
#define HCI_COMMAND_STATUS 0x0F
#define HCI_DISCONNECTION_COMPLETE 0x05
#define HCI_DISCONNECT 0x0406
#define HCI_WRITE_LE_HOST_SUPPORT 0x0C6D
#define HCI_HOST_FEATURE_LE 0x02
#define HCI_HOST_FEATURE_SIMULTANEOUS 0x04
#define HCI_REASON_LOCAL_HOST 0x16
#define HCI_OGF_LE 0x08
#define HCI_OGF_VENDOR 0x3F
#define HCI_READ_LOCAL_EXT_FEATURES 0x1004
#define HCI_READ_BUFFER_SIZE 0x1005
#define BLE_STAGE_ACL_SIZE 251
#define BLE_STAGE_ACL_COUNT 8

static bool ble_stage_answers(uint16_t opcode)
{
    switch (opcode) {
    case 0x0C01:
    case 0x0C03:
    case 0x0C31:
    case 0x0C33:
    case 0x0C35:
    case 0x1001:
    case 0x1002:
    case 0x1003:
    case 0x1009:
    case HCI_DISCONNECT:
        return false;
    default:
        return (opcode >> 10) != HCI_OGF_LE && (opcode >> 10) != HCI_OGF_VENDOR;
    }
}

static void ble_stage_answer(uint16_t opcode, const uint8_t *params, uint16_t params_len)
{
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (cb == NULL || cb->notify_host_recv == NULL) {
        return;
    }
    uint8_t event[17] = {H4_EVENT, HCI_COMMAND_COMPLETE, 0, 1, (uint8_t) opcode, (uint8_t) (opcode >> 8), 0};
    uint16_t len = 7;
    if (opcode == HCI_READ_BUFFER_SIZE) {
        event[len++] = (uint8_t) BLE_STAGE_ACL_SIZE;
        event[len++] = 0;
        event[len++] = 0;
        event[len++] = BLE_STAGE_ACL_COUNT;
        event[len++] = 0;
        event[len++] = 0;
        event[len++] = 0;
    } else if (opcode == HCI_READ_LOCAL_EXT_FEATURES) {
        const uint8_t page = params_len > 0 ? params[0] : 0;
        event[len++] = page;
        event[len++] = 1;
        if (page == 0) {
            event[len + 4] = 0x40;
        } else if (page == 1) {
            event[len] = 0x02;
        }
        len += 8;
    }
    event[2] = (uint8_t) (len - 3);
    host_recv(cb, event, len);
}

static esp_timer_handle_t s_local_answer_timer = NULL;
static uint16_t s_local_answers[LOCAL_ANSWERS];
static uint8_t s_local_answer_count = 0;
static uint8_t s_le_host_bits = 0;

#define HCI_TRACE_RECORDS 192
#define HCI_TRACE_BYTES 24
#define HCI_TRACE_HOST_BR 1
#define HCI_TRACE_HOST_LE 2
#define HCI_TRACE_BR_HOST 3
#define HCI_TRACE_LE_HOST 4
#define HCI_TRACE_LOCAL 5

typedef struct {
    uint32_t us;
    uint16_t len;
    uint8_t dir;
    uint8_t count;
    uint8_t data[HCI_TRACE_BYTES];
} hci_trace_record_t;

static hci_trace_record_t *s_hci_trace = NULL;
static uint16_t s_hci_trace_next = 0;
static uint16_t s_hci_trace_used = 0;
static volatile bool s_hci_trace_dump = false;
static portMUX_TYPE s_hci_trace_lock = portMUX_INITIALIZER_UNLOCKED;

static void hci_trace(uint8_t dir, const uint8_t *data, size_t len)
{
    if (s_hci_trace == NULL || s_hci_trace_dump || data == NULL) {
        return;
    }
    const uint32_t us = (uint32_t) esp_timer_get_time();
    const uint8_t count = len < HCI_TRACE_BYTES ? (uint8_t) len : HCI_TRACE_BYTES;
    taskENTER_CRITICAL(&s_hci_trace_lock);
    if (s_hci_trace != NULL) {
        hci_trace_record_t *record = &s_hci_trace[s_hci_trace_next];
        record->us = us;
        record->len = (uint16_t) len;
        record->dir = dir;
        record->count = count;
        memcpy(record->data, data, count);
        s_hci_trace_next = (uint16_t) ((s_hci_trace_next + 1) % HCI_TRACE_RECORDS);
        if (s_hci_trace_used < HCI_TRACE_RECORDS) {
            s_hci_trace_used++;
        }
    }
    taskEXIT_CRITICAL(&s_hci_trace_lock);
}

static void hci_trace_start(void)
{
    if (s_hci_trace != NULL || s_hci_trace_dump) {
        return;
    }
    hci_trace_record_t *records = (hci_trace_record_t *) calloc(HCI_TRACE_RECORDS, sizeof(hci_trace_record_t));
    if (records == NULL) {
        ESP_LOGW(TAG, "HCI trace: no RAM");
        return;
    }
    taskENTER_CRITICAL(&s_hci_trace_lock);
    s_hci_trace_next = 0;
    s_hci_trace_used = 0;
    s_hci_trace = records;
    taskEXIT_CRITICAL(&s_hci_trace_lock);
}

static void local_answer_fire(void *arg)
{
    for (;;) {
        uint16_t opcode = 0;
        taskENTER_CRITICAL(&s_pending_lock);
        if (s_local_answer_count > 0) {
            opcode = s_local_answers[0];
            s_local_answer_count--;
            memmove(s_local_answers, s_local_answers + 1, s_local_answer_count * sizeof(s_local_answers[0]));
        }
        taskEXIT_CRITICAL(&s_pending_lock);
        if (opcode == 0) {
            return;
        }
        if (s_on_dongle) {
            const uint8_t answered[2] = {(uint8_t) (opcode & 0xFF), (uint8_t) (opcode >> 8)};
            hci_trace(HCI_TRACE_LOCAL, answered, sizeof(answered));
        }
        if (s_on_dongle && opcode == HCI_READ_LOCAL_EXT_FEATURES) {
            split_local_ext_features();
        } else {
            ble_stage_answer(opcode, NULL, 0);
        }
    }
}

static void local_answer(uint16_t opcode)
{
    if (s_local_answer_timer == NULL) {
        const esp_timer_create_args_t args = {.callback = local_answer_fire, .name = "xbt_hci"};
        esp_timer_create(&args, &s_local_answer_timer);
    }
    taskENTER_CRITICAL(&s_pending_lock);
    if (s_local_answer_count < LOCAL_ANSWERS) {
        s_local_answers[s_local_answer_count++] = opcode;
    }
    taskEXIT_CRITICAL(&s_pending_lock);
    if (s_local_answer_timer != NULL) {
        esp_timer_start_once(s_local_answer_timer, 200);
    }
}

static void dongle_gone_answer(const uint8_t *data, uint16_t len)
{
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (data[0] != H4_COMMAND || len < 4 || cb == NULL || cb->notify_host_recv == NULL) {
        return;
    }
    const uint16_t opcode = (uint16_t) (data[1] | (data[2] << 8));
    if (opcode != HCI_DISCONNECT || len < 7) {
        ble_stage_answer(opcode, data + 4, (uint16_t) (len - 4));
        return;
    }
    uint8_t status[7] = {H4_EVENT, HCI_COMMAND_STATUS, 4, 0, 1, data[1], data[2]};
    host_recv(cb, status, sizeof(status));
    uint8_t complete[7] = {H4_EVENT, HCI_DISCONNECTION_COMPLETE, 4, 0, data[4], data[5], HCI_REASON_LOCAL_HOST};
    host_recv(cb, complete, sizeof(complete));
}

#define H4_ACL 0x02
#define SPLIT_SIDE_BR 0x01
#define SPLIT_SIDE_LE 0x02
#define SPLIT_EVENT_BUF 258
#define SPLIT_LE_LINKS 16
#define SPLIT_LE_HANDLE_BASE 0x0E00
#define SPLIT_NO_HANDLE 0xFFFF
#define HCI_ENCRYPTION_CHANGE 0x08
#define HCI_READ_REMOTE_VERSION_COMPLETE 0x0C
#define HCI_NUM_COMPLETED_PACKETS 0x13
#define HCI_ENCRYPTION_KEY_REFRESH 0x30
#define HCI_LE_META 0x3E
#define HCI_AUTH_PAYLOAD_TIMEOUT 0x57
#define HCI_ENCRYPTION_CHANGE_V2 0x59
#define HCI_STATUS_UNKNOWN_COMMAND 0x01
#define HCI_SET_EVENT_MASK 0x0C01
#define HCI_RESET 0x0C03
#define HCI_SET_C2H_FLOW_CONTROL 0x0C31
#define HCI_HOST_BUFFER_SIZE 0x0C33
#define HCI_HOST_NUM_COMPLETED_PACKETS 0x0C35
#define HCI_SET_EVENT_MASK_PAGE_2 0x0C63
#define HCI_READ_LOCAL_VERSION 0x1001
#define HCI_READ_LOCAL_COMMANDS 0x1002
#define HCI_READ_LOCAL_FEATURES 0x1003
#define HCI_READ_BD_ADDR 0x1009
#define HCI_BCM_WRITE_BD_ADDR 0xFC01
#define HCI_MANUFACTURER_BROADCOM 15
#define HCI_FEATURE_LE_OFF 4
#define HCI_FEATURE_LE_MASK 0x40
#define HCI_FEATURE_SIMUL_OFF 6
#define HCI_FEATURE_SIMUL_MASK 0x02

typedef enum {
    SPLIT_ROUTE_BR,
    SPLIT_ROUTE_LE,
    SPLIT_ROUTE_BOTH,
    SPLIT_ROUTE_LOCAL,
} split_route_t;

typedef enum {
    SPLIT_ADDR_NONE,
    SPLIT_ADDR_VERSION,
    SPLIT_ADDR_READ_LE,
    SPLIT_ADDR_WRITE_BR,
    SPLIT_ADDR_READ_BR,
} split_addr_t;

static SemaphoreHandle_t s_split_lock = NULL;
static struct {
    uint16_t opcode;
    uint8_t waiting;
    bool both;
    bool translate;
    bool broadcom;
    uint16_t host_handle;
    split_addr_t addr_stage;
    uint8_t br_max_page;
    uint8_t local_page;
    uint8_t answer[SPLIT_EVENT_BUF];
    size_t answer_len;
    uint8_t addr_answer[13];
    uint8_t own[10];
    uint16_t le_ctrl[SPLIT_LE_LINKS];
} s_split;

static void split_lock(void)
{
    xSemaphoreTake(s_split_lock, portMAX_DELAY);
}

static void split_unlock(void)
{
    xSemaphoreGive(s_split_lock);
}

static void split_clear_links(void)
{
    for (size_t i = 0; i < SPLIT_LE_LINKS; i++) {
        s_split.le_ctrl[i] = SPLIT_NO_HANDLE;
    }
}

static bool split_begin(void)
{
    if (s_split_lock == NULL) {
        s_split_lock = xSemaphoreCreateMutex();
    }
    if (s_split_lock == NULL) {
        return false;
    }
    memset(&s_split, 0, sizeof(s_split));
    split_clear_links();
    s_le_host_bits = 0;
    return true;
}

static uint16_t split_host_of(uint16_t ctrl)
{
    for (size_t i = 0; i < SPLIT_LE_LINKS; i++) {
        if (s_split.le_ctrl[i] == ctrl) {
            return (uint16_t) (SPLIT_LE_HANDLE_BASE + i);
        }
    }
    return SPLIT_NO_HANDLE;
}

static uint16_t split_ctrl_of(uint16_t host)
{
    if (host < SPLIT_LE_HANDLE_BASE || host >= SPLIT_LE_HANDLE_BASE + SPLIT_LE_LINKS) {
        return SPLIT_NO_HANDLE;
    }
    return s_split.le_ctrl[host - SPLIT_LE_HANDLE_BASE];
}

static void split_link_up(uint16_t ctrl)
{
    if (split_host_of(ctrl) != SPLIT_NO_HANDLE) {
        return;
    }
    for (size_t i = 0; i < SPLIT_LE_LINKS; i++) {
        if (s_split.le_ctrl[i] == SPLIT_NO_HANDLE) {
            s_split.le_ctrl[i] = ctrl;
            return;
        }
    }
}

static void split_link_down(uint16_t ctrl)
{
    for (size_t i = 0; i < SPLIT_LE_LINKS; i++) {
        if (s_split.le_ctrl[i] == ctrl) {
            s_split.le_ctrl[i] = SPLIT_NO_HANDLE;
        }
    }
}

static void split_to_host(uint8_t *p)
{
    const uint16_t ctrl = (uint16_t) ((p[0] | (p[1] << 8)) & 0x0FFF);
    const uint16_t host = split_host_of(ctrl);
    if (host == SPLIT_NO_HANDLE) {
        return;
    }
    p[0] = (uint8_t) (host & 0xFF);
    p[1] = (uint8_t) ((p[1] & 0xF0) | (host >> 8));
}

static void split_deliver(uint8_t *h4, size_t len)
{
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (cb != NULL && cb->notify_host_recv != NULL) {
        host_recv(cb, h4, (uint16_t) len);
    }
}

static void split_send_side(uint8_t side, uint8_t *h4, size_t len)
{
    hci_trace(side == SPLIT_SIDE_LE ? HCI_TRACE_HOST_LE : HCI_TRACE_HOST_BR, h4, len);
    if (side == SPLIT_SIDE_LE) {
        esp_vhci_host_send_packet(h4, (uint16_t) len);
        return;
    }
    esp_err_t err = frame_send(h4[0], h4 + 1, len - 1);
    if (err != ESP_OK) {
        s_h4_dropped++;
        ESP_LOGW(TAG, "H4 packet of %u bytes not sent: %s", (unsigned) len, esp_err_to_name(err));
    }
}

static split_route_t split_route_for(uint16_t opcode)
{
    switch (opcode) {
    case HCI_RESET:
    case HCI_SET_EVENT_MASK:
    case HCI_SET_EVENT_MASK_PAGE_2:
    case HCI_SET_C2H_FLOW_CONTROL:
    case HCI_HOST_BUFFER_SIZE:
    case HCI_READ_LOCAL_COMMANDS:
        return SPLIT_ROUTE_BOTH;
    case HCI_WRITE_LE_HOST_SUPPORT:
        return SPLIT_ROUTE_LOCAL;
    case HCI_READ_LOCAL_VERSION:
        return SPLIT_ROUTE_LE;
    default:
        break;
    }
    const uint8_t ogf = (uint8_t) (opcode >> 10);
    return ogf == HCI_OGF_LE || ogf == HCI_OGF_VENDOR ? SPLIT_ROUTE_LE : SPLIT_ROUTE_BR;
}

static bool split_names_connection(uint16_t opcode)
{
    switch (opcode) {
    case 0x0406:
    case 0x041D:
    case 0x0C08:
    case 0x0C2D:
    case 0x0C36:
    case 0x0C37:
    case 0x0C7B:
    case 0x0C7C:
    case 0x1403:
    case 0x1405:
    case 0x2013:
    case 0x2015:
    case 0x2016:
    case 0x2019:
    case 0x201A:
    case 0x201B:
    case 0x2020:
    case 0x2021:
    case 0x2022:
    case 0x2030:
    case 0x2032:
    case 0x205A:
    case 0x205B:
    case 0x205C:
    case 0x2076:
    case 0x2077:
    case 0x207E:
        return true;
    default:
        return false;
    }
}

static size_t split_own_command(uint16_t opcode, const uint8_t *params, uint8_t plen)
{
    s_split.own[0] = H4_COMMAND;
    s_split.own[1] = (uint8_t) (opcode & 0xFF);
    s_split.own[2] = (uint8_t) (opcode >> 8);
    s_split.own[3] = plen;
    if (plen != 0) {
        memcpy(s_split.own + 4, params, plen);
    }
    return (size_t) 4 + plen;
}

static void split_local_ext_features(void)
{
    const uint8_t page = s_split.local_page;
    uint8_t event[17] = {H4_EVENT, HCI_COMMAND_COMPLETE, 14, 1, (uint8_t) HCI_READ_LOCAL_EXT_FEATURES,
                         (uint8_t) (HCI_READ_LOCAL_EXT_FEATURES >> 8), 0, page,
                         s_split.br_max_page > 1 ? s_split.br_max_page : 1};
    event[9] = page == 1 ? s_le_host_bits : 0;
    split_deliver(event, sizeof(event));
}

static void split_send_host_completed(uint8_t *data, size_t len)
{
    if (len < 5) {
        return;
    }
    const uint8_t count = data[4];
    if (len < (size_t) 5 + 4 * (size_t) count) {
        return;
    }
    uint8_t le[5 + 4 * SPLIT_LE_LINKS];
    uint8_t n_br = 0;
    uint8_t n_le = 0;
    split_lock();
    for (uint8_t i = 0; i < count; i++) {
        const uint8_t *entry = data + 5 + 4 * i;
        const uint16_t ctrl = split_ctrl_of((uint16_t) ((entry[0] | (entry[1] << 8)) & 0x0FFF));
        if (ctrl == SPLIT_NO_HANDLE) {
            memmove(data + 5 + 4 * n_br, entry, 4);
            n_br++;
        } else if (n_le < SPLIT_LE_LINKS) {
            uint8_t *to = le + 5 + 4 * n_le;
            to[0] = (uint8_t) (ctrl & 0xFF);
            to[1] = (uint8_t) (ctrl >> 8);
            to[2] = entry[2];
            to[3] = entry[3];
            n_le++;
        }
    }
    split_unlock();
    if (n_br != 0) {
        data[3] = (uint8_t) (1 + 4 * n_br);
        data[4] = n_br;
        split_send_side(SPLIT_SIDE_BR, data, (size_t) 5 + 4 * n_br);
    }
    if (n_le != 0) {
        le[0] = H4_COMMAND;
        le[1] = (uint8_t) (HCI_HOST_NUM_COMPLETED_PACKETS & 0xFF);
        le[2] = (uint8_t) (HCI_HOST_NUM_COMPLETED_PACKETS >> 8);
        le[3] = (uint8_t) (1 + 4 * n_le);
        le[4] = n_le;
        split_send_side(SPLIT_SIDE_LE, le, (size_t) 5 + 4 * n_le);
    }
}

static void split_send_command(uint8_t *data, uint16_t len)
{
    if (len < 4) {
        return;
    }
    const uint16_t opcode = (uint16_t) (data[1] | (data[2] << 8));
    const uint8_t *params = data + 4;
    const uint16_t plen = (uint16_t) (len - 4);
    if (opcode == HCI_HOST_NUM_COMPLETED_PACKETS) {
        split_send_host_completed(data, len);
        return;
    }
    split_route_t route = split_route_for(opcode);
    bool translate = false;
    uint16_t host_handle = 0;
    size_t own_len = 0;
    split_lock();
    if (split_names_connection(opcode) && plen >= 2) {
        host_handle = (uint16_t) ((params[0] | (params[1] << 8)) & 0x0FFF);
        const uint16_t ctrl = split_ctrl_of(host_handle);
        if (ctrl != SPLIT_NO_HANDLE) {
            route = SPLIT_ROUTE_LE;
            translate = true;
            data[4] = (uint8_t) (ctrl & 0xFF);
            data[5] = (uint8_t) (ctrl >> 8);
        }
    }
    if (opcode == HCI_WRITE_LE_HOST_SUPPORT && plen >= 2) {
        s_le_host_bits = (uint8_t) ((params[0] != 0 ? HCI_HOST_FEATURE_LE : 0) |
                                    (params[1] != 0 ? HCI_HOST_FEATURE_SIMULTANEOUS : 0));
    }
    if (opcode == HCI_READ_LOCAL_EXT_FEATURES && plen >= 1 && params[0] > s_split.br_max_page) {
        route = SPLIT_ROUTE_LOCAL;
        s_split.local_page = params[0];
    }
    if (opcode == HCI_RESET) {
        split_clear_links();
    }
    s_split.addr_stage = SPLIT_ADDR_NONE;
    s_split.opcode = opcode;
    s_split.both = route == SPLIT_ROUTE_BOTH;
    s_split.translate = translate;
    s_split.host_handle = host_handle;
    s_split.answer_len = 0;
    s_split.waiting = route == SPLIT_ROUTE_BR     ? SPLIT_SIDE_BR
                      : route == SPLIT_ROUTE_LE   ? SPLIT_SIDE_LE
                      : route == SPLIT_ROUTE_BOTH ? (SPLIT_SIDE_BR | SPLIT_SIDE_LE)
                                                  : 0;
    if (opcode == HCI_READ_BD_ADDR) {
        s_split.addr_stage = SPLIT_ADDR_VERSION;
        s_split.opcode = HCI_READ_LOCAL_VERSION;
        s_split.waiting = SPLIT_SIDE_BR;
        own_len = split_own_command(HCI_READ_LOCAL_VERSION, NULL, 0);
    } else if (route == SPLIT_ROUTE_LOCAL) {
        s_split.opcode = 0;
    }
    split_unlock();
    if (own_len != 0) {
        split_send_side(SPLIT_SIDE_BR, s_split.own, own_len);
        return;
    }
    if (route == SPLIT_ROUTE_LOCAL) {
        local_answer(opcode);
        return;
    }
    if (route == SPLIT_ROUTE_BR || route == SPLIT_ROUTE_BOTH) {
        split_send_side(SPLIT_SIDE_BR, data, len);
    }
    if (route == SPLIT_ROUTE_LE || route == SPLIT_ROUTE_BOTH) {
        split_send_side(SPLIT_SIDE_LE, data, len);
    }
}

static void split_send_acl(uint8_t *data, uint16_t len)
{
    if (len < 5) {
        return;
    }
    split_lock();
    const uint16_t ctrl = split_ctrl_of((uint16_t) ((data[1] | (data[2] << 8)) & 0x0FFF));
    split_unlock();
    if (ctrl == SPLIT_NO_HANDLE) {
        split_send_side(SPLIT_SIDE_BR, data, len);
        return;
    }
    data[1] = (uint8_t) (ctrl & 0xFF);
    data[2] = (uint8_t) ((data[2] & 0xF0) | (ctrl >> 8));
    split_send_side(SPLIT_SIDE_LE, data, len);
}

static void split_patch_features(uint16_t opcode, uint8_t *ev, size_t len)
{
    if (len < 7 || ev[6] != 0) {
        return;
    }
    if (opcode == HCI_READ_LOCAL_FEATURES && len >= 7 + 8) {
        ev[7 + HCI_FEATURE_LE_OFF] |= HCI_FEATURE_LE_MASK;
        ev[7 + HCI_FEATURE_SIMUL_OFF] |= HCI_FEATURE_SIMUL_MASK;
    } else if (opcode == HCI_READ_LOCAL_EXT_FEATURES && len >= 9 + 8) {
        uint8_t *features = ev + 9;
        if (ev[7] == 0) {
            s_split.br_max_page = ev[8];
            features[HCI_FEATURE_LE_OFF] |= HCI_FEATURE_LE_MASK;
            features[HCI_FEATURE_SIMUL_OFF] |= HCI_FEATURE_SIMUL_MASK;
        } else if (ev[7] == 1) {
            features[0] |= s_le_host_bits;
        }
        if (ev[8] < 1) {
            ev[8] = 1;
        }
    }
}

static uint8_t split_answer_status(const uint8_t *ev, size_t len)
{
    if (len < 4) {
        return HCI_STATUS_UNKNOWN_COMMAND;
    }
    if (ev[1] == HCI_COMMAND_STATUS) {
        return ev[3];
    }
    return len >= 7 ? ev[6] : 0;
}

static size_t split_merge_answers(uint8_t side, uint8_t *ev, size_t len, uint8_t **out)
{
    const bool ev_is_br = side == SPLIT_SIDE_BR;
    uint8_t *br = ev_is_br ? ev : s_split.answer;
    uint8_t *le = ev_is_br ? s_split.answer : ev;
    const size_t br_len = ev_is_br ? len : s_split.answer_len;
    const size_t le_len = ev_is_br ? s_split.answer_len : len;
    const uint8_t br_status = split_answer_status(br, br_len);
    const uint8_t le_status = split_answer_status(le, le_len);
    const bool use_le = br_len == 0 || (br_status == HCI_STATUS_UNKNOWN_COMMAND && le_len != 0);
    *out = use_le ? le : br;
    if (s_split.opcode == HCI_READ_LOCAL_COMMANDS && br_status == 0 && le_status == 0) {
        const size_t n = br_len < le_len ? br_len : le_len;
        for (size_t i = 7; i < n; i++) {
            (*out)[i] = (uint8_t) (br[i] | le[i]);
        }
    }
    return use_le ? le_len : br_len;
}

static bool split_address_step(uint8_t side, uint16_t opcode, const uint8_t *ev, size_t len, uint8_t **out,
                               size_t *out_len, uint8_t *own_side, size_t *own_len)
{
    const bool ok = len >= 7 && ev[1] == HCI_COMMAND_COMPLETE && ev[6] == 0;
    if (s_split.addr_stage == SPLIT_ADDR_VERSION && opcode == HCI_READ_LOCAL_VERSION && side == SPLIT_SIDE_BR) {
        s_split.broadcom = ok && len >= 13 && (uint16_t) (ev[11] | (ev[12] << 8)) == HCI_MANUFACTURER_BROADCOM;
        s_split.addr_stage = s_split.broadcom ? SPLIT_ADDR_READ_LE : SPLIT_ADDR_NONE;
        s_split.opcode = HCI_READ_BD_ADDR;
        s_split.waiting = SPLIT_SIDE_LE;
        *own_side = SPLIT_SIDE_LE;
        *own_len = split_own_command(HCI_READ_BD_ADDR, NULL, 0);
        *out_len = 0;
        return true;
    }
    if (s_split.addr_stage == SPLIT_ADDR_READ_LE && opcode == HCI_READ_BD_ADDR) {
        if (ok && len >= 13) {
            memcpy(s_split.addr_answer, ev, sizeof(s_split.addr_answer));
            s_split.addr_stage = SPLIT_ADDR_WRITE_BR;
            s_split.opcode = HCI_BCM_WRITE_BD_ADDR;
            *own_len = split_own_command(HCI_BCM_WRITE_BD_ADDR, ev + 7, 6);
        } else {
            s_split.addr_stage = SPLIT_ADDR_READ_BR;
            *own_len = split_own_command(HCI_READ_BD_ADDR, NULL, 0);
        }
        s_split.waiting = SPLIT_SIDE_BR;
        *own_side = SPLIT_SIDE_BR;
        *out_len = 0;
        return true;
    }
    if (s_split.addr_stage == SPLIT_ADDR_WRITE_BR && opcode == HCI_BCM_WRITE_BD_ADDR) {
        s_split.addr_answer[3] = 1;
        *out = s_split.addr_answer;
        *out_len = sizeof(s_split.addr_answer);
        s_split.addr_stage = SPLIT_ADDR_NONE;
        s_split.waiting = 0;
        s_split.opcode = 0;
        return true;
    }
    return false;
}

static void split_on_command_answer(uint8_t side, uint8_t *ev, size_t len)
{
    const bool complete = ev[1] == HCI_COMMAND_COMPLETE;
    if (len < (complete ? 6u : 7u)) {
        return;
    }
    const size_t credits_at = complete ? 3 : 4;
    const uint16_t opcode = (uint16_t) (ev[credits_at + 1] | (ev[credits_at + 2] << 8));
    if (opcode == 0) {
        return;
    }
    uint8_t *out = ev;
    size_t out_len = len;
    uint8_t own_side = 0;
    size_t own_len = 0;
    split_lock();
    if (s_split.opcode == opcode && (s_split.waiting & side) != 0 &&
        split_address_step(side, opcode, ev, len, &out, &out_len, &own_side, &own_len)) {
    } else if (s_split.opcode != opcode || (s_split.waiting & side) == 0) {
        out[credits_at] = s_split.waiting != 0 ? 0 : 1;
    } else if (s_split.both) {
        s_split.waiting = (uint8_t) (s_split.waiting & ~side);
        if (s_split.waiting != 0) {
            const size_t copy = len < SPLIT_EVENT_BUF ? len : SPLIT_EVENT_BUF;
            memcpy(s_split.answer, ev, copy);
            s_split.answer_len = copy;
            split_unlock();
            return;
        }
        out_len = split_merge_answers(side, ev, len > SPLIT_EVENT_BUF ? SPLIT_EVENT_BUF : len, &out);
        out[credits_at] = 1;
        s_split.opcode = 0;
    } else {
        out[credits_at] = 1;
        if (complete && side == SPLIT_SIDE_LE && s_split.translate && out_len >= 9) {
            out[7] = (uint8_t) (s_split.host_handle & 0xFF);
            out[8] = (uint8_t) (s_split.host_handle >> 8);
        }
        if (complete && side == SPLIT_SIDE_BR) {
            split_patch_features(opcode, out, out_len);
        }
        s_split.waiting = 0;
        s_split.opcode = 0;
        s_split.addr_stage = SPLIT_ADDR_NONE;
    }
    split_unlock();
    if (own_len != 0) {
        split_send_side(own_side, s_split.own, own_len);
        return;
    }
    if (out_len != 0) {
        split_deliver(out, out_len);
    }
}

static bool split_le_event_names_connection(const uint8_t *ev, size_t len)
{
    switch (ev[1]) {
    case HCI_DISCONNECTION_COMPLETE:
    case HCI_ENCRYPTION_CHANGE:
    case HCI_READ_REMOTE_VERSION_COMPLETE:
    case HCI_NUM_COMPLETED_PACKETS:
    case HCI_ENCRYPTION_KEY_REFRESH:
    case HCI_AUTH_PAYLOAD_TIMEOUT:
    case HCI_ENCRYPTION_CHANGE_V2:
        return true;
    case HCI_LE_META:
        if (len < 4) {
            return false;
        }
        switch (ev[3]) {
        case 0x01:
        case 0x03:
        case 0x04:
        case 0x05:
        case 0x06:
        case 0x07:
        case 0x0A:
        case 0x0C:
        case 0x12:
        case 0x14:
        case 0x23:
        case 0x29:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

static void split_renumber_le_event(uint8_t *ev, size_t len)
{
    uint8_t *p = ev + 3;
    const size_t plen = len - 3;
    switch (ev[1]) {
    case HCI_DISCONNECTION_COMPLETE:
        if (plen >= 3) {
            const uint16_t ctrl = (uint16_t) ((p[1] | (p[2] << 8)) & 0x0FFF);
            split_to_host(p + 1);
            split_link_down(ctrl);
        }
        return;
    case HCI_ENCRYPTION_CHANGE:
    case HCI_READ_REMOTE_VERSION_COMPLETE:
    case HCI_ENCRYPTION_KEY_REFRESH:
    case HCI_ENCRYPTION_CHANGE_V2:
        if (plen >= 3) {
            split_to_host(p + 1);
        }
        return;
    case HCI_AUTH_PAYLOAD_TIMEOUT:
        if (plen >= 2) {
            split_to_host(p);
        }
        return;
    case HCI_NUM_COMPLETED_PACKETS:
        for (size_t i = 0; plen >= 1 && i < p[0] && 1 + 4 * i + 2 <= plen; i++) {
            split_to_host(p + 1 + 4 * i);
        }
        return;
    default:
        break;
    }
    switch (p[0]) {
    case 0x01:
    case 0x0A:
    case 0x29:
        if (plen >= 4 && p[1] == 0) {
            split_link_up((uint16_t) ((p[2] | (p[3] << 8)) & 0x0FFF));
        }
        if (plen >= 4) {
            split_to_host(p + 2);
        }
        return;
    case 0x03:
    case 0x04:
    case 0x0C:
    case 0x23:
        if (plen >= 4) {
            split_to_host(p + 2);
        }
        return;
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x14:
        if (plen >= 3) {
            split_to_host(p + 1);
        }
        return;
    case 0x12:
        if (plen >= 5) {
            split_to_host(p + 3);
        }
        return;
    default:
        return;
    }
}

static void split_on_event(uint8_t side, uint8_t *ev, size_t len)
{
    if (len < 3) {
        return;
    }
    if (ev[1] == HCI_COMMAND_COMPLETE || ev[1] == HCI_COMMAND_STATUS) {
        split_on_command_answer(side, ev, len);
        return;
    }
    if (side == SPLIT_SIDE_LE && split_le_event_names_connection(ev, len)) {
        split_lock();
        split_renumber_le_event(ev, len);
        split_unlock();
    }
    split_deliver(ev, len);
}

static int split_le_recv(uint8_t *data, uint16_t len)
{
    if (data == NULL || len < 2) {
        return -1;
    }
    hci_trace(HCI_TRACE_LE_HOST, data, len);
    if (data[0] == H4_EVENT) {
        split_on_event(SPLIT_SIDE_LE, data, len);
        return 0;
    }
    if (data[0] == H4_ACL && len >= 5) {
        split_lock();
        split_to_host(data + 1);
        split_unlock();
    }
    split_deliver(data, len);
    return 0;
}

static void hci_send(uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (s_on_dongle && s_dongle_gone) {
        dongle_gone_answer(data, len);
        return;
    }
    if (!s_on_dongle) {
        if (data[0] == H4_COMMAND && len >= 4) {
            const uint16_t opcode = (uint16_t) (data[1] | (data[2] << 8));
            if (ble_stage_answers(opcode)) {
                ble_stage_answer(opcode, data + 4, (uint16_t) (len - 4));
                return;
            }
        }
        esp_vhci_host_send_packet(data, len);
        return;
    }
    if (len < 2) {
        return;
    }
    if (data[0] == H4_COMMAND) {
        split_send_command(data, len);
    } else if (data[0] == H4_ACL) {
        split_send_acl(data, len);
    } else {
        split_send_side(SPLIT_SIDE_BR, data, len);
    }
}

static bool hci_check_send_available(void)
{
    if (!s_on_dongle) {
        return esp_vhci_host_check_send_available();
    }
    if (s_dongle_gone) {
        return true;
    }
    const bool room = h4_room();
    taskENTER_CRITICAL(&s_pending_lock);
    const bool available = s_send_available && room;
    if (!available) {
        s_h4_refused = true;
    }
    taskEXIT_CRITICAL(&s_pending_lock);
    return available && esp_vhci_host_check_send_available();
}

static esp_err_t hci_register_host_callback(const esp_bluedroid_hci_driver_callbacks_t *callback)
{
    s_host_cb = callback;
    return ESP_OK;
}

static const esp_bluedroid_hci_driver_operations_t HCI_OPS = {
    .send = hci_send,
    .check_send_available = hci_check_send_available,
    .register_host_callback = hci_register_host_callback,
};

#define FOUND_CLASSES 16

static struct {
    uint8_t bda[6];
    uint32_t device_class;
    bool in_scan;
} s_found_classes[FOUND_CLASSES];
static uint8_t s_found_next = 0;
static portMUX_TYPE s_found_lock = portMUX_INITIALIZER_UNLOCKED;

static void found_class_set(const uint8_t *bda, uint32_t device_class)
{
    if (device_class == 0) {
        return;
    }
    taskENTER_CRITICAL(&s_found_lock);
    int slot = -1;
    for (int i = 0; i < FOUND_CLASSES; i++) {
        if (memcmp(s_found_classes[i].bda, bda, 6) == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        slot = s_found_next;
        s_found_next = (uint8_t) ((s_found_next + 1) % FOUND_CLASSES);
        memcpy(s_found_classes[slot].bda, bda, 6);
    }
    s_found_classes[slot].device_class = device_class;
    s_found_classes[slot].in_scan = true;
    taskEXIT_CRITICAL(&s_found_lock);
}

static void found_scan_start(void)
{
    taskENTER_CRITICAL(&s_found_lock);
    for (int i = 0; i < FOUND_CLASSES; i++) {
        s_found_classes[i].in_scan = false;
    }
    taskEXIT_CRITICAL(&s_found_lock);
}

static bool found_in_scan(const uint8_t *bda)
{
    bool found = false;
    taskENTER_CRITICAL(&s_found_lock);
    for (int i = 0; i < FOUND_CLASSES; i++) {
        if (s_found_classes[i].in_scan && memcmp(s_found_classes[i].bda, bda, 6) == 0) {
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_found_lock);
    return found;
}

static uint32_t found_class_get(const uint8_t *bda)
{
    uint32_t value = 0;
    taskENTER_CRITICAL(&s_found_lock);
    for (int i = 0; i < FOUND_CLASSES; i++) {
        if (memcmp(s_found_classes[i].bda, bda, 6) == 0) {
            value = s_found_classes[i].device_class;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_found_lock);
    return value;
}

static void send_found(const uint8_t *bda, uint8_t transport, uint8_t addr_type, const uint8_t *name, size_t name_len,
                       uint32_t cod, uint8_t kind)
{
    found_class_set(bda, cod);
    uint8_t buf[14 + NAME_MAX_LEN];
    if (name_len > NAME_MAX_LEN) {
        name_len = NAME_MAX_LEN;
    }
    memcpy(buf, bda, 6);
    buf[6] = transport;
    buf[7] = addr_type;
    buf[8] = (uint8_t) name_len;
    if (name_len > 0) {
        memcpy(buf + 9, name, name_len);
    }
    memcpy(buf + 9 + name_len, &cod, 4);
    buf[13 + name_len] = kind;
    message_send(XBT_MSG_FOUND, buf, 14 + name_len);
}

#define SEEN_MAX 64

typedef struct {
    uint8_t bda[6];
    uint32_t hash;
} seen_t;

static seen_t s_seen_list[SEEN_MAX];
static seen_t *s_seen = NULL;
static uint8_t s_seen_count = 0;

static void seen_free(void)
{
    s_seen = NULL;
    s_seen_count = 0;
}
static uint32_t s_broadcast_hash[XBT_RECORDS_MAX];
static volatile bool s_listening = false;
static volatile bool s_listen_stopping = false;

static uint32_t adv_payload_hash(const uint8_t *adv, uint16_t len, bool *has_payload)
{
    uint32_t hash = 2166136261UL;
    *has_payload = false;
    for (uint16_t at = 0; at + 1 < len;) {
        const uint8_t field = adv[at];
        if (field == 0 || at + 1 + field > len) {
            break;
        }
        const uint8_t type = adv[at + 1];
        if (type == ESP_BLE_AD_MANUFACTURER_SPECIFIC_TYPE || type == ESP_BLE_AD_TYPE_SERVICE_DATA ||
            type == ESP_BLE_AD_TYPE_32SERVICE_DATA || type == ESP_BLE_AD_TYPE_128SERVICE_DATA) {
            *has_payload = true;
            for (uint16_t i = at + 2; i < at + 1 + field; i++) {
                hash ^= adv[i];
                hash *= 16777619UL;
            }
        }
        at += 1 + field;
    }
    return hash;
}

static bool payload_changed(const uint8_t *bda, const uint8_t *adv, uint16_t len)
{
    bool has_payload = false;
    const uint32_t hash = adv_payload_hash(adv, len, &has_payload);
    if (!has_payload || s_seen == NULL) {
        return false;
    }
    for (int i = 0; i < s_seen_count; i++) {
        if (memcmp(s_seen[i].bda, bda, 6) == 0) {
            return s_seen[i].hash != hash;
        }
    }
    if (s_seen_count < SEEN_MAX) {
        memcpy(s_seen[s_seen_count].bda, bda, 6);
        s_seen[s_seen_count++].hash = hash;
    }
    return false;
}

static void close_waiter_wake(void);
static void log_forward_set(uint8_t level, bool trace);
static void hci_trace_dump_out(void);

static uint8_t s_auth_bda[6];
static char s_auth_name[NAME_MAX_LEN + 1];
static volatile bool s_classic_scanning = false;
static volatile bool s_ble_scanning = false;
static bool s_scanning_sent = false;

static void scan_state_send(void)
{
    const bool scanning = s_classic_scanning || s_ble_scanning;
    if (scanning == s_scanning_sent) {
        return;
    }
    s_scanning_sent = scanning;
    const uint8_t value = scanning ? 1 : 0;
    message_send(XBT_MSG_SCAN_STATE, &value, 1);
}

static void pair_event_send(const uint8_t *bda, uint8_t kind, uint32_t value)
{
    uint8_t msg[11];
    memcpy(msg, bda, 6);
    msg[6] = kind;
    memcpy(msg + 7, &value, 4);
    message_send(XBT_MSG_PAIR_EVENT, msg, sizeof(msg));
}

static bool unclaimed_pairing(const uint8_t *bda)
{
    if (s_pair_audio && memcmp(bda, s_pair_bda, sizeof(s_pair_bda)) == 0) {
        return false;
    }
    if (!s_classic_scanning) {
        return false;
    }
    memcpy(s_incoming_bda, bda, sizeof(s_incoming_bda));
    s_incoming_pair = true;
    return true;
}

static int record_index(const uint8_t *bda);

static bool pairing_claimed(const uint8_t *bda)
{
    if (s_pair_audio && memcmp(bda, s_pair_bda, sizeof(s_pair_bda)) == 0) {
        return true;
    }
    if (memcmp(bda, s_connect_bda, sizeof(s_connect_bda)) == 0 || record_index(bda) >= 0) {
        return true;
    }
    return s_classic_scanning || s_ble_scan_seconds > 0;
}

static void incoming_audio_connect(const uint8_t *bda)
{
    memcpy(s_pair_bda, bda, sizeof(s_pair_bda));
    s_pair_audio = true;
    esp_bt_gap_get_remote_services(s_pair_bda);
    esp_a2d_source_connect(s_pair_bda);
}

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_CFM_REQ_EVT) {
        if (!pairing_claimed(param->cfm_req.bda)) {
            esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, false);
            return;
        }
        unclaimed_pairing(param->cfm_req.bda);
        pair_event_send(param->cfm_req.bda, XBT_PAIR_CONFIRM, param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        return;
    }
    if (event == ESP_BT_GAP_KEY_NOTIF_EVT) {
        pair_event_send(param->key_notif.bda, XBT_PAIR_PASSKEY, param->key_notif.passkey);
        return;
    }
    if (event == ESP_BT_GAP_PIN_REQ_EVT) {
        if ((s_pair_audio && memcmp(param->pin_req.bda, s_pair_bda, sizeof(s_pair_bda)) == 0) ||
            unclaimed_pairing(param->pin_req.bda)) {
            esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
            esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
            return;
        }
        pair_event_send(param->pin_req.bda, XBT_PAIR_PIN, param->pin_req.min_16_digit ? 16 : 0);
        return;
    }
    if (event == ESP_BT_GAP_KEY_REQ_EVT) {
        pair_event_send(param->key_req.bda, XBT_PAIR_CODE, 0);
        return;
    }
    if (event == ESP_BT_GAP_AUTH_CMPL_EVT) {
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            memcpy(s_auth_bda, param->auth_cmpl.bda, sizeof(s_auth_bda));
            strncpy(s_auth_name, (const char *) param->auth_cmpl.device_name, NAME_MAX_LEN);
            s_auth_name[NAME_MAX_LEN] = '\0';
        }
        pair_event_send(param->auth_cmpl.bda,
                        param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS ? XBT_PAIR_DONE : XBT_PAIR_FAILED,
                        (uint32_t) param->auth_cmpl.stat);
        if (s_incoming_pair && memcmp(param->auth_cmpl.bda, s_incoming_bda, sizeof(s_incoming_bda)) == 0) {
            s_incoming_pair = false;
            if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                if (s_classic_scanning) {
                    s_incoming_connect = true;
                    esp_bt_gap_cancel_discovery();
                } else {
                    memcpy(s_pair_bda, s_incoming_bda, sizeof(s_pair_bda));
                    s_pair_audio = true;
                    esp_a2d_source_connect(s_pair_bda);
                }
            }
        }
        return;
    }
    if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        s_classic_scanning = param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
        scan_state_send();
        if (!s_classic_scanning) {
            close_waiter_wake();
            if (s_incoming_connect) {
                s_incoming_connect = false;
                incoming_audio_connect(s_incoming_bda);
            } else if (s_sighted_connect) {
                s_sighted_connect = false;
                esp_bt_gap_get_remote_services(s_pair_bda);
                esp_a2d_source_connect(s_pair_bda);
            } else if (s_sighting_wanted) {
                s_sighting_wanted = false;
                s_pair_audio = false;
                pair_event_send(s_pair_bda, XBT_PAIR_FAILED, (uint32_t) ESP_ERR_NOT_FOUND);
            }
        }
        return;
    }
    if (event != ESP_BT_GAP_DISC_RES_EVT) {
        return;
    }
    if (s_sighting_wanted && memcmp(param->disc_res.bda, s_pair_bda, sizeof(s_pair_bda)) == 0) {
        s_sighting_wanted = false;
        s_sighted_connect = true;
        esp_bt_gap_cancel_discovery();
    }
    const uint8_t *name = NULL;
    size_t name_len = 0;
    uint32_t cod = 0;
    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *prop = &param->disc_res.prop[i];
        if (prop->type == ESP_BT_GAP_DEV_PROP_COD && prop->len >= 4) {
            memcpy(&cod, prop->val, sizeof(cod));
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_BDNAME && prop->len > 0) {
            name = prop->val;
            name_len = strnlen((const char *) prop->val, (size_t) prop->len);
        } else if (prop->type == ESP_BT_GAP_DEV_PROP_EIR && name == NULL) {
            uint8_t len = 0;
            uint8_t *eir_name = esp_bt_gap_resolve_eir_data(prop->val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &len);
            if (eir_name != NULL) {
                name = eir_name;
                name_len = len;
            }
        }
    }
    if (cod == 0) {
        cod = found_class_get(param->disc_res.bda);
    }
    const uint32_t major = esp_bt_gap_get_cod_major_dev(cod);
    if (major != ESP_BT_COD_MAJOR_DEV_PERIPHERAL && major != ESP_BT_COD_MAJOR_DEV_AV) {
        return;
    }
    send_found(param->disc_res.bda, XBT_TRANSPORT_CLASSIC, 0, name, name_len, cod, XBT_FOUND_HID);
}

static uint16_t ble_appearance(uint8_t *adv, uint16_t len)
{
    uint8_t found_len = 0;
    uint8_t *appearance = esp_ble_resolve_adv_data_by_type(adv, len, ESP_BLE_AD_TYPE_APPEARANCE, &found_len);
    return appearance != NULL && found_len >= 2 ? (uint16_t) (appearance[0] | (appearance[1] << 8)) : 0;
}

static bool ble_is_hid(uint8_t *adv, uint16_t len)
{
    uint8_t found_len = 0;
    if ((ble_appearance(adv, len) & 0xFFC0) == 0x03C0) {
        return true;
    }
    const esp_ble_adv_data_type types[] = {ESP_BLE_AD_TYPE_16SRV_CMPL, ESP_BLE_AD_TYPE_16SRV_PART};
    for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
        uint8_t *uuids = esp_ble_resolve_adv_data_by_type(adv, len, types[t], &found_len);
        for (uint8_t i = 0; uuids != NULL && i + 1 < found_len; i += 2) {
            if ((uuids[i] | (uuids[i + 1] << 8)) == 0x1812) {
                return true;
            }
        }
    }
    return false;
}

static bool has_broadcast_records(void)
{
    for (int i = 0; i < s_record_count; i++) {
        if (s_records[i].kind & XBT_KIND_BROADCAST) {
            return true;
        }
    }
    return false;
}

static bool le_hid_missing(void);
static void le_candidate(const uint8_t *bda, uint8_t addr_type);
static bool le_request(const uint8_t *bda, uint8_t addr_type, bool manual);
static volatile bool s_le_connecting = false;
static volatile bool s_le_manual = false;
static portMUX_TYPE s_le_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_le_task = NULL;
static uint8_t s_le_bda[6];
static uint8_t s_le_addr_type = 0;

#define LE_BUSY_ROUNDS 40

#define LE_RETRY_SLOTS 8
#define LE_RETRY_MAX_SHIFT 6

static struct {
    uint8_t bda[6];
    uint8_t fails;
    int64_t retry_at;
} s_le_retry[LE_RETRY_SLOTS];
static portMUX_TYPE s_le_retry_lock = portMUX_INITIALIZER_UNLOCKED;

static bool le_retry_due(const uint8_t *bda)
{
    const int64_t now = esp_timer_get_time();
    bool due = true;
    taskENTER_CRITICAL(&s_le_retry_lock);
    for (int i = 0; i < LE_RETRY_SLOTS; i++) {
        if (s_le_retry[i].fails != 0 && memcmp(s_le_retry[i].bda, bda, 6) == 0) {
            due = now >= s_le_retry[i].retry_at;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_le_retry_lock);
    return due;
}

static void le_retry_failed(const uint8_t *bda)
{
    const int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_le_retry_lock);
    int slot = -1;
    int oldest = 0;
    for (int i = 0; i < LE_RETRY_SLOTS; i++) {
        if (s_le_retry[i].fails != 0 && memcmp(s_le_retry[i].bda, bda, 6) == 0) {
            slot = i;
            break;
        }
        if (slot < 0 && s_le_retry[i].fails == 0) {
            slot = i;
        }
        if (s_le_retry[i].retry_at < s_le_retry[oldest].retry_at) {
            oldest = i;
        }
    }
    if (slot < 0) {
        slot = oldest;
    }
    if (s_le_retry[slot].fails == 0 || memcmp(s_le_retry[slot].bda, bda, 6) != 0) {
        memcpy(s_le_retry[slot].bda, bda, 6);
        s_le_retry[slot].fails = 0;
    }
    if (s_le_retry[slot].fails < LE_RETRY_MAX_SHIFT) {
        s_le_retry[slot].fails++;
    }
    s_le_retry[slot].retry_at = now + (1000000LL << s_le_retry[slot].fails);
    taskEXIT_CRITICAL(&s_le_retry_lock);
}

static void le_retry_clear(const uint8_t *bda)
{
    taskENTER_CRITICAL(&s_le_retry_lock);
    for (int i = 0; i < LE_RETRY_SLOTS; i++) {
        if (s_le_retry[i].fails != 0 && memcmp(s_le_retry[i].bda, bda, 6) == 0) {
            s_le_retry[i].fails = 0;
            s_le_retry[i].retry_at = 0;
        }
    }
    taskEXIT_CRITICAL(&s_le_retry_lock);
}

static void listen_update(void)
{
    if (!s_ready || s_ble_scan_seconds > 0 || s_listen_stopping || s_le_connecting) {
        return;
    }
    const bool broadcast = has_broadcast_records();
    const bool want = broadcast || le_hid_missing();
    if (want == s_listening) {
        return;
    }
    s_listening = want;
    if (!want) {
        esp_ble_gap_stop_scanning();
        return;
    }
    esp_ble_scan_params_t params = {
        .scan_type = BLE_SCAN_TYPE_PASSIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = broadcast ? 0x50 : 0x200,
        .scan_window = 0x30,
        .scan_duplicate = broadcast ? BLE_SCAN_DUPLICATE_DISABLE : BLE_SCAN_DUPLICATE_ENABLE,
    };
    esp_ble_gap_set_scan_params(&params);
}

static void broadcast_forward(const uint8_t *bda, const uint8_t *adv, uint16_t len)
{
    for (int i = 0; i < s_record_count; i++) {
        if (!(s_records[i].kind & XBT_KIND_BROADCAST) || memcmp(s_records[i].address, bda, 6) != 0) {
            continue;
        }
        bool has_payload = false;
        const uint32_t hash = adv_payload_hash(adv, len, &has_payload);
        if (!has_payload || hash == s_broadcast_hash[i]) {
            return;
        }
        s_broadcast_hash[i] = hash;
        uint8_t buf[6 + ESP_BLE_ADV_DATA_LEN_MAX + ESP_BLE_SCAN_RSP_DATA_LEN_MAX];
        if (len > sizeof(buf) - 6) {
            len = (uint16_t) (sizeof(buf) - 6);
        }
        memcpy(buf, bda, 6);
        memcpy(buf + 6, adv, len);
        message_send(XBT_MSG_BROADCAST_DATA, buf, 6 + len);
        return;
    }
}

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        if (s_ble_scan_seconds > 0) {
            esp_ble_gap_start_scanning(s_ble_scan_seconds);
        } else if (s_listening) {
            esp_ble_gap_start_scanning(0);
        }
        return;
    }
    if (event == ESP_GAP_BLE_SEC_REQ_EVT) {
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr,
                                 pairing_claimed(param->ble_security.ble_req.bd_addr));
        return;
    }
    if (event == ESP_GAP_BLE_PASSKEY_REQ_EVT) {
        pair_event_send(param->ble_security.ble_req.bd_addr, XBT_PAIR_CODE_LE, 0);
        return;
    }
    if (event == ESP_GAP_BLE_PASSKEY_NOTIF_EVT) {
        pair_event_send(param->ble_security.key_notif.bd_addr, XBT_PAIR_PASSKEY,
                        param->ble_security.key_notif.passkey);
        return;
    }
    if (event == ESP_GAP_BLE_NC_REQ_EVT) {
        if (!pairing_claimed(param->ble_security.key_notif.bd_addr)) {
            esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, false);
            return;
        }
        pair_event_send(param->ble_security.key_notif.bd_addr, XBT_PAIR_CONFIRM,
                        param->ble_security.key_notif.passkey);
        esp_ble_confirm_reply(param->ble_security.key_notif.bd_addr, true);
        return;
    }
    if (event == ESP_GAP_BLE_AUTH_CMPL_EVT) {
        pair_event_send(param->ble_security.auth_cmpl.bd_addr,
                        param->ble_security.auth_cmpl.success ? XBT_PAIR_DONE : XBT_PAIR_FAILED,
                        param->ble_security.auth_cmpl.fail_reason);
        return;
    }
    if (event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
        if (s_ble_scan_seconds > 0) {
            s_ble_scanning = param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS;
            scan_state_send();
        }
        return;
    }
    if (event == ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT && s_listen_stopping) {
        s_listen_stopping = false;
        listen_update();
        return;
    }
    if (event == ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT ||
        (event == ESP_GAP_BLE_SCAN_RESULT_EVT && param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT)) {
        if (s_ble_scan_seconds > 0) {
            s_ble_scan_seconds = 0;
            s_ble_scanning = false;
            seen_free();
            scan_state_send();
            listen_update();
        }
        return;
    }
    if (event != ESP_GAP_BLE_SCAN_RESULT_EVT || param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
        return;
    }
    uint16_t len = (uint16_t) param->scan_rst.adv_data_len + param->scan_rst.scan_rsp_len;
    le_candidate(param->scan_rst.bda, (uint8_t) param->scan_rst.ble_addr_type);
    broadcast_forward(param->scan_rst.bda, param->scan_rst.ble_adv, len);
    if (s_ble_scan_seconds == 0) {
        return;
    }
    uint8_t kind = XBT_FOUND_HID;
    if (!ble_is_hid(param->scan_rst.ble_adv, len)) {
        if (!payload_changed(param->scan_rst.bda, param->scan_rst.ble_adv, len)) {
            return;
        }
        kind = XBT_FOUND_BROADCAST;
    }
    uint8_t name_len = 0;
    uint8_t *name = esp_ble_resolve_adv_data_by_type(param->scan_rst.ble_adv, len, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    send_found(param->scan_rst.bda, XBT_TRANSPORT_LE, (uint8_t) param->scan_rst.ble_addr_type, name, name_len,
               ble_appearance(param->scan_rst.ble_adv, len), kind);
}

#define HID_OPEN_MAX 8

static struct {
    uint8_t bda[6];
    esp_hidh_dev_t *dev;
} s_open[HID_OPEN_MAX];

static TaskHandle_t volatile s_close_waiter = NULL;

static void close_waiter_wake(void)
{
    TaskHandle_t waiter = s_close_waiter;
    if (waiter != NULL) {
        xTaskNotifyGive(waiter);
    }
}

static portMUX_TYPE s_open_lock = portMUX_INITIALIZER_UNLOCKED;

static void open_set(const uint8_t *bda, esp_hidh_dev_t *dev)
{
    int free_slot = -1;
    taskENTER_CRITICAL(&s_open_lock);
    for (int i = 0; i < HID_OPEN_MAX; i++) {
        if (s_open[i].dev != NULL && memcmp(s_open[i].bda, bda, 6) == 0) {
            s_open[i].dev = dev;
            taskEXIT_CRITICAL(&s_open_lock);
            return;
        }
        if (s_open[i].dev == NULL && free_slot < 0) {
            free_slot = i;
        }
    }
    if (dev != NULL && free_slot >= 0) {
        memcpy(s_open[free_slot].bda, bda, 6);
        s_open[free_slot].dev = dev;
    }
    taskEXIT_CRITICAL(&s_open_lock);
}

#define HID_LINKS_MAX (CONFIG_BT_ACL_CONNECTIONS - 2)

static bool hid_link_free(const uint8_t *bda);

static esp_hidh_dev_t *open_get(const uint8_t *bda)
{
    esp_hidh_dev_t *dev = NULL;
    taskENTER_CRITICAL(&s_open_lock);
    for (int i = 0; i < HID_OPEN_MAX; i++) {
        if (s_open[i].dev != NULL && memcmp(s_open[i].bda, bda, 6) == 0) {
            dev = s_open[i].dev;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_open_lock);
    return dev;
}

static void listen_again(void);

static bool open_bda(const esp_hidh_dev_t *dev, uint8_t *bda)
{
    bool found = false;
    taskENTER_CRITICAL(&s_open_lock);
    for (int i = 0; i < HID_OPEN_MAX && dev != NULL; i++) {
        if (s_open[i].dev == dev) {
            memcpy(bda, s_open[i].bda, 6);
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&s_open_lock);
    return found;
}

static void hidh_cb(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *) event_data;
    pending_frame_t *frame = NULL;
    uint8_t *buf = NULL;
    switch ((esp_hidh_event_t) id) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.dev == NULL) {
            break;
        }
        if (param->open.status != ESP_OK) {
            const uint8_t *failed = esp_hidh_dev_bda_get(param->open.dev);
            pair_event_send(failed != NULL ? failed : s_connect_bda, XBT_PAIR_FAILED, (uint32_t) param->open.status);
            listen_again();
            break;
        }
        const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
        const char *name = esp_hidh_dev_name_get(param->open.dev);
        if (bda != NULL) {
            open_set(bda, param->open.dev);
        }
        size_t name_len = name != NULL ? strnlen(name, NAME_MAX_LEN) : 0;
        size_t num_maps = 0;
        esp_hid_raw_report_map_t *maps = NULL;
        esp_hidh_dev_report_maps_get(param->open.dev, &num_maps, &maps);
        uint16_t map_len = (num_maps > 0 && maps != NULL) ? maps[0].len : 0;
        if (bda == NULL || 6 + 1 + name_len + 2 + map_len + 5 > MESSAGE_MAX) {
            ESP_LOGW(TAG, "HID device opened, report map too large to forward");
            break;
        }
        buf = message_new(XBT_MSG_HID_OPEN, 6 + 1 + name_len + 2 + map_len + 5, &frame);
        if (buf == NULL) {
            break;
        }
        size_t pos = 0;
        memcpy(buf, bda, 6);
        pos = 6;
        buf[pos++] = (uint8_t) name_len;
        if (name_len > 0) {
            memcpy(buf + pos, name, name_len);
            pos += name_len;
        }
        buf[pos++] = (uint8_t) (map_len & 0xFF);
        buf[pos++] = (uint8_t) (map_len >> 8);
        if (map_len > 0) {
            memcpy(buf + pos, maps[0].data, map_len);
            pos += map_len;
        }
        const uint8_t transport = esp_hidh_dev_transport_get(param->open.dev) == ESP_HID_TRANSPORT_BLE
                                      ? XBT_TRANSPORT_LE
                                      : XBT_TRANSPORT_CLASSIC;
        const uint16_t vid = esp_hidh_dev_vendor_id_get(param->open.dev);
        const uint16_t pid = esp_hidh_dev_product_id_get(param->open.dev);
        buf[pos++] = (uint8_t) (vid & 0xFF);
        buf[pos++] = (uint8_t) (vid >> 8);
        buf[pos++] = (uint8_t) (pid & 0xFF);
        buf[pos++] = (uint8_t) (pid >> 8);
        buf[pos++] = transport;
        message_push(frame, pos);
        const uint8_t addr_type = memcmp(bda, s_connect_bda, 6) == 0 ? s_connect_addr_type : 0;
        queue_upsert(bda, transport, addr_type, XBT_KIND_HID, name, name_len);
        listen_again();
        break;
    }
    case ESP_HIDH_INPUT_EVENT: {
        uint8_t bda[6];
        if (param->input.length + 8 > MESSAGE_MAX || !input_room() || !open_bda(param->input.dev, bda)) {
            break;
        }
        buf = message_new(XBT_MSG_HID_INPUT, 8 + param->input.length, &frame);
        if (buf == NULL) {
            break;
        }
        memcpy(buf, bda, 6);
        buf[6] = (uint8_t) param->input.report_id;
        buf[7] = param->input.map_index;
        memcpy(buf + 8, param->input.data, param->input.length);
        message_push(frame, 8 + param->input.length);
        break;
    }
    case ESP_HIDH_FEATURE_EVENT: {
        uint8_t bda[6];
        const size_t length = param->feature.status == ESP_OK && param->feature.data != NULL ? param->feature.length : 0;
        if (6 + length > MESSAGE_MAX || !open_bda(param->feature.dev, bda)) {
            break;
        }
        buf = message_new(XBT_MSG_HID_FEATURE, 6 + length, &frame);
        if (buf == NULL) {
            break;
        }
        memcpy(buf, bda, 6);
        if (length > 0) {
            memcpy(buf + 6, param->feature.data, length);
        }
        message_push(frame, 6 + length);
        break;
    }
    case ESP_HIDH_CLOSE_EVENT: {
        uint8_t bda[6];
        if (open_bda(param->close.dev, bda)) {
            ESP_LOGI(TAG, "HID %02X:%02X:%02X:%02X:%02X:%02X closed: reason 0x%x", bda[0], bda[1], bda[2], bda[3],
                     bda[4], bda[5], (unsigned) param->close.reason);
            open_set(bda, NULL);
            message_send(XBT_MSG_HID_CLOSE, bda, 6);
        }
        esp_hidh_dev_free(param->close.dev);
        close_waiter_wake();
        listen_again();
        break;
    }
    default:
        break;
    }
}

static void start_scan(uint8_t seconds)
{
    if (seconds == 0) {
        seconds = 10;
    }
    uint32_t inquiry = ((uint32_t) seconds * 100 + 127) / 128;
    if (inquiry < 1) {
        inquiry = 1;
    }
    if (inquiry > 0x30) {
        inquiry = 0x30;
    }
    found_scan_start();
    if (s_on_dongle) {
        esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, (uint8_t) inquiry, 0);
    }
    s_seen_count = 0;
    s_ble_scan_seconds = seconds;
    s_seen = s_seen_list;
    if (s_listening) {
        s_listening = false;
        s_listen_stopping = true;
        esp_ble_gap_stop_scanning();
    }
    esp_ble_scan_params_t params = {
        .scan_type = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval = 0x50,
        .scan_window = 0x30,
        .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE,
    };
    esp_ble_gap_set_scan_params(&params);
}

typedef struct {
    uint8_t bda[6];
    uint8_t handle;
    bool used;
} classic_link_t;

static classic_link_t s_classic[HID_OPEN_MAX];

static classic_link_t *classic_by_bda(const uint8_t *bda)
{
    for (int i = 0; i < HID_OPEN_MAX; i++) {
        if (s_classic[i].used && memcmp(s_classic[i].bda, bda, 6) == 0) {
            return &s_classic[i];
        }
    }
    return NULL;
}

static classic_link_t *classic_by_handle(uint8_t handle)
{
    for (int i = 0; i < HID_OPEN_MAX; i++) {
        if (s_classic[i].used && s_classic[i].handle == handle) {
            return &s_classic[i];
        }
    }
    return NULL;
}

static void classic_opened(const uint8_t *bda, uint8_t handle)
{
    classic_link_t *link = classic_by_bda(bda);
    for (int i = 0; i < HID_OPEN_MAX && link == NULL; i++) {
        if (!s_classic[i].used) {
            link = &s_classic[i];
        }
    }
    if (link != NULL) {
        memcpy(link->bda, bda, 6);
        link->handle = handle;
        link->used = true;
    }
}

static void classic_closed(uint8_t handle)
{
    classic_link_t *link = classic_by_handle(handle);
    if (link != NULL) {
        link->used = false;
        message_send(XBT_MSG_HID_CLOSE, link->bda, 6);
    }
    close_waiter_wake();
}

static void classic_descriptor(const esp_hidh_cb_param_t *param)
{
    const classic_link_t *link = classic_by_handle(param->dscp.handle);
    if (link == NULL || param->dscp.status != ESP_HIDH_OK || param->dscp.dsc_list == NULL) {
        return;
    }
    const bool named = memcmp(s_auth_bda, link->bda, 6) == 0;
    const size_t name_len = named ? strnlen(s_auth_name, NAME_MAX_LEN) : 0;
    const uint16_t map_len = (uint16_t) param->dscp.dl_len;
    if (6 + 1 + name_len + 2 + map_len + 5 > MESSAGE_MAX) {
        ESP_LOGW(TAG, "HID device opened, report map too large to forward");
        return;
    }
    pending_frame_t *frame = NULL;
    uint8_t *buf = message_new(XBT_MSG_HID_OPEN, 6 + 1 + name_len + 2 + map_len + 5, &frame);
    if (buf == NULL) {
        return;
    }
    size_t pos = 6;
    memcpy(buf, link->bda, 6);
    buf[pos++] = (uint8_t) name_len;
    memcpy(buf + pos, s_auth_name, name_len);
    pos += name_len;
    buf[pos++] = (uint8_t) (map_len & 0xFF);
    buf[pos++] = (uint8_t) (map_len >> 8);
    memcpy(buf + pos, param->dscp.dsc_list, map_len);
    pos += map_len;
    buf[pos++] = (uint8_t) (param->dscp.vendor_id & 0xFF);
    buf[pos++] = (uint8_t) (param->dscp.vendor_id >> 8);
    buf[pos++] = (uint8_t) (param->dscp.product_id & 0xFF);
    buf[pos++] = (uint8_t) (param->dscp.product_id >> 8);
    buf[pos++] = XBT_TRANSPORT_CLASSIC;
    message_push(frame, pos);
    queue_upsert(link->bda, XBT_TRANSPORT_CLASSIC, 0, XBT_KIND_HID, named ? s_auth_name : NULL, name_len);
}

static void classic_report(uint32_t msg_id, uint8_t handle, const uint8_t *data, size_t len)
{
    const classic_link_t *link = classic_by_handle(handle);
    if (link == NULL || 6 + len > MESSAGE_MAX || (msg_id == XBT_MSG_HID_INPUT_RAW && !input_room())) {
        return;
    }
    pending_frame_t *frame = NULL;
    uint8_t *buf = message_new(msg_id, 6 + len, &frame);
    if (buf == NULL) {
        return;
    }
    memcpy(buf, link->bda, 6);
    if (len > 0) {
        memcpy(buf + 6, data, len);
    }
    message_push(frame, 6 + len);
}

static void hh_cb(esp_hidh_cb_event_t event, esp_hidh_cb_param_t *param)
{
    switch (event) {
    case ESP_HIDH_DEINIT_EVT:
        close_waiter_wake();
        break;
    case ESP_HIDH_OPEN_EVT:
        if (param->open.status == ESP_HIDH_OK && param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTING) {
            break;
        }
        if (param->open.status == ESP_HIDH_OK && param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED &&
            param->open.handle != 0xFF && !hid_link_free(param->open.bd_addr)) {
            ESP_LOGW(TAG, "Classic HID refused: %d HID links in use", HID_LINKS_MAX);
            esp_bt_hid_host_disconnect(param->open.bd_addr);
            pair_event_send(param->open.bd_addr, XBT_PAIR_FAILED, (uint32_t) ESP_ERR_NO_MEM);
        } else if (param->open.status == ESP_HIDH_OK && param->open.conn_status == ESP_HIDH_CONN_STATE_CONNECTED &&
            param->open.handle != 0xFF) {
            classic_opened(param->open.bd_addr, param->open.handle);
        } else {
            ESP_LOGW(TAG, "Classic HID open failed: status %d, state %d", param->open.status, param->open.conn_status);
            pair_event_send(param->open.bd_addr, XBT_PAIR_FAILED, (uint32_t) param->open.status);
        }
        break;
    case ESP_HIDH_CLOSE_EVT:
        ESP_LOGW(TAG, "Classic HID closed: reason %d", param->close.reason);
        classic_closed(param->close.handle);
        break;
    case ESP_HIDH_VC_UNPLUG_EVT:
        classic_closed(param->unplug.handle);
        break;
    case ESP_HIDH_GET_DSCP_EVT:
        classic_descriptor(param);
        break;
    case ESP_HIDH_DATA_IND_EVT:
        if (param->data_ind.status == ESP_HIDH_OK && param->data_ind.data != NULL && param->data_ind.len > 0) {
            classic_report(XBT_MSG_HID_INPUT_RAW, param->data_ind.handle, param->data_ind.data, param->data_ind.len);
        }
        break;
    case ESP_HIDH_GET_RPT_EVT:
        classic_report(XBT_MSG_HID_FEATURE, param->get_rpt.handle, param->get_rpt.data,
                       param->get_rpt.status == ESP_HIDH_OK && param->get_rpt.data != NULL ? param->get_rpt.len : 0);
        break;
    default:
        break;
    }
}

static void hid_report_command(uint32_t msg_id, uint8_t *data, size_t len)
{
    if (len < 7) {
        return;
    }
    if (classic_by_bda(data) != NULL) {
        if (msg_id == XBT_MSG_HID_OUTPUT) {
            esp_bt_hid_host_send_data(data, data + 6, len - 6);
        } else if (msg_id == XBT_MSG_HID_SET_REPORT && len > 7) {
            esp_bt_hid_host_set_report(data, (esp_hidh_report_type_t) data[6], data + 7, len - 7);
        } else if (msg_id == XBT_MSG_HID_GET_FEATURE && len >= 9) {
            esp_bt_hid_host_get_report(data, ESP_HIDH_REPORT_TYPE_FEATURE, data[6], data[7] | (data[8] << 8));
        }
        return;
    }
    esp_hidh_dev_t *dev = open_get(data);
    if (dev != NULL && msg_id == XBT_MSG_HID_OUTPUT && len > 7) {
        esp_hidh_dev_output_set(dev, 0, data[6], data + 7, len - 7);
    }
}

static void scan_stop_for_connect(void)
{
    s_close_waiter = xTaskGetCurrentTaskHandle();
    if (s_on_dongle && s_classic_scanning) {
        esp_bt_gap_cancel_discovery();
    }
    if (s_ble_scan_seconds > 0) {
        esp_ble_gap_stop_scanning();
    }
    for (int round = 0; round < 5 && s_on_dongle && s_classic_scanning; round++) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    s_close_waiter = NULL;
}

static bool hid_link_free(const uint8_t *bda)
{
    int used = 0;
    taskENTER_CRITICAL(&s_open_lock);
    for (int i = 0; i < HID_OPEN_MAX; i++) {
        if (s_open[i].dev != NULL && memcmp(s_open[i].bda, bda, 6) != 0) {
            used++;
        }
        if (s_classic[i].used && memcmp(s_classic[i].bda, bda, 6) != 0) {
            used++;
        }
    }
    taskEXIT_CRITICAL(&s_open_lock);
    return used < HID_LINKS_MAX;
}

static void connect_device(const uint8_t *data, uint8_t len)
{
    if (len < 8) {
        return;
    }
    if (!hid_link_free(data)) {
        ESP_LOGW(TAG, "HID device refused: %d HID links in use", HID_LINKS_MAX);
        pair_event_send(data, XBT_PAIR_FAILED, (uint32_t) ESP_ERR_NO_MEM);
        return;
    }
    scan_stop_for_connect();
    esp_bd_addr_t bda;
    memcpy(bda, data, 6);
    memcpy(s_connect_bda, data, 6);
    s_connect_addr_type = data[7];
    if (data[6] != XBT_TRANSPORT_LE) {
        if (found_in_scan(bda)) {
            esp_bt_gap_remove_bond_device(bda);
        }
        if (esp_bt_hid_host_connect(bda) != ESP_OK) {
            pair_event_send(bda, XBT_PAIR_FAILED, (uint32_t) ESP_FAIL);
        }
        return;
    }
    s_close_waiter = xTaskGetCurrentTaskHandle();
    bool asked = le_request(bda, data[7], true);
    for (int round = 0; round < LE_BUSY_ROUNDS && !asked; round++) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        asked = le_request(bda, data[7], true);
    }
    s_close_waiter = NULL;
    if (!asked) {
        pair_event_send(bda, XBT_PAIR_FAILED, (uint32_t) ESP_ERR_TIMEOUT);
    }
}

static volatile bool s_avrc_deinit_done = false;

static void avrc_ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    if (event == ESP_AVRC_CT_PROF_STATE_EVT && param->avrc_ct_init_stat.state == ESP_AVRC_DEINIT_SUCCESS) {
        s_avrc_deinit_done = true;
        close_waiter_wake();
    }
}

static int record_index(const uint8_t *bda);

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param)
{
    if (event == ESP_GATTC_CONNECT_EVT && param != NULL) {
        const int i = record_index(param->connect.remote_bda);
        if (i >= 0 && (s_records[i].kind & XBT_KIND_HID) && s_records[i].transport == XBT_TRANSPORT_LE) {
            esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
        }
    }
    esp_hidh_gattc_event_handler(event, gattc_if, param);
}

static bool start_stack(bool dongle)
{
    esp_err_t err = esp_bluedroid_attach_hci_driver(&HCI_OPS);
    if (err == ESP_OK) {
        err = esp_bluedroid_init();
    }
    heap_mark(HEAP_BD_INIT);
    if (err == ESP_OK) {
        err = esp_bluedroid_enable();
    }
    heap_mark(HEAP_BLUEDROID);
    if (err == ESP_OK && dongle) {
        err = esp_bt_gap_register_callback(bt_gap_cb);
    }
    if (err == ESP_OK && dongle) {
        err = esp_bt_gap_set_device_name("ESP32-P4 Panel");
    }
    if (err == ESP_OK) {
        err = esp_ble_gap_register_callback(ble_gap_cb);
    }
    if (err == ESP_OK && dongle) {
        err = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    }
    if (err == ESP_OK) {
        if (dongle) {
            esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
            esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap));
            esp_bt_pin_code_t no_pin = {0};
            esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_VARIABLE, 0, no_pin);
        }
        esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
        esp_ble_io_cap_t ble_iocap = ESP_IO_CAP_OUT;
        uint8_t key_size = 16;
        uint8_t keys = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &ble_iocap, sizeof(ble_iocap));
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &keys, sizeof(keys));
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &keys, sizeof(keys));
    }
    if (err == ESP_OK && dongle) {
        esp_avrc_ct_init();
        esp_avrc_ct_register_callback(avrc_ct_cb);
        err = esp_a2d_source_init();
    }
    if (err == ESP_OK && dongle) {
        err = esp_a2d_register_callback(a2d_cb);
    }
    if (err == ESP_OK && dongle) {
        err = esp_a2d_source_register_data_callback(a2d_data_cb);
    }
    if (err == ESP_OK) {
        err = esp_ble_gattc_register_callback(gattc_cb);
    }
    if (err == ESP_OK) {
        esp_hidh_config_t config = {
            .callback = hidh_cb,
            .event_stack_size = 4096,
            .callback_arg = NULL,
        };
        err = esp_hidh_init(&config);
    }
    if (err == ESP_OK && dongle) {
        err = esp_bt_hid_host_register_callback(hh_cb);
    }
    if (err == ESP_OK && dongle) {
        err = esp_bt_hid_host_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth host start failed: %s", esp_err_to_name(err));
        message_send(XBT_MSG_ERROR, (const uint8_t *) &err, sizeof(err));
        return false;
    }
    heap_mark(HEAP_PROFILES);
    return true;
}

#define STOP_ROUNDS 40

static volatile bool s_stopping = false;

static void stop_stack(void)
{
    s_stopping = true;
    s_close_waiter = xTaskGetCurrentTaskHandle();
    for (int round = 0; round < STOP_ROUNDS && s_le_connecting; round++) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    for (int round = 0; round < STOP_ROUNDS; round++) {
        for (int i = 0; i < HID_OPEN_MAX; i++) {
            if (s_open[i].dev != NULL) {
                esp_hidh_dev_close(s_open[i].dev);
            }
        }
        bool classic_open = false;
        for (int i = 0; i < HID_OPEN_MAX; i++) {
            if (s_classic[i].used) {
                classic_open = true;
                esp_bt_hid_host_disconnect(s_classic[i].bda);
            }
        }
        if (!classic_open && esp_hidh_deinit() != ESP_ERR_INVALID_STATE) {
            break;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    if (s_on_dongle) {
        esp_bt_hid_host_deinit();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    }
    memset(s_classic, 0, sizeof(s_classic));
    s_close_waiter = NULL;
    s_le_connecting = false;
    s_listen_stopping = false;
    if (s_on_dongle) {
        s_close_waiter = xTaskGetCurrentTaskHandle();
        s_avrc_deinit_done = false;
        if (esp_avrc_ct_deinit() == ESP_OK) {
            for (int round = 0; round < 5 && !s_avrc_deinit_done; round++) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            }
        }
        s_a2d_deinit_done = false;
        if (esp_a2d_source_deinit() == ESP_OK) {
            for (int round = 0; round < 5 && !s_a2d_deinit_done; round++) {
                ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            }
        }
        s_close_waiter = NULL;
    }
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    s_stopping = false;
}

static bool c6_controller_up(void)
{
    heap_mark(HEAP_CTRL_BEFORE);
    esp_err_t err = ESP_OK;
    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        err = esp_bt_controller_init(&cfg);
    }
    if (err == ESP_OK && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_INITED) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
        if (err == ESP_OK) {
            err = esp_vhci_host_register_callback(&VHCI_CB);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 Bluetooth controller not started: %s", esp_err_to_name(err));
        message_send(XBT_MSG_ERROR, (const uint8_t *) &err, sizeof(err));
        return false;
    }
    heap_mark(HEAP_CTRL);
    return true;
}

static bool start_on_c6(void)
{
    return c6_controller_up() && start_stack(false);
}

static bool switch_to_dongle(void)
{
    if (s_ready) {
        stop_stack();
    }
    s_ready = false;
    taskENTER_CRITICAL(&s_pending_lock);
    s_send_available = true;
    taskEXIT_CRITICAL(&s_pending_lock);
    if (!split_begin() || !c6_controller_up()) {
        return false;
    }
    s_on_dongle = true;
    return start_stack(true);
}

static bool return_to_c6(void)
{
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }
    if (s_ready) {
        stop_stack();
    }
    s_ready = false;
    if (s_reconnect_timer != NULL) {
        esp_timer_stop(s_reconnect_timer);
    }
    audio_close();
    s_audio_connected = false;
    s_audio_streaming = false;
    s_audio_paging = false;
    audio_state_send();
    s_on_dongle = false;
    s_dongle_gone = false;
    return start_on_c6();
}

static bool report_varint(uint8_t *rec, size_t *pos, uint32_t value)
{
    uint8_t tmp[5];
    size_t n = 0;
    do {
        tmp[n] = (uint8_t) (value & 0x7F);
        value >>= 7;
        if (value != 0) {
            tmp[n] |= 0x80;
        }
        n++;
    } while (value != 0);
    if (*pos + n > MESSAGE_MAX) {
        return false;
    }
    memcpy(rec + *pos, tmp, n);
    *pos += n;
    return true;
}

static bool report_name(uint8_t *rec, size_t *pos, const char *name)
{
    const size_t len = name != NULL ? strnlen(name, 32) : 0;
    if (*pos + 1 + len + 5 > MESSAGE_MAX) {
        return false;
    }
    rec[(*pos)++] = (uint8_t) len;
    if (len > 0) {
        memcpy(rec + *pos, name, len);
        *pos += len;
    }
    return true;
}

static void status_tasks(void)
{
    uint8_t *rec = (uint8_t *) malloc(MESSAGE_MAX);
    if (rec == NULL) {
        return;
    }
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    const UBaseType_t count = uxTaskGetNumberOfTasks() + 2;
    TaskStatus_t *tasks = (TaskStatus_t *) malloc(sizeof(TaskStatus_t) * count);
    if (tasks == NULL) {
        free(rec);
        return;
    }
    const UBaseType_t got = uxTaskGetSystemState(tasks, count, NULL);
    {
        size_t pos = 2;
        uint8_t listed = 0;
        rec[0] = XBT_REPORT_STACKS;
        for (UBaseType_t i = 0; i < got && listed < 255; i++) {
            if (!report_name(rec, &pos, tasks[i].pcTaskName) || !report_varint(rec, &pos, tasks[i].usStackHighWaterMark)) {
                break;
            }
            listed++;
        }
        rec[1] = listed;
        message_send(XBT_MSG_REPORT, rec, pos);
    }
#if CONFIG_HEAP_TASK_TRACKING
    const size_t max_totals = 32;
    heap_task_totals_t *totals = (heap_task_totals_t *) calloc(max_totals, sizeof(heap_task_totals_t));
    if (totals != NULL) {
        size_t num_totals = 0;
        heap_task_info_params_t params = {0};
        params.caps[0] = MALLOC_CAP_INTERNAL;
        params.mask[0] = MALLOC_CAP_INTERNAL;
        params.totals = totals;
        params.num_totals = &num_totals;
        params.max_totals = max_totals;
        heap_caps_get_per_task_info(&params);
        {
            uint8_t *orec = rec;
            size_t pos = 2;
            uint8_t listed = 0;
            orec[0] = XBT_REPORT_HEAP_OWNERS;
            for (size_t i = 0; i < num_totals && listed < 255; i++) {
                const char *name = "gone";
                if (totals[i].task == NULL) {
                    name = "boot";
                }
                for (UBaseType_t t = 0; t < got; t++) {
                    if (tasks[t].xHandle == totals[i].task) {
                        name = tasks[t].pcTaskName;
                        break;
                    }
                }
                if (!report_name(orec, &pos, name) || !report_varint(orec, &pos, totals[i].size[0]) ||
                    !report_varint(orec, &pos, totals[i].count[0])) {
                    break;
                }
                listed++;
            }
            orec[1] = listed;
            message_send(XBT_MSG_REPORT, orec, pos);
        }
        const size_t max_blocks = 300;
        heap_task_block_t *blocks = (heap_task_block_t *) calloc(max_blocks, sizeof(heap_task_block_t));
        if (blocks != NULL) {
            for (size_t i = 0; i < num_totals; i++) {
                if (totals[i].size[0] < 8192) {
                    continue;
                }
                const char *name = totals[i].task == NULL ? "boot" : "gone";
                for (UBaseType_t t = 0; t < got; t++) {
                    if (tasks[t].xHandle == totals[i].task) {
                        name = tasks[t].pcTaskName;
                        break;
                    }
                }
                TaskHandle_t owner = totals[i].task;
                size_t no_totals = 0;
                heap_task_info_params_t one = {0};
                one.num_totals = &no_totals;
                one.caps[0] = MALLOC_CAP_INTERNAL;
                one.mask[0] = MALLOC_CAP_INTERNAL;
                one.tasks = &owner;
                one.num_tasks = 1;
                one.blocks = blocks;
                one.max_blocks = max_blocks;
                const size_t got_blocks = heap_caps_get_per_task_info(&one);
                uint32_t small_count = 0;
                uint32_t small_bytes = 0;
                for (size_t b = 0; b < got_blocks; b++) {
                    if (blocks[b].size < 256) {
                        small_count++;
                        small_bytes += blocks[b].size;
                    }
                }
                uint8_t *brec = rec;
                size_t pos = 1;
                brec[0] = XBT_REPORT_BLOCKS;
                report_name(brec, &pos, name);
                report_varint(brec, &pos, totals[i].size[0]);
                report_varint(brec, &pos, (uint32_t) got_blocks);
                report_varint(brec, &pos, small_count);
                report_varint(brec, &pos, small_bytes);
                for (size_t b = 0; b < got_blocks; b++) {
                    if (blocks[b].size >= 256 && !report_varint(brec, &pos, blocks[b].size)) {
                        break;
                    }
                }
                message_send(XBT_MSG_REPORT, brec, pos);
            }
            free(blocks);
        }
        free(totals);
    }
#endif
    free(tasks);
#endif
    {
        uint8_t *mrec = rec;
        size_t pos = 1;
        mrec[0] = XBT_REPORT_MARKS;
        report_varint(mrec, &pos, s_rx_dropped);
        report_varint(mrec, &pos, s_h4_dropped);
        report_varint(mrec, &pos, (uint32_t) heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
        mrec[pos++] = HEAP_MARKS;
        for (int i = 0; i < HEAP_MARKS; i++) {
            if (!report_name(mrec, &pos, HEAP_MARK_NAMES[i]) || !report_varint(mrec, &pos, s_heap_marks[i])) {
                break;
            }
        }
        message_send(XBT_MSG_REPORT, mrec, pos);
    }
    {
        uint8_t *prec = rec;
        size_t pos = 2;
        uint8_t listed = 0;
        prec[0] = XBT_REPORT_PARTITIONS;
        const esp_partition_t *running = esp_ota_get_running_partition();
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it != NULL && listed < 255) {
            const esp_partition_t *part = esp_partition_get(it);
            if (!report_name(prec, &pos, part->label) || pos + 3 + 10 > MESSAGE_MAX) {
                break;
            }
            prec[pos++] = (uint8_t) part->type;
            prec[pos++] = (uint8_t) part->subtype;
            prec[pos++] = running != NULL && running->address == part->address ? 1 : 0;
            report_varint(prec, &pos, part->address);
            report_varint(prec, &pos, part->size);
            listed++;
            it = esp_partition_next(it);
        }
        if (it != NULL) {
            esp_partition_iterator_release(it);
        }
        prec[1] = listed;
        message_send(XBT_MSG_REPORT, prec, pos);
    }
    free(rec);
}

static void send_status(void)
{
    uint32_t audio_free = 0;
    RingbufHandle_t ring = audio_take();
    if (ring != NULL) {
        audio_free = (uint32_t) xRingbufferGetCurFreeSize(ring);
        audio_give();
    }
    xbt_status_t status = {
        .uptime_ms = (uint32_t) (esp_timer_get_time() / 1000),
        .heap_free = (uint32_t) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        .heap_min = (uint32_t) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        .heap_largest = (uint32_t) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        .tasks = (uint32_t) uxTaskGetNumberOfTasks(),
        .worker_stack_free = (uint32_t) uxTaskGetStackHighWaterMark(NULL),
        .audio_free = audio_free,
        .a2d_calls = s_a2d_calls,
        .a2d_max_us = s_a2d_max_us,
        .command_max_us = s_command_max_us,
        .frames_in = s_frames_in,
        .frames_out = s_frames_out,
        .send_max_us = s_send_max_us,
        .a2d_underruns = s_a2d_underruns,
        .reset_reason = (uint32_t) esp_reset_reason(),
        .state = (s_safe ? XBT_STATE_SAFE : 0) | (s_on_dongle ? XBT_STATE_DONGLE : 0) |
                 (s_ready ? XBT_STATE_READY : 0) | (s_dongle_requested ? XBT_STATE_REQUESTED : 0),
        .pending = (s_pending_h4 << 16) | (s_pending_messages & 0xFFFF),
        .crashed_starts = c6_flash_starts(),
    };
    s_frames_in = 0;
    s_frames_out = 0;
    s_send_max_us = 0;
    s_a2d_underruns = 0;
    s_a2d_calls = 0;
    s_a2d_max_us = 0;
    s_command_max_us = 0;
    const uint32_t fields[XBT_STATUS_FIELDS] = {
        status.uptime_ms,     status.heap_free,     status.heap_min,      status.heap_largest,   status.tasks,
        status.worker_stack_free, status.audio_free, status.a2d_calls,    status.a2d_max_us,     status.command_max_us,
        status.frames_in,     status.frames_out,    status.send_max_us,   status.a2d_underruns,  status.reset_reason,
        status.state,         status.pending,       status.crashed_starts, s_log_level,
    };
    uint8_t packed[XBT_STATUS_FIELDS * 5];
    size_t pos = 0;
    for (size_t i = 0; i < XBT_STATUS_FIELDS; i++) {
        uint32_t value = fields[i];
        do {
            packed[pos] = (uint8_t) (value & 0x7F);
            value >>= 7;
            if (value != 0) {
                packed[pos] |= 0x80;
            }
            pos++;
        } while (value != 0);
    }
    message_send(XBT_MSG_STATUS_PACKED, packed, pos);
}

#define INTERNAL_UPSERT 0x425450F0u

static void queue_upsert(const uint8_t *bda, uint8_t transport, uint8_t addr_type, uint8_t kind, const char *name,
                         size_t name_len)
{
    if (s_commands == NULL || bda == NULL) {
        return;
    }
    xbt_record_t *rec = (xbt_record_t *) calloc(1, sizeof(xbt_record_t));
    if (rec == NULL) {
        return;
    }
    memcpy(rec->address, bda, 6);
    rec->transport = transport;
    rec->addr_type = addr_type;
    rec->kind = kind;
    rec->device_class = found_class_get(bda);
    rec->name_len = (uint8_t) (name_len < XBT_RECORD_NAME ? name_len : XBT_RECORD_NAME);
    if (name != NULL && rec->name_len > 0) {
        memcpy(rec->name, name, rec->name_len);
    }
    command_t command = {.msg_id = INTERNAL_UPSERT, .big = (uint8_t *) rec, .big_len = sizeof(*rec)};
    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        free(rec);
    }
}

static void records_send(void)
{
    uint8_t buf[1 + sizeof(s_records)];
    buf[0] = s_record_count;
    memcpy(buf + 1, s_records, (size_t) s_record_count * sizeof(xbt_record_t));
    message_send(XBT_MSG_RECORDS, buf, 1 + (size_t) s_record_count * sizeof(xbt_record_t));
}

static void records_save(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_SPACES[0], NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    nvs_set_blob(handle, "records", s_records, (size_t) s_record_count * sizeof(xbt_record_t));
    nvs_commit(handle);
    nvs_close(handle);
}

static void records_load(void)
{
    nvs_handle_t handle;
    s_record_count = 0;
    if (nvs_open(NVS_SPACES[0], NVS_READONLY, &handle) != ESP_OK) {
        return;
    }
    size_t size = sizeof(s_records);
    if (nvs_get_blob(handle, "records", s_records, &size) == ESP_OK) {
        s_record_count = (uint8_t) (size / sizeof(xbt_record_t));
    }
    nvs_close(handle);
}

static int record_index(const uint8_t *bda)
{
    for (int i = 0; i < s_record_count; i++) {
        if (memcmp(s_records[i].address, bda, 6) == 0) {
            return i;
        }
    }
    return -1;
}

static void records_upsert(const xbt_record_t *rec)
{
    int i = record_index(rec->address);
    if (i < 0) {
        if (s_record_count >= XBT_RECORDS_MAX) {
            memmove(&s_records[0], &s_records[1], (XBT_RECORDS_MAX - 1) * sizeof(xbt_record_t));
            s_record_count--;
        }
        i = s_record_count++;
        memset(&s_records[i], 0, sizeof(xbt_record_t));
        memcpy(s_records[i].address, rec->address, 6);
    }
    xbt_record_t *dst = &s_records[i];
    const xbt_record_t before = *dst;
    dst->kind |= rec->kind;
    if (rec->kind & (XBT_KIND_HID | XBT_KIND_BROADCAST)) {
        dst->transport = rec->transport;
        dst->addr_type = rec->addr_type;
    }
    if (rec->name_len > 0) {
        dst->name_len = rec->name_len;
        memset(dst->name, 0, sizeof(dst->name));
        memcpy(dst->name, rec->name, rec->name_len);
    }
    if (rec->device_class != 0) {
        dst->device_class = rec->device_class;
    }
    if (memcmp(&before, dst, sizeof(before)) != 0) {
        records_save();
        records_send();
    }
}

static void records_forget(const uint8_t *bda)
{
    esp_bt_gap_remove_bond_device((uint8_t *) bda);
    esp_ble_remove_bond_device((uint8_t *) bda);
    int i = record_index(bda);
    if (i >= 0) {
        memmove(&s_records[i], &s_records[i + 1], (size_t) (s_record_count - i - 1) * sizeof(xbt_record_t));
        s_record_count--;
        memset(s_broadcast_hash, 0, sizeof(s_broadcast_hash));
        records_save();
    }
    records_send();
}

static void nvs_done(uint8_t op, esp_err_t err)
{
    uint8_t msg[5] = {op, 0, 0, 0, 0};
    const int32_t value = (int32_t) err;
    memcpy(msg + 1, &value, 4);
    answer_send(XBT_MSG_NVS_DONE, msg, sizeof(msg));
}

static bool nvs_key(const uint8_t *data, size_t len, size_t at, char *key, size_t *next)
{
    if (len < at + 1 || data[at] == 0 || data[at] >= NVS_KEY_NAME_MAX_SIZE || len < at + 1 + data[at]) {
        return false;
    }
    memcpy(key, data + at + 1, data[at]);
    key[data[at]] = '\0';
    *next = at + 1 + data[at];
    return true;
}

static void nvs_list(uint8_t space)
{
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, NVS_SPACES[space], NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        uint8_t msg[3 + NVS_KEY_NAME_MAX_SIZE];
        const size_t key_len = strnlen(info.key, NVS_KEY_NAME_MAX_SIZE - 1);
        msg[0] = space;
        msg[1] = (uint8_t) info.type;
        msg[2] = (uint8_t) key_len;
        memcpy(msg + 3, info.key, key_len);
        err = answer_send(XBT_MSG_NVS_ENTRY, msg, 3 + key_len);
        if (err == ESP_OK) {
            err = nvs_entry_next(&it);
        }
    }
    nvs_release_iterator(it);
    nvs_done(XBT_FRAME_TYPE(XBT_MSG_NVS_LIST), err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err);
}

static void nvs_get(const uint8_t *data, size_t len)
{
    const uint8_t op = XBT_FRAME_TYPE(XBT_MSG_NVS_GET);
    char key[NVS_KEY_NAME_MAX_SIZE];
    size_t at = 0;
    if (len < 1 || data[0] >= 2 || !nvs_key(data, len, 1, key, &at)) {
        nvs_done(op, ESP_ERR_INVALID_ARG);
        return;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_SPACES[data[0]], NVS_READONLY, &handle);
    const bool opened = err == ESP_OK;
    nvs_type_t type = NVS_TYPE_ANY;
    if (err == ESP_OK) {
        err = nvs_find_key(handle, key, &type);
    }
    const bool bytes = type == NVS_TYPE_BLOB || type == NVS_TYPE_STR;
    const size_t head = 3 + strlen(key) + 2;
    size_t size = bytes ? (s_host_big ? XBT_NVS_BLOB_MAX : NVS_VALUE_MAX) : 8;
    pending_frame_t *slot = NULL;
    uint8_t *msg = NULL;
    if (err == ESP_OK) {
        msg = frame_new_wait(XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(XBT_MSG_NVS_VALUE), head + size, &slot,
                             pdMS_TO_TICKS(ANSWER_WAIT_MS));
        err = msg != NULL ? ESP_OK : ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        uint8_t *value = msg + head;
        if (bytes) {
            err = type == NVS_TYPE_BLOB ? nvs_get_blob(handle, key, value, &size)
                                        : nvs_get_str(handle, key, (char *) value, &size);
        } else {
            uint64_t number = 0;
            if (type == NVS_TYPE_U64) {
                err = nvs_get_u64(handle, key, &number);
            } else {
                if (type == NVS_TYPE_U8) {
                    uint8_t v = 0;
                    err = nvs_get_u8(handle, key, &v);
                    number = v;
                } else if (type == NVS_TYPE_I8) {
                    int8_t v = 0;
                    err = nvs_get_i8(handle, key, &v);
                    number = (uint64_t) (int64_t) v;
                } else if (type == NVS_TYPE_U16) {
                    uint16_t v = 0;
                    err = nvs_get_u16(handle, key, &v);
                    number = v;
                } else if (type == NVS_TYPE_I16) {
                    int16_t v = 0;
                    err = nvs_get_i16(handle, key, &v);
                    number = (uint64_t) (int64_t) v;
                } else if (type == NVS_TYPE_U32) {
                    uint32_t v = 0;
                    err = nvs_get_u32(handle, key, &v);
                    number = v;
                } else if (type == NVS_TYPE_I32) {
                    int32_t v = 0;
                    err = nvs_get_i32(handle, key, &v);
                    number = (uint64_t) (int64_t) v;
                } else if (type == NVS_TYPE_I64) {
                    int64_t v = 0;
                    err = nvs_get_i64(handle, key, &v);
                    number = (uint64_t) v;
                }
            }
            memcpy(value, &number, 8);
        }
    }
    if (opened) {
        nvs_close(handle);
    }
    if (msg == NULL) {
        nvs_done(op, err);
        return;
    }
    if (err != ESP_OK) {
        const int32_t code = (int32_t) err;
        slot[TX_ITEM_HEAD] = XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(XBT_MSG_NVS_DONE);
        msg[0] = op;
        memcpy(msg + 1, &code, 4);
        frame_push(slot, 5);
        return;
    }
    msg[0] = data[0];
    msg[1] = (uint8_t) type;
    msg[2] = (uint8_t) (head - 5);
    memcpy(msg + 3, key, head - 5);
    msg[head - 2] = (uint8_t) (size & 0xFF);
    msg[head - 1] = (uint8_t) (size >> 8);
    frame_push(slot, head + size);
}

static void nvs_change(uint8_t op, const uint8_t *data, size_t len)
{
    char key[NVS_KEY_NAME_MAX_SIZE] = {0};
    size_t at = 1;
    const bool all = op == XBT_FRAME_TYPE(XBT_MSG_NVS_ERASE) && len == 2 && data[1] == 0;
    if (len < 2 || data[0] >= 2 || (!all && !nvs_key(data, len, 1, key, &at))) {
        nvs_done(op, ESP_ERR_INVALID_ARG);
        return;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_SPACES[data[0]], NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        nvs_done(op, err);
        return;
    }
    if (op == XBT_FRAME_TYPE(XBT_MSG_NVS_ERASE)) {
        err = all ? nvs_erase_all(handle) : nvs_erase_key(handle, key);
    } else if (len < at + 1) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        const nvs_type_t type = (nvs_type_t) data[at];
        const uint8_t *value = data + at + 1;
        const size_t value_len = len - at - 1;
        uint64_t number = 0;
        if (type != NVS_TYPE_BLOB && type != NVS_TYPE_STR) {
            memcpy(&number, value, value_len < 8 ? value_len : 8);
        }
        if (type == NVS_TYPE_BLOB) {
            err = nvs_set_blob(handle, key, value, value_len);
        } else if (type == NVS_TYPE_STR) {
            char *text = (char *) calloc(1, value_len + 1);
            err = text != NULL ? ESP_OK : ESP_ERR_NO_MEM;
            if (text != NULL) {
                memcpy(text, value, value_len);
                err = nvs_set_str(handle, key, text);
                free(text);
            }
        } else if (type == NVS_TYPE_U8) {
            err = nvs_set_u8(handle, key, (uint8_t) number);
        } else if (type == NVS_TYPE_I8) {
            err = nvs_set_i8(handle, key, (int8_t) number);
        } else if (type == NVS_TYPE_U16) {
            err = nvs_set_u16(handle, key, (uint16_t) number);
        } else if (type == NVS_TYPE_I16) {
            err = nvs_set_i16(handle, key, (int16_t) number);
        } else if (type == NVS_TYPE_U32) {
            err = nvs_set_u32(handle, key, (uint32_t) number);
        } else if (type == NVS_TYPE_I32) {
            err = nvs_set_i32(handle, key, (int32_t) number);
        } else if (type == NVS_TYPE_U64) {
            err = nvs_set_u64(handle, key, number);
        } else if (type == NVS_TYPE_I64) {
            err = nvs_set_i64(handle, key, (int64_t) number);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err == ESP_OK && data[0] == 0) {
        records_load();
        records_send();
    }
    nvs_done(op, err);
}

#define INTERNAL_DONGLE 0x425450F2u
#define INTERNAL_DONGLE_GONE 0x425450F3u
#define INTERNAL_LISTEN 0x425450F5u
#define INTERNAL_TRACE_DUMP 0x425450F6u
#define INTERNAL_RADIO 0x425450F7u
#define INTERNAL_CRASH 0x425450F8u

static const char *crash_reason_name(uint8_t reason)
{
    switch (reason) {
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_BROWNOUT:
        return "brownout";
    default:
        return "unknown";
    }
}

#define CRASH_STACK_WORDS 54
#define CRASH_STACK_PER_LINE 18
#define CRASH_FLASH_TEXT 0x42

static void crash_report(void)
{
    const uint8_t starts = c6_flash_starts();
    if (starts > 0) {
        char head[72];
        const int n = snprintf(head, sizeof(head), "restarted after a %s, crashed starts %u",
                               crash_reason_name(c6_flash_crash_reason()), (unsigned) starts);
        if (n > 0) {
            message_send(XBT_MSG_LOG, (const uint8_t *) head,
                         (size_t) n < sizeof(head) ? (size_t) n : sizeof(head) - 1);
        }
    }
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }
    esp_core_dump_summary_t *summary = (esp_core_dump_summary_t *) malloc(sizeof(esp_core_dump_summary_t));
    if (summary != NULL && esp_core_dump_get_summary(summary) == ESP_OK) {
        char line[176];
        const int n = snprintf(line, sizeof(line),
                               "crash before this start: task %.16s pc 0x%08lx ra 0x%08lx sp 0x%08lx mcause 0x%lx "
                               "mtval 0x%08lx elf %.8s",
                               summary->exc_task, (unsigned long) summary->exc_pc, (unsigned long) summary->ex_info.ra,
                               (unsigned long) summary->ex_info.sp, (unsigned long) summary->ex_info.mcause,
                               (unsigned long) summary->ex_info.mtval, (const char *) summary->app_elf_sha256);
        if (n > 0) {
            message_send(XBT_MSG_LOG, (const uint8_t *) line,
                         (size_t) n < sizeof(line) ? (size_t) n : sizeof(line) - 1);
        }
        const uint32_t size = summary->exc_bt_info.dump_size < sizeof(summary->exc_bt_info.stackdump)
                                  ? summary->exc_bt_info.dump_size
                                  : sizeof(summary->exc_bt_info.stackdump);
        size_t used = 0;
        uint8_t listed = 0;
        for (uint32_t at = 0; at + 4 <= size && listed < CRASH_STACK_WORDS; at += 4) {
            uint32_t word;
            memcpy(&word, summary->exc_bt_info.stackdump + at, sizeof(word));
            if ((word >> 24) != CRASH_FLASH_TEXT) {
                continue;
            }
            if (used == 0) {
                used = (size_t) snprintf(line, sizeof(line), "crash stack:");
            }
            used += (size_t) snprintf(line + used, sizeof(line) - used, " %08lx", (unsigned long) word);
            listed++;
            if (listed % CRASH_STACK_PER_LINE == 0) {
                message_send(XBT_MSG_LOG, (const uint8_t *) line, used);
                used = 0;
            }
        }
        if (used > 0) {
            message_send(XBT_MSG_LOG, (const uint8_t *) line, used);
        }
    }
    free(summary);
    esp_core_dump_image_erase();
}

static bool s_stack_decided = false;

static bool le_hid_missing(void)
{
    for (int i = 0; i < s_record_count; i++) {
        if ((s_records[i].kind & XBT_KIND_HID) && s_records[i].transport == XBT_TRANSPORT_LE &&
            open_get(s_records[i].address) == NULL) {
            return true;
        }
    }
    return false;
}

#define LE_OPEN_STACK 3072

static StackType_t s_le_stack[LE_OPEN_STACK];
static StaticTask_t s_le_tcb;

static bool le_request(const uint8_t *bda, uint8_t addr_type, bool manual)
{
    bool claimed = false;
    taskENTER_CRITICAL(&s_le_lock);
    if (!s_le_connecting && s_le_task != NULL) {
        memcpy(s_le_bda, bda, 6);
        s_le_addr_type = addr_type;
        s_le_manual = manual;
        s_le_connecting = true;
        claimed = true;
    }
    taskEXIT_CRITICAL(&s_le_lock);
    if (claimed) {
        xTaskNotifyGive(s_le_task);
    }
    return claimed;
}

static void le_candidate(const uint8_t *bda, uint8_t addr_type)
{
    if (s_le_connecting || s_stopping || !s_ready || s_ble_scan_seconds > 0 || s_le_task == NULL) {
        return;
    }
    const int i = record_index(bda);
    if (i < 0 || !(s_records[i].kind & XBT_KIND_HID) || s_records[i].transport != XBT_TRANSPORT_LE ||
        open_get(bda) != NULL || !le_retry_due(bda)) {
        return;
    }
    le_request(bda, addr_type, false);
}

static void listen_again(void)
{
    command_t command = {.msg_id = INTERNAL_LISTEN};
    if (s_commands != NULL) {
        xQueueSend(s_commands, &command, 0);
    }
}

static void le_open(uint8_t *bda, uint8_t addr_type, bool manual)
{
    if (!hid_link_free(bda)) {
        if (manual) {
            pair_event_send(bda, XBT_PAIR_FAILED, (uint32_t) ESP_ERR_NO_MEM);
        } else {
            le_retry_failed(bda);
        }
        return;
    }
    if (s_listening) {
        s_listening = false;
        s_listen_stopping = true;
        esp_ble_gap_stop_scanning();
    }
    if (esp_hidh_dev_open(bda, ESP_HID_TRANSPORT_BLE, addr_type) != NULL) {
        le_retry_clear(bda);
    } else if (manual) {
        pair_event_send(bda, XBT_PAIR_FAILED, (uint32_t) ESP_FAIL);
    } else {
        le_retry_failed(bda);
    }
}

static void le_task(void *arg)
{
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        uint8_t bda[6];
        memcpy(bda, s_le_bda, 6);
        const bool manual = s_le_manual;
        if (s_ready && !s_stopping) {
            le_open(bda, s_le_addr_type, manual);
        }
        s_le_connecting = false;
        close_waiter_wake();
        listen_again();
    }
}

static void stack_up(void)
{
    s_ready = true;
    s_listening = false;
    records_send();
    if (s_on_dongle) {
        for (int i = 0; i < s_record_count; i++) {
            if (s_records[i].kind & XBT_KIND_AUDIO) {
                memcpy(s_audio_bda, s_records[i].address, 6);
                s_audio_paging = true;
                reconnect_fire(NULL);
                break;
            }
        }
    }
    listen_update();
    heap_mark(HEAP_UP);
}

static void worker_task(void *arg)
{
    records_load();
    if (s_le_task == NULL) {
        s_le_task = xTaskCreateStatic(le_task, "xbt_le", LE_OPEN_STACK, NULL, WORKER_PRIORITY, s_le_stack, &s_le_tcb);
    }
    const esp_timer_create_args_t timer_args = {.callback = reconnect_fire, .name = "xbt_a2dp"};
    if (s_reconnect_timer == NULL) {
        esp_timer_create(&timer_args, &s_reconnect_timer);
    }
    command_t command;
    for (;;) {
        if (xQueueReceive(s_commands, &command, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (command.msg_id == INTERNAL_RADIO) {
            if (!s_ready && !s_on_dongle && start_on_c6()) {
                stack_up();
            }
            continue;
        }
        if (command.msg_id == INTERNAL_DONGLE) {
            if (switch_to_dongle()) {
                stack_up();
            }
            continue;
        }
        if (command.msg_id == INTERNAL_DONGLE_GONE) {
            if (return_to_c6()) {
                stack_up();
            }
            continue;
        }
        if (command.msg_id == INTERNAL_CRASH) {
            crash_report();
            continue;
        }
        if (!s_ready) {
            free(command.big);
            continue;
        }
        if (command.msg_id == XBT_MSG_RECORDS) {
            records_send();
            continue;
        }
        if (command.msg_id == INTERNAL_LISTEN) {
            listen_update();
            continue;
        }
        if (command.msg_id == INTERNAL_TRACE_DUMP) {
            if (s_hci_trace != NULL) {
                hci_trace_dump_out();
            } else {
                s_hci_trace_dump = false;
            }
            continue;
        }
        if (command.msg_id == INTERNAL_UPSERT) {
            if (command.big != NULL) {
                records_upsert((const xbt_record_t *) command.big);
            }
            free(command.big);
            listen_update();
            continue;
        }
        if (command.msg_id == XBT_MSG_FORGET && command.len >= 6) {
            records_forget(command.data);
            listen_update();
            continue;
        }
        if (command.msg_id == XBT_MSG_CANCEL_SCAN) {
            if (s_on_dongle) {
                esp_bt_gap_cancel_discovery();
            }
            if (s_ble_scan_seconds > 0) {
                esp_ble_gap_stop_scanning();
            }
            continue;
        }
        if (command.msg_id == XBT_MSG_BROADCAST_ADD) {
            const uint8_t *data = command.big != NULL ? command.big : command.data;
            const size_t len = command.big != NULL ? command.big_len : command.len;
            if (len >= 8 && len >= 8u + data[7]) {
                queue_upsert(data, XBT_TRANSPORT_LE, data[6], XBT_KIND_BROADCAST, (const char *) data + 8,
                             data[7]);
            }
            free(command.big);
            continue;
        }
        if (command.msg_id == XBT_MSG_HID_DISCONNECT && command.len >= 6) {
            if (classic_by_bda(command.data) != NULL) {
                esp_bt_hid_host_disconnect(command.data);
            }
            esp_hidh_dev_t *dev = open_get(command.data);
            if (dev != NULL) {
                esp_hidh_dev_close(dev);
            }
            continue;
        }
        if (command.msg_id == XBT_MSG_AUDIO_PAUSE && command.len >= 1) {
            s_audio_paused = command.data[0] != 0;
            if (s_audio_paused && s_reconnect_timer != NULL) {
                esp_timer_stop(s_reconnect_timer);
            }
            s_audio_paging = !s_audio_paused && !s_audio_connected && record_index(s_audio_bda) >= 0;
            if (s_audio_paging) {
                reconnect_schedule();
            }
            audio_state_send();
            continue;
        }
        if (command.msg_id == XBT_MSG_AUDIO_INTERVAL && command.len >= 2) {
            const uint16_t seconds = (uint16_t) (command.data[0] | (command.data[1] << 8));
            s_audio_interval_s = seconds > 0 ? seconds : 1;
            continue;
        }
        if (command.msg_id == XBT_MSG_AUDIO_RECONNECT) {
            if (!s_audio_paused && !s_audio_connected && record_index(s_audio_bda) >= 0) {
                esp_a2d_source_connect(s_audio_bda);
                reconnect_schedule();
            }
            continue;
        }
        if (command.msg_id == XBT_MSG_NVS_LIST && command.len >= 1 && command.data[0] < 2) {
            nvs_list(command.data[0]);
            continue;
        }
        if (command.msg_id == XBT_MSG_NVS_RELOAD) {
            records_load();
            if (s_ready && s_on_dongle) {
                stop_stack();
                s_ready = false;
                if (start_stack(true)) {
                    stack_up();
                }
            } else {
                records_send();
            }
            continue;
        }
        if (command.msg_id == XBT_MSG_NVS_GET || command.msg_id == XBT_MSG_NVS_SET ||
            command.msg_id == XBT_MSG_NVS_ERASE) {
            const uint8_t *data = command.big != NULL ? command.big : command.data;
            const size_t len = command.big != NULL ? command.big_len : command.len;
            if (command.msg_id == XBT_MSG_NVS_GET) {
                nvs_get(data, len);
            } else {
                nvs_change(XBT_FRAME_TYPE(command.msg_id), data, len);
            }
            free(command.big);
            continue;
        }
        if (command.msg_id == XBT_MSG_STATUS) {
            send_status();
            if (command.len > 0 && command.data[0] != 0) {
                status_tasks();
            }
            continue;
        }
        const int64_t start = esp_timer_get_time();
        if (command.msg_id == XBT_MSG_SCAN) {
            start_scan(command.len > 0 ? command.data[0] : 10);
        } else if (command.msg_id == XBT_MSG_CONNECT) {
            connect_device(command.data, command.len);
        } else if (command.msg_id == XBT_MSG_AUDIO_CONNECT && command.len >= 6 && s_audio_connected &&
                   memcmp(s_audio_bda, command.data, 6) != 0) {
            pair_event_send(command.data, XBT_PAIR_FAILED, (uint32_t) ESP_ERR_INVALID_STATE);
        } else if (command.msg_id == XBT_MSG_AUDIO_CONNECT && command.len >= 6) {
            memcpy(s_pair_bda, command.data, sizeof(s_pair_bda));
            s_pair_audio = true;
            s_sighting_wanted = true;
            if (s_ble_scan_seconds > 0) {
                esp_ble_gap_stop_scanning();
            }
            if (!s_classic_scanning) {
                esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
            }
        } else if (command.msg_id == XBT_MSG_AUDIO_DISCONNECT && command.len >= 6) {
            s_sighting_wanted = false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 2, 0)
            esp_a2d_source_disconnect_by_handle(s_audio_hdl);
#else
            esp_a2d_source_disconnect(command.data);
#endif
        } else if (command.msg_id == XBT_MSG_AUDIO_VOLUME && command.len >= 1) {
            s_volume = command.data[0] > 127 ? 127 : command.data[0];
        }
        const uint32_t took = (uint32_t) (esp_timer_get_time() - start);
        if (took > s_command_max_us) {
            s_command_max_us = took;
        }
    }
}

static void on_start(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    const bool plugged = data == NULL || len == 0 || data[0] != 0;
    if (s_safe || s_commands == NULL || (s_stack_decided && plugged == s_dongle_requested)) {
        return;
    }
    if (plugged && data != NULL && len > 1) {
        const size_t name_len = len - 1 < sizeof(s_bt_name) - 1 ? len - 1 : sizeof(s_bt_name) - 1;
        memcpy(s_bt_name, data + 1, name_len);
        s_bt_name[name_len] = '\0';
    }
    if (!s_stack_decided) {
        command_t first = {.msg_id = plugged ? INTERNAL_DONGLE : INTERNAL_RADIO};
        if (xQueueSend(s_commands, &first, 0) == pdTRUE) {
            s_stack_decided = true;
            s_dongle_requested = plugged;
        }
        return;
    }
    command_t command = {.msg_id = plugged ? INTERNAL_DONGLE : INTERNAL_DONGLE_GONE};
    if (xQueueSend(s_commands, &command, 0) == pdTRUE) {
        s_dongle_requested = plugged;
        if (!plugged) {
            s_dongle_gone = true;
        }
    }
}

static void on_host_ready(void *arg, esp_event_base_t base, int32_t id, void *event_data)
{
    if (!s_started) {
        s_started = true;
        if (xTaskCreate(worker_task, "xbt_bt", WORKER_STACK, NULL, WORKER_PRIORITY, NULL) != pdPASS) {
            ESP_LOGE(TAG, "No task for the Bluetooth host");
            s_started = false;
        }
    }
    s_host_big = false;
    s_credit_absolute = false;
    credit_reset();
    const uint8_t hello[3] = {XBT_HELLO_BATCH | XBT_HELLO_CREDIT_SYNC | XBT_HELLO_AUDIO_WINDOW |
                                  XBT_HELLO_BIG_FRAMES,
                              (uint8_t) (RX_GRANT & 0xFF),
                              (uint8_t) (RX_GRANT >> 8)};
    message_send(XBT_MSG_HELLO, hello, sizeof(hello));
}

static void pair_reply(const uint8_t *data, size_t len)
{
    if (data == NULL || len < 9 || data[8] > ESP_BT_PIN_CODE_LEN || len < 9u + data[8]) {
        return;
    }
    esp_bd_addr_t bda;
    memcpy(bda, data, sizeof(bda));
    const uint8_t kind = data[6];
    const bool accept = data[7] != 0;
    const uint8_t count = data[8];
    if (kind == XBT_PAIR_PIN) {
        esp_bt_pin_code_t pin = {0};
        memcpy(pin, data + 9, count);
        esp_bt_gap_pin_reply(bda, accept && count > 0, count, pin);
        return;
    }
    char text[ESP_BT_PIN_CODE_LEN + 1];
    memcpy(text, data + 9, count);
    text[count] = '\0';
    const uint32_t code = (uint32_t) strtoul(text, NULL, 10);
    if (kind == XBT_PAIR_CODE) {
        esp_bt_gap_ssp_passkey_reply(bda, accept, code);
    } else if (kind == XBT_PAIR_CODE_LE) {
        esp_ble_passkey_reply(bda, accept, code);
    }
}

static void on_command(uint8_t type, const uint8_t *data, size_t len)
{
    if ((XBT_MSG_TYPE_BASE | type) == XBT_MSG_PAIR_REPLY) {
        pair_reply(data, len);
        return;
    }
    if ((XBT_MSG_TYPE_BASE | type) == XBT_MSG_LOG_SET) {
        if (data != NULL && len >= 1) {
            log_forward_set(data[0], len >= 2 && data[1] != 0);
        }
        return;
    }
    if ((XBT_MSG_TYPE_BASE | type) == XBT_MSG_HID_OUTPUT ||
        (XBT_MSG_TYPE_BASE | type) == XBT_MSG_HID_SET_REPORT ||
        (XBT_MSG_TYPE_BASE | type) == XBT_MSG_HID_GET_FEATURE) {
        if (s_ready && data != NULL) {
            hid_report_command(XBT_MSG_TYPE_BASE | type, (uint8_t *) data, len);
        }
        return;
    }
    if (!s_ready && (XBT_MSG_TYPE_BASE | type) == XBT_MSG_STATUS) {
        send_status();
        return;
    }
    if (!s_ready && (XBT_MSG_TYPE_BASE | type) == XBT_MSG_RECORDS) {
        return;
    }
    if (!s_ready || s_commands == NULL) {
        const esp_err_t err = s_safe ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
        message_send(XBT_MSG_ERROR, (const uint8_t *) &err, sizeof(err));
        return;
    }
    command_t command = {.msg_id = XBT_MSG_TYPE_BASE | type, .len = 0};
    if (data != NULL && len > sizeof(command.data) && len <= UINT16_MAX) {
        command.big = (uint8_t *) malloc(len);
        if (command.big == NULL) {
            return;
        }
        memcpy(command.big, data, len);
        command.big_len = (uint16_t) len;
    } else if (data != NULL && len > 0) {
        command.len = (uint8_t) len;
        memcpy(command.data, data, command.len);
    }
    if (xQueueSend(s_commands, &command, 0) != pdTRUE) {
        free(command.big);
    }
}

static void on_pcm(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (data == NULL || len == 0) {
        return;
    }
    RingbufHandle_t ring = audio_take();
    if (ring != NULL) {
        xRingbufferSend(ring, data, len, 0);
        audio_give();
    }
}

static void on_pcm_at(const uint8_t *data, size_t len)
{
    if (data == NULL || len < sizeof(uint32_t)) {
        return;
    }
    RingbufHandle_t ring = audio_take();
    if (ring == NULL) {
        return;
    }
    uint32_t pos;
    memcpy(&pos, data, sizeof(pos));
    const size_t pcm = len - sizeof(pos);
    if (pcm > 0) {
        xRingbufferSend(ring, data + sizeof(pos), pcm, 0);
    }
    s_audio_next = pos + (uint32_t) pcm;
    s_audio_window = true;
    if (pcm == 0) {
        audio_window_send(ring);
    }
    audio_give();
}

static void on_h4_up(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (!s_on_dongle || cb == NULL || cb->notify_host_recv == NULL || data == NULL || len == 0 || len > UINT16_MAX) {
        return;
    }
    uint8_t *h4 = (uint8_t *) data;
    hci_trace(HCI_TRACE_BR_HOST, h4, len);
    if (len >= 3 && h4[0] == H4_EVENT) {
        split_on_event(SPLIT_SIDE_BR, h4, len);
        return;
    }
    host_recv(cb, h4, (uint16_t) len);
}

static void on_flow(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (data == NULL || len == 0) {
        return;
    }
    const bool available = data[0] != 0;
    const bool was = s_send_available;
    s_send_available = available;
    const esp_bluedroid_hci_driver_callbacks_t *cb = s_host_cb;
    if (available && !was && s_on_dongle && cb != NULL && cb->notify_host_send_available != NULL) {
        cb->notify_host_send_available();
    }
}

static void frame_dispatch(const uint8_t *frame, uint16_t len)
{
    s_frames_in++;
    if ((frame[0] & XBT_FRAME_MESSAGE) == 0) {
        on_h4_up(XBT_MSG_H4_UP, frame, len, NULL);
        return;
    }
    const uint8_t type = frame[0] & (uint8_t) ~XBT_FRAME_MESSAGE;
    const uint8_t *data = frame + 1;
    const size_t data_len = (size_t) len - 1;
    if (type == XBT_FRAME_TYPE(XBT_MSG_BATCH_OK)) {
        s_credit_absolute = data_len >= 1 && (data[0] & XBT_HOST_CREDIT_TOTAL) != 0;
        s_host_big = data_len >= 1 && (data[0] & XBT_HOST_BIG_FRAMES) != 0;
    } else if (type == XBT_FRAME_TYPE(XBT_MSG_SYNC)) {
        if (data_len >= 1) {
            const uint32_t total = credit_total();
            const uint8_t ack[5] = {data[0], (uint8_t) (total & 0xFF), (uint8_t) (total >> 8), (uint8_t) (total >> 16),
                                    (uint8_t) (total >> 24)};
            message_send(XBT_MSG_SYNC_ACK, ack, sizeof(ack));
        }
    } else if (type == XBT_FRAME_TYPE(XBT_MSG_HOST_READY)) {
        s_host_ready = true;
        const command_t crash = {.msg_id = INTERNAL_CRASH};
        if (s_commands != NULL) {
            xQueueSend(s_commands, &crash, 0);
        }
    } else if (type == XBT_FRAME_TYPE(XBT_MSG_START)) {
        on_start(XBT_MSG_START, data, data_len, NULL);
    } else if (type == XBT_FRAME_TYPE(XBT_MSG_FLOW)) {
        on_flow(XBT_MSG_FLOW, data, data_len, NULL);
    } else if (type == XBT_FRAME_TYPE(XBT_MSG_AUDIO_PCM)) {
        on_pcm(XBT_MSG_AUDIO_PCM, data, data_len, NULL);
    } else if (type == XBT_FRAME_TYPE(XBT_MSG_AUDIO_PCM_AT)) {
        on_pcm_at(data, data_len);
    } else {
        on_command(type, data, data_len);
    }
}

static void frames_dispatch(const uint8_t *frame, uint16_t len)
{
    if (frame[0] != XBT_FRAME_BATCH) {
        frame_dispatch(frame, len);
        return;
    }
    uint16_t pos = 1;
    while ((uint32_t) pos + 2 <= len) {
        const uint16_t item = (uint16_t) (frame[pos] | (frame[pos + 1] << 8));
        pos += 2;
        if (item == 0 || item > len - pos) {
            break;
        }
        frame_dispatch(frame + pos, item);
        pos += item;
    }
}

static TaskHandle_t s_receiver = NULL;

static void receiver_task(void *arg)
{
    for (;;) {
        size_t len = 0;
        uint8_t *frame = (uint8_t *) xRingbufferReceive(s_rx_ring, &len, portMAX_DELAY);
        if (frame == NULL) {
            continue;
        }
        frames_dispatch(frame, (uint16_t) len);
        vRingbufferReturnItem(s_rx_ring, frame);
        if (credit_add(XBT_RX_ITEM_COST(len)) >= RX_CREDIT_STEP && s_sender != NULL) {
            xTaskNotifyGive(s_sender);
        }
    }
}

static esp_err_t on_frame(void *buf, uint16_t body_len, void *eb)
{
    if (buf == NULL || s_receiver == NULL || s_rx_ring == NULL) {
        return ESP_OK;
    }
    if (((const uint8_t *) buf)[-1] == (XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(XBT_MSG_AUDIO_PCM))) {
        on_pcm(XBT_MSG_AUDIO_PCM, (const uint8_t *) buf, body_len, NULL);
        return ESP_OK;
    }
    if (((const uint8_t *) buf)[-1] == (XBT_FRAME_MESSAGE | XBT_FRAME_TYPE(XBT_MSG_AUDIO_PCM_AT))) {
        on_pcm_at((const uint8_t *) buf, body_len);
        return ESP_OK;
    }
    if (xRingbufferSend(s_rx_ring, (const uint8_t *) buf - 1, (size_t) body_len + 1, 0) != pdTRUE) {
        s_rx_dropped++;
        if (credit_add(XBT_RX_ITEM_COST((size_t) body_len + 1)) >= RX_CREDIT_STEP && s_sender != NULL) {
            xTaskNotifyGive(s_sender);
        }
    }
    return ESP_OK;
}

bool c6_flash_boot(void);

#define LOG_LINE_MAX 120
#define LOG_RING_SIZE 512

static RingbufHandle_t s_log_ring = NULL;
static vprintf_like_t s_log_uart = NULL;

static void log_forward_set(uint8_t level, bool trace)
{
    s_log_level = level;
    esp_log_level_set("*", (esp_log_level_t) level);
    if (trace) {
        hci_trace_start();
    } else if (s_hci_trace != NULL && !s_hci_trace_dump) {
        s_hci_trace_dump = true;
        command_t command = {.msg_id = INTERNAL_TRACE_DUMP};
        if (s_commands == NULL || xQueueSend(s_commands, &command, 0) != pdTRUE) {
            s_hci_trace_dump = false;
        }
    }
}

static void hci_trace_dump_out(void)
{
    static const char *const DIRECTIONS[] = {"?", "H>BR", "H>LE", "BR>H", "LE>H", "LOCAL"};
    char line[112];
    const uint16_t used = s_hci_trace_used;
    const uint16_t first = used < HCI_TRACE_RECORDS ? 0 : s_hci_trace_next;
    int n = snprintf(line, sizeof(line), "HCI trace: %u records", (unsigned) used);
    message_send(XBT_MSG_LOG, (const uint8_t *) line, (size_t) n);
    for (uint16_t i = 0; i < used; i++) {
        const hci_trace_record_t *record = &s_hci_trace[(first + i) % HCI_TRACE_RECORDS];
        n = snprintf(line, sizeof(line), "HCI %lu %s %u: ", (unsigned long) record->us,
                     DIRECTIONS[record->dir < 6 ? record->dir : 0], (unsigned) record->len);
        for (uint8_t b = 0; b < record->count && n + 2 < (int) sizeof(line); b++) {
            n += snprintf(line + n, sizeof(line) - (size_t) n, "%02X", record->data[b]);
        }
        message_send(XBT_MSG_LOG, (const uint8_t *) line, (size_t) n);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    taskENTER_CRITICAL(&s_hci_trace_lock);
    hci_trace_record_t *records = s_hci_trace;
    s_hci_trace = NULL;
    taskEXIT_CRITICAL(&s_hci_trace_lock);
    free(records);
    s_hci_trace_dump = false;
}

static int log_tap(const char *format, va_list args)
{
    if (s_log_level == ESP_LOG_NONE || s_log_ring == NULL || xPortInIsrContext()) {
        return s_log_uart != NULL ? s_log_uart(format, args) : 0;
    }
    char line[LOG_LINE_MAX];
    va_list copy;
    va_copy(copy, args);
    const int written = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (written > 0 && s_log_level != ESP_LOG_NONE && s_log_ring != NULL && !xPortInIsrContext()) {
        const size_t len = (size_t) written < sizeof(line) ? (size_t) written : sizeof(line) - 1;
        if (xRingbufferSend(s_log_ring, line, len, 0) == pdTRUE && s_sender != NULL) {
            xTaskNotifyGive(s_sender);
        }
    }
    return s_log_uart != NULL ? s_log_uart(format, args) : written;
}

static void log_drain(void)
{
    if (s_log_ring == NULL) {
        return;
    }
    size_t len = 0;
    uint8_t *line;
    for (;;) {
        taskENTER_CRITICAL(&s_pending_lock);
        const bool full = s_pending_messages >= INPUT_PENDING_MAX;
        taskEXIT_CRITICAL(&s_pending_lock);
        if (full || (line = (uint8_t *) xRingbufferReceive(s_log_ring, &len, 0)) == NULL) {
            break;
        }
        message_send(XBT_MSG_LOG, line, len);
        vRingbufferReturnItem(s_log_ring, line);
    }
}

static void log_forward_start(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);
    s_log_ring = xRingbufferCreate(LOG_RING_SIZE, RINGBUF_TYPE_NOSPLIT);
    if (s_log_ring != NULL) {
        s_log_uart = esp_log_set_vprintf(log_tap);
    }
}

esp_err_t c6_xbt_init(void)
{
    heap_mark(HEAP_INIT);
    audio_reserve();
    log_forward_start();
    s_safe = c6_flash_boot();
    if (s_commands == NULL) {
        s_commands = xQueueCreate(QUEUE_DEPTH, sizeof(command_t));
    }
    if (s_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_tx_h4 == NULL) {
        s_tx_h4 = xRingbufferCreate(TX_H4_RING_SIZE, RINGBUF_TYPE_NOSPLIT);
    }
    if (s_tx_msg == NULL) {
        s_tx_msg = xRingbufferCreate(TX_MSG_RING_SIZE, RINGBUF_TYPE_NOSPLIT);
    }
    if (s_rx_ring == NULL) {
        s_rx_ring = xRingbufferCreate(RX_RING_SIZE, RINGBUF_TYPE_NOSPLIT);
    }
    if (s_tx_h4 == NULL || s_tx_msg == NULL || s_rx_ring == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (s_sender == NULL &&
        xTaskCreate(sender_task, "xbt_tx", SENDER_STACK, NULL, WORKER_PRIORITY, &s_sender) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (s_receiver == NULL &&
        xTaskCreate(receiver_task, "xbt_rx", RECEIVER_STACK, NULL, WORKER_PRIORITY, &s_receiver) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = eh_cp_set_traffic_rx_cb(ESP_HCI_IF, on_frame);
    if (err == ESP_OK) {
        err = esp_event_handler_register(EH_CP_EVENT, EH_CP_EVT_PRIVATE_RPC_READY, on_host_ready, NULL);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HCI channel handler not registered: %s", esp_err_to_name(err));
    }
    heap_mark(HEAP_OWN);
    return err;
}

static esp_err_t c6_xbt_deinit(void)
{
    esp_event_handler_unregister(EH_CP_EVENT, EH_CP_EVT_PRIVATE_RPC_READY, on_host_ready);
    return eh_cp_set_traffic_rx_cb(ESP_HCI_IF, NULL);
}

EH_CP_FEAT_REGISTER(c6_xbt_init, c6_xbt_deinit, "xbt_bt", tskNO_AFFINITY, 300);

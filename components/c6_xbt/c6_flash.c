#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "eh_cp_core.h"
#include "eh_cp_event.h"
#include "eh_cp_feat_peer_data.h"
#include "c6_xbt_proto.h"

#define SAFE_AFTER_STARTS 3
#define STABLE_US (120LL * 1000 * 1000)
#define STORE_SPACE "xbt_c6"
#define STORE_KEY "starts"
#define STORE_REASON_KEY "reason"
#define STORE_IMAGE_KEY "image"
#define STORE_MINIMAL_KEY "minrev"

#if CONFIG_C6_XBT
#define SLOT_IS_MINIMAL 0
#else
#define SLOT_IS_MINIMAL 1
#endif

static const char *TAG = "c6_flash";

static esp_timer_handle_t s_stable_timer;
static bool s_crash_start;

#if SLOT_IS_MINIMAL
#include <stdlib.h>
#include "esp_flash.h"
#include "esp_flash_partitions.h"
#include "esp_image_format.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SWITCH_STACK 4096
#define SWITCH_CHUNK 4096
#define TABLE_SECTOR 0x1000
#define TABLE_TRIES 3

extern const uint8_t table_ours_start[] asm("_binary_table_xbt_bin_start");
extern const uint8_t table_ours_end[] asm("_binary_table_xbt_bin_end");
extern const uint8_t table_stock_start[] asm("_binary_table_stock_bin_start");
extern const uint8_t table_stock_end[] asm("_binary_table_stock_bin_end");

static TaskHandle_t s_switch_task;
static uint8_t s_switch_command;
static uint8_t s_switch_table;
#endif

static uint8_t starts_load(void)
{
    nvs_handle_t store;
    uint8_t starts = 0;
    if (nvs_open(STORE_SPACE, NVS_READONLY, &store) == ESP_OK) {
        nvs_get_u8(store, STORE_KEY, &starts);
        nvs_close(store);
    }
    return starts;
}

static void starts_save(uint8_t starts)
{
    nvs_handle_t store;
    if (nvs_open(STORE_SPACE, NVS_READWRITE, &store) == ESP_OK) {
        nvs_set_u8(store, STORE_KEY, starts);
        nvs_commit(store);
        nvs_close(store);
    }
}

uint8_t c6_flash_starts(void)
{
    return starts_load();
}

uint8_t c6_flash_crash_reason(void)
{
    nvs_handle_t store;
    uint8_t reason = 0;
    if (nvs_open(STORE_SPACE, NVS_READONLY, &store) == ESP_OK) {
        nvs_get_u8(store, STORE_REASON_KEY, &reason);
        nvs_close(store);
    }
    return reason;
}

static void crash_reason_save(uint8_t reason)
{
    nvs_handle_t store;
    if (nvs_open(STORE_SPACE, NVS_READWRITE, &store) == ESP_OK) {
        nvs_set_u8(store, STORE_REASON_KEY, reason);
        nvs_commit(store);
        nvs_close(store);
    }
}

#if CONFIG_C6_XBT
static void starts_follow_image(void)
{
    uint32_t image = 0;
    memcpy(&image, esp_app_get_description()->app_elf_sha256, sizeof(image));
    nvs_handle_t store;
    if (nvs_open(STORE_SPACE, NVS_READWRITE, &store) != ESP_OK) {
        return;
    }
    uint32_t saved = 0;
    if (nvs_get_u32(store, STORE_IMAGE_KEY, &saved) != ESP_OK || saved != image) {
        nvs_set_u32(store, STORE_IMAGE_KEY, image);
        nvs_set_u8(store, STORE_KEY, 0);
        nvs_commit(store);
    }
    nvs_close(store);
}
#endif

static void on_stable(void *arg)
{
    if (!s_crash_start) {
        starts_save(0);
    }
}

static uint8_t minimal_revision(void)
{
#if SLOT_IS_MINIMAL
    return XBT_MINIMAL_REVISION;
#else
    nvs_handle_t store;
    uint8_t revision = 0;
    if (nvs_open(STORE_SPACE, NVS_READONLY, &store) == ESP_OK) {
        nvs_get_u8(store, STORE_MINIMAL_KEY, &revision);
        nvs_close(store);
    }
    return revision;
#endif
}

#if SLOT_IS_MINIMAL
static void minimal_revision_save(void)
{
    nvs_handle_t store;
    if (nvs_open(STORE_SPACE, NVS_READWRITE, &store) != ESP_OK) {
        return;
    }
    uint8_t saved = 0;
    if (nvs_get_u8(store, STORE_MINIMAL_KEY, &saved) != ESP_OK || saved != XBT_MINIMAL_REVISION) {
        nvs_set_u8(store, STORE_MINIMAL_KEY, XBT_MINIMAL_REVISION);
        nvs_commit(store);
    }
    nvs_close(store);
}
#endif

static uint8_t slot_table_kind(void)
{
    const esp_partition_t *second = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (second == NULL) {
        return 0;
    }
    if (second->address == XBT_SLOT_OUR_SECOND_APP) {
        return XBT_SLOT_TABLE_OURS;
    }
    return second->address == XBT_SLOT_STOCK_SECOND_APP ? XBT_SLOT_TABLE_STOCK : 0;
}

static void slot_answer(uint8_t command, esp_err_t result)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t desc;
    xbt_slot_state_t state = {
        .command = command,
        .result = result == ESP_OK ? 0 : 1,
        .running_subtype = running != NULL ? running->subtype : UINT8_MAX,
        .boot_subtype = boot != NULL ? boot->subtype : UINT8_MAX,
        .minimal = SLOT_IS_MINIMAL,
        .other_valid = other != NULL && esp_ota_get_partition_description(other, &desc) == ESP_OK,
        .commands = SLOT_IS_MINIMAL ? XBT_SLOT_TABLE : XBT_SLOT_BOOT_MINIMAL,
        .table = slot_table_kind(),
        .first = running != NULL && running->address == XBT_SLOT_FIRST_APP,
        .error = (uint16_t) result,
        .revision = minimal_revision(),
    };
    eh_cp_feat_peer_data_send(XBT_SLOT_MSG_STATE, (const uint8_t *) &state, sizeof(state));
}

#if SLOT_IS_MINIMAL
static esp_err_t slot_clone(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || target == NULL || target->address == running->address) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_pos_t pos = {.offset = running->address, .size = running->size};
    esp_image_metadata_t meta;
    esp_err_t err = esp_image_get_metadata(&pos, &meta);
    if (err != ESP_OK) {
        return err;
    }
    if (meta.image_len > target->size) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *chunk = (uint8_t *) malloc(SWITCH_CHUNK);
    if (chunk == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_ota_handle_t handle = 0;
    err = esp_ota_begin(target, meta.image_len, &handle);
    const bool begun = err == ESP_OK;
    for (uint32_t at = 0; err == ESP_OK && at < meta.image_len; at += SWITCH_CHUNK) {
        const size_t part = meta.image_len - at < SWITCH_CHUNK ? meta.image_len - at : SWITCH_CHUNK;
        err = esp_partition_read(running, at, chunk, part);
        if (err == ESP_OK) {
            err = esp_ota_write(handle, chunk, part);
        }
    }
    free(chunk);
    if (begun && err == ESP_OK) {
        err = esp_ota_end(handle);
    } else if (begun) {
        esp_ota_abort(handle);
    }
    return err;
}

static const esp_partition_t *slot_first_app(void)
{
    const esp_partition_t *found = NULL;
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    while (it != NULL) {
        const esp_partition_t *part = esp_partition_get(it);
        if (part->address == XBT_SLOT_FIRST_APP) {
            found = part;
            esp_partition_iterator_release(it);
            break;
        }
        it = esp_partition_next(it);
    }
    return found;
}

static esp_err_t slot_table(uint8_t kind)
{
    if (kind != XBT_SLOT_TABLE_OURS && kind != XBT_SLOT_TABLE_STOCK) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t *table = kind == XBT_SLOT_TABLE_STOCK ? table_stock_start : table_ours_start;
    const size_t len = kind == XBT_SLOT_TABLE_STOCK ? (size_t) (table_stock_end - table_stock_start)
                                                      : (size_t) (table_ours_end - table_ours_start);
    if (len == 0 || len > ESP_PARTITION_TABLE_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint32_t chip_size = 0;
    if (kind == XBT_SLOT_TABLE_OURS &&
        (esp_flash_get_size(esp_flash_default_chip, &chip_size) != ESP_OK || chip_size < XBT_SLOT_OUR_CHIP_SIZE)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const esp_partition_t *first = slot_first_app();
    const esp_partition_t *boot_data = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (first == NULL || boot_data == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_pos_t pos = {.offset = first->address, .size = first->size};
    esp_image_metadata_t meta;
    esp_err_t err = esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta);
    if (err != ESP_OK) {
        return err;
    }
    if (kind == XBT_SLOT_TABLE_STOCK && meta.image_len > XBT_SLOT_STOCK_APP_SIZE) {
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *check = (uint8_t *) malloc(len);
    if (check == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_partition_erase_range(boot_data, 0, boot_data->size);
    for (int round = 0; err == ESP_OK && round < TABLE_TRIES; round++) {
        esp_err_t step = esp_flash_erase_region(esp_flash_default_chip, CONFIG_PARTITION_TABLE_OFFSET, TABLE_SECTOR);
        if (step == ESP_OK) {
            step = esp_flash_write(esp_flash_default_chip, table, CONFIG_PARTITION_TABLE_OFFSET, len);
        }
        if (step == ESP_OK) {
            step = esp_flash_read(esp_flash_default_chip, check, CONFIG_PARTITION_TABLE_OFFSET, len);
        }
        if (step == ESP_OK && memcmp(check, table, len) != 0) {
            step = ESP_ERR_INVALID_CRC;
        }
        err = step;
        if (step == ESP_OK) {
            break;
        }
        err = round + 1 < TABLE_TRIES ? ESP_OK : step;
    }
    free(check);
    return err;
}

static void switch_task(void *arg)
{
    const uint8_t command = s_switch_command;
    const esp_err_t result = command == XBT_SLOT_CLONE ? slot_clone() : slot_table(s_switch_table);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Slot command %u failed: 0x%x", command, result);
    }
    s_switch_task = NULL;
    slot_answer(command, result);
    vTaskDelete(NULL);
}

static esp_err_t switch_start(uint8_t command, uint8_t table)
{
    if (s_switch_task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_switch_command = command;
    s_switch_table = table;
    return xTaskCreate(switch_task, "c6_switch", SWITCH_STACK, NULL, tskIDLE_PRIORITY + 2, &s_switch_task) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
#endif

static esp_err_t slot_boot(const esp_partition_t *target)
{
    if (target == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    if (boot != NULL && boot->address == target->address) {
        return ESP_OK;
    }
    const esp_err_t result = esp_ota_set_boot_partition(target);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Slot at 0x%lx not bootable: 0x%x", (unsigned long) target->address, result);
    }
    return result;
}

static void on_slot_command(uint32_t msg_id, const uint8_t *data, size_t len, void *ctx)
{
    if (len == 0) {
        return;
    }
    esp_err_t result = ESP_OK;
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (data[0] == XBT_SLOT_BOOT_OTHER) {
        result = slot_boot(other);
    } else if (data[0] == XBT_SLOT_BOOT_MAIN) {
        result = slot_boot(SLOT_IS_MINIMAL ? other : running);
    } else if (data[0] == XBT_SLOT_BOOT_MINIMAL) {
        result = slot_boot(SLOT_IS_MINIMAL ? running : other);
#if SLOT_IS_MINIMAL
    } else if (data[0] == XBT_SLOT_CLONE || (data[0] == XBT_SLOT_TABLE && len >= 2)) {
        result = switch_start(data[0], len >= 2 ? data[1] : 0);
        if (result == ESP_OK) {
            return;
        }
#endif
    } else if (data[0] != XBT_SLOT_QUERY) {
        result = ESP_ERR_NOT_SUPPORTED;
    }
    slot_answer(data[0], result);
}

static void on_host_up(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_ota_mark_app_valid_cancel_rollback();
}

static void slot_start(void)
{
    if (eh_cp_feat_peer_data_init() != ESP_OK ||
        eh_cp_feat_peer_data_register_callback(XBT_SLOT_MSG_COMMAND, on_slot_command, NULL) != ESP_OK ||
        esp_event_handler_register(EH_CP_EVENT, EH_CP_EVT_PRIVATE_RPC_READY, on_host_up, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Slot commands not registered");
    }
}

bool c6_flash_boot(void)
{
    static bool done;
    static bool safe;
    if (done) {
        return safe;
    }
    done = true;
    slot_start();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
#if CONFIG_C6_XBT
    starts_follow_image();
#else
    minimal_revision_save();
#endif
    const esp_reset_reason_t reason = esp_reset_reason();
    s_crash_start = reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                    reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT;
    const uint8_t starts = starts_load();
    safe = starts >= SAFE_AFTER_STARTS;
    if (s_crash_start && starts < UINT8_MAX) {
        starts_save(starts + 1);
    }
    if (s_crash_start) {
        crash_reason_save((uint8_t) reason);
    }
    const esp_timer_create_args_t args = {.callback = on_stable, .name = "c6_stable"};
    if (esp_timer_create(&args, &s_stable_timer) == ESP_OK) {
        esp_timer_start_once(s_stable_timer, STABLE_US);
    }
    if (safe) {
        ESP_LOGW(TAG, "Safe mode after %u crashed starts: Bluetooth host not started", starts);
    }
    return safe;
}

#if !CONFIG_C6_XBT
static esp_err_t c6_flash_init(void)
{
    c6_flash_boot();
    return ESP_OK;
}

static esp_err_t c6_flash_deinit(void)
{
    return ESP_OK;
}

EH_CP_FEAT_REGISTER(c6_flash_init, c6_flash_deinit, "c6_flash", tskNO_AFFINITY, 300);
#endif

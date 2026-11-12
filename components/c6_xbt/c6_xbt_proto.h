#pragma once

#include <stdint.h>

#define XBT_MSG_START 0x42545001u
#define XBT_MSG_H4_UP 0x42545002u
#define XBT_MSG_H4_DOWN 0x42545003u
#define XBT_MSG_FLOW 0x42545004u
#define XBT_MSG_HELLO 0x42545005u
#define XBT_MSG_BATCH_OK 0x42545006u
#define XBT_MSG_HOST_READY 0x42545007u
#define XBT_MSG_RX_CREDIT 0x42545008u
#define XBT_MSG_SYNC 0x42545009u
#define XBT_MSG_SYNC_ACK 0x4254500Au
#define XBT_HELLO_BATCH 0x01u
#define XBT_HELLO_CREDIT_SYNC 0x02u
#define XBT_HELLO_AUDIO_WINDOW 0x04u
#define XBT_HELLO_BIG_FRAMES 0x08u
#define XBT_HOST_CREDIT_TOTAL 0x02u
#define XBT_HOST_BIG_FRAMES 0x08u
#define XBT_RX_ITEM_COST(len) (8u + (((uint32_t) (len) + 3u) & ~3u))
#define XBT_FRAME_BATCH 0x7Fu
#define XBT_BATCH_MAX 1500u
#define XBT_FRAME_MAX 1600u

#define XBT_MSG_HID_OPEN 0x42545010u
#define XBT_MSG_HID_INPUT 0x42545011u
#define XBT_MSG_HID_CLOSE 0x42545012u
#define XBT_MSG_HID_OUTPUT 0x42545013u
#define XBT_MSG_HID_SET_REPORT 0x42545014u
#define XBT_MSG_HID_GET_FEATURE 0x42545015u
#define XBT_MSG_HID_FEATURE 0x42545016u
#define XBT_MSG_LOG 0x42545017u
#define XBT_MSG_LOG_SET 0x42545018u
#define XBT_MSG_HID_INPUT_RAW 0x42545019u
#define XBT_MSG_SCAN 0x42545020u
#define XBT_MSG_FOUND 0x42545021u
#define XBT_MSG_CONNECT 0x42545022u
#define XBT_MSG_SCAN_STATE 0x42545023u
#define XBT_MSG_RECORDS 0x42545024u
#define XBT_MSG_CANCEL_SCAN 0x42545025u
#define XBT_MSG_FORGET 0x42545026u
#define XBT_MSG_HID_DISCONNECT 0x42545027u
#define XBT_MSG_PAIR_EVENT 0x42545028u
#define XBT_MSG_BROADCAST_ADD 0x42545029u
#define XBT_MSG_BROADCAST_DATA 0x4254502Au
#define XBT_MSG_ERROR 0x4254502Bu
#define XBT_MSG_PAIR_REPLY 0x4254502Cu

#define XBT_FOUND_HID 0
#define XBT_FOUND_BROADCAST 2

#define XBT_PAIR_PASSKEY 1
#define XBT_PAIR_CONFIRM 2
#define XBT_PAIR_DONE 3
#define XBT_PAIR_FAILED 4
#define XBT_PAIR_PIN 5
#define XBT_PAIR_CODE 6
#define XBT_PAIR_CODE_LE 7
#define XBT_MSG_AUDIO_PAUSE 0x42545036u
#define XBT_MSG_AUDIO_INTERVAL 0x42545037u
#define XBT_MSG_AUDIO_RECONNECT 0x425450F1u

#define XBT_MSG_NVS_LIST 0x42545060u
#define XBT_MSG_NVS_GET 0x42545061u
#define XBT_MSG_NVS_SET 0x42545062u
#define XBT_MSG_NVS_ERASE 0x42545063u
#define XBT_MSG_NVS_RELOAD 0x42545064u
#define XBT_MSG_NVS_ENTRY 0x42545068u
#define XBT_MSG_NVS_VALUE 0x42545069u
#define XBT_MSG_NVS_DONE 0x4254506Au
#define XBT_NVS_BLOB_MAX 1536u

#define XBT_SLOT_MSG_COMMAND 0x42545070u
#define XBT_SLOT_MSG_STATE 0x42545071u
#define XBT_SLOT_QUERY 0
#define XBT_SLOT_BOOT_OTHER 1
#define XBT_SLOT_BOOT_MAIN 2
#define XBT_SLOT_BOOT_MINIMAL 3
#define XBT_SLOT_CLONE 4
#define XBT_SLOT_TABLE 5
#define XBT_SLOT_TABLE_OURS 1
#define XBT_SLOT_TABLE_STOCK 2
#define XBT_SLOT_FIRST_APP 0x10000u
#define XBT_SLOT_OUR_SECOND_APP 0x2B0000u
#define XBT_SLOT_STOCK_SECOND_APP 0x1D0000u
#define XBT_SLOT_STOCK_APP_SIZE 0x1C0000u
#define XBT_SLOT_OUR_CHIP_SIZE 0x400000u
#define XBT_MINIMAL_REVISION 1

typedef struct __attribute__((packed)) {
    uint8_t command;
    uint8_t result;
    uint8_t running_subtype;
    uint8_t boot_subtype;
    uint8_t minimal;
    uint8_t other_valid;
    uint8_t commands;
    uint8_t table;
    uint8_t first;
    uint16_t error;
    uint8_t revision;
} xbt_slot_state_t;

#define XBT_RECORDS_MAX 16
#define XBT_RECORD_NAME 32
#define XBT_KIND_HID 0x01u
#define XBT_KIND_AUDIO 0x02u
#define XBT_KIND_BROADCAST 0x04u

typedef struct __attribute__((packed)) {
    uint8_t address[6];
    uint8_t transport;
    uint8_t addr_type;
    uint8_t kind;
    uint8_t name_len;
    char name[XBT_RECORD_NAME];
    uint32_t device_class;
} xbt_record_t;

#define XBT_TRANSPORT_CLASSIC 0
#define XBT_TRANSPORT_LE 1

#define XBT_MSG_AUDIO_CONNECT 0x42545030u
#define XBT_MSG_AUDIO_DISCONNECT 0x42545031u
#define XBT_MSG_AUDIO_PCM 0x42545032u
#define XBT_MSG_AUDIO_STATE 0x42545033u
#define XBT_MSG_AUDIO_CREDIT 0x42545034u
#define XBT_MSG_AUDIO_VOLUME 0x42545035u
#define XBT_MSG_AUDIO_PCM_AT 0x42545038u
#define XBT_MSG_AUDIO_WINDOW 0x42545039u

#define XBT_MSG_COMMAND 0x42545040u
#define XBT_MSG_STATUS 0x42545050u
#define XBT_MSG_STATUS_PACKED 0x42545051u
#define XBT_MSG_REPORT 0x42545052u
#define XBT_REPORT_STACKS 1u
#define XBT_REPORT_HEAP_OWNERS 2u
#define XBT_REPORT_BLOCKS 3u
#define XBT_REPORT_MARKS 4u
#define XBT_REPORT_PARTITIONS 5u
#define XBT_STATUS_FIELDS 19u
#define XBT_MSG_TYPE_BASE 0x42545000u

#define XBT_FRAME_MESSAGE 0x80u
#define XBT_FRAME_TYPE(msg) ((uint8_t) ((msg) & 0x7Fu))

typedef struct __attribute__((packed)) {
    uint32_t uptime_ms;
    uint32_t heap_free;
    uint32_t heap_min;
    uint32_t heap_largest;
    uint32_t tasks;
    uint32_t worker_stack_free;
    uint32_t audio_free;
    uint32_t a2d_calls;
    uint32_t a2d_max_us;
    uint32_t command_max_us;
    uint32_t frames_in;
    uint32_t frames_out;
    uint32_t send_max_us;
    uint32_t a2d_underruns;
    uint32_t reset_reason;
    uint32_t state;
    uint32_t pending;
    uint32_t crashed_starts;
} xbt_status_t;

#define XBT_STATE_SAFE 0x01u
#define XBT_STATE_DONGLE 0x02u
#define XBT_STATE_READY 0x04u
#define XBT_STATE_REQUESTED 0x08u

#define XBT_PARTITION_LABEL 16

typedef struct __attribute__((packed)) {
    char label[XBT_PARTITION_LABEL];
    uint8_t type;
    uint8_t subtype;
    uint8_t running;
    uint32_t offset;
    uint32_t size;
} xbt_partition_t;

#include "ui_settings_store.h"
#include <rtthread.h>
#include "mem_map.h"
#include "drv_flash.h"
#include <stddef.h>
#include <string.h>

#define SETTINGS_MAGIC 0x45505331u
#define SETTINGS_VERSION 1u
#define SECTOR_SIZE 4096u

typedef struct
{
    uint32_t magic, checksum, version, sequence;
    uint8_t values[UI_SETTING_COUNT];
} settings_record_t;

typedef char settings_fit[(sizeof(settings_record_t) <= SECTOR_SIZE &&
                          SETTINGS_STORE_SIZE >= 2 * SECTOR_SIZE) ? 1 : -1];
static uint32_t sequence;
static unsigned next_slot;
static bool saved_valid;
static uint8_t last_saved[UI_SETTING_COUNT];

static uint32_t checksum(const settings_record_t *record)
{
    const uint8_t *p = (const uint8_t *)&record->version;
    size_t size = sizeof(*record) - offsetof(settings_record_t, version);
    uint32_t crc = UINT32_MAX;
    while (size--)
    {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool ui_settings_store_load(uint8_t values[UI_SETTING_COUNT])
{
    settings_record_t record;
    saved_valid = false;
    sequence = next_slot = 0;
    for (unsigned slot = 0; slot < 2; ++slot)
    {
        uint32_t address = SETTINGS_STORE_START_ADDR + slot * SECTOR_SIZE;
        if (rt_flash_read(address, (uint8_t *)&record, sizeof(record)) != sizeof(record) ||
            record.magic != SETTINGS_MAGIC || record.version != SETTINGS_VERSION ||
            record.checksum != checksum(&record)) continue;
        bool valid = true;
        for (unsigned i = 0; i < UI_SETTING_COUNT; ++i)
            if (record.values[i] >= ui_settings_item((ui_setting_id_t)i)->option_count) valid = false;
        if (!valid || (saved_valid && (int32_t)(record.sequence - sequence) <= 0)) continue;
        sequence = record.sequence;
        next_slot = 1 - slot;
        memcpy(values, record.values, sizeof(record.values));
        memcpy(last_saved, values, sizeof(last_saved));
        saved_valid = true;
    }
    return saved_valid;
}

bool ui_settings_store_save(const uint8_t values[UI_SETTING_COUNT])
{
    if (saved_valid && !memcmp(values, last_saved, sizeof(last_saved))) return true;
    settings_record_t record = {0}, check;
    record.magic = SETTINGS_MAGIC;
    record.version = SETTINGS_VERSION;
    record.sequence = sequence + 1;
    memcpy(record.values, values, sizeof(record.values));
    record.checksum = checksum(&record);
    uint32_t address = SETTINGS_STORE_START_ADDR + next_slot * SECTOR_SIZE;
    /* Commit only after readback; keep the previous sector intact on failure. */
    if (rt_flash_erase(address, SECTOR_SIZE) != RT_EOK ||
        rt_flash_write(address + 4, (const uint8_t *)&record + 4, sizeof(record) - 4) != sizeof(record) - 4 ||
        rt_flash_read(address + 4, (uint8_t *)&check + 4, sizeof(check) - 4) != sizeof(check) - 4 ||
        memcmp((const uint8_t *)&record + 4, (const uint8_t *)&check + 4, sizeof(record) - 4) ||
        rt_flash_write(address, (const uint8_t *)&record.magic, 4) != 4 ||
        rt_flash_read(address, (uint8_t *)&check.magic, 4) != 4 || check.magic != SETTINGS_MAGIC)
        return false;
    sequence = record.sequence;
    next_slot = 1 - next_slot;
    memcpy(last_saved, values, sizeof(last_saved));
    saved_valid = true;
    return true;
}

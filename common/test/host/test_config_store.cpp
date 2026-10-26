// Beacon configuration store (main/config_store.cpp) against a simulated NVS: first boot, save and reload,
// failed saves that must keep the old configuration, corrupt data, unusable partition. The store must never erase
// anything on its own and never touch another partition (the Matter / Thread "nvs").
#include "test.h"

#include "config_store.h"
#include "fake_nvs.h"

static presence_config_t sample_config(uint8_t last_octet)
{
    presence_config_t cfg;
    presence_config_set_defaults(&cfg);
    cfg.slots[0].enabled = true;
    cfg.slots[0].has_addr = true;
    const uint8_t addr[6] = { 0xC0, 0x11, 0x22, 0x33, 0x44, last_octet };
    memcpy(cfg.slots[0].addr, addr, 6);
    cfg.slots[0].addr_type = BEACON_ADDR_PUBLIC;
    strcpy(cfg.slots[0].label, "Ví");
    cfg.slots[4].timeout_s = 120;
    return cfg;
}

static bool current_equals(const presence_config_t &expected)
{
    presence_config_t cur;
    config_store_get(&cur);
    return presence_config_equal(&cur, &expected);
}

static void never_erases_or_touches_matter()
{
    CHECK(g_nvs.erase_all_calls == 0);
    CHECK(g_nvs.foreign_partition_calls == 0);
}

static void first_boot_defaults()
{
    fake_nvs_reset();
    CHECK(config_store_init() == ESP_OK);
    CHECK(config_store_status() == CONFIG_STORE_DEFAULTS);
    presence_config_t defaults;
    presence_config_set_defaults(&defaults);
    CHECK(current_equals(defaults));
    CHECK(g_nvs.erase_calls == 0);
    never_erases_or_touches_matter();
}

static void save_and_reload()
{
    fake_nvs_reset();
    config_store_init();
    const presence_config_t cfg = sample_config(0x55);
    CHECK(config_store_save(&cfg) == ESP_OK);
    CHECK(config_store_status() == CONFIG_STORE_OK);
    CHECK(current_equals(cfg));
    // "Reboot": a fresh init reads it back from flash
    CHECK(config_store_init() == ESP_OK);
    CHECK(config_store_status() == CONFIG_STORE_OK);
    CHECK(current_equals(cfg));
    never_erases_or_touches_matter();
}

static void failed_saves_keep_old_config()
{
    const esp_err_t failures[] = { ESP_ERR_NVS_NOT_ENOUGH_SPACE, ESP_FAIL };
    for (esp_err_t failure : failures) {
        for (int where = 0; where < 3; where++) {
            fake_nvs_reset();
            config_store_init();
            const presence_config_t old_cfg = sample_config(0x01);
            CHECK(config_store_save(&old_cfg) == ESP_OK);

            const presence_config_t new_cfg = sample_config(0x02);
            if (where == 0) {
                g_nvs.set_result = failure;
            } else if (where == 1) {
                g_nvs.commit_result = failure;
            } else {
                g_nvs.open_result = failure;
            }
            CHECK(config_store_save(&new_cfg) != ESP_OK);
            CHECK(current_equals(old_cfg)); // in memory: the old one
            g_nvs.set_result = g_nvs.commit_result = g_nvs.open_result = ESP_OK;
            config_store_init(); // in flash: the old one
            CHECK(current_equals(old_cfg));
            never_erases_or_touches_matter();
        }
    }
}

static void read_back_mismatch_is_an_error()
{
    fake_nvs_reset();
    config_store_init();
    const presence_config_t old_cfg = sample_config(0x01);
    config_store_save(&old_cfg);
    g_nvs.corrupt_read = true;
    const presence_config_t new_cfg = sample_config(0x02);
    CHECK(config_store_save(&new_cfg) == ESP_ERR_INVALID_CRC);
    CHECK(current_equals(old_cfg));
}

static void corrupt_blob_rejected_and_kept()
{
    fake_nvs_reset();
    config_store_init();
    const presence_config_t cfg = sample_config(0x09);
    config_store_save(&cfg);
    auto &blob = g_nvs.committed["presence/cfg"];
    CHECK(!blob.empty());
    blob[20] ^= 0x40; // bit rot
    const auto damaged = blob;
    CHECK(config_store_init() == ESP_OK);
    CHECK(config_store_status() == CONFIG_STORE_INVALID);
    CHECK_STR(config_store_status_detail(), "bad CRC");
    presence_config_t defaults;
    presence_config_set_defaults(&defaults);
    CHECK(current_equals(defaults));                       // runs with every slot off
    CHECK(g_nvs.committed["presence/cfg"] == damaged);     // data left for inspection
    CHECK(g_nvs.erase_calls == 0);
    // A save by the user replaces it
    CHECK(config_store_save(&cfg) == ESP_OK);
    CHECK(config_store_status() == CONFIG_STORE_OK);
    never_erases_or_touches_matter();
}

static void unusable_partition_is_not_erased()
{
    const esp_err_t errors[] = { ESP_ERR_NVS_NO_FREE_PAGES, ESP_ERR_NVS_NEW_VERSION_FOUND };
    for (esp_err_t error : errors) {
        fake_nvs_reset();
        g_nvs.init_result = error;
        CHECK(config_store_init() == error);
        CHECK(config_store_status() == CONFIG_STORE_UNAVAILABLE);
        CHECK(g_nvs.erase_calls == 0); // never erased on its own
        presence_config_t defaults;
        presence_config_set_defaults(&defaults);
        CHECK(current_equals(defaults));
        const presence_config_t cfg = sample_config(0x03);
        CHECK(config_store_save(&cfg) == ESP_ERR_INVALID_STATE);

        // Format on explicit request: only this partition, then saving works
        CHECK(config_store_format() == ESP_OK);
        CHECK(g_nvs.erase_calls == 1);
        CHECK(config_store_status() == CONFIG_STORE_DEFAULTS);
        CHECK(config_store_save(&cfg) == ESP_OK);
        CHECK(current_equals(cfg));
        never_erases_or_touches_matter();
    }
}

static void format_failure_reported()
{
    fake_nvs_reset();
    g_nvs.init_result = ESP_ERR_NVS_NO_FREE_PAGES;
    g_nvs.init_result_after_erase = ESP_ERR_NVS_NO_FREE_PAGES; // flash broken for good
    config_store_init();
    CHECK(config_store_format() == ESP_ERR_NVS_NO_FREE_PAGES);
    CHECK(config_store_status() == CONFIG_STORE_UNAVAILABLE);
}

static void ap_password()
{
    fake_nvs_reset();
    config_store_init();
    char pw[CONFIG_STORE_AP_PASSWORD_SIZE];
    config_store_ap_password(pw);
    CHECK(strlen(pw) == 12);
    for (const char *p = pw; *p; p++) {
        CHECK(strchr("abcdefghijkmnpqrstuvwxyz23456789", *p) != nullptr);
    }
    // Stored once in the app partition, returned unchanged afterwards
    auto stored = g_nvs.committed.find("presence/ap_pw");
    CHECK(stored != g_nvs.committed.end());
    CHECK(stored != g_nvs.committed.end() && strcmp(reinterpret_cast<const char *>(stored->second.data()), pw) == 0);
    char again[CONFIG_STORE_AP_PASSWORD_SIZE];
    config_store_ap_password(again);
    CHECK_STR(again, pw);
    // Formatting keeps the password of this boot
    config_store_format();
    stored = g_nvs.committed.find("presence/ap_pw");
    CHECK(stored != g_nvs.committed.end() && strcmp(reinterpret_cast<const char *>(stored->second.data()), pw) == 0);
    never_erases_or_touches_matter();
}

// MAC watch records: their own blob, kept across a reboot and a configuration save; another slot count; corrupt
static void mac_watch_records()
{
    fake_nvs_reset();
    config_store_init();
    mac_watch_t w[20];
    CHECK(config_store_load_watch(w, 20) == ESP_ERR_NVS_NOT_FOUND && w[0].state == MAC_WATCH_NONE);
    const uint8_t mac[6] = { 0x48, 0x87, 0x2D, 0x9D, 0xC1, 0xDF };
    mac_watch_assign(&w[0], mac);
    mac_watch_assign(&w[19], mac);
    w[19].state = MAC_WATCH_FIXED;
    CHECK(config_store_save_watch(w, 20) == ESP_OK);
    const presence_config_t cfg = sample_config(0x56);
    CHECK(config_store_save(&cfg) == ESP_OK);
    CHECK(config_store_init() == ESP_OK);
    mac_watch_t r[20];
    CHECK(config_store_load_watch(r, 20) == ESP_OK && r[0].state == MAC_WATCH_RUNNING && r[19].state == MAC_WATCH_FIXED);
    CHECK(memcmp(r[0].mac, mac, 6) == 0);
    CHECK(current_equals(cfg));
    // A five-slot build reads the first five of the 20 records (the blob is larger than its own)
    mac_watch_t five[5];
    CHECK(config_store_load_watch(five, 5) == ESP_OK && five[0].state == MAC_WATCH_RUNNING);
    // Unreadable: all reset, nothing erased
    g_nvs.corrupt_read = true;
    CHECK(config_store_load_watch(r, 20) == ESP_ERR_INVALID_CRC && r[0].state == MAC_WATCH_NONE);
    g_nvs.corrupt_read = false;
    never_erases_or_touches_matter();
}

int main()
{
    RUN(first_boot_defaults);
    RUN(save_and_reload);
    RUN(failed_saves_keep_old_config);
    RUN(read_back_mismatch_is_an_error);
    RUN(corrupt_blob_rejected_and_kept);
    RUN(unusable_partition_is_not_erased);
    RUN(format_failure_reported);
    RUN(ap_password);
    RUN(mac_watch_records);
    return test_summary(PRESENCE_SLOT_COUNT == 5 ? "config_store" : "config_store 20");
}

/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// UART link of the two-chip version (frames: presence_link_proto.h), one task on each chip:
//
//   scanner (ESP32-C3)      sends a STATE frame every second and within 100 ms of a presence change; in the CONFIG
//                           mode it keeps sending (mode CONFIG) so the Matter node knows it is alive. After a
//                           restart a slot is "unknown" until its beacon is heard or a whole absence timeout of
//                           scanning has passed (plink_known_mask()). It sends the device of each slot (SLOT: MAC,
//                           model, battery), one slot every 250 ms so each one every 5 s, all of them within 2 s
//                           after a save, and once asked, a command (three times).
//   Matter node (ESP32-H2)  applies the known states of the STATE frames to the bridged Occupancy Sensors (only
//                           changes, and again until Matter accepts them) and keeps the others, brings each bridged
//                           device to its SLOT frame (added, removed, replaced, model and battery updated: one slot
//                           per 100 ms pass, again until done), sends a STATUS frame every second and its
//                           onboarding codes every 5 s, and when the scanner stays silent for
//                           CONFIG_PRESENCE_LINK_TIMEOUT_SEC turns every sensor off and marks the devices unreachable.
//
// Pins, UART and baud rate: Kconfig "Two-chip UART link". The console of both chips must not use those pins
// (sdkconfig.defaults of the two projects: console on USB-Serial-JTAG only).

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>

#include "presence_config.h"
#include "presence_link_proto.h"

typedef struct {
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t crc_errors;
    uint32_t bad_frames;
    int64_t last_rx_ms; // esp_timer time of the last valid frame from the other chip, 0: none yet
    bool have_peer_state;
    plink_state_t peer_state; // Matter node: last STATE received
    bool have_peer_status;
    plink_status_t peer_status; // scanner: last STATUS received
    bool have_pairing;
    plink_pairing_t pairing; // scanner: onboarding codes of the Matter node
    uint32_t link_losses;    // Matter node: times the scanner was declared gone
} presence_link_stats_t;

// Scanner side. cfg: the configuration presence_manager runs with (NORMAL mode), nullptr in CONFIG mode.
esp_err_t presence_link_start_scanner(bool config_mode, const presence_config_t *cfg);
// presence_report_fn_t of the scanner: records the state, the link task sends it. Never blocks, always delivered.
bool presence_link_report(int slot, bool present);
// Scanner, any task: the devices of the bridge are the active slots of cfg. send: cfg was read from flash or just
// saved; a scanner with nothing saved (or unreadable storage) sends no list, so it never empties the bridge.
void presence_link_set_slots(const presence_config_t *cfg, bool send);
// Scanner, any task: sends a PLINK_CMD_* to the Matter node (three frames, acted on once)
void presence_link_request_command(uint8_t command);

// Matter node side, all called from the link task
typedef struct {
    bool (*apply)(int slot, bool occupied); // false: Matter cannot take it yet (retried)
    void (*status)(plink_status_t *status); // fills the STATUS frame
    // Brings the slot's bridged device to want (nullptr: the scanner's wish is not known yet, only a device saved
    // on the Matter node is restored). true: done for this want; false: called again later (not ready, memory).
    // *added: a device was created, its occupancy is to be set again.
    bool (*bridge_step)(int slot, const plink_slot_t *want, bool *added);
    void (*reachable)(bool reachable);     // the scanner went silent or came back
    bool (*pairing)(plink_pairing_t *out); // onboarding codes; false: not known yet
    void (*command)(uint8_t command);      // PLINK_CMD_*, once per request
} presence_link_matter_ops_t;
esp_err_t presence_link_start_matter(const presence_link_matter_ops_t *ops);

void presence_link_get_stats(presence_link_stats_t *out);
// One line for the diagnostics log
void presence_link_log_diagnostics(void);

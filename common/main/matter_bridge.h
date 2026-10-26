/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Two-chip version, Matter node (CONFIG_PRESENCE_ROLE_MATTER_UART): the node is a Matter bridge
// (CONFIG_PRESENCE_PRODUCT_NAME, "iBeacon Bridge"). Endpoint 1 is an Aggregator; each active slot of the scanner
// (enabled, beacon assigned) is a bridged device under it: Bridged Node + Occupancy Sensor + Power Source (battery).
// Its Bridged Device Basic Information shows, in Apple Home and other controllers:
//   name (NodeLabel), serial number   the beacon's MAC, "C0:11:22:33:44:01"
//   manufacturer (VendorName)         CONFIG_PRESENCE_VENDOR_NAME, "Haxeko"
//   model (ProductName)               the beacon's model: its advertised name, or the one typed on the page
//                                     ("iBeacon" until one is known)
// and its Power Source the battery level from the beacon's Eddystone-TLM frames (BatPercentRemaining, null until
// the first frame; BatChargeLevel Warning / Critical and BatReplacementNeeded at the thresholds of the scanner).
// The scanner sends one SLOT frame per slot (presence_link.cpp): a device is added when a slot becomes active,
// removed when it no longer is, and replaced (new endpoint, new accessory in the controller) when another beacon is
// assigned to the slot; its model and battery are updated in place.
//
// esp-matter never reuses an endpoint ID (the next free one is kept in NVS). The table slot -> (endpoint, MAC, model,
// battery) is kept in the NVS namespace "pbridge", so the same endpoints come back at every boot before the scanner
// speaks.
//
// Memory: each device takes a few KB of heap (endpoint, clusters, attributes). A device is only created or restored
// while the free heap stays above a reserve: CONFIG_PRESENCE_BRIDGE_COMMISSIONING_RESERVE_KB before commissioning
// (BLE and the commissioning itself need it), CONFIG_PRESENCE_BRIDGE_HEAP_RESERVE_KB once commissioned (BLE is then
// released). The others wait (STATUS waiting_mask, shown on the scanner's page) and are added as soon as memory
// allows, e.g. right after commissioning.
//
// Everything here runs with the CHIP stack lock held (the Matter task, or a caller holding the lock), except
// matter_bridge_set_commissioned().

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_matter.h>

#include "presence_link_proto.h"

// Before esp_matter::start()
esp_err_t matter_bridge_create_aggregator(esp_matter::node_t *node, uint32_t occupancy_feature);

// After esp_matter::start(): reads the table and restores its devices while memory allows (the others follow from
// matter_bridge_step())
void matter_bridge_load(void);

// One step for a slot: restores its saved device, or brings it to want (nullptr: not known yet). true: done for this
// want; false: to be called again later (not loaded yet, memory, error). *added: a device was created or restored.
bool matter_bridge_step(int slot, const plink_slot_t *want, bool *added);

// matter_bridge_load() done
bool matter_bridge_ready(void);

// Endpoint of the slot's device, 0 if none (or not restored yet)
uint16_t matter_bridge_endpoint(int slot);

// bridged_mask, waiting_mask, error_mask and endpoint[] of the STATUS frame
void matter_bridge_get_status(plink_status_t *status);

// Reachable attribute of every bridged device (false while the scanner is silent)
void matter_bridge_set_reachable(bool reachable);

// Any task: commissioned or not, for the memory reserve
void matter_bridge_set_commissioned(bool commissioned);

// Forgets the table (factory reset)
void matter_bridge_erase(void);

void matter_bridge_log_diagnostics(void);

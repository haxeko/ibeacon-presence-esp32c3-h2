/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// CRC-32 (IEEE 802.3, reflected, the value zlib's crc32() returns): integrity check of the stored beacon
// configuration and of the CONFIG-mode request kept in RTC memory. No ESP-IDF dependency (host tests).

#pragma once

#include <stddef.h>
#include <stdint.h>

uint32_t crc32_ieee(const void *data, size_t len);

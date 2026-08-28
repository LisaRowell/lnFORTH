/*
 * lnFORTH - A portable token threaded figFORTH implementation.
 * Copyright (C) 2026 Lisa Rowell
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>
#include <stdint.h>

constexpr uint8_t EKeyF1      = 128;
constexpr uint8_t EKeyF2      = 129;
constexpr uint8_t EKeyF3      = 130;
constexpr uint8_t EKeyF4      = 131;
constexpr uint8_t EKeyF5      = 132;
constexpr uint8_t EKeyF6      = 133;
constexpr uint8_t EKeyF7      = 134;
constexpr uint8_t EKeyF8      = 135;
constexpr uint8_t EKeyF9      = 136;
constexpr uint8_t EKeyF10     = 137;
constexpr uint8_t EKeyF11     = 138;
constexpr uint8_t EKeyF12     = 139;
constexpr uint8_t EKeyLeft    = 140;
constexpr uint8_t EKeyRight   = 141;
constexpr uint8_t EKeyUp      = 142;
constexpr uint8_t EKeyDown    = 143;
constexpr uint8_t EKeyHome    = 144;
constexpr uint8_t EKeyEnd     = 145;
constexpr uint8_t EKeyPrior   = 146;
constexpr uint8_t EKeyNext    = 147;
constexpr uint8_t EKeyInsert  = 148;
constexpr uint8_t EKeyDelete  = 149;
constexpr uint8_t EKeyUnknown = 255;

extern void XInit();
extern char XKey();
extern uint8_t XEKey();
extern void XEmit(char character);
extern void XCR();
extern uint32_t XMillis();
extern bool XRead(uint8_t *addr, uint32_t blkDiskOffset, size_t length);
extern bool XWrite(uint8_t *addr, uint32_t blkDiskOffset, size_t length);
extern bool XUsing(char *filename, size_t filenameLen);
extern char XEnterKey();
extern char XDeleteKey();

#endif // PLATFORM_H

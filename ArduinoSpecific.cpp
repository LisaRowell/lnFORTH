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

#include "Platform.h"

#include <Arduino.h>

char XKey() {
    if (Serial.available()) {
        return Serial.read();
    } else {
        return 0;
    }
}

void XEmit(char character) {
    Serial.print(character);
}

uint32_t XMillis() {
    return millis();
}

bool XRead(uint8_t *addr, uint32_t blkDiskOffset, size_t length) {
    // Currently not implemented

    return false;
}

bool XWrite(uint8_t *addr, uint32_t blkDiskOffset, size_t length) {
    // Currently not implemented

    return false;
}


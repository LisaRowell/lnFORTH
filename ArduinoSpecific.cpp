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
#include <SD.h>
#include <SPI.h>
#include <stddef.h>
#include <string.h>

#ifdef ARDUINO_TEENSY41
#define HAS_SD
constexpr int chipSelect = BUILTIN_SDCARD;
#endif

static bool sdCardPresent;

constexpr size_t MAX_FILENAME_LEN = 80;
static char blkFilename[MAX_FILENAME_LEN + 1];

void XInit() {
    // The 9600 is ignored, but the call is required
    Serial.begin(9600);
    *blkFilename = 0;

#ifdef HAS_SD
    if (SD.begin(chipSelect)) {
        sdCardPresent = true;
    } else {
        Serial.println("Card failed, or not present");
        sdCardPresent = false;
    }
#endif
}

char XKey() {
    while (!Serial.available()) {
        yield();
    }
    return Serial.read();
}

uint8_t XEKey() {
    char key = XKey();
    if (key == 27) {
        key = XKey();
        if (key == '[') {
            key = XKey();
            switch (key) {
                case 'A':
                    return EKeyUp;
                case 'B':
                    return EKeyDown;
                case 'C':
                    return EKeyRight;
                case 'D':
                    return EKeyLeft;
                case '1':
                    key = XKey();
                    switch (key) {
                        case '7':
                            key = XKey();
                            switch (key) {
                                case '~':
                                    return EKeyF6;
                                default:
                                    return EKeyUnknown;
                            }
                        case '8':
                            key = XKey();
                            switch (key) {
                                case '~':
                                    return EKeyF7;
                                default:
                                    return EKeyUnknown;
                            }
                        case '9':
                            key = XKey();
                            switch (key) {
                                case '~':
                                    return EKeyF8;
                                default:
                                    return EKeyUnknown;
                            }
                        case '~':
                            return EKeyHome;
                        default:
                            return EKeyUnknown;
                    }
                case '2':
                    key = XKey();
                    switch (key) {
                        case '0':
                            key = XKey();
                            switch (key) {
                                case '~':
                                    return EKeyF9;
                                default:
                                    return EKeyUnknown;
                            }
                        case '1':
                            key = XKey();
                            switch (key) {
                                case '~':
                                    return EKeyF10;
                                default:
                                    return EKeyUnknown;
                            }
                        case '4':
                            key = XKey();
                            switch (key) {
                                case '~':
                                    return EKeyF12;
                                default:
                                    return EKeyUnknown;
                            }
                        case '~':
                            return EKeyInsert;
                        default:
                            return EKeyUnknown;
                    }
                case '3':
                    key = XKey();
                    switch (key) {
                        case '~':
                            return EKeyDelete;
                        default:
                            return EKeyUnknown;
                    }
                case '4':
                    key = XKey();
                    switch (key) {
                        case '~':
                            return EKeyEnd;
                        default:
                            return EKeyUnknown;
                    }
                case '5':
                    key = XKey();
                    switch (key) {
                        case '~':
                            return EKeyPrior;
                        default:
                            return EKeyUnknown;
                    }
                case '6':
                    key = XKey();
                    switch (key) {
                        case '~':
                            return EKeyNext;
                        default:
                            return EKeyUnknown;
                    }
                case '[':
                    key = XKey();
                    switch (key) {
                        case 'A':
                            return EKeyF1;
                        case 'B':
                            return EKeyF2;
                        case 'C':
                            return EKeyF3;
                        case 'D':
                            return EKeyF4;
                        case 'E':
                            return EKeyF5;
                        default:
                            return EKeyUnknown;
                    }
                default:
                    return EKeyUnknown;
            }
        } else {
            return EKeyUnknown;
        }
    } else {
        return key;
    }
}

void XEmit(char character) {
    Serial.print(character);
}

void XCR() {
    Serial.print((char)13);
    Serial.print((char)10);
}

uint32_t XMillis() {
    return millis();
}

bool XRead(uint8_t *addr, uint32_t blkDiskOffset, size_t length) {
    if (sdCardPresent && strlen(blkFilename) > 0) {
        File dataFile = SD.open(blkFilename, FILE_READ);
        if (dataFile) {
            if (!dataFile.seek(blkDiskOffset)) {
                return false;
            }

            if (dataFile.read(addr, length) != length) {
                return false;
            }

            dataFile.close();
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool XWrite(uint8_t *addr, uint32_t blkDiskOffset, size_t length) {
    if (sdCardPresent && strlen(blkFilename) > 0) {
        File dataFile = SD.open(blkFilename, FILE_WRITE);
        if (dataFile) {
            if (!dataFile.seek(blkDiskOffset)) {
                return false;
            }

            if (dataFile.write(addr, length) != length) {
                return false;
            }

            dataFile.close();
            return true;
        } else {
            return false;
        }
    } else {
        return false;
    }
}

bool XUsing(char *filename, size_t filenameLen) {
    if (filenameLen > MAX_FILENAME_LEN || filenameLen == 0) {
        return false;
    }

    // Needed due to oddities of WORD
    if (filenameLen == 1 && *filename == 0) {
        return false;
    }

    (void)memcpy(blkFilename, filename, filenameLen);
    *(blkFilename + filenameLen) = 0;

    return true;
}

char XEnterKey() {
    return 0x0d;
}

char XDeleteKey() {
    return 0x7f;
}

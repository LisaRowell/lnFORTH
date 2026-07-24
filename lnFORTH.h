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

#ifndef LNFORTH_H
#define LNFORTH_H

#include <stdint.h>
#include <stddef.h>

#define CELL_SIZE_16_BITS 1

#ifdef CELL_SIZE_16_BITS
typedef uint16_t cell_t;
typedef int16_t scell_t;
typedef uint32_t dcell_t;
typedef int32_t sdcell_t;
const cell_t cellMax = 0xffff;
const cell_t scellMax = 0x7fff;
const cell_t cellPaddingMask = 0x0001;
#else
typedef uint32_t cell_t;
typedef int32_t scell_t;
typedef uint64_t dcell_t;
typedef int64_t sdcell_t;
const cell_t cellMax = 0xffffffff;
const cell_t scellMax = 0x7fffffff;
const cell_t cellPaddingMask = 0x00000003;
#endif

const cell_t trueValue = cellMax;
const cell_t falseValue = 0;

const uint8_t bytesPerCell = sizeof(cell_t);
const uint8_t bitsPerCell = bytesPerCell * 8;

const size_t dataStackSize = 256;
const size_t returnStackSize = 256;
const size_t memorySize = 64*1024;

constexpr bool enableDataStackBoundsCheck = true;
constexpr bool enableReturnStackBoundsCheck = true;

#ifndef ARDUINO
constexpr bool debugStartup        = false;
constexpr bool traceVirtualMachine = false;
constexpr bool logMemoryErrors     = true;
constexpr bool logTokenErrors      = true;
#else
// The codebase current logs using std::cout, which doesn't work on Arduino.
// Later add a better way to display error information so we're less blind
// on embedded platforms.
constexpr bool debugStartup        = false;
constexpr bool traceVirtualMachine = false;
constexpr bool logMemoryErrors     = false;
constexpr bool logTokenErrors      = false;
#endif

enum class Token : cell_t {
    LIT,
    EXEC,
    BRAN,
    ZBRAN,
    PLOOP,
    PPLOO,
    PDO,
    DIGI,
    PFIND,
    ENCL,
    EMIT,
    KEY,
    QTERM,
    MILLIS,
    CR,
    CMOVE,
    USTAR,
    USLAS,
    AND,
    OR,
    XOR,
    SPAT,
    SPSTO,
    RPSTO,
    SEMIS,
    LEAVE,
    TOR,
    RFROM,
    R,
    IPRIME,
    J,
    ZEQU,
    ZLESS,
    PLUS,
    DPLUS,
    DLESS,
    MINUS,
    DMINUS,
    OVER,
    DROP,
    SWAP,
    DUP,
    PICK,
    PSTOR,
    TOGGLE,
    AT,
    CAT,
    STORE,
    CSTORE,
    DOCOL,
    DOCON,
    DOUSE,
    DOVAR,
    ULESS,
    LESS,
    DODOE,
    DREAD,
    DWRITE,
    MON
};

// Similar to other Forth implementations, we store word names with a leading length and use the most
// significant bits as flags
const size_t maxWordNameLength = UINT8_MAX >> 2;
const uint8_t wordValidFlag     = 0x80;
const uint8_t immediateWordFlag = 0x40;
const uint8_t smudgeWordFlag    = 0x20;
const uint8_t nameLengthMask    = 0x1f;

const cell_t TIBX = 0x0100;
const cell_t ORIG = 0x0200;
const cell_t UAREA = (cell_t)(memorySize * bytesPerCell) - 128;
const cell_t NBUFFERS = 3;
const cell_t BUFFER_SIZE = 2048;
const cell_t MEM_BYTES_PER_BUFFER = BUFFER_SIZE + 2 * bytesPerCell;
// Total buffer magnitude, in bytes
const cell_t BMAG = NBUFFERS * MEM_BYTES_PER_BUFFER;
const cell_t DAREA = UAREA - BMAG; // Disk buffer area
const cell_t MAX_BLOCK_NUMBER = 500;
const cell_t maxInputLineLength = 128;

const cell_t tibUserIndex       = 0;
const cell_t widthUserIndex     = 1;
const cell_t warningUserIndex   = 2;
const cell_t fenceUserIndex     = 3;
const cell_t dpUserIndex        = 4;
const cell_t voclUserIndex      = 5;
const cell_t deleteKeyUserIndex = 6;
const cell_t blkUserIndex       = 7;
const cell_t inUserIndex        = 8;
const cell_t outUserIndex       = 9;
const cell_t scrUserIndex       = 10;
const cell_t offsetUserIndex    = 11;
const cell_t contextUserIndex   = 12;
const cell_t currentUserIndex   = 13;
const cell_t stateUserIndex     = 14;
const cell_t baseUserIndex      = 15;
const cell_t dplUserIndex       = 16;
const cell_t fldUserIndex       = 17;
const cell_t cspUserIndex       = 18;
const cell_t rnumUserIndex      = 19;
const cell_t hldUserIndex       = 20;

static inline cell_t CellIndexToByteIndex(cell_t cellIndex) {
    return cellIndex * bytesPerCell;
}

static inline cell_t ByteIndexToCellIndex(cell_t byteIndex) {
    return byteIndex / bytesPerCell;
}

#endif /* LNFORTH_H */

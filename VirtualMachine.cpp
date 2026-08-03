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

#include "VirtualMachine.h"
#include "lnFORTH.h"
#include "Platform.h"

#include <stdint.h>
#include <stddef.h>

#include <iostream>
#include <iomanip>

enum class TokenResult : uint16_t {
    OK = 0,
    // These should match the errors in screen 4
    STACK_UNDERRUN = 1,
    STACK_OVERRUN = 7,
    RSTACK_UNDERRUN = 11,
    RSTACK_OVERRUN = 12,
    ILLEGAL_ACCESS = 5,
    UNIMPLEMENTED_TOKEN = 14
};

cell_t stack[dataStackSize];
cell_t rStack[returnStackSize];

union Mem {
    cell_t cell[memorySize];
    uint8_t byte[memorySize * bytesPerCell];
} memory;

cell_t sp;
cell_t rp;
cell_t ip;
cell_t w;

// This is a bad idea in an system on a chip. Get rid of it.
struct addrFormat {
    cell_t cell;

    addrFormat(cell_t x) : cell{x} {}
};

inline std::ostream& operator<<(std::ostream &out, addrFormat const &addr) {
    std::ios outState(NULL);
    outState.copyfmt(out);

    out << "0x" << std::hex << std::setfill('0') << std::setw(bytesPerCell * 2) << addr.cell;

    out.copyfmt(outState);
    return out;
}

TokenResult CheckStack(cell_t needs, cell_t adds) {
    if (enableDataStackBoundsCheck) {
        if (needs && (sp == cellMax || sp + 1 < needs)) {
            return TokenResult::STACK_UNDERRUN;
        }

        if (((sp == cellMax) && (adds > dataStackSize)) ||
            ((sp != cellMax) && (adds + sp >= dataStackSize))) {
            return TokenResult::STACK_OVERRUN;
        }
    }

    return TokenResult::OK;
}

#define CHECK_STACK(needs, adds)                                        \
{                                                                       \
    TokenResult csError;                                                \
    if ((csError = CheckStack((needs), (adds))) != TokenResult::OK) {   \
        return csError;                                                 \
    }                                                                   \
}

TokenResult CheckReturnStack(cell_t needs, cell_t adds) {
    if (enableReturnStackBoundsCheck) {
        if (needs && (rp == cellMax || rp  + 1< needs)) {
            return TokenResult::RSTACK_UNDERRUN;
        }

        if (((rp == cellMax) && (adds > returnStackSize)) ||
            ((rp != cellMax) && (adds + rp >= returnStackSize))) {
            return TokenResult::RSTACK_OVERRUN;
        }
    }

    return TokenResult::OK;
}

#define CHECK_RETURN_STACK(needs, adds)                                         \
{                                                                               \
    TokenResult crsError;                                                       \
    if ((crsError = CheckReturnStack((needs), (adds))) != TokenResult::OK) {    \
        return crsError;                                                        \
    }                                                                           \
}

inline TokenResult CheckAddr(cell_t addr) {
    if (addr > memorySize * bytesPerCell) {
        if (logMemoryErrors) {
            std::cerr << "Attempt to access illegal memory address "
                      << addrFormat(addr)
                      << " ip=" << addrFormat(CellIndexToByteIndex(ip))
                      << " w=" << addrFormat(addr) << std::endl;
        }
        return TokenResult::ILLEGAL_ACCESS;
    }

    return TokenResult::OK;
}

#define CHECK_ADDR(addr)                                    \
{                                                           \
    TokenResult caError;                                    \
    if ((caError = CheckAddr((addr))) != TokenResult::OK) { \
        return caError;                                     \
    }                                                       \
}

TokenResult LIT() {
    CHECK_STACK(0, 1);

    stack[++sp] = memory.cell[ip++];

    return TokenResult::OK;
}

TokenResult ExecuteTokenNonInline(Token token);
TokenResult EXEC() {
    CHECK_STACK(1, 0);

    w = stack[sp--];
    Token token = static_cast<Token>(memory.cell[ByteIndexToCellIndex(w)]);

    return ExecuteTokenNonInline(token);
}

TokenResult BRAN() {
    ip += (scell_t)memory.cell[ip] / bytesPerCell;

    return TokenResult::OK;
}

TokenResult ZBRAN() {
    CHECK_STACK(1, 0);

    if(stack[sp--] == 0) {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
    } else {
        ip++;
    }

    return TokenResult::OK;
}

TokenResult PLOOP() {
    CHECK_RETURN_STACK(2, 0);

    rStack[rp] += 1;
    if ((scell_t)rStack[rp] < (scell_t)rStack[rp - 1]) {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
    } else {
        ip++;
        rp -= 2;
    }

    return TokenResult::OK;
}

TokenResult PPLOO() {
    CHECK_STACK(1, 0);
    CHECK_RETURN_STACK(2, 0);

    scell_t n = (scell_t)stack[sp--];
    scell_t limit = (scell_t)rStack[rp - 1];
    scell_t count = (scell_t)rStack[rp];

    count += n;
    if (((n < 0) && (count < limit)) || ((n > 0) && (count >= limit))) {
        ip++;
        rp -= 2;
    } else {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
        rStack[rp] = (cell_t)count;
    }

    return TokenResult::OK;
}

TokenResult PDO() {
    CHECK_STACK(2, 0);
    CHECK_RETURN_STACK(0, 2);

    rStack[++rp] = stack[sp - 1];
    rStack[++rp] = stack[sp];
    sp -= 2;

    return TokenResult::OK;
}

TokenResult DIGI() {
    CHECK_STACK(2, 0);

    cell_t base = stack[sp--];
    char ch = (char)stack[sp--];
    char value;
    if (ch < '0') {
        stack[++sp] = falseValue;
        return TokenResult::OK;
    } else if (ch <= '9') {
        value = ch - '0';
    } else if (ch < 'A') {
        stack[++sp] = falseValue;
        return TokenResult::OK;
    } else if (ch <= 'Z') {
        value = 10 + ch - 'A';
    } else if (ch < 'a') {
        stack[++sp] = falseValue;
        return TokenResult::OK;
    } else if (ch <= 'z') {
        value = 10 + ch - 'a';
    } else {
        stack[++sp] = falseValue;
        return TokenResult::OK;
    }

    if (value >= base) {
        stack[++sp] = falseValue;
    } else {
        stack[++sp] = (cell_t)value;
        stack[++sp] = trueValue;
    }

    return TokenResult::OK;
}

bool WordNamesMatch(cell_t dictEntry, cell_t wordName) {
    // We include the smudge bit in the dictionary entries length field to
    // prevent matches on incomplete words.
    uint8_t dictLength = memory.byte[dictEntry++] & (nameLengthMask | smudgeWordFlag);
    uint8_t wordLength = memory.byte[wordName++] & nameLengthMask;

    if (dictLength != wordLength) {
        return false;
    } else {
        while (dictLength--) {
            if ((memory.byte[dictEntry++] & 0xff) != memory.byte[wordName++]) {
                return false;
            }
        }
        return true;
    }
}

inline uint8_t NameFieldLength(cell_t dictEntry) {
    uint8_t length = (memory.byte[dictEntry] & nameLengthMask) + 1;
    if (length & cellPaddingMask) {
        length = (length & (uint8_t)(~cellPaddingMask)) + bytesPerCell;
    }
    return length;
}

TokenResult PFIND() {
    CHECK_STACK(2, 0);
    cell_t dictEntry = stack[sp--];
    cell_t name = stack[sp--];

    do {
        if (WordNamesMatch(dictEntry, name)) {
            stack[++sp] = (cell_t)(dictEntry + NameFieldLength(dictEntry) +
                                    bytesPerCell);
            stack[++sp] = memory.byte[dictEntry];
            stack[++sp] = trueValue;
            return TokenResult::OK;
        }

        cell_t linkField = dictEntry + NameFieldLength(dictEntry);
        dictEntry = memory.cell[ByteIndexToCellIndex(linkField)];
        CHECK_ADDR(dictEntry);
    } while (dictEntry != 0);

    stack[++sp] = falseValue;

    return TokenResult::OK;
}

TokenResult ENCL() {
    CHECK_STACK(2, 2);

    char c = (char)stack[sp--];
    cell_t addr = stack[sp];

    cell_t n1 = 0;
    while (memory.byte[addr + n1] == c && memory.byte[addr + n1] != 0) {
        n1++;
    }
    stack[++sp] = n1;

    if (memory.byte[addr + n1] == 0) {
        stack[++sp] = n1 + 1;    // This might not be right. Allows for matching of NULL word
        stack[++sp] = n1;
        return TokenResult::OK;
    }

    cell_t n2 = n1 + 1;
    while (memory.byte[addr + n2] != c && memory.byte[addr + n2] != 0) {
        n2++;
    }
    stack[++sp] = n2;

    if (memory.byte[addr + n2] == 0) {
        stack[++sp] = n2;
        return TokenResult::OK;
    } else {
        stack[++sp] = n2 + 1;
    }

    return TokenResult::OK;
}

TokenResult EMIT() {
    CHECK_STACK(1, 0);

    char character = (char)stack[sp--];
    XEmit(character);

    memory.cell[ByteIndexToCellIndex(UAREA) + outUserIndex]++;

    return TokenResult::OK;
}

TokenResult KEY() {
    CHECK_STACK(0, 1);

    char key = XKey();
    stack[++sp] = (cell_t)key;

    return TokenResult::OK;
}

TokenResult QTERM() {
    // It's not clear that this functionality is going to be implementable in modern systems.
    // For now, just return false.
    CHECK_STACK(0, 1);

    stack[++sp] = 0;

    return TokenResult::OK;
}

TokenResult MILLIS() {
    CHECK_STACK(0, 1);

    stack[++sp] = (cell_t)XMillis();

    return TokenResult::OK;
}

TokenResult CR() {
    XCR();

    return TokenResult::OK;
}

TokenResult CMOVE() {
    CHECK_STACK(3, 0);

    cell_t count = stack[sp--];
    cell_t to = stack[sp--];
    cell_t from = stack[sp--];
    while (count--) {
        memory.byte[to++] = memory.byte[from++];
    }

    return TokenResult::OK;
}

TokenResult USTAR() {
    CHECK_STACK(2, 0);

    cell_t u1 = stack[sp];
    cell_t u2 = stack[sp - 1];
    dcell_t prod = u1 * u2;
    stack[sp - 1] = (cell_t)(prod & cellMax);
    stack[sp] = (cell_t)(prod >> bitsPerCell);

    return TokenResult::OK;
}

TokenResult USLAS() {
    CHECK_STACK(3, 0);

    dcell_t ud = (dcell_t)stack[sp - 2] | (((dcell_t)stack[sp - 1]) << bitsPerCell);
    cell_t u1 = stack[sp--];
    cell_t u2 = (cell_t)(ud % u1);
    cell_t u3 = (cell_t)(ud / u1);
    stack[sp - 1] = u2;
    stack[sp] = u3;

    return TokenResult::OK;
}

TokenResult AND() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a & b;

    return TokenResult::OK;
}

TokenResult OR() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a | b;

    return TokenResult::OK;
}

TokenResult XOR() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a ^ b;

    return TokenResult::OK;
}

TokenResult SPAT() {
    CHECK_STACK(0, 1);
    cell_t pos = sp;
    stack[++sp] = pos;

    return TokenResult::OK;
}

TokenResult SPSTO() {
    // In the source figFORTH listing SP! sets the stack pointer to the contents of a
    // hidden user variable. Here we just set it to -1 (our empty) as the stack isn't addressable
    // anyway.
    sp = cellMax;

    return TokenResult::OK;
}

TokenResult RPSTO() {
    // In the source figFORTH listing SP! sets the return stack pointer to the contents of a
    // hidden user variable. Here we just set it to -1 (our empty) as the stack isn't addressable
    // anyway.
    rp = cellMax;

    return TokenResult::OK;
}

TokenResult SEMIS() {
    CHECK_RETURN_STACK(1, 0);

    ip = ByteIndexToCellIndex(rStack[rp--]);

    return TokenResult::OK;
}

TokenResult LEAVE() {
    CHECK_RETURN_STACK(2, 0);

    rStack[rp - 1] = rStack[rp];

    return TokenResult::OK;
}

TokenResult TOR() {
    CHECK_STACK(1, 0);
    CHECK_RETURN_STACK(0, 1);

    rStack[++rp] = stack[sp--];

    return TokenResult::OK;
}

TokenResult RFROM() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(1, 0);

    stack[++sp] = rStack[rp--];

    return TokenResult::OK;
}

TokenResult R() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(1, 0);

    stack[++sp] = rStack[rp];

    return TokenResult::OK;
}

TokenResult IPrime() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(2, 0);

    stack[++sp] = rStack[rp - 1];

    return TokenResult::OK;
}

TokenResult J() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(3, 0);

    stack[++sp] = rStack[rp - 2];

    return TokenResult::OK;
}

TokenResult ZEQU() {
    CHECK_STACK(1, 0);

    stack[sp] = stack[sp] == 0 ? trueValue : falseValue;

    return TokenResult::OK;
}

TokenResult ZLESS() {
    CHECK_STACK(1, 0);

    stack[sp] = (scell_t)stack[sp] < 0 ? trueValue : falseValue;

    return TokenResult::OK;
}

TokenResult PLUS() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a + b;

    return TokenResult::OK;
}

TokenResult DPLUS() {
    CHECK_STACK(4, 0);

    dcell_t d1 = (dcell_t)stack[sp - 3] | (((dcell_t)stack[sp - 2]) << bitsPerCell);
    dcell_t d2 = (dcell_t)stack[sp - 1] | (((dcell_t)stack[sp]) << bitsPerCell);
    sp -= 2;
    dcell_t dsum = d1 + d2;
    stack[sp - 1] = (cell_t)(dsum & cellMax);
    stack[sp] = (cell_t)(dsum >> bitsPerCell);

    return TokenResult::OK;
}

TokenResult DLESS() {
    CHECK_STACK(4, 0);

    sdcell_t d1 = (sdcell_t)stack[sp - 3] | (((sdcell_t)stack[sp - 2]) << bitsPerCell);
    sdcell_t d2 = (sdcell_t)stack[sp - 1] | (((sdcell_t)stack[sp]) << bitsPerCell);
    sp -= 3;

    cell_t result = d1 < d2 ? trueValue : falseValue;

    stack[sp] = result;

    return TokenResult::OK;
}

TokenResult MINUS() {
    CHECK_STACK(1, 0);

    stack[sp] = (~stack[sp]) + 1;

    return TokenResult::OK;
}

TokenResult DMINUS() {
    CHECK_STACK(2, 0);

    dcell_t d1 = (dcell_t)stack[sp - 1] | (((dcell_t)stack[sp]) << bitsPerCell);
    dcell_t d2 = (~d1) + 1;
    stack[sp - 1] = (cell_t)(d2 & cellMax);
    stack[sp] = (cell_t)(d2 >> bitsPerCell);

    return TokenResult::OK;
}

TokenResult OVER() {
    CHECK_STACK(2, 1);

    stack[sp + 1] = stack[sp - 1];
    sp++;

    return TokenResult::OK;
}

TokenResult DROP() {
    CHECK_STACK(1, 0);

    sp--;

    return TokenResult::OK;
}

TokenResult SWAP() {
    CHECK_STACK(2, 0);

    cell_t temp = stack[sp - 1];
    stack[sp - 1] = stack[sp];
    stack[sp] = temp;

    return TokenResult::OK;
}

TokenResult DUP() {
    CHECK_STACK(1, 1);

    stack[sp + 1] = stack[sp];
    sp++;

    return TokenResult::OK;
}

TokenResult PICK() {
    CHECK_STACK(1, 0);

    cell_t pos = stack[sp--];
    CHECK_STACK(pos + 1, 0);
    cell_t value = stack[sp - pos];
    stack[++sp] = value;

    return TokenResult::OK;
}

TokenResult PSTOR() {
    CHECK_STACK(2, 0);

    cell_t addr = stack[sp--];
    cell_t n = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] += n;

    return TokenResult::OK;
}

TokenResult TOGGLE() {
    CHECK_STACK(2, 0);

    cell_t b = stack[sp--];
    cell_t addr = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] ^= b;

    return TokenResult::OK;
}

TokenResult AT() {
    CHECK_STACK(1, 0);

    cell_t addr = stack[sp--];

    CHECK_ADDR(addr);
    cell_t n = memory.cell[ByteIndexToCellIndex(addr)];
    stack[++sp] = n;

    return TokenResult::OK;
}

TokenResult CAT() {
    CHECK_STACK(1, 0);

    cell_t addr = stack[sp--];

    CHECK_ADDR(addr);
    uint8_t n = memory.byte[addr];
    stack[++sp] = (cell_t)n;

    return TokenResult::OK;
}

TokenResult STORE() {
    CHECK_STACK(2, 0);

    cell_t addr = stack[sp--];
    cell_t n = stack[sp--];

    CHECK_ADDR(addr);
    memory.cell[ByteIndexToCellIndex(addr)] = n;

    return TokenResult::OK;
}

TokenResult CSTORE() {
    CHECK_STACK(2, 0);

    cell_t addr = stack[sp--];
    cell_t value = stack[sp--];

    CHECK_ADDR(addr);
    memory.byte[addr] = (uint8_t)value;

    return TokenResult::OK;
}

TokenResult DOCOL() {
    CHECK_RETURN_STACK(0, 1);

    rStack[++rp] = CellIndexToByteIndex(ip);
    ip = ByteIndexToCellIndex(w) + 1;

    return TokenResult::OK;
}

TokenResult DOCON() {
    CHECK_STACK(0, 1);

    stack[++sp] = memory.cell[ByteIndexToCellIndex(w) + 1];

    return TokenResult::OK;
}

TokenResult DOVAR() {
    CHECK_STACK(0, 1);

    stack[++sp] = w + bytesPerCell;

    return TokenResult::OK;
}

TokenResult DOUSE() {
    CHECK_STACK(0, 1);

    stack[++sp] = UAREA + memory.cell[ByteIndexToCellIndex(w) + 1];

    return TokenResult::OK;
}

TokenResult ULESS() {
    CHECK_STACK(2, 0);

    cell_t n2 = stack[sp--];
    cell_t n1 = stack[sp--];
    cell_t f = n1 < n2 ? trueValue : falseValue;
    stack[++sp] = f;

    return TokenResult::OK;
}

TokenResult LESS() {
    CHECK_STACK(2, 0);

    scell_t n2 = (scell_t)stack[sp--];
    scell_t n1 = (scell_t)stack[sp--];
    cell_t f = n1 < n2 ? trueValue : falseValue;
    stack[++sp] = f;

    return TokenResult::OK;
}

TokenResult DODOE() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(0, 1);

    rStack[++rp] = CellIndexToByteIndex(ip);
    ip = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(w) + 1]);
    stack[++sp] = w + (bytesPerCell * 2);

    return TokenResult::OK;
}

TokenResult DREAD() {
    CHECK_STACK(2, 0);

    cell_t blk = stack[sp--];
    cell_t addr = stack[sp--];
    uint32_t blkDiskOffset = blk * BUFFER_SIZE;

    bool result = XRead(&memory.byte[addr], blkDiskOffset, BUFFER_SIZE);
    stack[++sp] = result ? trueValue : falseValue;

    return TokenResult::OK;
}

TokenResult DWRITE() {
    CHECK_STACK(2, 0);

    cell_t blk = stack[sp--];
    cell_t addr = stack[sp--];
    uint32_t blkDiskOffset = blk * BUFFER_SIZE;

    bool result = XWrite(&memory.byte[addr], blkDiskOffset, BUFFER_SIZE);
    stack[++sp] = result ? trueValue : falseValue;

    return TokenResult::OK;
}

TokenResult MON() {
    (void)CR();

    std::exit(0);
}

const char *TokenName(Token token) {
    switch (token) {
    case Token::LIT:
        return "LIT";
    case Token::EXEC:
        return "EXECUTE";
    case Token::BRAN:
        return "BRAN";
    case Token::ZBRAN:
        return "ZBRAN";
    case Token::PLOOP:
        return "PLOOP";
    case Token::PPLOO:
        return "PPLOO";
    case Token::PDO:
        return "PDO";
    case Token::DIGI:
        return "DIGI";
    case Token::PFIND:
        return "PFIND";
    case Token::ENCL:
        return "ENCL";
    case Token::EMIT:
        return "EMIT";
    case Token::KEY:
        return "KEY";
    case Token::QTERM:
        return "QTERM";
    case Token::MILLIS:
        return "MILLIS";
    case Token::CR:
        return "CR";
    case Token::CMOVE:
        return "CMOVE";
    case Token::USTAR:
        return "USTAR";
    case Token::USLAS:
        return "USLAS";
    case Token::AND:
        return "AND";
    case Token::OR:
        return "OR";
    case Token::XOR:
        return "XOR";
    case Token::SPAT:
        return "SPAT";
    case Token::SPSTO:
        return "SPSTO";
    case Token::RPSTO:
        return "RPSTO";
    case Token::SEMIS:
        return "SEMIS";
    case Token::LEAVE:
        return "LEAVE";
    case Token::TOR:
        return "TOR";
    case Token::RFROM:
        return "RFROM";
    case Token::R:
        return "R";
    case Token::IPRIME:
        return "I'";
    case Token::J:
        return "J";
    case Token::ZEQU:
        return "ZEQU";
    case Token::ZLESS:
        return "ZLESS";
    case Token::PLUS:
        return "PLUS";
    case Token::DPLUS:
        return "DPLUS";
    case Token::DLESS:
        return "DLESS";
    case Token::MINUS:
        return "MINUS";
    case Token::DMINUS:
        return "DMINUS";
    case Token::OVER:
        return "OVER";
    case Token::DROP:
        return "DROP";
    case Token::SWAP:
        return "SWAP";
    case Token::DUP:
        return "DUP";
    case Token::PICK:
        return "PICK";
    case Token::PSTOR:
        return "PSTOR";
    case Token::TOGGLE:
        return "TOGGLE";
    case Token::AT:
        return "AT";
    case Token::CAT:
        return "CAT";
    case Token::STORE:
        return "STORE";
    case Token::CSTORE:
        return "CSTORE";
    case Token::DOCOL:
        return "DOCOL";
    case Token::DOCON:
        return "DOCON";
    case Token::DOUSE:
        return "DOUSE";
    case Token::DOVAR:
        return "DOVAR";
    case Token::ULESS:
        return "ULESS";
    case Token::LESS:
        return "LESS";
    case Token::DODOE:
        return "DODOE";
    case Token::DREAD:
        return "DREAD";
    case Token::DWRITE:
        return "DWRITE";
    case Token::MON:
        return "MON";
    }
    return "Unknown";
};

inline TokenResult executeToken(Token token) {
    switch(token) {
    case Token::LIT:
        return LIT();
    case Token::EXEC:
        return EXEC();
    case Token::BRAN:
        return BRAN();
    case Token::ZBRAN:
        return ZBRAN();
    case Token::PLOOP:
        return PLOOP();
    case Token::PPLOO:
        return PPLOO();
    case Token::PDO:
        return PDO();
    case Token::DIGI:
        return DIGI();
    case Token::PFIND:
        return PFIND();
    case Token::ENCL:
        return ENCL();
    case Token::EMIT:
        return EMIT();
    case Token::KEY:
        return KEY();
    case Token::QTERM:
        return QTERM();
    case Token::MILLIS:
        return MILLIS();
    case Token::CR:
        return CR();
    case Token::CMOVE:
        return CMOVE();
    case Token::USTAR:
        return USTAR();
    case Token::USLAS:
        return USLAS();
    case Token::AND:
        return AND();
    case Token::OR:
        return OR();
    case Token::XOR:
        return XOR();
    case Token::SPAT:
        return SPAT();
    case Token::SPSTO:
        return SPSTO();
    case Token::RPSTO:
        return RPSTO();
    case Token::SEMIS:
        return SEMIS();
    case Token::LEAVE:
        return LEAVE();
    case Token::TOR:
        return TOR();
    case Token::RFROM:
        return RFROM();
    case Token::R:
        return R();
    case Token::IPRIME:
        return IPrime();
    case Token::J:
        return J();
    case Token::ZEQU:
        return ZEQU();
    case Token::ZLESS:
        return ZLESS();
    case Token::PLUS:
        return PLUS();
    case Token::DPLUS:
        return DPLUS();
    case Token::DLESS:
        return DLESS();
    case Token::MINUS:
        return MINUS();
    case Token::DMINUS:
        return DMINUS();
    case Token::OVER:
        return OVER();
    case Token::DROP:
        return DROP();
    case Token::SWAP:
        return SWAP();
    case Token::DUP:
        return DUP();
    case Token::PICK:
        return PICK();
    case Token::PSTOR:
        return PSTOR();
    case Token::TOGGLE:
        return TOGGLE();
    case Token::AT:
        return AT();
    case Token::CAT:
        return CAT();
    case Token::STORE:
        return STORE();
    case Token::CSTORE:
        return CSTORE();
    case Token::DOCOL:
        return DOCOL();
    case Token::DOCON:
        return DOCON();
    case Token::DOUSE:
        return DOUSE();
    case Token::DOVAR:
        return DOVAR();
    case Token::ULESS:
        return ULESS();
    case Token::LESS:
        return LESS();
    case Token::DODOE:
        return DODOE();
    case Token::DREAD:
        return DREAD();
    case Token::DWRITE:
        return DWRITE();
    case Token::MON:
        return MON();
    default:
        return TokenResult::UNIMPLEMENTED_TOKEN;
    }
}

TokenResult ExecuteTokenNonInline(Token token) {
    return executeToken(token);
}

void InitDictionary(size_t initialDictionarySize,
                    const cell_t *initialDictionary) {
    for (size_t word = 0; word < initialDictionarySize; word++) {
        memory.cell[word] = initialDictionary[word];
    }

    // The dictionary is built platform independent, but there are some
    // user variables that we want to set platform specific.
    memory.cell[ByteIndexToCellIndex(ORIG) + 3 + enterKeyUserIndex] = XEnterKey();
    memory.cell[ByteIndexToCellIndex(ORIG) + 3 + deleteKeyUserIndex] = XDeleteKey();
}

void InitVirtualMachine() {
    cell_t cold = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(ORIG)]) + 1;
    ip = cold;
}

void RunVirtualMachine() {
    while(1) {
        w = memory.cell[ip];
        Token token = static_cast<Token>(memory.cell[ByteIndexToCellIndex(w)]);

        if (traceVirtualMachine) {
            std::cout << "ip = " << addrFormat(CellIndexToByteIndex(ip))
                      << " w = " << addrFormat(w) << " "
                      << TokenName(token) << " (" << static_cast<cell_t>(token) << ")"
                      << " sp = " << sp << " rp = " << rp << std::endl;
        }

        ip++;
        TokenResult result = executeToken(token);
        if (result != TokenResult::OK) {
            rp = cellMax;
            sp = 0;
            stack[sp] = (cell_t)result;
            if (traceVirtualMachine) {
                std::cout << TokenName(token) << " returned " << (cell_t)result << std::endl;
            }
            ip = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(ORIG) + 1]) + 1;
        }
    }
}

void StepVirtualMachine() {
    w = memory.cell[ip];
    Token token = static_cast<Token>(memory.cell[ByteIndexToCellIndex(w)]);

    if (traceVirtualMachine) {
        std::cout << "ip = " << addrFormat(CellIndexToByteIndex(ip))
                  << " w = " << addrFormat(w) << " "
                  << TokenName(token) << " (" << static_cast<cell_t>(token) << ")"
                  << " sp = " << sp << " rp = " << rp << std::endl;
    }

    ip++;
    TokenResult result = executeToken(token);
    if (result != TokenResult::OK) {
        rp = cellMax;
        sp = 0;
        stack[sp] = (cell_t)result;
        if (traceVirtualMachine) {
            std::cout << TokenName(token) << " returned " << (cell_t)result << std::endl;
        }
        ip = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(ORIG) + 1]) + 1;
    }
}

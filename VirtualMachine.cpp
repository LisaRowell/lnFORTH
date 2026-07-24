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

#include <stddef.h>

#include <iostream>
#include <iomanip>

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

cell_t CheckStack(cell_t needs, cell_t adds) {
    if (enableDataStackBoundsCheck) {
        if (needs && (sp == cellMax || sp + 1 < needs)) {
            return 1;
        }

        if (((sp == cellMax) && (adds > dataStackSize)) ||
            ((sp != cellMax) && (adds + sp >= dataStackSize))) {
            return 2;
        }
    }

    return 0;
}

#define CHECK_STACK(needs, adds)                            \
{                                                       \
        cell_t csError;                                     \
        if ((csError = CheckStack((needs), (adds))) != 0) { \
            return csError;                                 \
    }                                                   \
}

cell_t CheckReturnStack(cell_t needs, cell_t adds) {
    if (enableReturnStackBoundsCheck) {
        if (needs && (rp == cellMax || rp  + 1< needs)) {
            return 11;
        }

        if (((rp == cellMax) && (adds > returnStackSize)) ||
            ((rp != cellMax) && (adds + rp >= returnStackSize))) {
            return 12;
        }
    }

    return 0;
}

#define CHECK_RETURN_STACK(needs, adds)                        \
cell_t crsError;                                               \
    if ((crsError = CheckReturnStack((needs), (adds))) != 0) { \
        return crsError;                                       \
}

inline cell_t CheckAddr(cell_t addr) {
    if (addr > memorySize * bytesPerCell) {
        std::cerr << "Attempt to access illegal memory address "
                  << addrFormat(addr)
                  << " ip=" << addrFormat(CellIndexToByteIndex(ip))
                  << " w=" << addrFormat(addr) << std::endl;
        return 5;
    }

    return 0;
}

#define CHECK_ADDR(addr)                      \
cell_t caError;                               \
    if ((caError = CheckAddr((addr))) != 0) { \
        return caError;                       \
}

cell_t LIT() {
    CHECK_STACK(0, 1);

    stack[++sp] = memory.cell[ip++];

    return 0;
}

cell_t ExecuteTokenNonInline(Token token);
cell_t EXEC() {
    CHECK_STACK(1, 0);

    w = stack[sp--];
    Token token = static_cast<Token>(memory.cell[ByteIndexToCellIndex(w)]);

    return ExecuteTokenNonInline(token);
}

cell_t BRAN() {
    ip += (scell_t)memory.cell[ip] / bytesPerCell;

    return 0;
}

cell_t ZBRAN() {
    CHECK_STACK(1, 0);

    if(stack[sp--] == 0) {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
    } else {
        ip++;
    }

    return 0;
}

cell_t PLOOP() {
    CHECK_RETURN_STACK(2, 0);

    rStack[rp] += 1;
    if ((scell_t)rStack[rp] < (scell_t)rStack[rp - 1]) {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
    } else {
        ip++;
        rp -= 2;
    }

    return 0;
}

cell_t PPLOO() {
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

    return 0;
}

cell_t PDO() {
    CHECK_STACK(2, 0);
    CHECK_RETURN_STACK(0, 2);

    rStack[++rp] = stack[sp - 1];
    rStack[++rp] = stack[sp];
    sp -= 2;

    return 0;
}

cell_t DIGI() {
    CHECK_STACK(2, 0);

    cell_t base = stack[sp--];
    char ch = (char)stack[sp--];
    char value;
    if (ch < '0') {
        stack[++sp] = falseValue;
        return 0;
    } else if (ch <= '9') {
        value = ch - '0';
    } else if (ch < 'A') {
        stack[++sp] = falseValue;
        return 0;
    } else if (ch <= 'Z') {
        value = 10 + ch - 'A';
    } else if (ch < 'a') {
        stack[++sp] = falseValue;
        return 0;
    } else if (ch <= 'z') {
        value = 10 + ch - 'a';
    } else {
        stack[++sp] = falseValue;
        return 0;
    }

    if (value >= base) {
        stack[++sp] = falseValue;
    } else {
        stack[++sp] = (cell_t)value;
        stack[++sp] = trueValue;
    }

    return 0;
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

cell_t PFIND() {
    CHECK_STACK(2, 0);
    cell_t dictEntry = stack[sp--];
    cell_t name = stack[sp--];

    do {
        if (WordNamesMatch(dictEntry, name)) {
            stack[++sp] = (cell_t)(dictEntry + NameFieldLength(dictEntry) +
                                    bytesPerCell);
            stack[++sp] = memory.byte[dictEntry];
            stack[++sp] = trueValue;
            return 0;
        }

        cell_t linkField = dictEntry + NameFieldLength(dictEntry);
        dictEntry = memory.cell[ByteIndexToCellIndex(linkField)];
        CHECK_ADDR(dictEntry);
    } while (dictEntry != 0);

    stack[++sp] = falseValue;

    return 0;
}

cell_t ENCL() {
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
        return 0;
    }

    cell_t n2 = n1 + 1;
    while (memory.byte[addr + n2] != c && memory.byte[addr + n2] != 0) {
        n2++;
    }
    stack[++sp] = n2;

    if (memory.byte[addr + n2] == 0) {
        stack[++sp] = n2;
        return 0;
    } else {
        stack[++sp] = n2 + 1;
    }

    return 0;
}

cell_t EMIT() {
    CHECK_STACK(1, 0);

    char character = (char)stack[sp--];
    XEmit(character);

    memory.cell[ByteIndexToCellIndex(UAREA) + outUserIndex]++;

    return 0;
}

cell_t KEY() {
    CHECK_STACK(0, 1);

    char key = XKey();
    stack[++sp] = (cell_t)key;

    return 0;
}

cell_t QTERM() {
    // It's not clear that this functionality is going to be implementable in modern systems.
    // For now, just return false.
    CHECK_STACK(0, 1);

    stack[++sp] = 0;

    return 0;
}

cell_t MILLIS() {
    CHECK_STACK(0, 1);

    stack[++sp] = (cell_t)XMillis();

    return 0;
}

cell_t CR() {
    std::cout << (char)10;

    return 0;
}

cell_t CMOVE() {
    CHECK_STACK(3, 0);

    cell_t count = stack[sp--];
    cell_t to = stack[sp--];
    cell_t from = stack[sp--];
    while (count--) {
        memory.byte[to++] = memory.byte[from++];
    }

    return 0;
}

cell_t USTAR() {
    CHECK_STACK(2, 0);

    cell_t u1 = stack[sp];
    cell_t u2 = stack[sp - 1];
    dcell_t prod = u1 * u2;
    stack[sp - 1] = (cell_t)(prod & cellMax);
    stack[sp] = (cell_t)(prod >> bitsPerCell);

    return 0;
}

cell_t USLAS() {
    CHECK_STACK(3, 0);

    dcell_t ud = (dcell_t)stack[sp - 2] | (((dcell_t)stack[sp - 1]) << bitsPerCell);
    cell_t u1 = stack[sp--];
    cell_t u2 = (cell_t)(ud % u1);
    cell_t u3 = (cell_t)(ud / u1);
    stack[sp - 1] = u2;
    stack[sp] = u3;

    return 0;
}

cell_t AND() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a & b;

    return 0;
}

cell_t OR() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a | b;

    return 0;
}

cell_t XOR() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a ^ b;

    return 0;
}

cell_t SPAT() {
    CHECK_STACK(0, 1);
    cell_t pos = sp;
    stack[++sp] = pos;

    return 0;
}

cell_t SPSTO() {
    // In the source figFORTH listing SP! sets the stack pointer to the contents of a
    // hidden user variable. Here we just set it to -1 (our empty) as the stack isn't addressable
    // anyway.
    sp = cellMax;

    return 0;
}

cell_t RPSTO() {
    // In the source figFORTH listing SP! sets the return stack pointer to the contents of a
    // hidden user variable. Here we just set it to -1 (our empty) as the stack isn't addressable
    // anyway.
    rp = cellMax;

    return 0;
}

cell_t SEMIS() {
    CHECK_RETURN_STACK(1, 0);

    ip = ByteIndexToCellIndex(rStack[rp--]);

    return 0;
}

cell_t LEAVE() {
    CHECK_RETURN_STACK(2, 0);

    rStack[rp - 1] = rStack[rp];

    return 0;
}

cell_t TOR() {
    CHECK_STACK(1, 0);
    CHECK_RETURN_STACK(0, 1);

    rStack[++rp] = stack[sp--];

    return 0;
}

cell_t RFROM() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(1, 0);

    stack[++sp] = rStack[rp--];

    return 0;
}

cell_t R() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(1, 0);

    stack[++sp] = rStack[rp];

    return 0;
}

cell_t IPrime() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(2, 0);

    stack[++sp] = rStack[rp - 1];

    return 0;
}

cell_t J() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(3, 0);

    stack[++sp] = rStack[rp - 2];

    return 0;
}

cell_t ZEQU() {
    CHECK_STACK(1, 0);

    stack[sp] = stack[sp] == 0 ? trueValue : falseValue;

    return 0;
}

cell_t ZLESS() {
    CHECK_STACK(1, 0);

    stack[sp] = (scell_t)stack[sp] < 0 ? trueValue : falseValue;

    return 0;
}

cell_t PLUS() {
    CHECK_STACK(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a + b;

    return 0;
}

cell_t DPLUS() {
    CHECK_STACK(4, 0);

    dcell_t d1 = (dcell_t)stack[sp - 3] | (((dcell_t)stack[sp - 2]) << bitsPerCell);
    dcell_t d2 = (dcell_t)stack[sp - 1] | (((dcell_t)stack[sp]) << bitsPerCell);
    sp -= 2;
    dcell_t dsum = d1 + d2;
    stack[sp - 1] = (cell_t)(dsum & cellMax);
    stack[sp] = (cell_t)(dsum >> bitsPerCell);

    return 0;
}

cell_t DLESS() {
    CHECK_STACK(4, 0);

    sdcell_t d1 = (sdcell_t)stack[sp - 3] | (((sdcell_t)stack[sp - 2]) << bitsPerCell);
    sdcell_t d2 = (sdcell_t)stack[sp - 1] | (((sdcell_t)stack[sp]) << bitsPerCell);
    sp -= 3;

    cell_t result = d1 < d2 ? trueValue : falseValue;

    stack[sp] = result;

    return 0;
}

cell_t MINUS() {
    CHECK_STACK(1, 0);

    stack[sp] = (~stack[sp]) + 1;

    return 0;
}

cell_t DMINUS() {
    CHECK_STACK(2, 0);

    dcell_t d1 = (dcell_t)stack[sp - 1] | (((dcell_t)stack[sp]) << bitsPerCell);
    dcell_t d2 = (~d1) + 1;
    stack[sp - 1] = (cell_t)(d2 & cellMax);
    stack[sp] = (cell_t)(d2 >> bitsPerCell);

    return 0;
}

cell_t OVER() {
    CHECK_STACK(2, 1);

    stack[sp + 1] = stack[sp - 1];
    sp++;

    return 0;
}

cell_t DROP() {
    CHECK_STACK(1, 0);

    sp--;

    return 0;
}

cell_t SWAP() {
    CHECK_STACK(2, 0);

    cell_t temp = stack[sp - 1];
    stack[sp - 1] = stack[sp];
    stack[sp] = temp;

    return 0;
}

cell_t DUP() {
    CHECK_STACK(1, 1);

    stack[sp + 1] = stack[sp];
    sp++;

    return 0;
}

cell_t PICK() {
    CHECK_STACK(1, 0);

    cell_t pos = stack[sp--];
    CHECK_STACK(pos + 1, 0);
    cell_t value = stack[sp - pos];
    stack[++sp] = value;

    return 0;
}

cell_t PSTOR() {
    CHECK_STACK(2, 0);

    cell_t addr = stack[sp--];
    cell_t n = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] += n;

    return 0;
}

cell_t TOGGLE() {
    CHECK_STACK(2, 0);

    cell_t b = stack[sp--];
    cell_t addr = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] ^= b;

    return 0;
}

cell_t AT() {
    CHECK_STACK(1, 0);

    cell_t addr = stack[sp--];

    CHECK_ADDR(addr);
    cell_t n = memory.cell[ByteIndexToCellIndex(addr)];
    stack[++sp] = n;

    return 0;
}

cell_t CAT() {
    CHECK_STACK(1, 0);

    cell_t addr = stack[sp--];

    CHECK_ADDR(addr);
    uint8_t n = memory.byte[addr];
    stack[++sp] = (cell_t)n;

    return 0;
}

cell_t STORE() {
    CHECK_STACK(2, 0);

    cell_t addr = stack[sp--];
    cell_t n = stack[sp--];

    CHECK_ADDR(addr);
    memory.cell[ByteIndexToCellIndex(addr)] = n;

    return 0;
}

cell_t CSTORE() {
    CHECK_STACK(2, 0);

    cell_t addr = stack[sp--];
    cell_t value = stack[sp--];

    CHECK_ADDR(addr);
    memory.byte[addr] = (uint8_t)value;

    return 0;
}

cell_t DOCOL() {
    CHECK_RETURN_STACK(0, 1);

    rStack[++rp] = CellIndexToByteIndex(ip);
    ip = ByteIndexToCellIndex(w) + 1;

    return 0;
}

cell_t DOCON() {
    CHECK_STACK(0, 1);

    stack[++sp] = memory.cell[ByteIndexToCellIndex(w) + 1];

    return 0;
}

cell_t DOVAR() {
    CHECK_STACK(0, 1);

    stack[++sp] = w + bytesPerCell;

    return 0;
}

cell_t DOUSE() {
    CHECK_STACK(0, 1);

    stack[++sp] = UAREA + memory.cell[ByteIndexToCellIndex(w) + 1];

    return 0;
}

cell_t ULESS() {
    CHECK_STACK(2, 0);

    cell_t n2 = stack[sp--];
    cell_t n1 = stack[sp--];
    cell_t f = n1 < n2 ? trueValue : falseValue;
    stack[++sp] = f;

    return 0;
}

cell_t LESS() {
    CHECK_STACK(2, 0);

    scell_t n2 = (scell_t)stack[sp--];
    scell_t n1 = (scell_t)stack[sp--];
    cell_t f = n1 < n2 ? trueValue : falseValue;
    stack[++sp] = f;

    return 0;
}

cell_t DODOE() {
    CHECK_STACK(0, 1);
    CHECK_RETURN_STACK(0, 1);

    rStack[++rp] = CellIndexToByteIndex(ip);
    ip = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(w) + 1]);
    stack[++sp] = w + (bytesPerCell * 2);

    return 0;
}

cell_t DREAD() {
    CHECK_STACK(2, 0);

    cell_t blk = stack[sp--];
    cell_t addr = stack[sp--];
    uint32_t blkDiskOffset = blk * BUFFER_SIZE;

    bool result = XRead(&memory.byte[addr], blkDiskOffset, BUFFER_SIZE);
    stack[++sp] = result ? trueValue : falseValue;

    return 0;
}

cell_t DWRITE() {
    CHECK_STACK(2, 0);

    cell_t blk = stack[sp--];
    cell_t addr = stack[sp--];
    uint32_t blkDiskOffset = blk * BUFFER_SIZE;

    bool result = XWrite(&memory.byte[addr], blkDiskOffset, BUFFER_SIZE);
    stack[++sp] = result ? trueValue : falseValue;

    return 0;
}

cell_t MON() {
    std::cout << std::endl << "Exiting with stack:";
    for (cell_t i = 0; i <= sp && sp != cellMax; i++) {
        std::cout << " "<< addrFormat(stack[i]);
    }
    std::cout << std::endl;

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

inline cell_t executeToken(Token token) {
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
        std::cerr << "Unimplemented token! (" << static_cast<cell_t>(token)
                  << ") ip=" << addrFormat(ip) << " w=" << addrFormat(w)
                  << std::endl;

        std::exit(1);
    }
}

cell_t ExecuteTokenNonInline(Token token) {
    return executeToken(token);
}

void InitDictionary(size_t initialDictionarySize,
                    const cell_t *initialDictionary) {
    for (size_t word = 0; word < initialDictionarySize; word++) {
        memory.cell[word] = initialDictionary[word];
    }
}

void InitUserMemory() {
    cell_t variablesToInit = memory.cell[ByteIndexToCellIndex(ORIG) + 2];
    for (cell_t i = 0; i < variablesToInit; i++) {
        memory.cell[ByteIndexToCellIndex(UAREA) + i] =
            memory.cell[ByteIndexToCellIndex(ORIG) + 3 + i];
    }
}

void InitVirtualMachine() {
    sp = cellMax;
    rp = cellMax;

    cell_t cold = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(ORIG)]) + 1;
    ip = cold;
}

void RunVirtualMachine() {
    while(1) {
        w = memory.cell[ip];
        Token token = static_cast<Token>(memory.byte[w]);

        if (traceVirtualMachine) {
            std::cout << "ip = " << addrFormat(CellIndexToByteIndex(ip))
            << " w = " << addrFormat(w) << " "
            << TokenName(token) << " (" << static_cast<cell_t>(token) << ")"
            << " sp = " << sp << " rp = " << rp << std::endl;
        }

        ip++;
        cell_t error = executeToken(token);
        if (error != 0) {
            rp = cellMax;
            sp = 0;
            stack[sp] = error;
            if (traceVirtualMachine) {
                std::cout << TokenName(token) << " returned " << error << std::endl;
            }
            ip = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(ORIG) + 1]) + 1;
        }
    }
}

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

#include "../lnFORTH.h"
#include "../Platform.h"

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <iostream>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>

static std::ofstream logFile;

union Mem {
    cell_t cell[memorySize];
    uint8_t byte[memorySize * bytesPerCell];
} memory;

cell_t here;
cell_t forthLastWordReference;
cell_t coldLastWordReference;

// For building the initial dictionary only, we use a standard libary map to find words. It's not used
// after Forth is up and running, then the normal thread of words is used.
std::map<const char *, cell_t> words;

// If a word is referenced before it's defined, we need to keep track of where the usage is for later
// when we define it. At that time all the references are resolved. At end of compilation of the
// initial dictionary, we check for unresolved references and flag them as errors.
std::map<const char *, std::set<cell_t>> forwardReferences;

// Label database to make writing branches simpler and less error prone
std::map<const char *, cell_t> labels;

// A mapping to find DOES> entry points while compiling the initial dictionary.
std::map<const char *, cell_t> doesEntries;
std::map<const char *, std::set<cell_t>> forwardLabelReferences;

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

bool byteIndexCellAligned(cell_t byteIndex) {
    return (byteIndex & (bytesPerCell - 1)) == 0;
}

void copyStringToMemPadded(const char *name, cell_t &byteHere, size_t length) {
    memcpy(&memory.byte[byteHere], name, length);
    byteHere += (cell_t)length;

    while (!byteIndexCellAligned(byteHere)) {
        memory.byte[byteHere++] = 0;
    }
}

void Header(const char *name, uint8_t flags) {
    static cell_t previousWord = 0;

    // Make sure we didn't mess up and define a word twice
    if (words.count(name)) {
        std::cerr << "Attempt to redefine word \"" << name << "\"" << std::endl;
        std::exit(1);
    }

    cell_t wordStart = here;

    size_t nameLength = strlen(name);
    if (nameLength > maxWordNameLength) {
        std::cerr << "Attempt to define a word with too long of a name (\"" << name << "\" " << nameLength
                  << ")" << std::endl;
        std::exit(1);
    }

    cell_t byteHere = CellIndexToByteIndex(here);
    logFile << addrFormat(byteHere) << " " << name << std::endl;

    // Special case a "" name which we use to indicate the special word with a null string
    // as its name.
    if (nameLength == 0) {
        nameLength = 1;
    }
    memory.byte[byteHere++] = (uint8_t)nameLength | wordValidFlag | flags;
    copyStringToMemPadded(name, byteHere, nameLength);

    here = ByteIndexToCellIndex(byteHere);
    memory.cell[here++] = previousWord;
    previousWord = CellIndexToByteIndex(wordStart);

    byteHere = CellIndexToByteIndex(here);
    words[name] = byteHere;

    // If this word was used before now we have forward references to resolve
    auto it = forwardReferences.find(name);
    if (it != forwardReferences.end()) {
        std::set<cell_t> &references = it->second;
        for (auto it2 = references.begin();
             it2 != references.end(); it2++) {
            memory.cell[*it2] = byteHere;
        }
    }
    forwardReferences.erase(name);
}

void Primitive(const char *name, Token token, uint8_t flags = 0) {
    Header(name, flags);
    memory.cell[here++] = static_cast<cell_t>(token);
}

void User(const char *name, cell_t index, uint8_t flags = 0) {
    Header(name, flags);
    memory.cell[here++] = static_cast<cell_t>(Token::DOUSE);
    memory.cell[here++] = CellIndexToByteIndex(index);
}

void Constant(const char *name, cell_t value, uint8_t flags = 0) {
    Header(name, flags);
    memory.cell[here++] = static_cast<cell_t>(Token::DOCON);
    memory.cell[here++] = value;
}

void Variable(const char *name, cell_t value, uint8_t flags = 0) {
    Header(name, flags);
    memory.cell[here++] = static_cast<cell_t>(Token::DOVAR);
    memory.cell[here++] = value;
}

void Colon(const char *name, uint8_t flags = 0) {
    Header(name, flags);
    memory.cell[here++] = static_cast<cell_t>(Token::DOCOL);
}

void DoesEntry(const char *name) {
    doesEntries[name] = CellIndexToByteIndex(here);
}

void DoesWord(const char *name, const char *doesName, uint8_t flags = 0) {
    Header(name, flags);
    memory.cell[here++] = static_cast<cell_t>(Token::DODOE);

    auto it = doesEntries.find(doesName);
    if (it != doesEntries.end()) {
        cell_t doesEntry = it->second;
        memory.cell[here++] = doesEntry;
    } else {
        std::cerr << "Attempt to create DOES word \"" << name << "\" with unknown DOES \""
                  << doesName << "\"" << std::endl;
        std::exit(1);
    }
}

void Word(const char *name) {
    auto it = words.find(name);
    if (it != words.end()) {
        cell_t word = it->second;
        memory.cell[here++] = word;
    } else {
        forwardReferences[name].insert(here);
        memory.cell[here++] = 0;
    }
}

void Comma(cell_t cell) {
    memory.cell[here++] = cell;
}

void String(const char *string) {
    size_t length = strlen(string);
    cell_t byteHere = CellIndexToByteIndex(here);

    memory.byte[byteHere++] = (uint8_t)length;
    copyStringToMemPadded(string, byteHere, length);

    here = ByteIndexToCellIndex(byteHere);
}

// Since we're token threaded, (;CODE) is a little difference in that what follows isn't
// machine code, but rather a single token which will become the token of the created word.
void SemicolonCode(Token token) {
    Word("(;CODE)");
    Comma(static_cast<cell_t>(token));
}

void Label(const char *name) {
    //Make sure we're not making a duplicate
    if (labels.count(name)) {
        std::cerr << "Attempt to redefine label \"" << name << "\"" << std::endl;
        std::exit(1);
    }

    labels[name] = here;

    // If this lavel was used before now we have forward references to resolve
    auto it = forwardLabelReferences.find(name);
    if (it != forwardLabelReferences.end()) {
        std::set<cell_t> &references = it->second;
        for (auto it2 = references.begin();
             it2 != references.end(); it2++) {
            scell_t offset = here - *it2;
            memory.cell[*it2] = (cell_t)(offset * bytesPerCell);
        }
        forwardLabelReferences.erase(name);
    }
}

void OffsetTo(const char *labelName) {
    auto it = labels.find(labelName);
    if (it != labels.end()) {
        cell_t target = it->second;
        scell_t offset = target - here;
        memory.cell[here++] = (cell_t)(offset * bytesPerCell);
    } else {
        forwardLabelReferences[labelName].insert(here);
        memory.cell[here++] = 0;
    }
}

void Branch(const char *labelName) {
    Word("BRANCH");
    OffsetTo(labelName);
}

void ZBranch(const char *labelName) {
    Word("0BRANCH");
    OffsetTo(labelName);
}

void Loop(const char *labelName) {
    Word("(LOOP)");
    OffsetTo(labelName);
}

void PlusLoop(const char *labelName) {
    Word("(+LOOP)");
    OffsetTo(labelName);
}


void DefineCOLON() {
    Colon(":", immediateWordFlag);
    Word("?EXEC");
    Word("!CSP");
    Word("CURRENT");
    Word("@");
    Word("CONTEXT");
    Word("!");
    Word("CREATE");
    Word("SMUDGE");
    Word("]");
    SemicolonCode(Token::DOCOL);
}

void DefineSEMICOLON() {
    Colon(";", immediateWordFlag);
    Word("?CSP");
    Word("COMPILE");
    Word(";S");
    Word("SMUDGE");
    Word("[");
    Word(";S");
}

void DefineCONST() {
    Colon("CONSTANT");
    Word("CREATE");
    Word(",");
    SemicolonCode(Token::DOCON);
}

void DefineVAR() {
    Colon("VARIABLE");
    Word("CREATE");         // This is a departure from the figFORTH model which
    Word("0");              // expected an initial variable value as an input
    Word(",");
    SemicolonCode(Token::DOVAR);
}

void DefineUSER() {
    Colon("USER");
    Word("CONSTANT");
    SemicolonCode(Token::DOUSE);
}

void DefinePORIG() {
    Colon("+ORIGIN");
    Word("LIT");
    Comma(ORIG);
    Word("+");
    Word(";S");
}

void DefineONEP() {
    Colon("1+");
    Word("1");
    Word("+");
    Word(";S");
}

void DefineTWOP() {
    Colon("2+");
    Word("2");
    Word("+");
    Word(";S");
}

void DefineHERE() {
    Colon("HERE");
    Word("DP");
    Word("@");
    Word(";S");
}

void DefineALLOT() {
    Colon("ALLOT");
    Word("DP");
    Word("+!");
    Word(";S");
}

void DefineCOMMA() {
    Colon(",");
    Word("HERE");
    Word("!");
    Word("B/CELL");
    Word("ALLOT");
    Word(";S");
}

void DefineCCOMMA() {
    Colon("C,");
    Word("HERE");
    Word("C!");
    Word("1");
    Word("ALLOT");
    Word(";S");
}

void DefineSUB() {
    Colon("-");
    Word("MINUS");
    Word("+");
    Word(";S");
}

void DefineEQUAL() {
    Colon("=");
    Word("-");
    Word("0=");
    Word(";S");
}

void DefineUGREATER() {
    Colon("U>");
    Word("SWAP");
    Word("U<");
    Word(";S");
}

void DefineGREAT() {
    Colon(">");
    Word("SWAP");
    Word("<");
    Word(";S");
}

void DefineROT() {
    Colon("ROT");
    Word(">R");
    Word("SWAP");
    Word("R>");
    Word("SWAP");
    Word(";S");
}

void DefineSPACE() {
    Colon("SPACE");
    Word("BL");
    Word("EMIT");
    Word(";S");
}

void DefineDDUP() {
    Colon("-DUP");
    Word("DUP");
    ZBranch("L1303");
    Word("DUP");
    Label("L1303");
    Word(";S");
}

void DefineTRAVERSE() {
    Colon("TRAVERSE");
    Word("SWAP");
    Label("L1312");
    Word("OVER");
    Word("+");
    Word("LIT");
    Comma(0x7f);
    Word("OVER");
    Word("C@");
    Word("<");
    ZBranch("L1312");
    Word("SWAP");
    Word("DROP");
    Word(";S");
}

void DefineLATEST() {
    Colon("LATEST");
    Word("CURRENT");
    Word("@");
    Word("@");
    Word(";S");
}

void DefineLFA() {
    Colon("LFA");
    Word("LIT");
    Comma(bytesPerCell * 2);
    Word("-");
    Word(";S");
}

void DefineCFA() {
    Colon("CFA");
    Word("B/CELL");
    Word("-");
    Word(";S");
}

void DefineNFA() {
    Colon("NFA");
    Word("LIT");
    Comma(bytesPerCell + 1);
    Word("-");
    Word("LIT");
    Comma(cellMax);
    Word("TRAVERSE");
    Word(";S");
}

// This implementation is a departure from the figFORTH listing used as source
// Instead of calling TRAVERSE to forward skip over the name, we us the length
// instead. This allows us to avoid setting the high order bit of the last
// character in the name. We also skip padding.
void DefinePFA() {
    Colon("PFA");
    Word("DUP");
    Word("C@");
    Word("LIT");
    Comma(nameLengthMask);
    Word("AND");
    Word("+");
    Word("1+");
    Label("PFA1");
    Word("DUP");
    Word("LIT");
    Comma(cellPaddingMask);
    Word("AND");
    ZBranch("PFA2");
    Word("1+");
    Branch("PFA1");
    Label("PFA2");
    Word("LIT");
    Comma(bytesPerCell * 2);
    Word("+");
    Word(";S");
}

void DefineSCSP() {
    Colon("!CSP");
    Word("SP@");
    Word("CSP");
    Word("!");
    Word(";S");
}

void DefineQERR() {
    Colon("?ERROR");
    Word("SWAP");
    ZBranch("L1406");
    Word("ERROR");
    Branch("L1407");
    Label("L1406");
    Word("DROP");
    Label("L1407");
    Word(";S");
}

void DefineQCOMP() {
    Colon("?COMP");
    Word("STATE");
    Word("@");
    Word("0=");
    Word("LIT");
    Comma(0x11);
    Word("?ERROR");
    Word(";S");
}

void DefineQEXEC() {
    Colon("?EXEC");
    Word("STATE");
    Word("@");
    Word("LIT");
    Comma(0x12);
    Word("?ERROR");
    Word(";S");
}

void DefineQPAIRS() {
    Colon("?PAIRS");
    Word("-");
    Word("LIT");
    Comma(0x13);
    Word("?ERROR");
    Word(";S");
}

void DefineQCSP() {
    Colon("?CSP");
    Word("SP@");
    Word("CSP");
    Word("@");
    Word("-");
    Word("LIT");
    Comma(0x14);
    Word("?ERROR");
    Word(";S");
}

void DefineQLOAD() {
    Colon("?LOADING");
    Word("BLK");
    Word("@");
    Word("0=");
    Word("LIT");
    Comma(0x16);
    Word("?ERROR");
    Word(";S");
}

void DefineCOMPILE() {
    Colon("COMPILE");
    Word("?COMP");
    Word("R>");
    Word("DUP");
    Word("B/CELL");
    Word("+");
    Word(">R");
    Word("@");
    Word(",");
    Word(";S");
}

void DefineLBRAC() {
    Colon("[", immediateWordFlag);
    Word("0");
    Word("STATE");
    Word("!");
    Word(";S");
}

void DefineRBRAC() {
    Colon("]");
    Word("LIT");
    Comma(0xc0);
    Word("STATE");
    Word("!");
    Word(";S");
}

void DefineSMUDG() {
    Colon("SMUDGE");
    Word("LATEST");
    Word("LIT");
    Comma(smudgeWordFlag);
    Word("TOGGLE");
    Word(";S");
}

void DefineHEX() {
    Colon("HEX");
    Word("LIT");
    Comma(16);
    Word("BASE");
    Word("!");
    Word(";S");
}

void DefineDECIMAL() {
    Colon("DECIMAL");
    Word("LIT");
    Comma(10);
    Word("BASE");
    Word("!");
    Word(";S");
}

void DefinePSCOD() {
    Colon("(;CODE)");
    Word("R>");
    Word("@");
    Word("LATEST");
    Word("PFA");
    Word("CFA");
    Word("!");
    Word(";S");
}

void DefineSCODE() {
    Colon(";CODE", immediateWordFlag);
    Word("?CSP");
    Word("COMPILE");
    Word("(;CODE)");
    Word("[");
    Word("SMUDGE");
    Word(";S");
}

void DefineBUILD() {
    Colon("<BUILDS");
    Word("0");
    Word("CONSTANT");
    Word(";S");
}

void DefineDOES() {
    Colon("DOES>");
    Word("R>");
    Word("LATEST");
    Word("PFA");
    Word("!");
    SemicolonCode(Token::DODOE);
}

void DefineCOUNT() {
    Colon("COUNT");
    Word("DUP");
    Word("1+");
    Word("SWAP");
    Word("C@");
    Word(";S");
}

void DefineTYPE() {
    Colon("TYPE");
    Word("-DUP");
    ZBranch("L1651");
    Word("OVER");
    Word("+");
    Word("SWAP");
    Word("(DO)");
    Label("L1644");
    Word("I");
    Word("C@");
    Word("EMIT");
    Loop("L1644");
    Branch("L1652");
    Label("L1651");
    Word("DROP");
    Label("L1652");
    Word(";S");
}

void DefineDTRAI() {
    Colon("-TRAILING");
    Word("DUP");
    Word("0");
    Word("(DO)");
    Label("L1663");
    Word("OVER");
    Word("OVER");
    Word("+");
    Word("1");
    Word("-");
    Word("C@");
    Word("BL");
    Word("-");
    ZBranch("L1676");
    Word("LEAVE");
    Branch("L1678");
    Label("L1676");
    Word("1");
    Word("-");
    Label("L1678");
    Loop("L1663");
    Word(";S");
}

void DefinePDOTQ() {
    Colon("(.\")");
    Word("R");
    Word("COUNT");
    Word("DUP");
    Label("PDOTQ1");
    Word("1+");
    Word("DUP");   // Added to skip padding
    Word("LIT");
    Comma(cellPaddingMask);
    Word("AND");
    Word("0=");
    ZBranch("PDOTQ1"); // End of added code
    Word("R>");
    Word("+");
    Word(">R");
    Word("TYPE");
    Word(";S");
}

void DefineDOTQ() {
    Colon(".\"", immediateWordFlag);
    Word("LIT");
    Comma(0x22);
    Word("STATE");
    Word("@");
    ZBranch("L1719");
    Word("COMPILE");
    Word("(.\")");
    Word("WORD");
    Word("HERE");
    Word("C@");
    Word("LIT");
    Comma(bytesPerCell);
    Word("OVER");
    Word("LIT");
    Comma(cellPaddingMask);
    Word("AND");
    Word("-");
    Word("+");
    Word("ALLOT");
    Branch("L1723");
    Label("L1719");
    Word("WORD");
    Word("HERE");
    Word("COUNT");
    Word("TYPE");
    Label("L1723");
    Word(";S");
}

void DefineEXPEC() {
    Colon("EXPECT");
    Word("OVER");
    Word("+");
    Word("OVER");
    Word("(DO)");
    Label("L1736");
    Word("KEY");
    Word("DUP");
    Word("DELETE-KEY");
    Word("@");
    Word("=");
    ZBranch("L1760");
    Word("DROP");
    Word("LIT");
    Comma(0x08);
    Word("DUP");      // Added from the model as now days 0x08 only moves and
    Word("EMIT");     // doesn't erase.
    Word("SPACE");
    Word("OVER");
    Word("I");
    Word("=");
    Word("DUP");
    Word("R>");
    Word("2");
    Word("-");
    Word("+");
    Word(">R");
    Word("-");
    Branch("L1779");
    Label("L1760");
    Word("DUP");
    Word("ENTER-KEY");
    Word("@");
    Word("=");
    ZBranch("L1772");
    Word("LEAVE");
    Word("DROP");
    Word("BL");
    Word("0");
    Branch("L1773");
    Label("L1772");
    Word("DUP");
    Label("L1773");
    Word("I");
    Word("C!");
    Word("0");
    Word("I");
    Word("1+");
    Word("C!");
    Label("L1779");
    Word("EMIT");
    Loop("L1736");
    Word("DROP");
    Word(";S");
}

void DefineQUERY() {
    Colon("QUERY");
    Word("TIB");
    Word("@");
    Word("LIT");
    Comma(maxInputLineLength);
    Word("EXPECT");
    Word("0");
    Word("IN");
    Word("!");
    Word(";S");
}

// This is special cased by Header to make a one character name with a null
// used to end interpretation of a buffer.
void DefineX() {
    Colon("", immediateWordFlag);
    Word("BLK");
    Word("@");
    ZBranch("L1830");
    Word("1");
    Word("BLK");
    Word("+!");
    Word("0");
    Word("IN");
    Word("!");
    Word("BLK");
    Word("@");
    Word("0");
    Word("B/SCR");
    Word("U/");
    Word("DROP");
    Word("0=");
    ZBranch("L1828");
    Word("?EXEC");
    Word("R>");
    Word("DROP");
    Label("L1828");
    Branch("L1832");
    Label("L1830");
    Word("R>");
    Word("DROP");
    Label("L1832");
    Word(";S");
}

void DefineFILL() {
    Colon("FILL");
    Word("SWAP");
    Word(">R");
    Word("OVER");
    Word("C!");
    Word("DUP");
    Word("1+");
    Word("R>");
    Word("1");
    Word("-");
    Word("CMOVE");
    Word(";S");
}

void DefineERASE() {
    Colon("ERASE");
    Word("0");
    Word("FILL");
    Word(";S");
}

void DefineBLANK() {
    Colon("BLANKS");
    Word("BL");
    Word("FILL");
    Word(";S");
}

void DefineHOLD() {
    Colon("HOLD");
    Word("LIT");
    Comma(cellMax);
    Word("HLD");
    Word("+!");
    Word("HLD");
    Word("@");
    Word("C!");
    Word(";S");
}

void DefinePAD() {
    Colon("PAD");
    Word("HERE");     // PAD is line length plus a cell aboove HERE
    Word("C/L");
    Word("B/CELL");
    Word("+");
    Word("+");
    Word(";S");
}

void DefineWORD() {
    Colon("WORD");
    Word("BLK");
    Word("@");
    ZBranch("L1914");
    Word("BLK");
    Word("@");
    Word("BLOCK");
    Branch("L1916");
    Label("L1914");
    Word("TIB");
    Word("@");
    Label("L1916");
    Word("IN");
    Word("@");
    Word("+");
    Word("SWAP");
    Word("ENCLOSE");
    Word("HERE");
    Word("LIT");
    Comma(0x22);
    Word("BLANKS");
    Word("IN");
    Word("+!");
    Word("OVER");
    Word("-");
    Word(">R");
    Word("R");
    Word("HERE");
    Word("C!");
    Word("+");
    Word("HERE");
    Word("1+");
    Word("R>");
    Word("CMOVE");
    Word(";S");
}

void DefinePNUMB() {
    Colon("(NUMBER)");
    Label("L1971");
    Word("1+");
    Word("DUP");
    Word(">R");
    Word("C@");
    Word("BASE");
    Word("@");
    Word("DIGIT");
    ZBranch("L2001");
    Word("SWAP");
    Word("BASE");
    Word("@");
    Word("U*");
    Word("DROP");
    Word("ROT");
    Word("BASE");
    Word("@");
    Word("U*");
    Word("D+");
    Word("DPL");
    Word("@");
    Word("1+");
    ZBranch("L1998");
    Word("1");
    Word("DPL");
    Word("+!");
    Label("L1998");
    Word("R>");
    Branch("L1971");
    Label("L2001");
    Word("R>");
    Word(";S");
}

void DefineNUMBER() {
    Colon("NUMBER");
    Word("0");
    Word("0");
    Word("ROT");
    Word("DUP");
    Word("1+");
    Word("C@");
    Word("LIT");
    Comma(0x2d);
    Word("=");
    Word("DUP");
    Word(">R");
    ZBranch("NUMBER1");  // Departure from the model to work with -1 as true
    Word("1+");
    Label("NUMBER1");
    Word("LIT");
    Comma(cellMax);
    Label("L2023");
    Word("DPL");
    Word("!");
    Word("(NUMBER)");
    Word("DUP");
    Word("C@");
    Word("BL");
    Word("-");
    ZBranch("L2042");
    Word("DUP");
    Word("C@");
    Word("LIT");
    Comma(0x2e);
    Word("-");
    Word("0");
    Word("?ERROR");
    Word("0");
    Branch("L2023");
    Label("L2042");
    Word("DROP");
    Word("R>");
    ZBranch("L2047");
    Word("DMINUS");
    Label("L2047");
    Word(";S");
}

void DefineDFIND() {
    Colon("-FIND");
    Word("BL");
    Word("WORD");
    Word("HERE");
    Word("CONTEXT");
    Word("@");
    Word("@");
    Word("(FIND)");
    Word("DUP");
    Word("0=");
    ZBranch("L2073");
    Word("DROP");
    Word("HERE");
    Word("LATEST");
    Word("(FIND)");
    Label("L2073");
    Word(";S");
}

void DefinePABOR() {
    Colon("(ABORT)");
    Word("ABORT");
    Word(";S");
}

void DefineERROR() {
    Colon("ERROR");
    Word("WARNING");
    Word("@");
    Word("0<");
    ZBranch("L2096");
    Word("(ABORT)");
    Label("L2096");
    Word("HERE");
    Word("COUNT");
    Word("TYPE");
    Word("(.\")");
    String("  ? ");
    Word("MESSAGE");
    Word("SP!");
    Word("IN");
    Word("@");
    Word("BLK");
    Word("@");
    Word("QUIT");
    Word(";S");
}

// This was able to be simplified from the 6502 source
// since we do not set the MSB of the last character in
// the word name.
void DefineIDDOT() {
    Colon("ID.");
    Word("DUP");
    Word("C@");
    Word("CLIT");
    Comma(nameLengthMask);
    Word("AND");
    Word("SWAP");
    Word("1+");
    Word("SWAP");
    Word("TYPE");
    Word("SPACE");
    Word(";S");
}

void DefineCREAT() {
    Colon("CREATE");
    Word("TIB");
    Word("HERE");
    Word("LIT");
    Comma(0xa0);
    Word("+");
    Word("U<");
    Word("2");
    Word("?ERROR");
    Word("-FIND");
    ZBranch("L2163");
    Word("DROP");
    Word("NFA");
    Word("ID.");
    Word("LIT");
    Comma(4);
    Word("MESSAGE");
    Word("SPACE");
    Label("L2163");
    Word("HERE");
    Word("DUP");
    Word("C@");
    Word("WIDTH");
    Word("@");
    Word("MIN");
    Word("1+");
    Word("ALLOT");
    Word("DUP");
    Word("LIT");
    Comma(0x80);       // Departure from model. : sets the smudge bit itself
    Word("TOGGLE");
    // The following pads out the name field so that the link field is
    // cell aligned.
    Label("CREATEPadLoop");
    Word("HERE");
    Word("LIT");
    Comma(cellPaddingMask);
    Word("AND");
    ZBranch("CREATEPadDone");
    Word("0");
    Word("C,");
    Branch("CREATEPadLoop");
    Label("CREATEPadDone");
    Word("LATEST");
    Word(",");
    Word("CURRENT");
    Word("@");
    Word("!");
    // For token threading we compile a token that has the word return the address
    // of the parameter field (PFA). Words built with a DOES> will overright this.
    Word("LIT");
    Comma(static_cast<cell_t>(Token::DOVAR));
    Word(",");
    Word(";S");
}

void DefineBCOMPILE() {
    Colon("[COMPILE]", immediateWordFlag);
    Word("-FIND");
    Word("0=");
    Word("0");
    Word("?ERROR");
    Word("DROP");
    Word(",");
    Word(";S");
}

void DefineLITER() {
    Colon("LITERAL", immediateWordFlag);
    Word("STATE");
    Word("@");
    ZBranch("L2226");
    Word("COMPILE");
    Word("LIT");
    Word(",");
    Label("L2226");
    Word(";S");
}

void DefineDLIT() {
    Colon("DLITERAL", immediateWordFlag);
    Word("STATE");
    Word("@");
    ZBranch("L2242");
    Word("SWAP");
    Word("LITERAL");
    Word("LITERAL");
    Label("L2242");
    Word(";S");
}

void DefineQSTAC() {
    Colon("?STACK");
    Word("LIT");
    Comma(dataStackSize);
    Word("SP@");
    Word("U<");
    Word("1");
    Word("?ERROR");
    Word("SP@");
    Word("LIT");
    Comma(cellMax-1);
    Word("<");    // Altered from model as -1 is empty stack
    Word("LIT");
    Comma(7);
    Word("?ERROR");
    Word(";S");
}

void DefineINTER() {
    Colon("INTERPRET");
    Label("L2272");
    Word("-FIND");
    ZBranch("L2289");
    Word("STATE");
    Word("@");
    Word("<");
    ZBranch("L2284");
    Word(",");
    Branch("L2286");
    Label("L2284");
    Word("EXECUTE");
    Label("L2286");
    Word("?STACK");
    Branch("L2302");
    Label("L2289");
    Word("HERE");
    Word("NUMBER");
    Word("DPL");
    Word("@");
    Word("1+");
    ZBranch("L2299");
    Word("DLITERAL");
    Branch("L2301");
    Label("L2299");
    Word("DROP");
    Word("LITERAL");
    Label("L2301");
    Word("?STACK");
    Label("L2302");
    Branch("L2272");
}

void DefineIMMEDIATE() {
    Colon("IMMEDIATE");
    Word("LATEST");
    Word("LIT");
    Comma(immediateWordFlag);
    Word("TOGGLE");
    Word(";S");
}

void DefineVOCABULARY() {
    Colon("VOCABULARY");
    Word("<BUILDS");
    Word("LIT");
    Comma(0xA081);        // From original 6502 listing. Beats me what it does
    Word(",");
    Word("CURRENT");
    Word("@");
    Word("CFA");
    Word(",");
    Word("HERE");
    Word("VOC-LINK");
    Word("@");
    Word(",");
    Word("VOC-LINK");
    Word("!");
    Word("DOES>");
    DoesEntry("DOVOC");
    Word("B/CELL");   // Altered from 6502 source to handle variable cell size
    Word("+");        // End of change
    Word("CONTEXT");
    Word("!");
    Word(";S");
}

void DefineFORTH() {
    DoesWord("FORTH", "DOVOC", immediateWordFlag);
    Comma(0xA081);      // I have no idea why this value is here. Copied from 6502 listing
    // We save the current here, so that when we define the final word in the FORTH vocabulary
    // we can fill in its address here to resolve the forward reference.
    forthLastWordReference = here;
    Comma(0);           // Spot for the forward reference
    Comma(0);           // last vocabulary link ends at zero
}

void DefineDEFIN() {
    Colon("DEFINITIONS");
    Word("CONTEXT");
    Word("@");
    Word("CURRENT");
    Word("!");
    Word(";S");
}

void DefinePAREN() {
    Colon("(", immediateWordFlag);
    Word("LIT");
    Comma(0x29);
    Word("WORD");
    Word(";S");
}

void DefineQUIT() {
    Colon("QUIT");
    Word("0");
    Word("BLK");
    Word("!");
    Word("[");
    Label("L2388");
    Word("RP!");
    Word("CR");
    Word("QUERY");
    Word("INTERPRET");
    Word("STATE");
    Word("@");
    Word("0=");
    ZBranch("L2399");
    Word("(.\")");
    String("OK");
    Label("L2399");
    Branch("L2388");
    Word(";S");
}

void DefineABORT() {
    Colon("ABORT");
    Word("SP!");
    Word("DECIMAL");
    Word("DR0");
    Word("CR");
    Word("(.\")");
    String("fig-FORTH  1.0");
    Word("CR");
    Word("FORTH");
    Word("DEFINITIONS");
    Word("QUIT");
}

void DefineSTOD() {
    Colon("S->D");
    Word("DUP");
    Word("0<");
    //    Word("MINUS");
    Word(";S");
}

void DefinePM() {
    Colon("+-");
    Word("0<");
    ZBranch("L2471");
    Word("MINUS");
    Label("L2471");
    Word(";S");
}

void DefineDPM() {
    Colon("D+-");
    Word("0<");
    ZBranch("L2483");
    Word("DMINUS");
    Label("L2483");
    Word(";S");
}

void DefineABS() {
    Colon("ABS");
    Word("DUP");
    Word("+-");
    Word(";S");
}

void DefineDABS() {
    Colon("DABS");
    Word("DUP");
    Word("D+-");
    Word(";S");
}

void DefineMIN() {
    Colon("MIN");
    Word("OVER");
    Word("OVER");
    Word(">");
    ZBranch("L2517");
    Word("SWAP");
    Label("L2517");
    Word("DROP");
    Word(";S");
}

void DefineMAX() {
    Colon("MAX");
    Word("OVER");
    Word("OVER");
    Word("<");
    ZBranch("L2532");
    Word("SWAP");
    Label("L2532");
    Word("DROP");
    Word(";S");
}

void DefineMSTAR() {
    Colon("M*");
    Word("OVER");
    Word("OVER");
    Word("XOR");
    Word(">R");
    Word("ABS");
    Word("SWAP");
    Word("ABS");
    Word("U*");
    Word("R>");
    Word("D+-");
    Word(";S");
}

void DefineMSLAS() {
    Colon("M/");
    Word("OVER");
    Word(">R");
    Word(">R");
    Word("DABS");
    Word("R");
    Word("ABS");
    Word("U/");
    Word("R>");
    Word("R");
    Word("XOR");
    Word("+-");
    Word("SWAP");
    Word("R>");
    Word("+-");
    Word("SWAP");
    Word(";S");
}

void DefineSTAR() {
    Colon("*");
    Word("U*");
    Word("DROP");
    Word(";S");
}

void DefineSLMOD() {
    Colon("/MOD");
    Word(">R");
    Word("S->D");
    Word("R>");
    Word("M/");
    Word(";S");
}
void DefineSLASH() {
    Colon("/");
    Word("/MOD");
    Word("SWAP");
    Word("DROP");
    Word(";S");
}

void DefineMOD() {
    Colon("MOD");
    Word("/MOD");
    Word("DROP");
    Word(";S");
}

void DefineSSMOD() {
    Colon("*/MOD");
    Word(">R");
    Word("M*");
    Word("R>");
    Word("M/");
    Word(";S");
}

void DefineSSLAS() {
    Colon("*/");
    Word("*/MOD");
    Word("SWAP");
    Word("DROP");
    Word(";S");
}

void DefineMSMOD() {
    Colon("M/MOD");
    Word(">R");
    Word("0");
    Word("R");
    Word("U/");
    Word("R>");
    Word("SWAP");
    Word(">R");
    Word("U/");
    Word("R>");
    Word(";S");
}

void DefinePBUF() {
    Colon("+BUF");
    Word("LIT");
    Comma(MEM_BYTES_PER_BUFFER);
    Word("+");
    Word("DUP");
    Word("LIMIT");
    Word("=");
    ZBranch("L2691");
    Word("DROP");
    Word("FIRST");
    Label("L2691");
    Word("DUP");
    Word("PREV");
    Word("@");
    Word("-");
    Word(";S");
}

void DefineUPDAT() {
    Colon("UPDATE");
    Word("PREV");
    Word("@");
    Word("@");
    Word("LIT");
    Comma(scellMax + 1);
    Word("OR");
    Word("PREV");
    Word("@");
    Word("!");
    Word(";S");
}

void DefineFLUSH() {
    Colon("FLUSH");
    Word("LIMIT");
    Word("FIRST");
    Word("-");
    Word("B/BUF");
    Word("LIT");
    Comma(2 * bytesPerCell);
    Word("+");
    Word("/");
    Word("1+");
    Word("0");
    Word("(DO)");
    Label("L2835");
    Word("LIT");
    Comma(scellMax);
    Word("BUFFER");
    Word("DROP");
    Loop("L2835");
    Word(";S");
}

void DefineEMPTYBUFFERS() {
    Colon("EMPTY-BUFFERS");
    Word("FIRST");
    Word("LIMIT");
    Word("OVER");
    Word("-");
    Word("ERASE");
    Word(";S");
}

void DefineDR0() {
    Colon("DR0");
    Word("0");
    Word("OFFSET");
    Word("!");
    Word(";S");
}

void DefineBUFFR() {
    Colon("BUFFER");
    Word("USE");
    Word("@");
    Word("DUP");
    Word(">R");
    Label("L2758");
    Word("+BUF");
    ZBranch("L2758");
    Word("USE");
    Word("!");
    Word("R");
    Word("@");
    Word("0<");
    ZBranch("L2776");
    Word("R");
    Word("B/CELL");
    Word("+");
    Word("R");
    Word("@");
    Word("LIT");
    Comma(scellMax);
    Word("AND");
    Word("0");
    Word("R/W");
    Label("L2776");
    Word("R");
    Word("!");
    Word("R");
    Word("PREV");
    Word("!");
    Word("R>");
    Word("B/CELL");
    Word("+");
    Word(";S");
}

void DefineBLOCK() {
    Colon("BLOCK");
    Word("OFFSET");
    Word("@");
    Word("+");
    Word(">R");
    Word("PREV");
    Word("@");
    Word("DUP");
    Word("@");
    Word("R");
    Word("-");
    Word("DUP");
    Word("+");
    ZBranch("L2830");
    Label("L2805");
    Word("+BUF");
    Word("0=");
    ZBranch("L2818");
    Word("DROP");
    Word("R");
    Word("BUFFER");
    Word("DUP");
    Word("R");
    Word("1");
    Word("R/W");
    Word("B/CELL");
    Word("-");
    Label("L2818");
    Word("DUP");
    Word("@");
    Word("R");
    Word("-");
    Word("DUP");
    Word("+");
    Word("0=");
    ZBranch("L2805");
    Word("DUP");
    Word("PREV");
    Word("!");
    Label("L2830");
    Word("R>");
    Word("DROP");
    Word("B/CELL");
    Word("+");
    Word(";S");
}

void DefinePLINE() {
    Colon("(LINE)");
    Word(">R");
    Word("C/L");
    Word("B/BUF");
    Word("*/MOD");
    Word("R>");
    Word("B/SCR");
    Word("*");
    Word("+");
    Word("BLOCK");
    Word("+");
    Word("C/L");
    Word(";S");
}

void DefineDLINE() {
    Colon(".LINE");
    Word("(LINE)");
    Word("-TRAILING");
    Word("TYPE");
    Word(";S");
}

void DefineMESS() {
    Colon("MESSAGE");
    Word("WARNING");
    Word("@");
    ZBranch("L2888");
    Word("-DUP");
    ZBranch("L2886");
    Word("LIT");
    Comma(4);
    Word("OFFSET");
    Word("@");
    Word("B/SCR");
    Word("/");
    Word("-");
    Word(".LINE");
    Label("L2886");
    Branch("L2891");
    Label("L2888");
    Word("(.\")");
    String("MSG # ");
    Word(".");
    Label("L2891");
    Word(";S");
}

void DefineLOAD() {
    Colon("LOAD");
    Word("BLK");
    Word("@");
    Word(">R");
    Word("IN");
    Word("@");
    Word(">R");
    Word("0");
    Word("IN");
    Word("!");
    Word("B/SCR");
    Word("*");
    Word("BLK");
    Word("!");
    Word("INTERPRET");
    Word("R>");
    Word("IN");
    Word("!");
    Word("R>");
    Word("BLK");
    Word("!");
    Word(";S");
}

void DefineNEXTSCREEN() {
    Colon("-->");
    Word("?LOADING");
    Word("0");
    Word("IN");
    Word("!");
    Word("B/SCR");
    Word("BLK");
    Word("@");
    Word("OVER");
    Word("MOD");
    Word("-");
    Word("BLK");
    Word("+!");
    Word(";S");
}

// Modified from original model
void DefineRSLW() {
    Colon("R/W");
    Word("OVER");
    Word("LIT");
    Comma(MAX_BLOCK_NUMBER);
    Word(">");
    Word("LIT");
    Comma(6);
    Word("?ERROR");
    ZBranch("RSLWWrite");
    Word("DREAD");
    Branch("RSLWErrorCheck");
    Label("RSLWWrite");
    Word("DWRITE");
    Label("RSLWErrorCheck");
    Word("0=");
    Word("LIT");
    Comma(8);
    Word("?ERROR");
    Word(";S");
}

// The figFORTH model defined ' to be immediate, which does not match
// later standards. Here we depart from figFORTH, making ' non-immediate
// and adding an immediate ['].
void DefineTICK() {
    Colon("'");
    Word("-FIND");
    Word("0=");
    Word("0");
    Word("?ERROR");
    Word("DROP");
    Word(";S");
}

// See above comment
void DefineBTICK() {
    Colon("[']", immediateWordFlag);
    Word("'");
    Word("LITERAL");
    Word(";S");
}

void DefineFORG() {
    Colon("FORGET");
    Word("'");
    Word("NFA");
    Word("DUP");
    Word("FENCE");
    Word("@");
    Word("U<");
    Word("LIT");
    Comma(0x15);
    Word("?ERROR");
    Word(">R");
    Word("VOC-LINK");
    Word("@");
    Label("L3220");
    Word("R");
    Word("OVER");
    Word("U<");
    ZBranch("L3225");
    Word("FORTH");
    Word("DEFINITIONS");
    Word("@");
    Word("DUP");
    Word("VOC-LINK");
    Word("!");
    Branch("L3220");
    Label("L3225");
    Word("DUP");
    Word("LIT");
    Comma(bytesPerCell * 2);
    Word("-");
    Label("L3228");
    Word("PFA");
    Word("LFA");
    Word("@");
    Word("DUP");
    Word("R");
    Word("U<");
    ZBranch("L3228");
    Word("OVER");
    Word("LIT");
    Comma(bytesPerCell);
    Word("-");
    Word("!");
    Word("@");
    Word("-DUP");
    Word("0=");
    ZBranch("L3225");
    Word("R>");
    Word("DP");
    Word("!");
    Word(";S");
}

void DefineBACK() {
    Colon("BACK");
    Word("HERE");
    Word("-");
    Word(",");
    Word(";S");
}

void DefineBEGIN() {
    Colon("BEGIN", immediateWordFlag);
    Word("?COMP");
    Word("HERE");
    Word("1");
    Word(";S");
}

void DefineENDIF() {
    Colon("ENDIF", immediateWordFlag);
    Word("?COMP");
    Word("2");
    Word("?PAIRS");
    Word("HERE");
    Word("OVER");
    Word("-");
    Word("SWAP");
    Word("!");
    Word(";S");
}

void DefineTHEN() {
    Colon("THEN", immediateWordFlag);
    Word("ENDIF");
    Word(";S");
}

void DefineDO() {
    Colon("DO", immediateWordFlag);
    Word("COMPILE");
    Word("(DO)");
    Word("HERE");
    Word("3");
    Word(";S");
}

void DefineLOOP() {
    Colon("LOOP", immediateWordFlag);
    Word("3");
    Word("?PAIRS");
    Word("COMPILE");
    Word("(LOOP)");
    Word("BACK");
    Word(";S");
}

void DefinePLOOP() {
    Colon("+LOOP", immediateWordFlag);
    Word("3");
    Word("?PAIRS");
    Word("COMPILE");
    Word("(+LOOP)");
    Word("BACK");
    Word(";S");
}

void DefineUNTIL() {
    Colon("UNTIL", immediateWordFlag);
    Word("1");
    Word("?PAIRS");
    Word("COMPILE");
    Word("0BRANCH");
    Word("BACK");
    Word(";S");
}

void DefineEND() {
    Colon("END", immediateWordFlag);
    Word("UNTIL");
    Word(";S");
}

void DefineAGAIN() {
    Colon("AGAIN", immediateWordFlag);
    Word("1");
    Word("?PAIRS");
    Word("COMPILE");
    Word("BRANCH");
    Word("BACK");
    Word(";S");
}

void DefineREPEAT() {
    Colon("REPEAT", immediateWordFlag);
    Word(">R");
    Word(">R");
    Word("AGAIN");
    Word("R>");
    Word("R>");
    Word("2");
    Word("-");
    Word("ENDIF");
    Word(";S");
}

void DefineIF() {
    Colon("IF", immediateWordFlag);
    Word("COMPILE");
    Word("0BRANCH");
    Word("HERE");
    Word("0");
    Word(",");
    Word("2");
    Word(";S");
}

void DefineELSE() {
    Colon("ELSE", immediateWordFlag);
    Word("2");
    Word("?PAIRS");
    Word("COMPILE");
    Word("BRANCH");
    Word("HERE");
    Word("0");
    Word(",");
    Word("SWAP");
    Word("2");
    Word("ENDIF");
    Word("2");
    Word(";S");
}

void DefineWHILE() {
    Colon("WHILE", immediateWordFlag);
    Word("IF");
    Word("2+");
    Word(";S");
}

void DefineSPACS() {
    Colon("SPACES");
    Word("0");
    Word("MAX");
    Word("-DUP");
    ZBranch("L3455");
    Word("0");
    Word("(DO)");
    Label("L3452");
    Word("SPACE");
    Loop("L3452");
    Label("L3455");
    Word(";S");
}

void DefineBDIGS() {
    Colon("<#");
    Word("PAD");
    Word("HLD");
    Word("!");
    Word(";S");
}

void DefineEDIGS() {
    Colon("#>");
    Word("DROP");
    Word("DROP");
    Word("HLD");
    Word("@");
    Word("PAD");
    Word("OVER");
    Word("-");
    Word(";S");
}

void DefineSIGN() {
    Colon("SIGN");
    Word("ROT");
    Word("0<");
    ZBranch("L3496");
    Word("LIT");
    Comma(0x2d);
    Word("HOLD");
    Label("L3496");
    Word(";S");
}

void DefineDIG() {
    Colon("#");
    Word("BASE");
    Word("@");
    Word("M/MOD");
    Word("ROT");
    Word("LIT");
    Comma(9);
    Word("OVER");
    Word("<");
    ZBranch("L3517");
    Word("LIT");
    Comma(7);
    Word("+");
    Label("L3517");
    Word("LIT");
    Comma(0x30);
    Word("+");
    Word("HOLD");
    Word(";S");
}

void DefineDIGS() {
    Colon("#S");
    Label("L3529");
    Word("#");
    Word("OVER");
    Word("OVER");
    Word("OR");
    Word("0=");
    ZBranch("L3529");
    Word(";S");
}

void DefineDDOTR() {
    Colon("D.R");
    Word(">R");
    Word("SWAP");
    Word("OVER");
    Word("DABS");
    Word("<#");
    Word("#S");
    Word("SIGN");
    Word("#>");
    Word("R>");
    Word("OVER");
    Word("-");
    Word("SPACES");
    Word("TYPE");
    Word(";S");
}

void DefineDDOT() {
    Colon("D.");
    Word("0");
    Word("D.R");
    Word("SPACE");
    Word(";S");
}

void DefineDOTR() {
    Colon(".R");
    Word(">R");
    Word("S->D");
    Word("R>");
    Word("D.R");
    Word(";S");
}

void DefineDOT() {
    Colon(".");
    Word("S->D");
    Word("D.");
    Word(";S");
}

void DefineQUES() {
    Colon("?");
    Word("@");
    Word(".");
    Word(";S");
}

void DefineLIST() {
    Colon("LIST");
    Word("DECIMAL");
    Word("CR");
    Word("DUP");
    Word("SCR");
    Word("!");
    Word("(.\")");
    String("SCR # ");
    Word(".");
    Word("L/SCR");
    Word("0");
    Word("(DO)");
    Label("L3620");
    Word("CR");
    Word("I");
    Word("3");
    Word(".R");
    Word("SPACE");
    Word("I");
    Word("SCR");
    Word("@");
    Word(".LINE");
    Loop("L3620");
    Word("CR");
    Word(";S");
}

// Modified from the source listing to have a CR at the end and to
// not clear the screen immediately after on VT100 devices.
void DefineINDEX() {
    Colon("INDEX");
    Word("CR");
    Word("1+");
    Word("SWAP");
    Word("(DO)");
    Label("L3647");
    Word("I");
    Word("3");
    Word(".R");
    Word("SPACE");
    Word("0");
    Word("I");
    Word(".LINE");
    Word("CR");
    Word("?TERMINAL");
    ZBranch("L3659");
    Word("LEAVE");
    Label("L3659");
    Loop("L3647");
    Word(";S");
}

void DefineTRIAD() {
    Colon("TRIAD");
    Word("3");
    Word("/");
    Word("3");
    Word("*");
    Word("3");
    Word("OVER");
    Word("+");
    Word("SWAP");
    Word("(DO)");
    Label("L3681");
    Word("CR");
    Word("I");
    Word("LIST");
    Loop("L3681");
    Word("CR");
    Word("LIT");
    Comma(0x0f);
    Word("MESSAGE");
    Word("CR");
    Word("LIT");
    Comma(0x0c);
    Word("EMIT");
    Word(";S");
}

// Changed from the 6502 listing to avoid line wrap which
// was caused by a logic error in the original code.
void DefineVLIST() {
    Colon("VLIST");
    Word("C/L");
    Word("OUT");
    Word("!");
    Word("CONTEXT");
    Word("@");
    Word("@");
    Label("VLIST1");
    Word("DUP");
    Word("C@");
    Word("LIT");
    Comma(nameLengthMask);
    Word("AND");
    Word("OUT");
    Word("@");
    Word("+");
    Word("C/L");
    Word("1");
    Word("-");
    Word("<");
    ZBranch("VLIST2");
    Word("SPACE");
    Word("SPACE");
    Branch("VLIST3");
    Label("VLIST2");
    Word("CR");
    Word("0");
    Word("OUT");
    Word("!");
    Label("VLIST3");
    Word("DUP");
    Word("ID.");
    Word("PFA");
    Word("LFA");
    Word("@");
    Word("-DUP");
    Word("0=");
    ZBranch("VLIST1");
    Word(";S");
}

void DefineCOLD() {
    Colon("COLD");
    Word("SP!");
    Word("RP!");
    Word("LIT");
    coldLastWordReference = here;
    Comma(0);       // Filled in later
    Word("LIT");
    Comma(CellIndexToByteIndex(forthLastWordReference));
    Word("!");
    Word("LIT");
    Comma(ORIG + bytesPerCell * 2);
    Word("@");
    Word("B/CELL");
    Word("*");
    Word("0");
    Word("(DO)");
    Label("COLD1");
    Word("LIT");
    Comma(ORIG + bytesPerCell * 3);
    Word("I");
    Word("+");
    Word("@");
    Word("I");
    Word("LIT");
    Comma(UAREA);
    Word("+");
    Word("!");
    Word("B/CELL");
    PlusLoop("COLD1");
    Word("ABORT");
    Word(";S");
}

void LogDictionary() {
    logFile << std::hex << std::setfill('0') << std::endl << "Memory dump:";

    cell_t byteHere = CellIndexToByteIndex(here);
    for (cell_t byte = 0; byte < byteHere; byte++) {
        if (byte % 16 == 0) {
            logFile << std::endl << addrFormat(byte);
        }
        logFile << " " << std::setw(2) << (uint16_t)memory.byte[byte];
    }

    logFile << std::dec << std::setfill(' ') << std::endl;
}

void DumpForwardReferences() {
    for (auto it = forwardReferences.begin(); it != forwardReferences.end(); it++) {
        std::cout << it->first << std::endl;
    }
}

void BuildDictionary() {
    here = ByteIndexToCellIndex(ORIG);

    // Cold start word
    Word("COLD");
    // Error handling word
    Word("ERROR");

    // Number of user area entries to initialize
    Comma(8);
    Comma(TIBX);         // TIB
    Comma(31);           // WIDTH
    Comma(0);            // WARNING (0=no disk 1=disk)
    Comma(0);            // FENCE
    Comma(0);            // DP
    Comma(0);            // VOCL
    Comma(0);            // ENTER-KEY filled in by virtual machine for platform
    Comma(0);            // DELETE-KEY filled in by virtual machine for platform

    Primitive("LIT", Token::LIT);
    Primitive("CLIT", Token::LIT);   // Since we cell align words, there no point in CLIT differing from LIT
    Primitive("EXECUTE", Token::EXEC);
    Primitive("BRANCH", Token::BRAN);
    Primitive("0BRANCH", Token::ZBRAN);
    Primitive("(LOOP)", Token::PLOOP);
    Primitive("(+LOOP)", Token::PPLOO);
    Primitive("(DO)", Token::PDO);
    Primitive("I", Token::R);  // Shares the same code as R
    Primitive("DIGIT", Token::DIGI);
    Primitive("(FIND)", Token::PFIND);
    Primitive("ENCLOSE", Token::ENCL);
    Primitive("EMIT", Token::EMIT);
    Primitive("KEY", Token::KEY);
    Primitive("?TERMINAL", Token::QTERM);
    Primitive("MILLIS", Token::MILLIS);
    Primitive("CR", Token::CR);
    Primitive("CMOVE", Token::CMOVE);
    Primitive("U*", Token::USTAR);
    Primitive("U/", Token::USLAS);
    Primitive("AND", Token::AND);
    Primitive("OR", Token::OR);
    Primitive("XOR", Token::XOR);
    Primitive("SP@", Token::SPAT);
    Primitive("SP!", Token::SPSTO);
    Primitive("RP!", Token::RPSTO);
    Primitive(";S", Token::SEMIS);
    Primitive("LEAVE", Token::LEAVE);
    Primitive(">R", Token::TOR);
    Primitive("R>", Token::RFROM);
    Primitive("R", Token::R);
    Primitive("I'", Token::IPRIME);
    Primitive("J", Token::J);
    Primitive("0=", Token::ZEQU);
    Primitive("0<", Token::ZLESS);
    Primitive("+", Token::PLUS);
    Primitive("D+", Token::DPLUS);
    Primitive("D<", Token::DLESS);
    Primitive("MINUS", Token::MINUS);
    Primitive("DMINUS", Token::DMINUS);
    Primitive("OVER", Token::OVER);
    Primitive("DROP", Token::DROP);
    Primitive("SWAP", Token::SWAP);
    Primitive("DUP", Token::DUP);
    Primitive("PICK", Token::PICK);
    Primitive("+!", Token::PSTOR);
    Primitive("TOGGLE", Token::TOGGLE);
    Primitive("@", Token::AT);
    Primitive("C@", Token::CAT);
    Primitive("!", Token::STORE);
    Primitive("C!", Token::CSTORE);

    DefineCOLON();
    DefineSEMICOLON();
    DefineCONST();
    DefineVAR();
    DefineUSER();

    Constant("TRUE", cellMax);
    Constant("FALSE", 0);
    Constant("0", 0);
    Constant("1", 1);
    Constant("2", 2);
    Constant("3", 3);
    Constant("BL", (cell_t)' ');
    Constant("C/L", 80);
    Constant("L/SCR", 25);
    Constant("FIRST", DAREA);
    Constant("LIMIT", UAREA);
    Constant("B/BUF", BUFFER_SIZE);
    Constant("B/SCR", 1);
    Constant("B/CELL", bytesPerCell);
    Constant("BUFFERS", NBUFFERS);
    Constant("CELL-MAX", scellMax);
    Constant("UCELL-MAX", cellMax);

    DefinePORIG();
    User("TIB", tibUserIndex);
    User("WIDTH", widthUserIndex);
    User("WARNING", warningUserIndex);
    User("FENCE", fenceUserIndex);
    User("DP", dpUserIndex);
    User("VOC-LINK", voclUserIndex);
    User("ENTER-KEY", enterKeyUserIndex);
    User("DELETE-KEY", deleteKeyUserIndex);
    User("BLK", blkUserIndex);
    User("IN", inUserIndex);
    User("OUT", outUserIndex);
    User("SCR", scrUserIndex);
    User("OFFSET", offsetUserIndex);
    User("CONTEXT", contextUserIndex);
    User("CURRENT", currentUserIndex);
    User("STATE", stateUserIndex);
    User("BASE", baseUserIndex);
    User("DPL", dplUserIndex);
    User("FLD", fldUserIndex);
    User("CSP", cspUserIndex);
    User("R#", rnumUserIndex);
    User("HLD", hldUserIndex);

    DefineONEP();
    DefineTWOP();
    DefineHERE();
    DefineALLOT();
    DefineCOMMA();
    DefineCCOMMA();
    DefineSUB();
    DefineEQUAL();
    Primitive("U<", Token::ULESS);
    DefineUGREATER();
    Primitive("<", Token::LESS);
    DefineGREAT();
    DefineROT();
    DefineSPACE();
    DefineDDUP();
    DefineTRAVERSE();
    DefineLATEST();
    DefineLFA();
    DefineCFA();
    DefineNFA();
    DefinePFA();
    DefineSCSP();
    DefineQERR();
    DefineQCOMP();
    DefineQEXEC();
    DefineQPAIRS();
    DefineQCSP();
    DefineQLOAD();
    DefineCOMPILE();
    DefineLBRAC();
    DefineRBRAC();
    DefineSMUDG();
    DefineHEX();
    DefineDECIMAL();
    DefinePSCOD();
    DefineSCODE();
    DefineBUILD();
    DefineDOES();
    DefineCOUNT();
    DefineTYPE();
    DefineDTRAI();
    DefinePDOTQ();
    DefineDOTQ();
    DefineEXPEC();
    DefineQUERY();
    DefineX();
    DefineFILL();
    DefineERASE();
    DefineBLANK();
    DefineHOLD();
    DefinePAD();
    DefineWORD();
    DefinePNUMB();
    DefineNUMBER();
    DefineDFIND();
    DefinePABOR();
    DefineERROR();
    DefineIDDOT();
    DefineCREAT();
    DefineBCOMPILE();
    DefineLITER();
    DefineDLIT();
    DefineQSTAC();
    DefineINTER();
    DefineIMMEDIATE();
    DefineVOCABULARY();
    DefineFORTH();
    DefineDEFIN();
    DefinePAREN();
    DefineQUIT();
    DefineABORT();
    DefineSTOD();
    DefinePM();
    DefineDPM();
    DefineABS();
    DefineDABS();
    DefineMIN();
    DefineMAX();
    DefineMSTAR();
    DefineMSLAS();
    DefineSTAR();
    DefineSLMOD();
    DefineSLASH();
    DefineMOD();
    DefineSSMOD();
    DefineSSLAS();
    DefineMSMOD();

    Variable("USE", DAREA);
    Variable("PREV", DAREA);

    DefinePBUF();
    DefineUPDAT();
    DefineFLUSH();
    DefineEMPTYBUFFERS();
    DefineDR0();
    DefineBUFFR();
    DefineBLOCK();
    DefinePLINE();
    DefineDLINE();
    DefineMESS();
    DefineLOAD();
    DefineNEXTSCREEN();
    Primitive("DREAD", Token::DREAD);
    Primitive("DWRITE", Token::DWRITE);
    DefineRSLW();
    DefineTICK();
    DefineBTICK();
    DefineFORG();
    DefineBACK();
    DefineBEGIN();
    DefineENDIF();
    DefineTHEN();
    DefineDO();
    DefineLOOP();
    DefinePLOOP();
    DefineUNTIL();
    DefineEND();
    DefineAGAIN();
    DefineREPEAT();
    DefineIF();
    DefineELSE();
    DefineWHILE();
    DefineSPACS();
    DefineBDIGS();
    DefineEDIGS();
    DefineSIGN();
    DefineDIG();
    DefineDIGS();
    DefineDDOTR();
    DefineDDOT();
    DefineDOTR();
    DefineDOT();
    DefineQUES();
    DefineLIST();
    DefineINDEX();
    DefineTRIAD();
    DefineVLIST();
    DefineCOLD();

    // The last word defined needs to be pointed to by the VOCABULARY word FORTH. Before we define it,
    // resolve the reference.
    memory.cell[forthLastWordReference] = CellIndexToByteIndex(here);
    // This is also set by COLD.
    memory.cell[coldLastWordReference] = CellIndexToByteIndex(here);
    Primitive("MON", Token::MON);

    logFile << addrFormat(CellIndexToByteIndex(here)) << std::endl;

    // At this point there shouldn't be any remaining unresolved forward references. If there are,
    // print them and bomb out.
    if (!forwardReferences.empty()) {
        std::cerr << "Unresolved forward references exit after dictionary build:" << std::endl;
        DumpForwardReferences();
        logFile.close();
        std::exit(1);
    }

    memory.cell[ByteIndexToCellIndex(ORIG) + 6] = CellIndexToByteIndex(here);
    memory.cell[ByteIndexToCellIndex(ORIG) + 7] = CellIndexToByteIndex(here);
    memory.cell[ByteIndexToCellIndex(ORIG) + 8] =
        CellIndexToByteIndex(forthLastWordReference + 1);
}

void WriteHFile() {
    std::ofstream hFile;
    hFile.open("Dictionary.h");

    hFile << "// This file is generated by mkDictionary and should not need"
             " to be altered directly." << std::endl;
    hFile << std::endl;

    hFile << "#ifndef DICTIONARY_H" << std::endl;
    hFile << "#define DICTIONARY_H" << std::endl;
    hFile << std::endl;
    hFile << "#include \"lnFORTH.h\"" << std::endl;
    hFile << std::endl;
    hFile << "#include \"stddef.h\"" << std::endl;
    hFile << std::endl;
    hFile << "static constexpr size_t initialDictionarySize = " << here
          << ";" << std::endl;
    hFile << "extern const cell_t initialDictionary[initialDictionarySize];"
          << std::endl;
    hFile << std::endl;
    hFile << "#endif /* DICTIONARY_H */" << std::endl;

    hFile.close();
}

void WriteCPPFile() {
    std::ofstream cppFile;
    cppFile.open("Dictionary.cpp");

    cppFile << "// This file is generated by mkDictionary and should not need"
               " to be altered directly." << std::endl;
    cppFile << std::endl;
    cppFile << "#include \"Dictionary.h\"" << std::endl;
    cppFile << "#include \"lnFORTH.h\"" << std::endl;
    cppFile << std::endl;
    cppFile << "#ifdef ARDUINO" << std::endl;
    cppFile << "#include <Arduino.h>" << std::endl;
    cppFile << "#else" << std::endl;
    cppFile << "#define PROGMEM" << std::endl;
    cppFile << "#endif" << std::endl;
    cppFile << std::endl;

    cppFile << "PROGMEM const cell_t initialDictionary[initialDictionarySize] {"
            << std::endl;

    constexpr size_t wordsPerLine = 16 / bytesPerCell;
    for (size_t word = 0; word < here; word++) {
        if (word % wordsPerLine == 0) {
            cppFile << std::endl;
            cppFile << "    ";
        } else {
            cppFile << " ";
        }
        cppFile << "0x" << std::hex << std::setw(bytesPerCell * 2)
                << std::setfill('0') << memory.cell[word];
        if (word + 1 != here) {
            cppFile << ",";
        }
    }
    cppFile << std::endl;

    cppFile << "};" << std::endl;

    cppFile.close();
}

void WriteDictionary() {
    WriteHFile();
    WriteCPPFile();
}

int main(int argc, char* argv[]) {
    logFile.open("Dictionary.log");

    BuildDictionary();
    LogDictionary();
    WriteDictionary();

    logFile.close();
}

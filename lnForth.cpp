#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include <iostream>
#include <iomanip>
#include <map>
#include <set>

#include "Platform.h"

#define CELL_SIZE_32_BITS 1

#ifdef CELL_SIZE_16_BITS
typedef uint16_t cell_t;
typedef int16_t scell_t;
typedef uint32_t double_t;
typedef int32_t sdouble_t;
const cell_t cellMax = 0xffff;
const cell_t scellMax = 0x7fff;
const cell_t cellPaddingMask = 0x0001;
#else
typedef uint32_t cell_t;
typedef int32_t scell_t;
typedef uint64_t double_t;
typedef int64_t sdouble_t;
const cell_t cellMax = 0xffffffff;
const cell_t scellMax = 0x7fffffff;
const cell_t cellPaddingMask = 0x00000003;
#endif

const uint8_t bytesPerCell = sizeof(cell_t);
const uint8_t bitsPerCell = bytesPerCell * 8;

const size_t dataStackSize = 256;
const size_t returnStackSize = 256;
const size_t memorySize = 64*1024;

const bool enableDataStackBoundsCheck = true;
const bool enableReturnStackBoundsCheck = true;

const bool debugWordCreation = true;
const bool dumpDictionary = true;
const bool traceVirtualMachine = false;

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
    ZEQU,
    ZLESS,
    PLUS,
    DPLUS,
    MINUS,
    DMINUS,
    OVER,
    DROP,
    SWAP,
    DUP,
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
    LESS,
    DODOE,
    DREAD,
    DWRITE,
    MON
};

// Similar to other Forth implementations, we store word names with a leading length and use the most
// significant bits as flags
const size_t maxWordNameLength = UINT8_MAX >> 2;
const uint8_t wordValidFlag = 0x80;
const uint8_t immediateWordFlag = 0x40;
const uint8_t nameLengthMask = 0x1f;

cell_t stack[dataStackSize];
cell_t rStack[returnStackSize];
union Mem {
    cell_t cell[memorySize];
    uint8_t byte[memorySize * bytesPerCell];
} memory;

const cell_t TIBX = 0x0100;
const cell_t ORIG = 0x0200;
const cell_t UAREA = (cell_t)(memorySize * bytesPerCell) - 128;
const cell_t NBUFFERS = 3;
const cell_t BUFFER_SIZE = 1024;
const cell_t MEM_BYTES_PER_BUFFER = BUFFER_SIZE + 2 * bytesPerCell;
// Total buffer magnitude, in bytes
const cell_t BMAG = NBUFFERS * MEM_BYTES_PER_BUFFER;
const cell_t DAREA = UAREA - BMAG; // Disk buffer area
const cell_t MAX_BLOCK_NUMBER = 500;

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

cell_t sp;
cell_t rp;
cell_t ip;
cell_t w;
cell_t here;
cell_t forthLastWordReference;

// For building the initial dictionary only, we use a standard libary map to find words. It's not used
// after Forth is up and running, then the normal thread of words is used.
std::map<const char *, cell_t> words;

// If a word is referenced before it's defined, we need to keep track of where the usage is for later
// when we define it. At that time all the references are resolved. At end of compilation of the
// initial dictionary, we check for unresolved references and flag them as errors.
std::map<const char *, std::set<cell_t>> forwardReferences;

// Label database to make writing branches simplar and less error prone
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

static inline cell_t CellIndexToByteIndex(cell_t cellIndex) {
    return cellIndex * bytesPerCell;
}

static inline cell_t ByteIndexToCellIndex(cell_t byteIndex) {
    return byteIndex / bytesPerCell;
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
    if (debugWordCreation) {
        std::cout << addrFormat(byteHere) << " " << name << std::endl;
    }

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

void CheckStack(cell_t needs, cell_t adds) {
    if (enableDataStackBoundsCheck) {
        // For now, this will check stack bounds and, if there's an issue, output an error message and exit.
        // Later this should do something reasonable like throwing an exception or calling cold.
        if (needs && (sp == cellMax || sp + 1 < needs)) {
            std::cerr << "Stack underflow detected!" << std::endl;
            std::exit(1);
        }

        if (((sp == cellMax) && (adds > dataStackSize)) ||
            ((sp != cellMax) && (adds + sp >= dataStackSize))) {
            std::cerr << "Stack overflow detected!" << std::endl;
            std::exit(1);
        }
    }
}

void CheckReturnStack(cell_t needs, cell_t adds) {
    if (enableReturnStackBoundsCheck) {
        // For now, this will check stack bounds and, if there's an issue, output an error message and exit.
        // Later this should do something reasonable like throwing an exception or calling cold.
        if (needs && (rp == cellMax || rp  + 1< needs)) {
            std::cerr << "Return stack underflow detected!" << std::endl;
            std::exit(1);
        }

        if (((rp == cellMax) && (adds > returnStackSize)) ||
            ((rp != cellMax) && (adds + rp >= returnStackSize))) {
            std::cerr << "Return stack overflow detected!" << std::endl;
            std::exit(1);
        }
    }
}

inline void CheckAddr(cell_t addr) {
    if (addr > memorySize * bytesPerCell) {
        std::cerr << "Attempt to access illegal memory address "
                  << addrFormat(addr)
                  << " ip=" << addrFormat(CellIndexToByteIndex(ip))
                  << " w=" << addrFormat(addr) << std::endl;
        std::exit(1);
    }
}

void LIT() {
    CheckStack(0, 1);

    stack[++sp] = memory.cell[ip++];
}

void ExecuteTokenNonInline(Token token);
void EXEC() {
    CheckStack(1, 0);

    w = stack[sp--];
    Token token = static_cast<Token>(memory.cell[ByteIndexToCellIndex(w)]);

    ExecuteTokenNonInline(token);
}

void BRAN() {
    ip += (scell_t)memory.cell[ip] / bytesPerCell;
}

void ZBRAN() {
    CheckStack(1, 0);

    if(stack[sp--] == 0) {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
    } else {
        ip++;
    }
}

void PLOOP() {
    CheckReturnStack(2, 0);

    rStack[rp] += 1;
    if (rStack[rp] < rStack[rp - 1]) {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
    } else {
        ip++;
        rp -= 2;
    }
}

void PPLOO() {
    CheckStack(1, 0);
    CheckReturnStack(2, 0);

    cell_t n = stack[sp--];
    rStack[rp] += n;
    if (rStack[rp] < rStack[rp - 1]) {
        ip += (scell_t)memory.cell[ip] / bytesPerCell;
    } else {
        ip++;
        rp -= 2;
    }
}

void PDO() {
    CheckStack(2, 0);
    CheckReturnStack(0, 2);

    rStack[++rp] = stack[sp - 1];
    rStack[++rp] = stack[sp];
    sp -= 2;
}

void DIGI() {
    CheckStack(2, 0);

    cell_t base = stack[sp--];
    char ch = (char)stack[sp--];
    char value;
    if (ch < '0') {
        stack[++sp] = 0;
        return;
    } else if (ch <= '9') {
        value = ch - '0';
    } else if (ch < 'A') {
        stack[++sp] = 0;
        return;
    } else if (ch <= 'Z') {
        value = 10 + ch - 'A';
    } else if (ch < 'a') {
        stack[++sp] = 0;
        return;
    } else if (ch <= 'z') {
        value = 10 + ch - 'a';
    } else {
        stack[++sp] = 0;
        return;
    }

    if (value >= base) {
        stack[++sp] = 0;
    } else {
        stack[++sp] = (cell_t)value;
        stack[++sp] = 1;
    }
}

bool WordNamesMatch(cell_t dictEntry, cell_t wordName) {
    uint8_t dictLength = memory.byte[dictEntry++] & nameLengthMask;
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

void PFIND() {
    CheckStack(2, 0);

    cell_t dictEntry = stack[sp--];
    cell_t name = stack[sp--];

    do {
        if (WordNamesMatch(dictEntry, name)) {
            stack[++sp] = (cell_t)(dictEntry + NameFieldLength(dictEntry) +
                                   bytesPerCell * 2);
            stack[++sp] = memory.byte[dictEntry];
            stack[++sp] = 1;
            return;
        }

        cell_t linkField = dictEntry + NameFieldLength(dictEntry);
        dictEntry = memory.cell[ByteIndexToCellIndex(linkField)];
    } while (dictEntry != 0);

    stack[++sp] = 0;
}

void ENCL() {
    CheckStack(2, 2);

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
        return;
    }

    cell_t n2 = n1 + 1;
    while (memory.byte[addr + n2] != c && memory.byte[addr + n2] != 0) {
        n2++;
    }
    stack[++sp] = n2;

    if (memory.byte[addr + n2] == 0) {
        stack[++sp] = n2;
        return;
    }
    cell_t n3 = n2 + 1;
    while (memory.byte[addr + n3] == c) {
        n3++;
    }
    stack[++sp] = n3;
}

void EMIT() {
    CheckStack(1, 0);

    char character = (char)stack[sp--];
    XEmit(character);

    memory.cell[ByteIndexToCellIndex(UAREA) + outUserIndex]++;
}

void KEY() {
    CheckStack(0, 1);

    char c;
    ssize_t count = read(STDIN_FILENO, &c, 1);

    if (count == 1) {
        stack[++sp] = (cell_t)c;
    } else if (count == 0) {
        // For now, if end of file is detected we exit.
        std::cerr << "End of file in KEY. Exiting..." << std::endl;
        exit(0);
    } else if (count < 0) {
        std::cerr << "Error reading input in KEY: " << count << std::endl;
        exit(1);
    }
}

void QTERM() {
    // It's not clear that this functionality is going to be implementable in modern systems.
    // For now, just return false.
    CheckStack(0, 1);

    stack[++sp] = 0;
}

void CR() {
    std::cout << (char)10;
}

void CMOVE() {
    CheckStack(3, 0);

    cell_t count = stack[sp--];
    cell_t to = stack[sp--];
    cell_t from = stack[sp--];
    while (count--) {
        memory.byte[to++] = memory.byte[from++];
    }
}

void USTAR() {
    CheckStack(2, 0);
    cell_t u1 = stack[sp];
    cell_t u2 = stack[sp - 1];
    double_t prod = u1 * u2;
    stack[sp - 1] = (cell_t)(prod & cellMax);
    stack[sp] = (cell_t)(prod >> bitsPerCell);
}

void USLAS() {
    CheckStack(3, 0);

    double_t ud = (double_t)stack[sp - 2] | (((double_t)stack[sp - 1]) << bitsPerCell);
    cell_t u1 = stack[sp--];
    cell_t u2 = (cell_t)(ud % u1);
    cell_t u3 = (cell_t)(ud / u1);
    stack[sp -1] = u2;
    stack[sp] = u3;
}

void AND() {
    CheckStack(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a & b;
}

void OR() {
    CheckStack(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a | b;
}

void XOR() {
    CheckStack(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a ^ b;
}

void SPAT() {
    CheckStack(0, 1);
    cell_t pos = sp;
    stack[++sp] = pos;
}

void SPSTO() {
    // In the source figFORTH listing SP! sets the stack pointer to the contents of a
    // hidden user variable. Here we just set it to -1 (our empty) as the stack isn't addressable
    // anyway.
    sp = cellMax;
}

void RPSTO() {
    // In the source figFORTH listing SP! sets the return stack pointer to the contents of a
    // hidden user variable. Here we just set it to -1 (our empty) as the stack isn't addressable
    // anyway.
    rp = cellMax;
}

void SEMIS() {
    CheckReturnStack(1, 0);

    ip = ByteIndexToCellIndex(rStack[rp--]);
}

void LEAVE() {
    CheckReturnStack(2, 0);

    rStack[rp - 1] = rStack[rp];
}

void TOR() {
    CheckStack(1, 0);
    CheckReturnStack(0, 1);

    rStack[++rp] = stack[sp--];
}

void RFROM() {
    CheckStack(0, 1);
    CheckReturnStack(1, 0);

    stack[++sp] = rStack[rp--];
}

void R() {
    CheckStack(0, 1);
    CheckReturnStack(1, 0);

    stack[++sp] = rStack[rp];
}

void ZEQU() {
    CheckStack(1, 0);

    stack[sp] = stack[sp] == 0 ? 1 : 0;
}

void ZLESS() {
    CheckStack(1, 0);

    stack[sp] = (scell_t)stack[sp] < 0 ? 1 : 0;
}

void PLUS() {
    CheckStack(2, 0);

    cell_t a = stack[sp--];
    cell_t b = stack[sp];
    stack[sp] = a + b;
}

void DPLUS() {
    CheckStack(4, 0);

    double_t d1 = (double_t)stack[sp - 3] | (((double_t)stack[sp - 2]) << bitsPerCell);
    double_t d2 = (double_t)stack[sp - 1] | (((double_t)stack[sp]) << bitsPerCell);
    sp -= 2;
    double_t dsum = d1 + d2;
    stack[sp - 1] = (cell_t)(dsum & cellMax);
    stack[sp] = (cell_t)(dsum >> bitsPerCell);
}

void MINUS() {
    CheckStack(1, 0);

    stack[sp] = (~stack[sp]) + 1;
}

void DMINUS() {
    CheckStack(2, 0);

    double_t d1 = (double_t)stack[sp - 1] | (((double_t)stack[sp]) << bitsPerCell);
    double_t d2 = (~d1) + 1;
    stack[sp - 1] = (cell_t)(d2 & cellMax);
    stack[sp] = (cell_t)(d2 >> bitsPerCell);
}

void OVER() {
    CheckStack(2, 1);

    stack[sp + 1] = stack[sp - 1];
    sp++;
}

void DROP() {
    CheckStack(1, 0);

    sp--;
}

void SWAP() {
    CheckStack(2, 0);

    cell_t temp = stack[sp - 1];
    stack[sp - 1] = stack[sp];
    stack[sp] = temp;
}

void DUP() {
    CheckStack(1, 1);

    stack[sp + 1] = stack[sp];
    sp++;
}

void PSTOR() {
    CheckStack(2, 0);

    cell_t addr = stack[sp--];
    cell_t n = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] += n;
}

void TOGGLE() {
    CheckStack(2, 0);

    cell_t b = stack[sp--];
    cell_t addr = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] ^= b;
}

void AT() {
    CheckStack(1, 0);

    cell_t addr = stack[sp--];

    CheckAddr(addr);
    cell_t n = memory.cell[ByteIndexToCellIndex(addr)];
    stack[++sp] = n;
}

void CAT() {
    CheckStack(1, 0);

    cell_t addr = stack[sp--];

    CheckAddr(addr);
    uint8_t n = memory.byte[addr];
    stack[++sp] = (cell_t)n;
}

void STORE() {
    CheckStack(2, 0);

    cell_t addr = stack[sp--];
    cell_t n = stack[sp--];

    CheckAddr(addr);
    memory.cell[ByteIndexToCellIndex(addr)] = n;
}

void CSTORE() {
    CheckStack(2, 0);

    cell_t addr = stack[sp--];
    cell_t value = stack[sp--];

    CheckAddr(addr);
    memory.byte[addr] = (uint8_t)value;
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
    Word("]");
    SemicolonCode(Token::DOCOL);
}

void DOCOL() {
    CheckReturnStack(0, 1);

    rStack[++rp] = CellIndexToByteIndex(ip);
    ip = ByteIndexToCellIndex(w) + 1;
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
    Word("SMUDGE");
    Word(",");
    SemicolonCode(Token::DOCON);
}

void DOCON() {
    CheckStack(0, 1);

    stack[++sp] = memory.cell[ByteIndexToCellIndex(w) + 1];
}

void DefineVAR() {
    Colon("VARIABLE");
    Word("CONSTANT");
    SemicolonCode(Token::DOVAR);
}

void DOVAR() {
    CheckStack(0, 1);

    stack[++sp] = w + bytesPerCell;
}

void DefineUSER() {
    Colon("USER");
    Word("CONSTANT");
    SemicolonCode(Token::DOUSE);
}

void DOUSE() {
    CheckStack(0, 1);

    stack[++sp] = UAREA + memory.cell[ByteIndexToCellIndex(w) + 1];
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

void DefineULESS() {
    Colon("U<");
    Word("-");
    Word("0<");
    Word(";S");
}

void LESS() {
    CheckStack(2, 0);

    scell_t n2 = (scell_t)stack[sp--];
    scell_t n1 = (scell_t)stack[sp--];
    cell_t f = n1 < n2 ? 1 : 0;
    stack[++sp] = f;
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
    Comma(bytesPerCell * 2 + 1);
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
    Colon("[");
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
    Comma(0x20);
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

void DODOE() {
    CheckStack(0, 1);
    CheckReturnStack(0, 1);

    rStack[++rp] = CellIndexToByteIndex(ip);
    ip = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(w) + 1]);
    stack[++sp] = w + (bytesPerCell * 2);
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
    Comma(4);
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
    Word("LIT");
    Comma(0x0a);
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
    Comma(80);
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
    Word("HERE");
    Word("LIT");
    Comma(68);      // PAD is 68 bytes about here.
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

// This routine converts text to upper case.  It allows interpretation
// from a terminal without a shift lock.
void DefineUPPER() {
    Colon("UPPER");
    Word("OVER");
    Word("+");
    Word("SWAP");
    Word("PDO");
Label("L1950");
    Word("I");
    Word("C@");
    Word("LIT");
    Comma(0x5f);
    Word(">");
    ZBranch("L1961");
    Word("I");
    Word("LIT");
    Comma(0x20);
    Word("TOGGLE");
Label("L1961");
    Loop("L1950");
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
    Word("BRANCH");
    Comma(0xffe3);
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
    Word("+");
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

void DefineIDDOT() {
    Colon("ID.");
    Word("PAD");
    Word("LIT");
    Comma(32);
    Word("CLIT");
    Comma(0x5f);
    Word("FILL");
    Word("DUP");
    Word("PFA");
    Word("LFA");
    Word("OVER");
    Word("-");
    Word("PAD");
    Word("SWAP");
    Word("CMOVE");
    Word("PAD");
    Word("COUNT");
    Word("LIT");
    Comma(0x1f);
    Word("AND");
    Word("TYPE");
    Word("SPACE");
    Word(";S");
}

void DefineCREAT() {
    Colon("CREATE", immediateWordFlag);
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
    Comma(0xa0);
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
    Word("HERE");
    Word("B/CELL");
    Word("+");
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
    Word("CFA");
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
    Word("CFA");
    Word(",");
    Branch("L2286");
Label("L2284");
    Word("CFA");
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
    Word("MINUS");
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

void DREAD() {
    CheckStack(2, 0);

    cell_t blk = stack[sp--];
    cell_t addr = stack[sp--];
    long blkDiskOffset = blk * BUFFER_SIZE;

    FILE *file = fopen("disk", "r");
    if (file == nullptr) {
        stack[++sp] = 0;
        return;
    }

    if (fseek(file, blkDiskOffset, SEEK_SET) != 0) {
        stack[++sp] = 0;
        (void)fclose(file);
        return;
    }
    if (fread(&memory.byte[addr], 1, BUFFER_SIZE, file) != BUFFER_SIZE) {
        stack[++sp] = 0;
        (void)fclose(file);
        return;
    }

    if (fclose(file) != 0) {
        stack[++sp] = 0;
        return;
    }

    stack[++sp] = 1;
}

void DWRITE() {
    CheckStack(2, 0);

    cell_t blk = stack[sp--];
    cell_t addr = stack[sp--];
    long blkDiskOffset = blk * BUFFER_SIZE;

    FILE *file = fopen("disk", "w");
    if (file == nullptr) {
        stack[++sp] = 0;
        return;
    }

    if (fseek(file, blkDiskOffset, SEEK_SET) != 0) {
        stack[++sp] = 0;
        (void)fclose(file);
        return;
    }
    if (fwrite(&memory.byte[addr], 1, BUFFER_SIZE, file) != BUFFER_SIZE) {
        stack[++sp] = 0;
        (void)fclose(file);
        return;
    }

    if (fclose(file) != 0) {
        stack[++sp] = 0;
        return;
    }

    stack[++sp] = 1;
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

void DefineTICK() {
    Colon("'", immediateWordFlag);
    Word("-FIND");
    Word("0=");
    Word("0");
    Word("?ERROR");
    Word("DROP");
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
    Colon("+LOOP");
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

void DefineINDEX() {
    Colon("INDEX");
    Word("CR");
    Word("1+");
    Word("SWAP");
    Word("(DO)");
Label("L3647");
    Word("CR");
    Word("I");
    Word("3");
    Word(".R");
    Word("SPACE");
    Word("0");
    Word("I");
    Word(".LINE");
    Word("?TERMINAL");
    ZBranch("L3659");
    Word("LEAVE");
Label("L3659");
    Loop("L3647");
    Word("LIT");
    Comma(0x0c);    // FORM FEED FOR PRINTER
    Word("EMIT");
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

void DefineVLIST() {
    Colon("VLIST");
    Word("LIT");
    Comma(0x80);
    Word("OUT");
    Word("!");
    Word("CONTEXT");
    Word("@");
    Word("@");
Label("L3706");
    Word("OUT");
    Word("@");
    Word("C/L");
    Word(">");
    ZBranch("L3716");
    Word("CR");
    Word("0");
    Word("OUT");
    Word("!");
Label("L3716");
    Word("DUP");
    Word("ID.");
    Word("SPACE");
    Word("SPACE");
    Word("PFA");
    Word("LFA");
    Word("@");
    Word("DUP");
    Word("0=");
    Word("?TERMINAL");
    Word("OR");
    ZBranch("L3706");
    Word("DROP");
    Word(";S");
}

void MON() {
    std::cout << std::endl << "Exiting with stack:";
    for (cell_t i = 0; i <= sp && sp != cellMax; i++) {
        std::cout << " "<< addrFormat(stack[i]);
    }
    std::cout << std::endl;

    std::exit(0);
}

void DumpDictionary() {
    std::cout << std::hex << std::setfill('0')
              << std::endl << "Memory dump:";

    cell_t byteHere = CellIndexToByteIndex(here);
    for (cell_t byte = 0; byte < byteHere; byte++) {
        if (byte % 16 == 0) {
            std::cout << std::endl << addrFormat(byte);
        }
        std::cout << " " << std::setw(2) << (uint16_t)memory.byte[byte];
    }

    std::cout << std::dec << std::setfill(' ') << std::endl;
}

void DumpForwardReferences() {
    for (auto it = forwardReferences.begin(); it != forwardReferences.end(); it++) {
        std::cout << it->first << std::endl;
    }
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
        case Token::ZEQU:
            return "ZEQU";
        case Token::ZLESS:
            return "ZLESS";
        case Token::PLUS:
            return "PLUS";
        case Token::DPLUS:
            return "DPLUS";
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

inline void executeToken(Token token) {
    switch(token) {
        case Token::LIT:
            LIT();
            break;
        case Token::EXEC:
            EXEC();
            break;
        case Token::BRAN:
            BRAN();
            break;
        case Token::ZBRAN:
            ZBRAN();
            break;
        case Token::PLOOP:
            PLOOP();
            break;
        case Token::PPLOO:
            PPLOO();
            break;
        case Token::PDO:
            PDO();
            break;
        case Token::DIGI:
            DIGI();
            break;
        case Token::PFIND:
            PFIND();
            break;
        case Token::ENCL:
            ENCL();
            break;
        case Token::EMIT:
            EMIT();
            break;
        case Token::KEY:
            KEY();
            break;
        case Token::QTERM:
            QTERM();
            break;
        case Token::CR:
            CR();
            break;
        case Token::CMOVE:
            CMOVE();
            break;
        case Token::USTAR:
            USTAR();
            break;
        case Token::USLAS:
            USLAS();
            break;
        case Token::AND:
            AND();
            break;
        case Token::OR:
            OR();
            break;
        case Token::XOR:
            XOR();
            break;
        case Token::SPAT:
            SPAT();
            break;
        case Token::SPSTO:
            SPSTO();
            break;
        case Token::RPSTO:
            RPSTO();
            break;
        case Token::SEMIS:
            SEMIS();
            break;
        case Token::LEAVE:
            LEAVE();
            break;
        case Token::TOR:
            TOR();
            break;
        case Token::RFROM:
            RFROM();
            break;
        case Token::R:
            R();
            break;
        case Token::ZEQU:
            ZEQU();
            break;
        case Token::ZLESS:
            ZLESS();
            break;
        case Token::PLUS:
            PLUS();
            break;
        case Token::DPLUS:
            DPLUS();
            break;
        case Token::MINUS:
            MINUS();
            break;
        case Token::DMINUS:
            DMINUS();
            break;
        case Token::OVER:
            OVER();
            break;
        case Token::DROP:
            DROP();
            break;
        case Token::SWAP:
            SWAP();
            break;
        case Token::DUP:
            DUP();
            break;
        case Token::PSTOR:
            PSTOR();
            break;
        case Token::TOGGLE:
            TOGGLE();
            break;
        case Token::AT:
            AT();
            break;
        case Token::CAT:
            CAT();
            break;
        case Token::STORE:
            STORE();
            break;
        case Token::CSTORE:
            CSTORE();
            break;
        case Token::DOCOL:
            DOCOL();
            break;
        case Token::DOCON:
            DOCON();
            break;
        case Token::DOUSE:
            DOUSE();
            break;
        case Token::DOVAR:
            DOVAR();
            break;
        case Token::LESS:
            LESS();
            break;
        case Token::DODOE:
            DODOE();
            break;
        case Token::DREAD:
            DREAD();
            break;
        case Token::DWRITE:
            DWRITE();
            break;
        case Token::MON:
            MON();
            break;
        default:
            std::cerr << "Unimplemented token! (" << static_cast<cell_t>(token)
                      << ") ip=" << addrFormat(ip) << " w=" << addrFormat(w)
                      << std::endl;

            std::exit(1);
    }
}

void ExecuteTokenNonInline(Token token) {
    executeToken(token);
}

void InitUserMemory() {
    cell_t variablesToInit = memory.cell[ByteIndexToCellIndex(ORIG) + 1];
    for (cell_t i = 0; i < variablesToInit; i++) {
        memory.cell[ByteIndexToCellIndex(UAREA) + i] =
            memory.cell[ByteIndexToCellIndex(ORIG) + 2 + i];
    }
}

void VirtualMachine() {
    std::cout << "Starting Virtual Machine" << std::endl;

    InitUserMemory();

    cell_t cold = ByteIndexToCellIndex(memory.cell[ByteIndexToCellIndex(ORIG)]) + 1;

    ip = cold;
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
        executeToken(token);
    }
}

int main(int ac, char* av[]) {
    XInit();

    sp = cellMax;
    rp = cellMax;
    here = ByteIndexToCellIndex(ORIG);

    std::cout << "Building dictionary" << std::endl;

    // Cold start word
    Word("ABORT");

    // Number of user area entries to initialize
    Comma(7);
    Comma(TIBX);         // TIB
    Comma(31);           // WIDTH
    Comma(0);            // WARNING (0=no disk 1=disk)
    Comma(0);            // FENCE
    Comma(0);            // DP
    Comma(0);            // VOCL
    Comma(XDeleteKey()); // DELETE-KEY

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
    Primitive("0=", Token::ZEQU);
    Primitive("0<", Token::ZLESS);
    Primitive("+", Token::PLUS);
    Primitive("D+", Token::DPLUS);
    Primitive("MINUS", Token::MINUS);
    Primitive("DMINUS", Token::DMINUS);
    Primitive("OVER", Token::OVER);
    Primitive("DROP", Token::DROP);
    Primitive("SWAP", Token::SWAP);
    Primitive("DUP", Token::DUP);
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

    Constant("0", 0);
    Constant("1", 1);
    Constant("2", 2);
    Constant("3", 3);
    Constant("BL", (cell_t)' ');
    Constant("C/L", 64);
    Constant("L/SCR", 16);
    Constant("FIRST", DAREA);
    Constant("LIMIT", UAREA);
    Constant("B/BUF", BUFFER_SIZE);
    Constant("B/SCR", 1);
    Constant("B/CELL", bytesPerCell);

    DefinePORIG();
    User("TIB", tibUserIndex);
    User("WIDTH", widthUserIndex);
    User("WARNING", warningUserIndex);
    User("FENCE", fenceUserIndex);
    User("DP", dpUserIndex);
    User("VOC-LINK", voclUserIndex);
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
    DefineULESS();
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

    // The last word defined needs to be pointed to by the VOCABULARY word FORTH. Before we define it,
    // resolve the reference.
    memory.cell[forthLastWordReference] = CellIndexToByteIndex(here);
    Primitive("MON", Token::MON);

    std::cout << addrFormat(CellIndexToByteIndex(here)) << std::endl;

    // At this point there shouldn't be any remaining unresolved forward references. If there are,
    // print them and bomb out.
    if (!forwardReferences.empty()) {
        std::cerr << "Unresolved forward references exit after dictionary build:" << std::endl;
        DumpForwardReferences();
        std::exit(1);
    }

    memory.cell[ByteIndexToCellIndex(ORIG) + 5] = CellIndexToByteIndex(here);
    memory.cell[ByteIndexToCellIndex(ORIG) + 6] = CellIndexToByteIndex(here);
    memory.cell[ByteIndexToCellIndex(ORIG) + 7] =
        CellIndexToByteIndex(forthLastWordReference + 1);

    if (dumpDictionary) {
        DumpDictionary();
    }

    VirtualMachine();
}

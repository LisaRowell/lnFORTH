#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <iostream>
#include <iomanip>
#include <map>
#include <set>

typedef uint16_t cell_t;
typedef int16_t scell_t;
typedef uint32_t double_t;
typedef int32_t sdouble_t;
const uint8_t bytesPerCell = sizeof(cell_t);
const uint8_t bitsPerCell = bytesPerCell * 8;
const cell_t cellMax = 0xffff;
const cell_t cellPaddingMask = 0x0001;

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
    DDISC,
    MON
};

// Similar to other Forth implementations, we store word names with a leading length and use the most
// significant bits as flags
const size_t maxWordNameLength = UINT8_MAX >> 2;
const uint8_t wordValidFlag = 0x80;
const uint8_t immediateWordFlag = 0x40;
const uint8_t nameLengthMask = 0x3f;

cell_t stack[dataStackSize];
cell_t rStack[returnStackSize];
union Mem {
    cell_t cell[memorySize];
    uint8_t byte[memorySize * bytesPerCell];
} memory;

const cell_t SSIZE = 1024;    // Sector size in bytes

const cell_t TIBX = 0x0100;
const cell_t ORIG = 0x0200;
const cell_t UAREA = (cell_t)(memorySize * bytesPerCell) - 128;
const cell_t BMAG =1056;           // Total buffer magnitude, in bytes
const cell_t DAREA = UAREA - BMAG; // Disk buffer area
const cell_t SECTR = 800;          // REVISIT!!!
const cell_t SECTL = 1600;          // REVISIT!!!

const cell_t tibUserIndex     = 0x00;
const cell_t widthUserIndex   = 0x02;
const cell_t warningUserIndex = 0x04;
const cell_t fenceUserIndex   = 0x06;
const cell_t dpUserIndex      = 0x08;
const cell_t voclUserIndex    = 0x0a;
const cell_t blkUserIndex     = 0x16;
const cell_t inUserIndex      = 0x18;
const cell_t outUserIndex     = 0x1a;
const cell_t scrUserIndex     = 0x1c;
const cell_t offsetUserIndex  = 0x1e;
const cell_t contextUserIndex = 0x20;
const cell_t currentUserIndex = 0x22;
const cell_t stateUserIndex   = 0x24;
const cell_t baseUserIndex    = 0x26;
const cell_t dplUserIndex     = 0x28;
const cell_t fldUserIndex     = 0x2a;
const cell_t cspUserIndex     = 0x2c;
const cell_t rnumUserIndex    = 0x2e;
const cell_t hldUserIndex     = 0x30;

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


// A mapping to find DOES> entry points while compiling the initial dictionary.
std::map<const char *, cell_t> doesEntries;

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
    memory.cell[here++] = index;
}

void Constant(const char *name, cell_t value, uint8_t flags = 0) {
    Header(name, flags);
    memory.cell[here++] = static_cast<cell_t>(Token::DOCON);
    memory.cell[here++] = value;
}

void Variable(const char *name, cell_t value, uint8_t flags = 0) {
std::cout << "Defining var " << name << " with value=" << addrFormat(value) << std::endl;
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

void Semicolon() {
    Word(";S");
}

// Since we're token thread, (;CODE) is a little difference in that what follows isn't
// machine code, but rather a single token which will become the token of the created word.
void SemicolonCode(Token token) {
    Word("(;CODE)");
    Comma(static_cast<cell_t>(token));
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
    ip += memory.cell[ip];
}

void ZBRAN() {
    CheckStack(1, 0);

    if(stack[sp--] == 0) {
        ip += memory.cell[ip];
    } else {
        ip++;
    }
}

void PLOOP() {
    CheckReturnStack(2, 0);

    rStack[rp] += 1;
    if (rStack[rp] < rStack[rp - 1]) {
        ip += memory.cell[ip];
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
        ip += memory.cell[ip];
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
        length = (length & (uint8_t)(~cellPaddingMask)) + 2;
    }
    return length;
}

void PFIND() {
    CheckStack(2, 0);

    cell_t dictEntry = stack[sp--];
    cell_t name = stack[sp--];

    do {
        if (WordNamesMatch(dictEntry, name)) {
            stack[++sp] = (cell_t)(dictEntry + NameFieldLength(dictEntry) + bytesPerCell * 2);
            stack[++sp] = memory.byte[dictEntry] & nameLengthMask;
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
    std::cout << character;

    memory.cell[ByteIndexToCellIndex(UAREA + outUserIndex)]++;
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
    CheckReturnStack(0, 2);

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

    cell_t addr = stack[sp--];
    cell_t b = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] ^= b;
}

void AT() {
    CheckStack(1, 0);

    cell_t addr = stack[sp--];
    cell_t n = memory.cell[ByteIndexToCellIndex(addr)];
    stack[++sp] = n;
}

void CAT() {
    CheckStack(1, 0);

    cell_t addr = stack[sp--];
    uint8_t n = memory.byte[addr];
    stack[++sp] = (cell_t)n;
}

void STORE() {
    CheckStack(2, 0);

    cell_t addr = stack[sp--];
    cell_t n = stack[sp--];
    memory.cell[ByteIndexToCellIndex(addr)] = n;
}

void CSTORE() {
    CheckStack(2, 0);

    cell_t addr = stack[sp--];
    cell_t value = stack[sp--];
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
    Semicolon();
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
    Semicolon();
}

void DefineONEP() {
    Colon("1+");
    Word("1");
    Word("+");
    Semicolon();
}

void DefineTWOP() {
    Colon("2+");
    Word("2");
    Word("+");
    Semicolon();
}

void DefineHERE() {
    Colon("HERE");
    Word("DP");
    Word("@");
    Semicolon();
}

void DefineALLOT() {
    Colon("ALLOT");
    Word("DP");
    Word("+!");
    Semicolon();
}

void DefineCOMMA() {
    Colon(",");
    Word("HERE");
    Word("!");
    Word("2");
    Word("ALLOT");
    Semicolon();
}

void DefineSUB() {
    Colon("-");
    Word("MINUS");
    Word("+");
    Semicolon();
}

void DefineEQUAL() {
    Colon("=");
    Word("-");
    Word("0=");
    Semicolon();
}

void DefineULESS() {
    Colon("U<");
    Word("-");
    Word("0<");
    Semicolon();
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
    Semicolon();
}

void DefineROT() {
    Colon("ROT");
    Word(">R");
    Word("SWAP");
    Word("R>");
    Word("SWAP");
    Semicolon();
}

void DefineSPACE() {
    Colon("SPACE");
    Word("BL");
    Word("EMIT");
    Semicolon();
}

void DefineDDUP() {
    Colon("-DUP");
    Word("DUP");
    Word("0BRANCH");
    Comma(2);
    Word("DUP");
    Semicolon();
}

void DefineTRAVERSE() {
    Colon("TRAVERSE");
    Word("SWAP");
    Word("OVER");
    Word("+");
    Word("LIT");
    Comma(0x7f);
    Word("OVER");
    Word("C@");
    Word("<");
    Word("0BRANCH");
    Comma(0xfff8);
    Word("SWAP");
    Word("DROP");
    Semicolon();
}

void DefineLATEST() {
    Colon("LATEST");
    Word("CURRENT");
    Word("@");
    Word("@");
    Semicolon();
}

void DefineLFA() {
    Colon("LFA");
    Word("LIT");
    Comma(bytesPerCell * 2);
    Word("-");
    Semicolon();
}

void DefineCFA() {
    Colon("CFA");
    Word("LIT");
    Comma(bytesPerCell);
    Word("-");
    Semicolon();
}

void DefineNFA() {
    Colon("NFA");
    Word("LIT");
    Comma(bytesPerCell * 2 + 1);
    Word("-");
    Word("LIT");
    Comma(cellMax);
    Word("TRAVERSE");
    Semicolon();
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
    Word("DUP");
    Word("LIT");
    Comma(cellPaddingMask);
    Word("AND");
    Word("0BRANCH");
    Comma(4);
    Word("1+");
    Word("BRANCH");
    Comma(0xfff8);
    Word("LIT");
    Comma(bytesPerCell * 2);
    Word("+");
    Semicolon();
}

void DefineSCSP() {
    Colon("!CSP");
    Word("SP@");
    Word("CSP");
    Word("!");
    Semicolon();
}

void DefineQERR() {
    Colon("?ERROR");
    Word("SWAP");
    Word("0BRANCH");
    Comma(4);
    Word("ERROR");
    Word("BRANCH");
    Comma(2);
    Word("DROP");
    Semicolon();
}

void DefineQCOMP() {
    Colon("?COMP");
    Word("STATE");
    Word("@");
    Word("0=");
    Word("LIT");
    Comma(0x11);
    Word("?ERROR");
    Semicolon();
}

void DefineQEXEC() {
    Colon("?EXEC");
    Word("STATE");
    Word("@");
    Word("LIT");
    Comma(0x12);
    Word("?ERROR");
    Semicolon();
}

void DefineQPAIRS() {
    Colon("?PAIRS");
    Word("-");
    Word("LIT");
    Comma(0x13);
    Word("?ERROR");
    Semicolon();
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
    Semicolon();
}

void DefineQLOAD() {
    Colon("?LOADING");
    Word("BLK");
    Word("@");
    Word("0=");
    Comma(0x16);
    Word("?ERROR");
    Semicolon();
}

void DefineCOMPILE() {
    Colon("COMPILE");
    Word("?COMP");
    Word("R>");
    Word("DUP");
    Word("LIT");
    Comma(bytesPerCell);
    Word("+");
    Word(">R");
    Word("@");
    Word(",");
    Semicolon();
}

void DefineLBRAC() {
    Colon("[");
    Word("0");
    Word("STATE");
    Word("!");
    Semicolon();
}

void DefineRBRAC() {
    Colon("]");
    Word("LIT");
    Comma(0xc0);
    Word("STATE");
    Word("!");
    Semicolon();
}

void DefineSMUDG() {
    Colon("SMUDGE");
    Word("LATEST");
    Word("LIT");
    Comma(0x20);
    Word("TOGGLE");
    Semicolon();
}

void DefineHEX() {
    Colon("HEX");
    Word("LIT");
    Comma(16);
    Word("BASE");
    Word("!");
    Semicolon();
}

void DefineDECIMAL() {
    Colon("DECIMAL");
    Word("LIT");
    Comma(10);
    Word("BASE");
    Word("!");
    Semicolon();
}

void DefinePSCOD() {
    Colon("(;CODE)");
    Word("R>");
    Word("LATEST");
    Word("PFA");
    Word("CFA");
    Word("!");
    Semicolon();
}

void DefineSCODE() {
    Colon(";CODE", immediateWordFlag);
    Word("?CSP");
    Word("COMPILE");
    Word("(;CODE)");
    Word("[");
    Word("SMUDGE");
    Semicolon();
}

void DefineBUILD() {
    Colon("<BUILDS");
    Word("0");
    Word("CONSTANT");
    Semicolon();
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
    Semicolon();
}

void DefineTYPE() {
    Colon("TYPE");
    Word("-DUP");
    Word("0BRANCH");
    Comma(12);
    Word("OVER");
    Word("+");
    Word("SWAP");
    Word("(DO)");
    Word("I");
    Word("C@");
    Word("EMIT");
    Word("(LOOP)");
    Comma(-4);
    Word("BRANCH");
    Comma(2);
    Word("DROP");
    Semicolon();
}

void DefineDTRAI() {
    Colon("-TRAILING");
    Word("DUP");
    Word("0");
    Word("(DO)");
    Word("OVER");
    Word("OVER");
    Word("+");
    Word("1");
    Word("-");
    Word("C@");
    Word("BL");
    Word("-");
    Word("0BRANCH");
    Comma(4);
    Word("LEAVE");
    Word("BRANCH");
    Comma(3);
    Word("1");
    Word("-");
    Word("(LOOP)");
    Comma(0xfff0);
    Semicolon();
}

void DefinePDOTQ() {
    Colon("(.\")");
    Word("R");
    Word("COUNT");
    Word("DUP");
    Word("1+");
    Word("DUP");    // Added to skip padding
    Word("LIT");
    Comma(cellPaddingMask);
    Word("AND");
    Word("0BRANCH");
    Comma(2);
    Word("1+");     // End of added code
    Word("R>");
    Word("+");
    Word(">R");
    Word("TYPE");
    Semicolon();
}

void DefineDOTQ() {
    Colon(".\"", immediateWordFlag);
    Word("LIT");
    Comma(0x22);
    Word("STATE");
    Word("@");
    Word("0BRANCH");
    Comma(17);
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
    Word("BRANCH");
    Comma(5);
    Word("WORD");
    Word("HERE");
    Word("COUNT");
    Word("TYPE");
    Semicolon();
}

void DefineEXPEC() {
    Colon("EXPECT");
    Word("OVER");
    Word("+");
    Word("OVER");
    Word("(DO)");
    Word("KEY");
    Word("DUP");
    Word("LIT");
    Comma(0x0e);
    Word("+ORIGIN");
    Word("@");
    Word("=");
    Word("0BRANCH");
    Comma(16);
    Word("DROP");
    Word("LIT");
    Comma(0x08);
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
    Word("BRANCH");
    Comma(20);
    Word("DUP");
    Word("LIT");
    Comma(0x0a);
    Word("=");
    Word("0BRANCH");
    Comma(7);
    Word("LEAVE");
    Word("DROP");
    Word("BL");
    Word("0");
    Word("BRANCH");
    Comma(2);
    Word("DUP");
    Word("I");
    Word("C!");
    Word("0");
    Word("I");
    Word("1+");
    Word("C!");
    Word("EMIT");
    Word("(LOOP)");
    Comma(0xffd3);
    Word("DROP");
    Semicolon();
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
    Semicolon();
}

// This is special cased by Header to make a one character name with a null
// used to end interpretation of a buffer.
void DefineX() {
    Colon("", immediateWordFlag);
    Word("BLK");
    Word("@");
    Word("0BRANCH");
    Comma(21);
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
    Word("0BRANCH");
    Comma(4);
    Word("?EXEC");
    Word("R>");
    Word("DROP");
    Word("BRANCH");
    Comma(3);
    Word("R>");
    Word("DROP");
    Semicolon();
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
    Semicolon();
}

void DefineERASE() {
    Colon("ERASE");
    Word("0");
    Word("FILL");
    Semicolon();
}

void DefineBLANK() {
    Colon("BLANKS");
    Word("BL");
    Word("FILL");
    Semicolon();
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
    Semicolon();
}

void DefinePAD() {
    Colon("PAD");
    Word("HERE");
    Word("LIT");
    Comma(68);      // PAD is 68 bytes about here.
    Word("+");
    Semicolon();
}

void DefineWORD() {
    Colon("WORD");
    Word("BLK");
    Word("@");
    Word("0BRANCH");
    Comma(6);
    Word("BLK");
    Word("@");
    Word("BLOCK");
    Word("BRANCH");
    Comma(3);
    Word("TIB");
    Word("@");
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
    Semicolon();
}

void DefinePNUMB() {
    Colon("(NUMBER)");
    Word("1+");
    Word("DUP");
    Word(">R");
    Word("C@");
    Word("BASE");
    Word("@");
    Word("DIGIT");
    Word("0BRANCH");
    Comma(22);
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
    Word("0BRANCH");
    Comma(4);
    Word("1");
    Word("DPL");
    Word("+!");
    Word("R>");
    Word("BRANCH");
    Comma(0xffe3);
    Word("R>");
    Semicolon();
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
    Word("DPL");
    Word("!");
    Word("(NUMBER)");
    Word("DUP");
    Word("C@");
    Word("BL");
    Word("-");
    Word("0BRANCH");
    Comma(11);
    Word("DUP");
    Word("C@");
    Word("LIT");
    Comma(0x2e);
    Word("-");
    Word("0");
    Word("?ERROR");
    Word("0");
    Word("BRANCH");
    Comma(0xffee);
    Word("DROP");
    Word("R>");
    Word("0BRANCH");
    Comma(2);
    Word("DMINUS");
    Semicolon();
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
    Word("0BRANCH");
    Comma(5);
    Word("DROP");
    Word("HERE");
    Word("LATEST");
    Word("(FIND)");
    Semicolon();
}

void DefinePABOR() {
    Colon("(ABORT)");
    Word("ABORT");
    Semicolon();
}

void DefineERROR() {
    Colon("ERROR");
    Word("WARNING");
    Word("@");
    Word("0<");
    Word("0BRANCH");
    Comma(2);
    Word("(ABORT)");
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
    Semicolon();
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
    Semicolon();
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
    Word("0BRANCH");
    Comma(8);
    Word("DROP");
    Word("NFA");
    Word("ID.");
    Word("LIT");
    Comma(4);
    Word("MESSAGE");
    Word("SPACE");
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
    Word("HERE");
    Word("1");
    Word("-");
    Word("LIT");
    Comma(0x80);
    Word("TOGGLE");
    Word("LATEST");
    Word(",");
    Word("CURRENT");
    Word("@");
    Word("!");
    Word("HERE");
    Word("2+");
    Word(",");
    Semicolon();
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
    Semicolon();
}

void DefineLITER() {
    Colon("LITERAL", immediateWordFlag);
    Word("STATE");
    Word("@");
    Word("0BRANCH");
    Comma(4);
    Word("COMPILE");
    Word("LIT");
    Word(",");
    Semicolon();
}

void DefineDLIT() {
    Colon("DLITERAL", immediateWordFlag);
    Word("STATE");
    Word("@");
    Word("0BRANCH");
    Comma(4);
    Word("SWAP");
    Word("LITERAL");
    Word("LITERAL");
    Semicolon();
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
    Semicolon();
}

void DefineINTER() {
    Colon("INTERPRET");
    Word("-FIND");
    Word("0BRANCH");
    Comma(15);
    Word("STATE");
    Word("@");
    Word("<");
    Word("0BRANCH");
    Comma(5);
    Word("CFA");
    Word(",");
    Word("BRANCH");
    Comma(3);
    Word("CFA");
    Word("EXECUTE");
    Word("?STACK");
    Word("BRANCH");
    Comma(14);
    Word("HERE");
    Word("NUMBER");
    Word("DPL");
    Word("@");
    Word("1+");
    Word("0BRANCH");
    Comma(4);
    Word("DLITERAL");
    Word("BRANCH");
    Comma(3);
    Word("DROP");
    Word("LITERAL");
    Word("?STACK");
    Word("BRANCH");
    Comma(0xffe1);
}

void DefineIMMEDIATE() {
    Colon("IMMEDIATE");
    Word("LATEST");
    Word("LIT");
    Comma(immediateWordFlag);
    Word("TOGGLE");
    Semicolon();
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
    Word("LIT");         // Altered from 6502 source to handle variable cell size
    Comma(bytesPerCell);
    Word("+");           // End of change
    Word("CONTEXT");
    Word("!");
    Semicolon();
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
    Semicolon();
}

void DefinePAREN() {
    Colon("(", immediateWordFlag);
    Word("LIT");
    Comma(0x29);
    Word("WORD");
    Semicolon();
}

void DefineQUIT() {
    Colon("QUIT");
    Word("0");
    Word("BLK");
    Word("!");
    Word("[");
    Word("RP!");
    Word("CR");
    Word("QUERY");
    Word("INTERPRET");
    Word("STATE");
    Word("@");
    Word("0=");
    Word("0BRANCH");
    Comma(4);
    Word("(.\")");
    String("OK");
    Word("BRANCH");
    Comma(0xfff3);
    Semicolon();
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
    Semicolon();
}

void DefinePM() {
    Colon("+-");
    Word("0<");
    Word("0BRANCH");
    Comma(2);
    Word("MINUS");
    Semicolon();
}

void DefineDPM() {
    Colon("D+-");
    Word("0<");
    Word("0BRANCH");
    Comma(2);
    Word("DMINUS");
    Semicolon();
}

void DefineABS() {
    Colon("ABS");
    Word("DUP");
    Word("+-");
    Semicolon();
}

void DefineDABS() {
    Colon("DABS");
    Word("DUP");
    Word("D+-");
    Semicolon();
}

void DefineMIN() {
    Colon("MIN");
    Word("OVER");
    Word("OVER");
    Word(">");
    Word("0BRANCH");
    Comma(2);
    Word("SWAP");
    Word("DROP");
    Semicolon();
}

void DefineMAX() {
    Colon("MAX");
    Word("OVER");
    Word("OVER");
    Word("<");
    Word("0BRANCH");
    Comma(2);
    Word("SWAP");
    Word("DROP");
    Semicolon();
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
    Semicolon();
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
    Semicolon();
}

void DefineSTAR() {
    Colon("*");
    Word("U*");
    Word("DROP");
    Semicolon();
}

void DefineSLMOD() {
    Colon("/MOD");
    Word(">R");
    Word("S->D");
    Word("R>");
    Word("M/");
    Semicolon();
}
void DefineSLASH() {
    Colon("/");
    Word("/MOD");
    Word("SWAP");
    Word("DROP");
    Semicolon();
}

void DefineMOD() {
    Colon("MOD");
    Word("/MOD");
    Word("DROP");
    Semicolon();
}

void DefineSSMOD() {
    Colon("*/MOD");
    Word(">R");
    Word("M*");
    Word("R>");
    Word("M/");
    Semicolon();
}

void DefineSSLAS() {
    Colon("*/");
    Word("*/MOD");
    Word("SWAP");
    Word("DROP");
    Semicolon();
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
    Semicolon();
}

void DefinePBUF() {
    Colon("+BUF");
    Word("LIT");
    Comma(SSIZE + 4); //    holds block #, one sector, two nu
    Word("+");
    Word("DUP");
    Word("LIMIT");
    Word("=");
    Word("0BRANCH");
    Comma(3);
    Word("DROP");
    Word("FIRST");
    Word("DUP");
    Word("PREV");
    Word("@");
    Word("-");
    Semicolon();
}

void DefineUPDAT() {
    Colon("UPDATE");
    Word("PREV");
    Word("@");
    Word("@");
    Word("LIT");
    Comma(0x8000);
    Word("OR");
    Word("PREV");
    Word("@");
    Word("!");
    Semicolon();
}

void DefineFLUSH() {
    Colon("FLUSH");
    Word("LIMIT");
    Word("FIRST");
    Word("-");
    Word("B/BUF");
    Word("LIT");
    Comma(4);
    Word("+");
    Word("/");
    Word("1+");
    Word("0");
    Word("(DO)");
    Word("LIT");
    Comma(0x7fff);
    Word("BUFFER");
    Word("DROP");
    Word("(LOOP)");
    Comma(0xfffb);
    Semicolon();
}

void DefineEMPTYBUFFERS() {
    Colon("EMPTY-BUFFERS");
    Word("FIRST");
    Word("LIMIT");
    Word("OVER");
    Word("-");
    Word("ERASE");
    Semicolon();
}

void DefineDR0() {
    Colon("DR0");
    Word("0");
    Word("OFFSET");
    Word("!");
    Semicolon();
}

void DefineDR1() {
    Colon("DR1");
    Word("LIT");
    Comma(SECTR);    // Sectors per drive
    Word("OFFSET");
    Word("!");
    Semicolon();
}

void DefineBUFFR() {
    Colon("BUFFER");
    Word("USE");
    Word("@");
    Word("DUP");
    Word(">R");
    Word("+BUF");
    Word("0BRANCH");
    Comma(0xfffe);
    Word("USE");
    Word("!");
    Word("R");
    Word("@");
    Word("0<");
    Word("0BRANCH");
    Comma(10);
    Word("R");
    Word("2+");
    Word("R");
    Word("@");
    Word("LIT");
    Comma(0x7fff);
    Word("AND");
    Word("0");
    Word("R/W");
    Word("R");
    Word("!");
    Word("R");
    Word("PREV");
    Word("!");
    Word("R>");
    Word("2+");
    Semicolon();
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
    Word("0BRANCH");
    Comma(26);
    Word("+BUF");
    Word("0=");
    Word("0BRANCH");
    Comma(10);
    Word("DROP");
    Word("R");
    Word("BUFFER");
    Word("DUP");
    Word("R");
    Word("1");
    Word("R/W");
    Word("2");
    Word("-");
    Word("DUP");
    Word("@");
    Word("R");
    Word("-");
    Word("DUP");
    Word("+");
    Word("0=");
    Word("0BRANCH");
    Comma(0xffe8);
    Word("DUP");
    Word("PREV");
    Word("!");
    Word("R>");
    Word("DROP");
    Word("2+");
    Semicolon();
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
    Semicolon();
}

void DefineDLINE() {
    Colon(".LINE");
    Word("(LINE)");
    Word("-TRAILING");
    Word("TYPE");
    Semicolon();
}

void DefineMESS() {
    Colon("MESSAGE");
    Word("WARNING");
    Word("@");
    Word("0BRANCH");
    Comma(14);
    Word("-DUP");
    Word("0BRANCH");
    Comma(9);
    Word("LIT");
    Comma(4);
    Word("OFFSET");
    Word("@");
    Word("B/SCR");
    Word("/");
    Word("-");
    Word(".LINE");
    Word("BRANCH");
    Comma(7);
    Word("(.\")");
    String("MSG # ");
    Word(".");
    Semicolon();
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
    Semicolon();
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
    Semicolon();
}

void DDISC() {
    // Write me
}

void DefineDBCD() {
    Colon("-BCD");
    Word("0");
    Word("LIT");
    Comma(10);
    Word("U/");
    Word("LIT");
    Comma(16);
    Word("*");
    Word("OR");
    Semicolon();
}

void DefineRSLW() {
    Colon("R/W");
    Word("0=");
    Word("LIT");
    Comma(0xc4da);        // YIKES!! Revisit
    Word("C!");
    Word("SWAP");
    Word("0");
    Word("!");
    Word("0");
    Word("OVER");
    Word(">");
    Word("OVER");
    Word("LIT");
    Comma(SECTL - 1);
    Word(">");
    Word("OR");
    Word("LIT");
    Comma(6);
    Word("?ERROR");
    Word("0");
    Word("LIT");
    Comma(SECTR);
    Word("U/");
    Word("1+");
    Word("SWAP");
    Word("0");
    Word("LIT");
    Comma(0x12);
    Word("U/");
    Word("-BCD");
    Word("SWAP");
    Word("1+");
    Word("-BCD");
    Word("-DISC");
    Word("LIT");
    Comma(8);
    Word("?ERROR");
    Semicolon();
}

void DefineTICK() {
    Colon("'", immediateWordFlag);
    Word("-FIND");
    Word("0=");
    Word("0");
    Word("?ERROR");
    Word("DROP");
    Word("LITERAL");
    Semicolon();
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
    Word("R");
    Word("OVER");
    Word("U<");
    Word("0BRANCH");
    Comma(9);
    Word("FORTH");
    Word("DEFINITIONS");
    Word("@");
    Word("DUP");
    Word("VOC-LINK");
    Word("!");
    Word("BRANCH");
    Comma(0xffee);
    Word("DUP");
    Word("LIT");
    Comma(4);
    Word("-");
    Word("PFA");
    Word("LFA");
    Word("@");
    Word("DUP");
    Word("R");
    Word("U<");
    Word("0BRANCH");
    Comma(0xfff9);
    Word("OVER");
    Word("2");
    Word("-");
    Word("!");
    Word("@");
    Word("-DUP");
    Word("0=");
    Word("0BRANCH");
    Comma(0xffe0);
    Word("R>");
    Word("DP");
    Word("!");
    Semicolon();
}

void DefineBACK() {
    Colon("BACK");
    Word("HERE");
    Word("-");
    Word(",");
    Semicolon();
}

void DefineBEGIN() {
    Colon("BEGIN", immediateWordFlag);
    Word("?COMP");
    Word("HERE");
    Word("1");
    Semicolon();
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
    Semicolon();
}

void DefineTHEN() {
    Colon("THEN", immediateWordFlag);
    Word("ENDIF");
    Semicolon();
}
 
void DefineDO() {
    Colon("DO", immediateWordFlag);
    Word("COMPILE");
    Word("(DO)");
    Word("HERE");
    Word("3");
    Semicolon();
}

void DefineLOOP() {
    Colon("LOOP", immediateWordFlag);
    Word("3");
    Word("?PAIRS");
    Word("COMPILE");
    Word("(LOOP)");
    Word("BACK");
    Semicolon();
}

void DefinePLOOP() {
    Colon("+LOOP");
    Word("3");
    Word("?PAIRS");
    Word("COMPILE");
    Word("(+LOOP)");
    Word("BACK");
    Semicolon();
}

void DefineUNTIL() {
    Colon("UNTIL", immediateWordFlag);
    Word("1");
    Word("?PAIRS");
    Word("COMPILE");
    Word("0BRANCH");
    Word("BACK");
    Semicolon();
}

void DefineEND() {
    Colon("END", immediateWordFlag);
    Word("UNTIL");
    Semicolon();
}

void DefineAGAIN() {
    Colon("AGAIN", immediateWordFlag);
    Word("1");
    Word("?PAIRS");
    Word("COMPILE");
    Word("BRANCH");
    Word("BACK");
    Semicolon();
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
    Semicolon();
}

void DefineIF() {
    Colon("IF", immediateWordFlag);
    Word("COMPILE");
    Word("0BRANCH");
    Word("HERE");
    Word("0");
    Word(",");
    Word("2");
    Semicolon();
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
    Semicolon();
}

void DefineWHILE() {
    Colon("WHILE", immediateWordFlag);
    Word("IF");
    Word("2+");
    Semicolon();
}

void DefineSPACS() {
    Colon("SPACES");
    Word("0");
    Word("MAX");
    Word("-DUP");
    Word("0BRANCH");
    Comma(6);
    Word("0");
    Word("(DO)");
    Word("SPACE");
    Word("(LOOP)");
    Comma(0xfffe);
    Semicolon();
}

void DefineBDIGS() {
    Colon("<#");
    Word("PAD");
    Word("HLD");
    Word("!");
    Semicolon();
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
    Semicolon();
}

void DefineSIGN() {
    Colon("SIGN");
    Word("ROT");
    Word("0<");
    Word("0BRANCH");
    Comma(4);
    Word("LIT");
    Comma(0x2d);
    Word("HOLD");
    Semicolon();
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
    Word("0BRANCH");
    Comma(4);
    Word("LIT");
    Comma(7);
    Word("+");
    Word("LIT");
    Comma(0x30);
    Word("+");
    Word("HOLD");
    Semicolon();
}

void DefineDIGS() {
    Colon("#S");
    Word("#");
    Word("OVER");
    Word("OVER");
    Word("OR");
    Word("0=");
    Word("0BRANCH");
    Comma(0xfffa);
    Semicolon();
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
    Semicolon();
}

void DefineDDOT() {
    Colon("D.");
    Word("0");
    Word("D.R");
    Word("SPACE");
    Semicolon();
}

void DefineDOTR() {
    Colon(".R");
    Word(">R");
    Word("S->D");
    Word("R>");
    Word("D.R");
    Semicolon();
}

void DefineDOT() {
    Colon(".");
    Word("S->D");
    Word("D.");
    Semicolon();
}

void DefineQUES() {
    Colon("?");
    Word("@");
    Word(".");
    Semicolon();
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
    Word("LIT");
    Comma(16);
    Word("0");
    Word("(DO)");
    Word("CR");
    Word("I");
    Word("3");
    Word(".R");
    Word("SPACE");
    Word("I");
    Word("SCR");
    Word("@");
    Word(".LINE");
    Word("(LOOP)");
    Comma(0xfff0);
    Word("CR");
    Semicolon();
}

void DefineINDEX() {
    Colon("INDEX");
    Word("CR");
    Word("1+");
    Word("SWAP");
    Word("(DO)");
    Word("CR");
    Word("I");
    Word("3");
    Word(".R");
    Word("SPACE");
    Word("0");
    Word("I");
    Word(".LINE");
    Word("?TERMINAL");
    Word("0BRANCH");
    Comma(2);
    Word("LEAVE");
    Word("(LOOP)");
    Comma(0xfff3);
    Word("LIT");
    Comma(0x0c);    // FORM FEED FOR PRINTER
    Word("EMIT");
    Semicolon();
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
    Word("CR");
    Word("I");
    Word("LIST");
    Word("(LOOP)");
    Comma(0xfffc);
    Word("CR");
    Word("LIT");
    Comma(0x0f);
    Word("MESSAGE");
    Word("CR");
    Word("LIT");
    Comma(0x0c);
    Word("EMIT");
    Semicolon();
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
    Word("OUT");
    Word("@");
    Word("C/L");
    Word(">");
    Word("0BRANCH");
    Comma(5);
    Word("CR");
    Word("0");
    Word("OUT");
    Word("!");
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
    Word("0BRANCH");
    Comma(0xffea);
    Word("DROP");
    Semicolon();
}

void MON() {
    std::cout << std::endl << "Exiting with stack:";
    for (cell_t i = 0; i <= sp && sp != 0xffff; i++) {
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
        case Token::DDISC:
            return "DDISC";
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
        case Token::DDISC:
            DDISC();
            break;
        case Token::MON:
            MON();
            break;
        default:
            std::cerr << "Unimplemented token!" << std::endl;
            std::exit(1);
    }
}

void ExecuteTokenNonInline(Token token) {
    executeToken(token);
}

void InitUserMemory() {
    cell_t variablesToInit = memory.cell[ByteIndexToCellIndex(ORIG) + 1];
    for (cell_t i = 0; i < variablesToInit; i++) {

        memory.cell[ByteIndexToCellIndex(UAREA) + i] = memory.cell[ByteIndexToCellIndex(ORIG) + 2 + i];
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
    sp = cellMax;
    rp = cellMax;
    here = ByteIndexToCellIndex(ORIG);

    std::cout << "Building dictionary" << std::endl;

    // Cold start word
    Word("ABORT");

    // Number of user area entries to initialize
    Comma(6);
    Comma(TIBX);    // TIB
    Comma(31);      // WIDTH
    Comma(0);       // Warning 0=no disk 1=disk
    Comma(0);       // FENCE
    Comma(0);       // DP
    Comma(0);       // VOCL

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
    Constant("FIRST", DAREA);
    Constant("LIMIT", UAREA);
    Constant("B/BUF", SSIZE);
    Constant("B/SCR", 1);

    DefinePORIG();
    User("TIB", tibUserIndex);
    User("WIDTH", widthUserIndex);
    User("WARNING", warningUserIndex);
    User("FENCE", fenceUserIndex);
    User("DP", dpUserIndex);
    User("VOC-LINK", voclUserIndex);
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
    DefineDR1();
    DefineBUFFR();
    DefineBLOCK();
    DefinePLINE();
    DefineDLINE();
    DefineMESS();
    DefineLOAD();
    DefineNEXTSCREEN();
    Primitive("-DISC", Token::DDISC);
    DefineDBCD();
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

    memory.cell[ByteIndexToCellIndex(ORIG + 10)] = CellIndexToByteIndex(here);
    memory.cell[ByteIndexToCellIndex(ORIG + 12)] = CellIndexToByteIndex(here);
    memory.cell[ByteIndexToCellIndex(ORIG + 14)] = CellIndexToByteIndex(forthLastWordReference + 1);

    if (dumpDictionary) {
        DumpDictionary();
    }

    VirtualMachine();
}

/*
 * mkdisk - Create a disk file for lnFORTH running under Linux.
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

#include <iostream>
#include <fstream>

bool ParseBlocksArg(const char *blockString, unsigned &blocks) {
    errno = 0;
    char *endPtr;
    long value = strtol(blockString, &endPtr, 10);

    if (errno == ERANGE) {
        return false;
    }

    if (endPtr == blockString) {
        return false;
    }

    if (*endPtr != 0) {
        return false;
    }

    if (value < 0) {
        return false;
    }

    blocks = (unsigned)value;
    return true;
}

void CreateDisk(const char *fileName, unsigned blocks) {
    std::ofstream file;

    unsigned fileSize = blocks * 2048;

    file.open(fileName);
    if (!file.is_open()) {
        std::cerr << "Failed to open file \"" << fileName << "\" for writing"
                  << std::endl;
        std::exit(1);
    }

    // This is stupidly inefficient, but I don't think it matters much in
    // the overall scheme of life.
    for (unsigned byte = 0; byte < fileSize; byte++) {
        file << " ";
    }

    file.close();
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: " << argv[0] << " <filename> <number blocks>"
                  << std::endl;
        std::exit(1);
    }

    const char *fileName = argv[1];
    unsigned blocks;
    if (!ParseBlocksArg(argv[2], blocks)) {
        std::cerr << "Invalid number of blocks parameter \"" << argv[2] << "\""
                  << std::endl;
        std::exit(1);
    }
    std::cout << "Creating block file \"" << fileName <<"\" with " << blocks
              << " blocks" << std::endl;

    CreateDisk(fileName, blocks);
}

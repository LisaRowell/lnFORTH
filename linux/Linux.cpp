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

#include <stdlib.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>

#include <iostream>

#include "../Platform.h"

static struct termios originalTermios;

static void DisableRawMode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &originalTermios);
}

static void EnableRawMode() {
    tcgetattr(STDIN_FILENO, &originalTermios);
    atexit(DisableRawMode);

    struct termios newTermios = originalTermios;
    newTermios.c_lflag &= ~(ECHO | ICANON);
    newTermios.c_cc[VMIN] = 1;
    newTermios.c_cc[VTIME] = 0;

    tcsetattr(0, TCSANOW, &newTermios);
}

void XInit() {
    EnableRawMode();
}

char XKey() {
    char key;

    if (read(STDIN_FILENO, &key, 1) != 1) {
        std::cerr << "XKey failed to read character" << std::endl;
        std::exit(1);
    }

    return key;
}

void XEmit(char character) {
    (void)write(STDIN_FILENO, &character, 1);
}

void XCR() {
    XEmit(10);
}

uint32_t XMillis() {
    struct timeval time;

    (void)gettimeofday(&time, NULL);
    uint64_t millis = time.tv_sec * 1000 + time.tv_usec / 1000;

    return (uint32_t)millis;
}

bool XRead(uint8_t *addr, uint32_t blkDiskOffset, size_t length) {
    FILE *file = fopen("disk", "r");
    if (file == nullptr) {
        return false;
    }

    if (fseek(file, blkDiskOffset, SEEK_SET) != 0) {
        (void)fclose(file);
        return false;
    }
    if (fread(addr, 1, length, file) != length) {
        (void)fclose(file);
        return false;
    }

    if (fclose(file) != 0) {
        return false;
    }

    return true;
}

bool XWrite(uint8_t *addr, uint32_t blkDiskOffset, size_t length) {
    int file = open("disk", O_WRONLY);
    if (file < 0) {
        return false;
    }

    if (lseek(file, blkDiskOffset, SEEK_SET) != blkDiskOffset) {
        (void)close(file);
        return false;
    }
    if (write(file, addr, length) != length) {
        (void)close(file);
        return false;
    }

    if (close(file) != 0) {
        return false;
    }

    return true;
}

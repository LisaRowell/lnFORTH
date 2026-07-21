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
#include "lnFORTH.h"

#include <iostream>

const bool debugStartup = false;

int main(int ac, char* av[]) {
    XInit();

    BuildDictionary();
    InitUserMemory();

    if (debugStartup) {
        std::cout << "Starting Virtual Machine" << std::endl;
    }

    RunVirtualMachine();
}

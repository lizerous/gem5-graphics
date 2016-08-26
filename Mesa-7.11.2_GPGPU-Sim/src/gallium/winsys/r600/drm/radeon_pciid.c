/*
 * Copyright 2010 Jerome Glisse <glisse@freedesktop.org>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * on the rights to use, copy, modify, merge, publish, distribute, sub
 * license, and/or sell copies of the Software, and to permit persons to whom
 * the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHOR(S) AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *      Jerome Glisse
 */
#include <stdlib.h>
#include "r600_priv.h"

struct pci_id {
	unsigned	vendor;
	unsigned	device;
	unsigned	family;
};

static const struct pci_id radeon_pci_id[] = {
#define CHIPSET(chip, name, family) { 0x1002, chip, CHIP_##family },
#include "pci_ids/r600_pci_ids.h"
	{0, 0},
};

unsigned radeon_family_from_device(unsigned device)
{
	unsigned i;

	for (i = 0; ; i++) {
		if (!radeon_pci_id[i].vendor)
			return CHIP_UNKNOWN;
		if (radeon_pci_id[i].device == device)
			return radeon_pci_id[i].family;
	}
	return CHIP_UNKNOWN;
}

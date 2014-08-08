
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <malloc.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/vga/vga.h>
#include <hw/pci/pci.h>
#include <hw/dos/dos.h>
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/vga/vgagui.h>
#include <hw/vga/vgatty.h>
#include <hw/ide/idelib.h>

#include "testutil.h"

unsigned char sanitizechar(unsigned char c) {
	if (c < 32) return '.';
	return c;
}

int wait_for_enter_or_escape() {
	int c;

	do {
		c = getch();
		if (c == 0) c = getch() << 8;
	} while (!(c == 13 || c == 27));

	return c;
}

/* construct ATAPI/SCSI-MMC READ command according to user's choice, either READ(10) or READ(12) */
void do_construct_atapi_scsi_mmc_read(unsigned char *buf/*must be 12 bytes*/,uint32_t sector,uint32_t tlen_sect,unsigned char read_mode) {
	memset(buf,0,12);
	if (read_mode == 12) {
		/* command: READ(12) */
		buf[0] = 0xA8;

		/* fill in the Logical Block Address */
		buf[2] = sector >> 24;
		buf[3] = sector >> 16;
		buf[4] = sector >> 8;
		buf[5] = sector;

		buf[6] = tlen_sect >> 24UL;
		buf[7] = tlen_sect >> 16UL;
		buf[8] = tlen_sect >> 8UL;
		buf[9] = tlen_sect;
	}
	else {
		/* command: READ(10) */
		buf[0] = 0x28;

		/* fill in the Logical Block Address */
		buf[2] = sector >> 24;
		buf[3] = sector >> 16;
		buf[4] = sector >> 8;
		buf[5] = sector;

		buf[7] = tlen_sect >> 8;
		buf[8] = tlen_sect;
	}
}



#if defined(USE_DOSLIB)
# include <hw/cpu/cpu.h>
# include <hw/dos/dos.h>
# include <hw/vga/vga.h>
# include <hw/vesa/vesa.h>
# include <hw/8254/8254.h>
# include <hw/8259/8259.h>
# include <hw/8042/8042.h>
# include <hw/dos/doswin.h>
# include <hw/dosboxid/iglib.h> /* for debugging */

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>

#include "ifict.h"
#include "utils.h"
#include "debug.h"
#include "fatal.h"
#include "palette.h"

unsigned char*			vesa_lfb = NULL; /* video memory, linear framebuffer */
unsigned char*			vesa_lfb_offscreen = NULL; /* system memory framebuffer, to copy to video memory */
uint32_t			vesa_lfb_physaddr = 0;
uint32_t			vesa_lfb_map_size = 0;
uint32_t			vesa_lfb_stride = 0;
uint16_t			vesa_lfb_height = 0;
uint16_t			vesa_lfb_width = 0;
bool				vesa_setmode = false;
bool				vesa_8bitpal = false; /* 8-bit DAC */
uint16_t			vesa_mode = 0;

unsigned char			vesa_pal[256*4];

uint32_t			pit_count = 0;
uint16_t			pit_prev = 0;

ifevidinfo_t			ifevidinfo_doslib;

/* keyboard controller and IRQ 1, IBM PC/AT 8042 */
bool				keybirq_quit = false;
bool				keybirq_init = false;
void				(__interrupt *keybirq_old)() = NULL;
unsigned char			keybirq_buf[64];
unsigned char			keybirq_head=0,keybirq_tail=0;

unsigned char			p_keybin[3];
unsigned char			p_keybinpos=0;
unsigned char			p_keybinlen=0;

uint32_t			keyb_mod=0;

IFEMouseStatus			ifemousestat;

/* NTS: Do not attempt a protected mode INT 33h driver callback routine, it won't work and will crash a lot */

/* read keyboard buffer */
int keybirq_read_buf(void) {
	int r = -1;

	/* protect from interruption by PUSH+CLI+....+POPF */
	SAVE_CPUFLAGS( _cli() ) {
		if (keybirq_head != keybirq_tail) {
			r = (int)((unsigned char)keybirq_buf[keybirq_head]);
			if (++keybirq_head >= sizeof(keybirq_buf))
				keybirq_head = 0;
		}
	} RESTORE_CPUFLAGS();

	return r;
}

/* IRQ 1 keyboard interrupt */
void __interrupt keybirq() {
	while (inp(K8042_STATUS) & 0x01) {
		unsigned char np = (unsigned char)(keybirq_tail + 1u);

		if (np >= sizeof(keybirq_buf))
			np = 0;

		if (np == keybirq_head)
			break;

		keybirq_buf[keybirq_tail] = inp(K8042_DATA);
		keybirq_tail = np;
	}

	p8259_OCW2(1,P8259_OCW2_SPECIFIC_EOI | 1); /* specific EOI ack IRQ 1 */
}

void keybirq_hook(void) {
	if (!keybirq_init) {
		SAVE_CPUFLAGS( _cli() ) {
			/* drain BIOS keyboard buffer */
			while (kbhit()) getch();

			p8259_mask(1); /* mask the IRQ so we can safely work on it */
			k8042_drain_buffer();
			p8259_OCW2(1,P8259_OCW2_SPECIFIC_EOI | 1); /* make sure the PIC is ready to work */
			keybirq_head = keybirq_tail = 0;
			p_keybinpos = p_keybinlen = 0;

			/* okay, hook the IRQ */
			keybirq_old = _dos_getvect(irq2int(1));
			_dos_setvect(irq2int(1),keybirq);


			/* alter BIOS data area to clear BIOS keyboard shift states */
			*((unsigned char far*)MK_FP(0x40,0x17)) &= ~0x0F; // clear alt/ctrl/lshift/rshift down status
			*((unsigned char far*)MK_FP(0x40,0x18))  =  0x00; // clear other key down status

			/* ready to work */
			p8259_unmask(1); /* unmask the IRQ */

			/* done */
			keybirq_init = true;
		} RESTORE_CPUFLAGS();
	}
}

void keybirq_unhook(void) {
	if (keybirq_init) {
		SAVE_CPUFLAGS( _cli() ) {
			/* drain BIOS keyboard buffer */
			while (kbhit()) getch();

			p8259_mask(1); /* mask the IRQ so we can safely work on it */
			k8042_drain_buffer();
			p8259_OCW2(1,P8259_OCW2_SPECIFIC_EOI | 1); /* make sure the PIC is ready to work */

			/* okay, hook the IRQ */
			_dos_setvect(irq2int(1),keybirq_old);
			keybirq_old = NULL;

			/* ready to work */
			p8259_unmask(1); /* unmask the IRQ */

			/* done */
			keybirq_init = false;
		} RESTORE_CPUFLAGS();
	}
}

static void p_SetPaletteColors(const unsigned int first,const unsigned int count,IFEPaletteEntry *pal) {
	unsigned int i;

	/* IBM PC/AT */
	if (vesa_8bitpal) {
		for (i=0;i < count;i++) {
			/* VGA 8-bit RGB */
			vesa_pal[i*4u + 0u] = (unsigned char)(pal[i].r);
			vesa_pal[i*4u + 1u] = (unsigned char)(pal[i].g);
			vesa_pal[i*4u + 2u] = (unsigned char)(pal[i].b);
		}
	}
	else {
		for (i=0;i < count;i++) {
			/* VGA 6-bit RGB */
			vesa_pal[i*4u + 0u] = (unsigned char)(pal[i].r >> 2u);
			vesa_pal[i*4u + 1u] = (unsigned char)(pal[i].g >> 2u);
			vesa_pal[i*4u + 2u] = (unsigned char)(pal[i].b >> 2u);
		}
	}

	vesa_set_palette_data(first,count,(char*)vesa_pal);
}

static uint32_t p_GetTicks(void) {
	uint32_t w,p;
	{
		uint16_t pit_cur = read_8254(T8254_TIMER_INTERRUPT_TICK);
		pit_count += (uint32_t)((uint16_t)(pit_prev - pit_cur)); /* 8254 counts DOWN, not UP, typecast to ensure 16-bit rollover */
		pit_prev = pit_cur;

		/* convert ticks to milliseconds */
		w = pit_count / (uint32_t)T8254_REF_CLOCK_HZ;
		p = ((pit_count % (uint32_t)T8254_REF_CLOCK_HZ) * (uint32_t)1000ul) / (uint32_t)T8254_REF_CLOCK_HZ;
	}

	return (w * (uint32_t)1000ul) + p;
}

static void p_ResetTicks(const uint32_t base) {
	/* convert milliseconds back to timer ticks and adjust */
	pit_count -= (base / (uint32_t)1000ul) * (uint32_t)T8254_REF_CLOCK_HZ;
	pit_count -= ((base % (uint32_t)1000ul) * (uint32_t)T8254_REF_CLOCK_HZ) / (uint32_t)1000ul;
}

static void p_UpdateFullScreen(void) {
	memcpy(vesa_lfb,vesa_lfb_offscreen,vesa_lfb_map_size);
}

static ifevidinfo_t* p_GetVidInfo(void) {
	return &ifevidinfo_doslib;
}

static bool p_UserWantsToQuit(void) {
	return keybirq_quit;
}

/* Assume, as all DOS machines have it, that the 8042 is emitting scan code set 1 */
static void p_ProcessScanCode(void) {
	IFEKeyEvent ke;

#define SETMOD(f,c) do { \
	if (c) keyb_mod |= f; \
	else keyb_mod &= ~f; } while (0)
#define TOGMOD(f,c) do { \
	if (c) keyb_mod ^= f; } while (0)
#define MAP(x,y) \
	case x: ke.code = (uint32_t)y; break;

	memset(&ke,0,sizeof(ke));

	if (p_keybinlen == 3) {
		IFEDBG("Key 0x%02x 0x%02x 0x%02x",p_keybin[0],p_keybin[1],p_keybin[2]);

		ke.raw_code = ((uint32_t)(p_keybin[2] & 0x7Fu)) | ((uint32_t)(p_keybin[1] & 0x7Fu) << 8u) | (uint32_t)0xE10000; /* this does not happen unless 0xE1 <xx> <xx> */
		ke.flags = ((p_keybin[2] & 0x80u) ? 0 : IFEKeyEvent_FLAG_DOWN) | keyb_mod; /* bit 7 clear if make code */
	}
	else if (p_keybinlen == 2) {
		IFEDBG("Key 0x%02x 0x%02x",p_keybin[0],p_keybin[1]);

		/* CTRL+Break. Note that pressing the key immediately sends make+break without
		 * the user having to release the key. In this engine, CTRL+BREAK is the signal
		 * to quit. */
		if (p_keybin[1] == 0xC6) {
			/* match break code */
			keybirq_quit = true;
			IFEDBG("User wants to quit (CTRL+BREAK)");
		}
		else if (p_keybin[1] == 0x46) {
			/* make code, ignore */
			return;
		}

		/* watch shift states */
		switch (p_keybin[1]&0x7Fu) {
			case 0x1D: SETMOD(IFEKeyEvent_FLAG_RCTRL,!(p_keybin[1]&0x80)); break;
			case 0x38: SETMOD(IFEKeyEvent_FLAG_RALT,!(p_keybin[1]&0x80)); break;
			default: break;
		}

		ke.raw_code = ((uint32_t)(p_keybin[1] & 0x7Fu)) | (uint32_t)0xE000; /* this does not happen unless 0xE0 <xx> */
		ke.flags = ((p_keybin[1] & 0x80u) ? 0 : IFEKeyEvent_FLAG_DOWN) | keyb_mod; /* bit 7 clear if make code */
	}
	else if (p_keybinlen == 1) {
		IFEDBG("Key 0x%02x",p_keybin[0]);

		/* watch shift states */
		switch (p_keybin[0]&0x7Fu) {
			case 0x2A: SETMOD(IFEKeyEvent_FLAG_LSHIFT,!(p_keybin[0]&0x80)); break;
			case 0x36: SETMOD(IFEKeyEvent_FLAG_RSHIFT,!(p_keybin[0]&0x80)); break;
			case 0x1D: SETMOD(IFEKeyEvent_FLAG_LCTRL,!(p_keybin[0]&0x80)); break;
			case 0x38: SETMOD(IFEKeyEvent_FLAG_LALT,!(p_keybin[0]&0x80)); break;
			case 0x3A: TOGMOD(IFEKeyEvent_FLAG_CAPS,!(p_keybin[0]&0x80)); break;
			case 0x45: TOGMOD(IFEKeyEvent_FLAG_NUMLOCK,!(p_keybin[0]&0x80)); break;
			default: break;
		}

		ke.raw_code = (uint32_t)(p_keybin[0] & 0x7Fu);
		ke.flags = ((p_keybin[0] & 0x80u) ? 0 : IFEKeyEvent_FLAG_DOWN) | keyb_mod; /* bit 7 clear if make code */

		switch (p_keybin[0]&0x7Fu) {
			MAP(0x1C,IFEKEY_RETURN);
			MAP(0x01,IFEKEY_ESCAPE);
			MAP(0x0E,IFEKEY_BACKSPACE);
			MAP(0x0F,IFEKEY_TAB);
			MAP(0x39,IFEKEY_SPACE);
			MAP(0x1E,IFEKEY_A);
			MAP(0x30,IFEKEY_B);
			MAP(0x2E,IFEKEY_C);
			MAP(0x20,IFEKEY_D);
			MAP(0x12,IFEKEY_E);
			MAP(0x21,IFEKEY_F);
			MAP(0x22,IFEKEY_G);
			MAP(0x23,IFEKEY_H);
			MAP(0x17,IFEKEY_I);
			MAP(0x24,IFEKEY_J);
			MAP(0x25,IFEKEY_K);
			MAP(0x26,IFEKEY_L);
			MAP(0x32,IFEKEY_M);
			MAP(0x31,IFEKEY_N);
			MAP(0x18,IFEKEY_O);
			MAP(0x19,IFEKEY_P);
			MAP(0x10,IFEKEY_Q);
			MAP(0x13,IFEKEY_R);
			MAP(0x1F,IFEKEY_S);
			MAP(0x14,IFEKEY_T);
			MAP(0x16,IFEKEY_U);
			MAP(0x2F,IFEKEY_V);
			MAP(0x11,IFEKEY_W);
			MAP(0x2D,IFEKEY_X);
			MAP(0x15,IFEKEY_Y);
			MAP(0x2C,IFEKEY_Z);
			MAP(0x0B,IFEKEY_0);
			MAP(0x02,IFEKEY_1);
			MAP(0x03,IFEKEY_2);
			MAP(0x04,IFEKEY_3);
			MAP(0x05,IFEKEY_4);
			MAP(0x06,IFEKEY_5);
			MAP(0x07,IFEKEY_6);
			MAP(0x08,IFEKEY_7);
			MAP(0x09,IFEKEY_8);
			MAP(0x0A,IFEKEY_9);
			MAP(0x33,IFEKEY_COMMA);
			MAP(0x34,IFEKEY_PERIOD);
			default: break;
		}
	}

	IFEDBG("Key event raw=%08x code=%08x flags=%08x",
		(unsigned long)ke.raw_code,
		(unsigned long)ke.code,
		(unsigned long)ke.flags);

	if (!IFEKeyQueue.add(ke))
		IFEDBG("ProcessKeyboardEvent: Queue full");

	IFEKeyboardProcessRawToCooked(ke);
#undef MAP
}

static void p_CheckKeyboard(void) {
	int c;

	/* Windows 3.1/95/98 hack:
	 *
	 *       Despite hooking IRQ 1 within the DOS VM, Windows 3.1/95/98 will NOT send any
	 *       keystrokes our way unless we poll for keyboard input using INT 21h. INT 21h
	 *       and DOS aren't going to see anything because we're handling the keyboard
	 *       controller directly anyway, but never mind that, gotta poll or Windows will
	 *       not send us keyboard input!
	 *
	 *       It turns out kbhit() is sufficient to make it happen, don't need to worry about
	 *       reading anything. */
	if (windows_mode != WINDOWS_NONE && keybirq_init) {
		kbhit();
	}

	do {
		if (p_keybinpos == 0) {
			c = keybirq_read_buf();
			if (c < 0) return;

			p_keybin[p_keybinpos++] = (unsigned char)c;

			if (c == 0xE1) /* 0xE1 <xx> <xx> */
				p_keybinlen=3;
			else if (c == 0xE0) /* 0xE0 <xx> */
				p_keybinlen=2;
			else {
				p_keybinlen=1;
				p_ProcessScanCode();
				p_keybinpos=0;
			}
		}
		else if (p_keybinpos < p_keybinlen) {
			c = keybirq_read_buf();
			if (c < 0) return;

			p_keybin[p_keybinpos++] = (unsigned char)c;
			if (p_keybinpos >= p_keybinlen) {
				p_ProcessScanCode();
				p_keybinpos=0;
			}
		}
		else {
			p_ProcessScanCode();
			p_keybinpos=0;
		}
	} while (1);
}

static void p_CheckMouse(void) {
	unsigned short btn=0,cnt=0;
	signed short x=0,y=0;
	bool latent=false;
	bool moved=false;
	IFEMouseEvent me;

	memset(&me,0,sizeof(me));

	__asm {
		mov	ax,3		; read position and button status
		int	33h
		mov	btn,bx		; BX=button status
		mov	x,cx		; CX=x coordinate
		mov	y,dx		; DX=y coordinate
	}

	me.pstatus = ifemousestat.status;

	if (btn & 1u)
		ifemousestat.status |= IFEMouseStatus_LBUTTON;
	else
		ifemousestat.status &= ~IFEMouseStatus_LBUTTON;

	if (btn & 2u)
		ifemousestat.status |= IFEMouseStatus_RBUTTON;
	else
		ifemousestat.status &= ~IFEMouseStatus_RBUTTON;

	if (btn & 4u)
		ifemousestat.status |= IFEMouseStatus_MBUTTON;
	else
		ifemousestat.status &= ~IFEMouseStatus_MBUTTON;

	/* read position and button status shows instantaneous button status at this moment.
	 * if the program is moving slowly, it might miss button clicks that way, so ask the
	 * mouse driver if any button clicks happened since we last checked. the call will
	 * clear the count after returning it. */
	__asm {
		mov	ax,5		; return button press data
		mov	bx,0		; left mouse button
		int	33h
		mov	cnt,bx		; BX=number of times the button was pressed
	}
	if (cnt != 0) { /* if any clicks recorded, then act as if the left button is done because it WAS */
		ifemousestat.status |= IFEMouseStatus_LBUTTON;
		latent = true;
	}

	if (ifemousestat.x != x || ifemousestat.y != y) {
		ifemousestat.x = x;
		ifemousestat.y = y;
		moved = true;
	}

	me.status = ifemousestat.status;

	if (moved || ((me.status^me.pstatus) & (IFEMouseStatus_LBUTTON|IFEMouseStatus_MBUTTON|IFEMouseStatus_RBUTTON)) != 0) {
		IFEDBG("Mouse event x=%d y=%d pstatus=%08x status=%08x latent=%u",
			ifemousestat.x,
			ifemousestat.y,
			(unsigned int)me.pstatus,
			(unsigned int)me.status,
			latent?1:0);
	}

	/* Add only button events */
	if ((me.status^me.pstatus) & (IFEMouseStatus_LBUTTON|IFEMouseStatus_MBUTTON|IFEMouseStatus_RBUTTON)) {
		if (!IFEMouseQueue.add(me))
			IFEDBG("Mouse event overrun");
	}
}

static void p_CheckEvents(void) {
	/* check keyboard input */
	p_CheckKeyboard();
	/* mouse input */
	p_CheckMouse();
}

static void p_WaitEvent(const int wait_ms) {
	(void)wait_ms;
	p_CheckEvents();
}

static bool p_BeginScreenDraw(void) {
	return true; // nothing to do
}

static void p_EndScreenDraw(void) {
	// nothing to do
}

static void p_ShutdownVideo(void) {
	/* IBM PC/AT */

	/* reset mouse */
	{ /* TODO: If someday we support an alternate to INT 33h */
		__asm {
			mov	ax,0		; reset driver and read status
			int	33h
		}
	}

	keybirq_unhook();
	_sti();
	if (vesa_lfb_offscreen != NULL) {
		ifevidinfo_doslib.buf_base = ifevidinfo_doslib.buf_first_row = NULL;
		free((void*)vesa_lfb_offscreen);
		vesa_lfb_offscreen = NULL;
	}
	if (vesa_lfb != NULL) {
		ifevidinfo_doslib.vram_base = NULL;
		dpmi_phys_addr_free((void*)vesa_lfb);
		vesa_lfb = NULL;
	}
	if (vesa_setmode) {
		vesa_setmode = false;
		int10_setmode(3); /* back to 80x25 text */
	}
}

static void p_InitVideo(void) {
	/* IBM PC/AT compatible */
	/* Find 640x480 256-color mode.
	 * Linear framebuffer required (we'll support older bank switched stuff later) */
	{
		const uint32_t wantf1 = VESA_MODE_ATTR_HW_SUPPORTED | VESA_MODE_ATTR_GRAPHICS_MODE | VESA_MODE_ATTR_LINEAR_FRAMEBUFFER_AVAILABLE;
		struct vbe_mode_info mi = {0};
		uint16_t found_mode = 0;
		unsigned int entry;
		uint16_t mode;

		memset(&ifevidinfo_doslib,0,sizeof(ifevidinfo_doslib));

		for (entry=0;entry < 4096;entry++) {
			mode = vbe_read_mode_entry(vbe_info->video_mode_ptr,entry);
			if (mode == 0xFFFF) break;

			if (vbe_read_mode_info(mode,&mi)) {
				vbe_fill_in_mode_info(mode,&mi);
				if ((mi.mode_attributes & wantf1) == wantf1 && mi.x_resolution == 640 && mi.y_resolution == 480 &&
					mi.bits_per_pixel == 8 && mi.memory_model == 0x04/*packed pixel*/ &&
					mi.phys_base_ptr != 0x00000000ul && mi.phys_base_ptr != 0xFFFFFFFFul &&
					mi.bytes_per_scan_line >= 640) {
					vesa_lfb_physaddr = mi.phys_base_ptr;
					vesa_lfb_map_size = mi.bytes_per_scan_line * mi.y_resolution;
					vesa_lfb_stride = mi.bytes_per_scan_line;
					vesa_lfb_width = mi.x_resolution;
					vesa_lfb_height = mi.y_resolution;
					found_mode = mode;
					break;
				}
			}
		}

		if (found_mode == 0)
			IFEFatalError("VESA BIOS video mode (640 x 480 256-color) not found");

		vesa_mode = found_mode;
	}

	if (!vesa_setmode) {
		/* Set the video mode */
		if (!vbe_set_mode((uint16_t)(vesa_mode | VBE_MODE_LINEAR),NULL))
			IFEFatalError("Unable to set VESA video mode");

		/* we set the mode, set the flag so FatalError can unset it properly */
		vesa_setmode = true;

		ifevidinfo_doslib.width = vesa_lfb_width;
		ifevidinfo_doslib.height = vesa_lfb_height;
		ifevidinfo_doslib.buf_pitch = ifevidinfo_doslib.vram_pitch = vesa_lfb_stride;
		ifevidinfo_doslib.buf_alloc = ifevidinfo_doslib.buf_size = ifevidinfo_doslib.vram_size = vesa_lfb_map_size;

		/* use 8-bit DAC if available */
		if (vbe_info->capabilities & VBE_CAP_8BIT_DAC) {
			if (vbe_set_dac_width(8) == 8)
				vesa_8bitpal = true;
		}

		/* As a 32-bit DOS program atop DPMI we cannot assume a 1:1 mapping between linear and physical,
		 * though plenty of DOS extenders will do just that if EMM386.EXE is not loaded */
		vesa_lfb = (unsigned char*)dpmi_phys_addr_map(vesa_lfb_physaddr,vesa_lfb_map_size);
		if (vesa_lfb == NULL)
			IFEFatalError("DPMI VESA LFB map fail");

		/* video memory is slow, and unlike Windows and SDL2, vesa_lfb points directly at video memory.
		 * Provide an offscreen copy of video memory to work from for compositing. */
		vesa_lfb_offscreen = (unsigned char*)malloc(vesa_lfb_map_size);
		if (vesa_lfb_offscreen == NULL)
			IFEFatalError("DPMI VESA LFB shadow malloc fail");

		ifevidinfo_doslib.vram_base = vesa_lfb;
		ifevidinfo_doslib.buf_base = ifevidinfo_doslib.buf_first_row = vesa_lfb_offscreen;
	}

	/* hook keyboard */
	keybirq_hook();

	/* Is a mouse driver there? NTS: DOS4/GW and Open Watcom might return a stub interrupt proc
	 * in protected mode even if the real mode vector is NULL. Checking against NULL is required
	 * to avoid a crash on INT 33h. There's no guarantee there is an interrupt vector there when
	 * a mouse driver is not installed. Let's not be as sloppy as some PC-98 games I've tested
	 * that ASSUME INT 33h is there --J.C. */
	/* Ref: INT 33h, Ralph Brown Interrupt List [http://www.ctyme.com/intr/int-33.htm] */
	if (*((uint32_t*)(0x33ul * 4ul)) == 0) /* read the real-mode table directly */
		IFEFatalError("Mouse driver not present, INT 33h is NULL");

	memset(&ifemousestat,0,sizeof(ifemousestat));

	{
		unsigned short ist=0,btns=0;

		__asm {
			mov	ax,0		; reset driver and read status
			int	33h
			mov	ist,ax		; AX=FFFFh if installed
			mov	btns,bx		; BX=number of buttons, 0 if not 2 buttons, 2 if two buttons, 3 if three buttons, FFFFh if two buttons. Make sense?
		}

		if (ist != 0xFFFFu)
			IFEFatalError("Mouse driver not present");
	}

	/* mouse drivers automatically set a default range appropriate for standard VGA modes,
	 * but they are oblivious to VESA BIOS modes and we will need to set the cursor range
	 * for it */
	{
		unsigned short w=(unsigned short)(ifevidinfo_doslib.width-1u);
		unsigned short h=(unsigned short)(ifevidinfo_doslib.height-1u);

		__asm {
			mov	ax,7		; set horizontal cursor range
			mov	cx,0		; min=0
			mov	dx,w		; max=width-1
			int	33h

			mov	ax,8		; set vertical cursor range
			mov	cx,0		; min=0
			mov	dx,h		; max=height-1
			int	33h
		}
	}
}

bool priv_IFEMainInit(int argc,char **argv) {
	//not used yet
	(void)argc;
	(void)argv;

	cpu_probe();
	probe_dos();
	detect_windows();
	probe_dpmi();
	probe_vcpi();
	dos_ltp_probe();

	if (!probe_8254())
		IFEFatalError("8254 timer not detected");
	if (!probe_8259())
		IFEFatalError("8259 interrupt controller not detected");
	if (!k8042_probe())
		IFEFatalError("8042 keyboard controller not found");
	if (!probe_vga())
		IFEFatalError("Unable to detect video card");
	if (!(vga_state.vga_flags & VGA_IS_VGA))
		IFEFatalError("Standard VGA not detected");
	if (!vbe_probe() || vbe_info == NULL || vbe_info->video_mode_ptr == 0)
			IFEFatalError("VESA BIOS extensions not detected");

	if (probe_dosbox_id()) {
		printf("DOSBox Integration Device detected\n");
		dosbox_ig = ifedbg_en = true;

		IFEDBG("Using DOSBox Integration Device for debug info. This should appear in your DOSBox/DOSBox-X log file");
	}

	/* make sure the timer is ticking at 18.2Hz. */
	write_8254_system_timer(0);

	/* establish the base line timer tick */
	pit_prev = read_8254(T8254_TIMER_INTERRUPT_TICK);

	/* Windows 95 bug: After reading the 8254, the TF flag (Trap Flag) is stuck on, and this
	 * program runs extremely slowly. Clear the TF flag. It may have something to do with
	 * read_8254 or any other code that does PUSHF + CLI + POPF to protect against interrupts.
	 * POPF is known to not cause a fault on 386-level IOPL3 protected mode, and therefore
	 * a VM monitor has difficulty knowing whether interrupts are enabled, so perhaps setting
	 * TF when the VM executes the CLI instruction is Microsoft's hack to try to work with
	 * DOS games regardless. */
	IFE_win95_tf_hang_check();

	return true;
}

void p_FlushKeyboardInput(void) {
	IFEKeyQueueEmptyAll();
}

IFEKeyEvent *p_GetRawKeyboardInput(void) {
	return IFEKeyQueue.get();
}

IFECookedKeyEvent *p_GetCookedKeyboardInput(void) {
	return IFECookedKeyQueue.get();
}

IFEMouseStatus *p_GetMouseStatus(void) {
	return &ifemousestat;
}

void p_FlushMouseInput(void) {
	IFEMouseQueueEmptyAll();
	ifemousestat.status = 0;
}

IFEMouseEvent *p_GetMouseInput(void) {
	return IFEMouseQueue.get();
}

void p_UpdateScreen(void) {
}

void p_AddScreenUpdate(int x1,int y1,int x2,int y2) {
	(void)x1;
	(void)x2;
	(void)y1;
	(void)y2;
}

ifeapi_t ifeapi_doslib = {
	"DOSLIB (IBM PC/AT)",
	p_SetPaletteColors,
	p_GetTicks,
	p_ResetTicks,
	p_UpdateFullScreen,
	p_GetVidInfo,
	p_UserWantsToQuit,
	p_CheckEvents,
	p_WaitEvent,
	p_BeginScreenDraw,
	p_EndScreenDraw,
	p_ShutdownVideo,
	p_InitVideo,
	p_FlushKeyboardInput,
	p_GetRawKeyboardInput,
	p_GetCookedKeyboardInput,
	p_GetMouseStatus,
	p_FlushMouseInput,
	p_GetMouseInput,
	p_UpdateScreen,
	p_AddScreenUpdate
};
#endif


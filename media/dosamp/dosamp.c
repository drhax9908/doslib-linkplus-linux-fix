
#include <stdio.h>
#include <conio.h> /* this is where Open Watcom hides the outp() etc. functions */
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <malloc.h>
#include <direct.h>
#include <ctype.h>
#include <fcntl.h>
#include <dos.h>

#include <hw/vga/vga.h>
#include <hw/dos/dos.h>
#include <hw/8237/8237.h>		/* 8237 DMA */
#include <hw/8254/8254.h>		/* 8254 timer */
#include <hw/8259/8259.h>		/* 8259 PIC interrupts */
#include <hw/sndsb/sndsb.h>
#include <hw/sndsb/sb16asp.h>
#include <hw/vga/vgagui.h>
#include <hw/vga/vgatty.h>
#include <hw/dos/doswin.h>
#include <hw/dos/tgusmega.h>
#include <hw/dos/tgussbos.h>
#include <hw/dos/tgusumid.h>

#if TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__COMPACT__) || defined(__SMALL__))
  /* chop features out of the Compact memory model build to ensure all code fits inside 64KB */
  /* don't include FX */
  /* don't include live config */
  /* don't include mixer */
#else
# define ISAPNP
#endif

#ifdef ISAPNP
#include <hw/isapnp/isapnp.h>
#include <hw/sndsb/sndsbpnp.h>
#endif

static struct dma_8237_allocation *sb_dma = NULL; /* DMA buffer */

static struct sndsb_ctx*	sb_card = NULL;

/*============================TODO: move to library=============================*/
static int vector_is_iret(const unsigned char vector) {
	const unsigned char far *p;
	uint32_t rvector;

#if TARGET_MSDOS == 32
	rvector = ((uint32_t*)0)[vector];
	if (rvector == 0) return 0;
	p = (const unsigned char*)(((rvector >> 16UL) << 4UL) + (rvector & 0xFFFFUL));
#else
	rvector = *((uint32_t far*)MK_FP(0,(vector*4)));
	if (rvector == 0) return 0;
	p = (const unsigned char far*)MK_FP(rvector>>16UL,rvector&0xFFFFUL);
#endif

	if (*p == 0xCF) {
		// IRET. Yep.
		return 1;
	}
	else if (p[0] == 0xFE && p[1] == 0x38) {
		// DOSBox callback. Probably not going to ACK the interrupt.
		return 1;
	}

	return 0;
}
/*==============================================================================*/

enum {
	GOLDRATE_MATCH=0,
	GOLDRATE_DOUBLE,
	GOLDRATE_MAX
};

static unsigned char		animator = 0;
static int			wav_fd = -1;
static char			temp_str[512];
#ifdef ISAPNP
static unsigned char far	devnode_raw[4096];
#endif
static char			wav_file[130] = {0};
static unsigned char		wav_stereo = 0,wav_16bit = 0,wav_bytes_per_sample = 1;
static unsigned long		wav_data_offset = 44,wav_data_length = 0,wav_sample_rate = 8000,wav_position = 0,wav_buffer_filepos = 0;
static unsigned char		dont_chain_irq = 0;
static unsigned char		wav_playing = 0;
static signed char		reduced_irq_interval = 0;

static volatile unsigned char	IRQ_anim = 0;

static inline unsigned char xdigit2int(char c) {
	if (c >= '0' && c <= '9')
		return (unsigned char)(c - '0');
	else if (c >= 'a' && c <= 'f')
		return (unsigned char)(c - 'a' + 10);
	else if (c >= 'A' && c <= 'F')
		return (unsigned char)(c - 'A' + 10);
	return 0;
}

static void stop_play();

static void draw_irq_indicator() {
	VGA_ALPHA_PTR wr = vga_state.vga_alpha_ram;
	unsigned char i;

	wr[0] = 0x1E00 | 'S';
	wr[1] = 0x1E00 | 'B';
	wr[2] = 0x1E00 | '-';
	wr[3] = 0x1E00 | 'I';
	wr[4] = 0x1E00 | 'R';
	wr[5] = 0x1E00 | 'Q';
	for (i=0;i < 4;i++) wr[i+6] = (uint16_t)(i == IRQ_anim ? 'x' : '-') | 0x1E00;
}

static uint32_t irq_0_count = 0;
static uint32_t irq_0_adv = 1;
static uint32_t irq_0_max = 1;
#if TARGET_MSDOS == 32
static unsigned char irq_0_had_warned = 0;
#endif

/* IRQ 0 watchdog: when playing audio it is possible to exceed the rate
   that the CPU can possibly handle servicing the interrupt. This results
   in audio that still plays but the UI is "frozen" because no CPU time
   is available. If the UI is not there to reset the watchdog, the ISR
   will auto-stop playback, allowing the user to regain control without
   hitting the RESET button. */
static volatile uint32_t irq_0_watchdog = 0x10000UL;
static void irq_0_watchdog_ack() {
	if (irq_0_watchdog != 0UL) {
		irq_0_watchdog += 0x800UL; /* 1/32 of max. This should trigger even if UI is only reduced to tar speeds by ISR */
		if (irq_0_watchdog > 0x10000UL) irq_0_watchdog = 0x10000UL;
	}
}

static void irq_0_watchdog_reset() {
	irq_0_watchdog = 0x10000UL;
}

/* WARNING!!! This interrupt handler calls subroutines. To avoid system
 * instability in the event the IRQ fires while, say, executing a routine
 * in the DOS kernel, you must compile this code with the -zu flag in
 * 16-bit real mode Large and Compact memory models! Without -zu, minor
 * memory corruption in the DOS kernel will result and the system will
 * hang and/or crash. */
static unsigned char old_irq_masked = 0;
static void (interrupt *old_irq_0)() = NULL;
static void interrupt irq_0() { /* timer IRQ */
	/* if we're playing the DSP in direct mode, then it's our job to do the direct DAC/ADC commands */
	if (wav_playing && irq_0_watchdog > 0UL) {
		if (sb_card && sb_card->timer_tick_func != NULL)
			sb_card->timer_tick_func(sb_card);

		if (--irq_0_watchdog == 0UL) {
			/* try to help by setting the timer rate back down */
			write_8254_system_timer(0); /* restore 18.2 tick/sec */
			irq_0_count = 0;
			irq_0_adv = 1;
			irq_0_max = 1;
		}
	}

	/* tick rate conversion. we may run the timer at a faster tick rate but the BIOS may have problems
	 * unless we chain through it's ISR at the 18.2 ticks/sec it expects to be called. If we don't,
	 * most BIOS services will work fine but some parts usually involving the floppy drive will have
	 * problems and premature timeouts, or may turn the motor off too soon.  */
	irq_0_count += irq_0_adv;
	if (irq_0_count >= irq_0_max) {
		/* NOTE TO SELF: Apparently the 32-bit protmode version
		   has to chain back to the BIOS or else keyboard input
		   doesn't work?!? */
		irq_0_count -= irq_0_max;
		old_irq_0(); /* call BIOS underneath at 18.2 ticks/sec */
	}
	else {
		p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);
	}
}

/* WARNING!!! This interrupt handler calls subroutines. To avoid system
 * instability in the event the IRQ fires while, say, executing a routine
 * in the DOS kernel, you must compile this code with the -zu flag in
 * 16-bit real mode Large and Compact memory models! Without -zu, minor
 * memory corruption in the DOS kernel will result and the system will
 * hang and/or crash. */
static void (interrupt *old_irq)() = NULL;
static void interrupt sb_irq() {
	unsigned char c;

	sb_card->irq_counter++;
	if (++IRQ_anim >= 4) IRQ_anim = 0;
	draw_irq_indicator();

	/* ack soundblaster DSP if DSP was the cause of the interrupt */
	/* NTS: Experience says if you ack the wrong event on DSP 4.xx it
	   will just re-fire the IRQ until you ack it correctly...
	   or until your program crashes from stack overflow, whichever
	   comes first */
	c = sndsb_interrupt_reason(sb_card);
	sndsb_interrupt_ack(sb_card,c);

	/* FIXME: The sndsb library should NOT do anything in
	   send_buffer_again() if it knows playback has not started! */
	/* for non-auto-init modes, start another buffer */
	if (wav_playing) sndsb_irq_continue(sb_card,c);

	/* NTS: we assume that if the IRQ was masked when we took it, that we must not
	 *      chain to the previous IRQ handler. This is very important considering
	 *      that on most DOS systems an IRQ is masked for a very good reason---the
	 *      interrupt handler doesn't exist! In fact, the IRQ vector could easily
	 *      be unitialized or 0000:0000 for it! CALLing to that address is obviously
	 *      not advised! */
	if (old_irq_masked || old_irq == NULL || dont_chain_irq) {
		/* ack the interrupt ourself, do not chain */
		if (sb_card->irq >= 8) p8259_OCW2(8,P8259_OCW2_NON_SPECIFIC_EOI);
		p8259_OCW2(0,P8259_OCW2_NON_SPECIFIC_EOI);
	}
	else {
		/* chain to the previous IRQ, who will acknowledge the interrupt */
		old_irq();
	}
}

static void load_audio(struct sndsb_ctx *cx,uint32_t up_to,uint32_t min,uint32_t max,uint8_t initial) { /* load audio up to point or max */
	unsigned char FAR *buffer = sb_dma->lin;
	VGA_ALPHA_PTR wr = vga_state.vga_alpha_ram + 80 - 6;
	unsigned char load=0;
	uint16_t prev[6];
	int rd,i,bufe=0;
	uint32_t how;

	/* caller should be rounding! */
	assert((up_to & 3UL) == 0UL);
	if (up_to >= cx->buffer_size) return;
	if (cx->buffer_size < 32) return;
	if (cx->buffer_last_io == up_to) return;

	if (sb_card->dsp_adpcm > 0 && (wav_16bit || wav_stereo)) return;
	if (max == 0) max = cx->buffer_size/4;
	if (max < 16) return;
	lseek(wav_fd,wav_data_offset + (wav_position * (unsigned long)wav_bytes_per_sample),SEEK_SET);

	if (cx->buffer_last_io == 0)
		wav_buffer_filepos = wav_position;

	while (max > 0UL) {
		/* the most common "hang" apparently is when IRQ 0 triggers too much
		   and then somehow execution gets stuck here */
		if (irq_0_watchdog < 16UL)
			break;

		if (cx->backwards) {
			if (up_to > cx->buffer_last_io) {
				how = cx->buffer_last_io;
				if (how == 0) how = cx->buffer_size - up_to;
				bufe = 1;
			}
			else {
				how = (cx->buffer_last_io - up_to);
				bufe = 0;
			}
		}
		else {
			if (up_to < cx->buffer_last_io) {
				how = (cx->buffer_size - cx->buffer_last_io); /* from last IO to end of buffer */
				bufe = 1;
			}
			else {
				how = (up_to - cx->buffer_last_io); /* from last IO to up_to */
				bufe = 0;
			}
		}

		if (how > 16384UL)
			how = 16384UL;

		if (how == 0UL)
			break;
		else if (how > max)
			how = max;
		else if (!bufe && how < min)
			break;

		if (!load) {
			load = 1;
			prev[0] = wr[0];
			wr[0] = '[' | 0x0400;
			prev[1] = wr[1];
			wr[1] = 'L' | 0x0400;
			prev[2] = wr[2];
			wr[2] = 'O' | 0x0400;
			prev[3] = wr[3];
			wr[3] = 'A' | 0x0400;
			prev[4] = wr[4];
			wr[4] = 'D' | 0x0400;
			prev[5] = wr[5];
			wr[5] = ']' | 0x0400;
		}

		if (cx->buffer_last_io == 0)
			wav_buffer_filepos = wav_position;

        {
            uint32_t oa,adj;

			oa = cx->buffer_last_io;
			if (cx->backwards) {
				if (cx->buffer_last_io == 0) {
					cx->buffer_last_io = cx->buffer_size - how;
				}
				else if (cx->buffer_last_io >= how) {
					cx->buffer_last_io -= how;
				}
				else {
					abort();
				}

				adj = (uint32_t)how / wav_bytes_per_sample;
				if (wav_position >= adj) wav_position -= adj;
				else if (wav_position != 0UL) wav_position = 0;
				else {
					wav_position = lseek(wav_fd,0,SEEK_END);
					if (wav_position >= adj) wav_position -= adj;
					else if (wav_position != 0UL) wav_position = 0;
					wav_position /= wav_bytes_per_sample;
				}

				lseek(wav_fd,wav_data_offset + (wav_position * (unsigned long)wav_bytes_per_sample),SEEK_SET);
			}

			assert(cx->buffer_last_io <= cx->buffer_size);
#if TARGET_MSDOS == 32
			rd = _dos_xread(wav_fd,buffer + cx->buffer_last_io,how);
#else
            {
                uint32_t o;

                o  = (uint32_t)FP_SEG(buffer) << 4UL;
                o += (uint32_t)FP_OFF(buffer);
                o += cx->buffer_last_io;
                rd = _dos_xread(wav_fd,MK_FP(o >> 4UL,o & 0xFUL),how);
            }
#endif
			if (rd == 0 || rd == -1) {
				if (!cx->backwards) {
					wav_position = 0;
					lseek(wav_fd,wav_data_offset + (wav_position * (unsigned long)wav_bytes_per_sample),SEEK_SET);
					rd = _dos_xread(wav_fd,buffer + cx->buffer_last_io,how);
					if (rd == 0 || rd == -1) {
						/* hmph, fine */
#if TARGET_MSDOS == 32
						memset(buffer+cx->buffer_last_io,128,how);
#else
						_fmemset(buffer+cx->buffer_last_io,128,how);
#endif
						rd = (int)how;
					}
				}
				else {
					rd = (int)how;
				}
			}

			assert((cx->buffer_last_io+((uint32_t)rd)) <= cx->buffer_size);
			if (sb_card->audio_data_flipped_sign) {
				if (wav_16bit)
					for (i=0;i < (rd-1);i += 2) buffer[cx->buffer_last_io+i+1] ^= 0x80;
				else
					for (i=0;i < rd;i++) buffer[cx->buffer_last_io+i] ^= 0x80;
			}

			if (!cx->backwards) {
				cx->buffer_last_io += (uint32_t)rd;
				wav_position += (uint32_t)rd / wav_bytes_per_sample;
			}
		}

		assert(cx->buffer_last_io <= cx->buffer_size);
		if (!cx->backwards) {
			if (cx->buffer_last_io == cx->buffer_size) cx->buffer_last_io = 0;
		}
		max -= (uint32_t)rd;
	}

	if (cx->buffer_last_io == 0)
		wav_buffer_filepos = wav_position;

	if (load) {
		for (i=0;i < 6;i++)
			wr[i] = prev[i];
	}
}

#define DMA_WRAP_DEBUG

static void wav_idle() {
	const unsigned int leeway = sb_card->buffer_size / 100;
	uint32_t pos;
#ifdef DMA_WRAP_DEBUG
	uint32_t pos2;
#endif

	if (!wav_playing || wav_fd < 0)
		return;

	/* if we're playing without an IRQ handler, then we'll want this function
	 * to poll the sound card's IRQ status and handle it directly so playback
	 * continues to work. if we don't, playback will halt on actual Creative
	 * Sound Blaster 16 hardware until it gets the I/O read to ack the IRQ */
	sndsb_main_idle(sb_card);

	_cli();
#ifdef DMA_WRAP_DEBUG
	pos2 = sndsb_read_dma_buffer_position(sb_card);
#endif
	pos = sndsb_read_dma_buffer_position(sb_card);
#ifdef DMA_WRAP_DEBUG
	if (sb_card->backwards) {
		if (pos2 < 0x1000 && pos >= (sb_card->buffer_size-0x1000)) {
			/* normal DMA wrap-around, no problem */
		}
		else {
			if (pos > pos2)	fprintf(stderr,"DMA glitch! 0x%04lx 0x%04lx\n",pos,pos2);
			else		pos = max(pos,pos2);
		}

		pos += leeway;
		if (pos >= sb_card->buffer_size) pos -= sb_card->buffer_size;
	}
	else {
		if (pos < 0x1000 && pos2 >= (sb_card->buffer_size-0x1000)) {
			/* normal DMA wrap-around, no problem */
		}
		else {
			if (pos < pos2)	fprintf(stderr,"DMA glitch! 0x%04lx 0x%04lx\n",pos,pos2);
			else		pos = min(pos,pos2);
		}

		if (pos < leeway) pos += sb_card->buffer_size - leeway;
		else pos -= leeway;
	}
#endif
	pos &= (~3UL); /* round down */
	_sti();

    /* load from disk */
    load_audio(sb_card,pos,min(wav_sample_rate/8,4096)/*min*/,
        sb_card->buffer_size/4/*max*/,0/*first block*/);
}

static void update_cfg();

static unsigned long playback_live_position() {
	signed long xx = (signed long)sndsb_read_dma_buffer_position(sb_card);

	if (sb_card->backwards) {
		if (sb_card->buffer_last_io >= (unsigned long)xx)
			xx += sb_card->buffer_size;

		xx -= sb_card->buffer_size; /* because we started from the end */
	}
	else {
		if (sb_card->buffer_last_io <= (unsigned long)xx)
			xx -= sb_card->buffer_size;
	}

	if (sb_card->dsp_adpcm == ADPCM_4BIT) xx *= 2;
	else if (sb_card->dsp_adpcm == ADPCM_2_6BIT) xx *= 3;
	else if (sb_card->dsp_adpcm == ADPCM_2BIT) xx *= 4;
	xx += wav_buffer_filepos * wav_bytes_per_sample;
	if (xx < 0) xx += wav_data_length;
	return ((unsigned long)xx) / wav_bytes_per_sample;
}

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
static unsigned char dos_vm_yield_counter = 0;
#endif

static uint32_t last_dma_position = 1;
static void ui_anim(int force) {
	VGA_ALPHA_PTR wr = vga_state.vga_alpha_ram + 10;
	const unsigned int width = 70 - 4;
	unsigned long temp,rem,rem2;
	unsigned int pH,pM,pS,pSS;
	const char *msg = "DMA:";
	unsigned int i,cc;

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	/* Under Windows, yield every so often. Under Windows NT/2000/XP this prevents
	 * NTVDM.EXE from pegging the CPU at 100%, allowing the rest of the OS to run
	 * smoother. */
	if (windows_mode == WINDOWS_NT || windows_mode == WINDOWS_ENHANCED) {
		unsigned char do_yield = 1;

		if (sb_card != NULL && wav_playing)
			do_yield = sndsb_recommend_vm_wait(sb_card);

		if (do_yield) {
			if (dos_vm_yield_counter == 0)
				dos_vm_yield();

			if (++dos_vm_yield_counter >= 10)
				dos_vm_yield_counter = 0;
		}
	}
#endif

	wav_idle();

	rem = 0;
	if (sb_card != NULL) rem = sndsb_read_dma_buffer_position(sb_card);
	if (force || last_dma_position != rem) {
		last_dma_position = rem;
		if (rem != 0) rem--;
		rem = (unsigned int)(((unsigned long)rem * (unsigned long)width) / (unsigned long)sb_card->buffer_size);

		rem2 = 0;
		if (sb_card != NULL) rem2 = sb_card->buffer_last_io;
		if (rem2 != 0) rem2--;
		rem2 = (unsigned int)(((unsigned long)rem2 * (unsigned long)width) / (unsigned long)sb_card->buffer_size);

		while (*msg) *wr++ = (uint16_t)(*msg++) | 0x1E00;
		for (i=0;i < width;i++) {
			if (i == rem2)
				wr[i] = (uint16_t)(i == rem ? 'x' : (i < rem ? '-' : ' ')) | 0x7000;
			else
				wr[i] = (uint16_t)(i == rem ? 'x' : (i < rem ? '-' : ' ')) | 0x1E00;
		}

		if (wav_playing) temp = playback_live_position();
		else temp = wav_position;
		pSS = (unsigned int)(((temp % wav_sample_rate) * 100UL) / wav_sample_rate);
		temp /= wav_sample_rate;
		pS = (unsigned int)(temp % 60UL);
		pM = (unsigned int)((temp / 60UL) % 60UL);
		pH = (unsigned int)((temp / 3600UL) % 24UL);

		msg = temp_str;
		sprintf(temp_str,"%ub %s %5luHz @%c%u:%02u:%02u.%02u",wav_16bit ? 16 : 8,wav_stereo ? "ST" : "MO",
			wav_sample_rate,wav_playing ? (0 ? 'r' : 'p') : 's',pH,pM,pS,pSS);
		for (wr=vga_state.vga_alpha_ram+(80*1),cc=0;cc < 29 && *msg != 0;cc++) *wr++ = 0x1F00 | ((unsigned char)(*msg++));
		for (;cc < 29;cc++) *wr++ = 0x1F20;
		msg = sndsb_dspoutmethod_str[sb_card->dsp_play_method];
		rem = sndsb_dsp_out_method_supported(sb_card,wav_sample_rate,wav_stereo,wav_16bit) ? 0x1A : 0x1C;
		for (;cc < 36 && *msg != 0;cc++) *wr++ = (rem << 8) | ((unsigned char)(*msg++));
		if (cc < 36) {
			if (rem == 0x1C) *wr++ = 0x1E00 + '?';
			else if (sb_card->reason_not_supported) *wr++ = 0x1A00 + '?';
		}
		for (;cc < 36;cc++) *wr++ = 0x1F20;

		if (sb_card->dsp_adpcm != 0) {
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
			msg = sndsb_adpcm_mode_str[sb_card->dsp_adpcm];
			for (;cc < 52 && *msg != 0;cc++) *wr++ = 0x1F00 | ((unsigned char)(*msg++));
#endif
		}
		else if (sb_card->audio_data_flipped_sign) {
			msg = "[flipsign]";
			for (;cc < 52 && *msg != 0;cc++) *wr++ = 0x1F00 | ((unsigned char)(*msg++));
		}

		/* fill */
		for (;cc < 57;cc++) *wr++ = 0x1F20;

		msg = temp_str;
		temp = sndsb_read_dma_buffer_position(sb_card);
		sprintf(temp_str,"%05lx/%05lx @%08lx",
			(unsigned long)temp,
			(unsigned long)sb_card->buffer_size,
            (unsigned long)sb_card->buffer_phys + (unsigned long)temp);
		for (;cc < 80 && *msg != 0;cc++) *wr++ = 0x1F00 | ((unsigned char)(*msg++));

		/* finish */
		for (;cc < 80;cc++) *wr++ = 0x1F20;
	}

	irq_0_watchdog_ack();

	{
		static const unsigned char anims[] = {'-','/','|','\\'};
		if (++animator >= 4) animator = 0;
		wr = vga_state.vga_alpha_ram + 80 + 79;
		*wr = anims[animator] | 0x1E00;
	}
}

static void close_wav() {
	if (wav_fd >= 0) {
		close(wav_fd);
		wav_fd = -1;
	}
}

static void open_wav() {
	char tmp[64];

	wav_position = 0;
	if (wav_fd < 0) {
		if (strlen(wav_file) < 1) return;
		wav_fd = open(wav_file,O_RDONLY|O_BINARY);
		if (wav_fd < 0) return;
		wav_data_offset = 0;
		wav_data_length = (unsigned long)lseek(wav_fd,0,SEEK_END);
		lseek(wav_fd,0,SEEK_SET);
		read(wav_fd,tmp,sizeof(tmp));

		/* FIXME: This is a dumb quick and dirty WAVE header reader */
		if (!memcmp(tmp,"RIFF",4) && !memcmp(tmp+8,"WAVEfmt ",8) && wav_data_length > 44) {
			unsigned char *fmtc = tmp + 20;
			/* fmt chunk at 12, where 'fmt '@12 and length of fmt @ 16, fmt data @ 20, 16 bytes long */
			/* WORD    wFormatTag
			 * WORD    nChannels
			 * DWORD   nSamplesPerSec
			 * DWORD   nAvgBytesPerSec
			 * WORD    nBlockAlign
			 * WORD    wBitsPerSample */
			wav_sample_rate = *((uint32_t*)(fmtc + 4));
            if (wav_sample_rate == 0UL) wav_sample_rate = 1UL;
			wav_stereo = *((uint16_t*)(fmtc + 2)) > 1;
			wav_16bit = *((uint16_t*)(fmtc + 14)) > 8;
			wav_bytes_per_sample = (wav_stereo ? 2 : 1) * (wav_16bit ? 2 : 1);
			wav_data_offset = 44;
			wav_data_length -= 44;
			wav_data_length -= wav_data_length % wav_bytes_per_sample;
		}
	}
}

static void free_dma_buffer() {
    if (sb_dma != NULL) {
        dma_8237_free_buffer(sb_dma);
        sb_dma = NULL;
    }
}

static void realloc_dma_buffer() {
    uint32_t choice;
    int8_t ch;

    free_dma_buffer();

    ch = sndsb_dsp_playback_will_use_dma_channel(sb_card,wav_sample_rate,wav_stereo,wav_16bit);

    if (ch >= 4)
        choice = sndsb_recommended_16bit_dma_buffer_size(sb_card,0);
    else
        choice = sndsb_recommended_dma_buffer_size(sb_card,0);

    do {
        if (ch >= 4)
            sb_dma = dma_8237_alloc_buffer_dw(choice,16);
        else
            sb_dma = dma_8237_alloc_buffer_dw(choice,8);

        if (sb_dma == NULL) choice -= 4096UL;
    } while (sb_dma == NULL && choice > 4096UL);

    if (!sndsb_assign_dma_buffer(sb_card,sb_dma))
        return;
    if (sb_dma == NULL)
        return;
}

static void begin_play() {
	unsigned long choice_rate;

	if (wav_playing)
		return;

    if (sb_dma == NULL)
        realloc_dma_buffer();

    {
        int8_t ch = sndsb_dsp_playback_will_use_dma_channel(sb_card,wav_sample_rate,wav_stereo,wav_16bit);
        if (ch >= 0) {
            if (sb_dma->dma_width != (ch >= 4 ? 16 : 8))
                realloc_dma_buffer();
            if (sb_dma == NULL)
                return;
        }
    }

    if (sb_dma != NULL) {
        if (!sndsb_assign_dma_buffer(sb_card,sb_dma))
            return;
    }

	if (wav_fd < 0)
		return;

	choice_rate = wav_sample_rate;

	update_cfg();
	irq_0_watchdog_reset();
	if (!sndsb_prepare_dsp_playback(sb_card,choice_rate,wav_stereo,wav_16bit))
		return;

	sndsb_setup_dma(sb_card);

    load_audio(sb_card,sb_card->buffer_size/2,0/*min*/,0/*max*/,1/*first block*/);

	/* make sure the IRQ is acked */
	if (sb_card->irq >= 8) {
		p8259_OCW2(8,P8259_OCW2_SPECIFIC_EOI | (sb_card->irq & 7));
		p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | 2);
	}
	else if (sb_card->irq >= 0) {
		p8259_OCW2(0,P8259_OCW2_SPECIFIC_EOI | sb_card->irq);
	}
	if (sb_card->irq >= 0)
		p8259_unmask(sb_card->irq);

	if (!sndsb_begin_dsp_playback(sb_card))
		return;

	_cli();
	if (sb_card->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT) {
		unsigned long nr = (unsigned long)sb_card->buffer_rate * 2UL;
		write_8254_system_timer(T8254_REF_CLOCK_HZ / nr);
		irq_0_count = 0;
		irq_0_adv = 182UL;		/* 18.2Hz */
		irq_0_max = nr * 10UL;		/* sample rate */
	}
	wav_playing = 1;
	_sti();
}

static void stop_play() {
	if (!wav_playing) return;
	draw_irq_indicator();

    wav_position = playback_live_position();
    wav_position -= wav_position % (unsigned long)wav_bytes_per_sample;

	_cli();
	if (sb_card->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT) {
		irq_0_count = 0;
		irq_0_adv = 1;
		irq_0_max = 1;
		write_8254_system_timer(0); /* restore 18.2 tick/sec */
	}
	sndsb_stop_dsp_playback(sb_card);
	wav_playing = 0;
	_sti();

	ui_anim(1);
}

static void vga_write_until(unsigned int x) {
	while (vga_state.vga_pos_x < x)
		vga_writec(' ');
}

static int change_param_idx = 0;

/* NTS: the 13000, 15000, 23000 values come from Creative documentation */
static const unsigned short param_preset_rates[] = {
	4000,	5512,	5675,	6000,
	8000,	11025,	11111,	12000,
	13000,	15000,	16000,	22050,
	22222,	23000,	24000,  32000,
	44100,	48000,	54000,	58000};

#if TARGET_MSDOS == 32
static const char *dos32_irq_0_warning =
	"WARNING: The timer is made to run at the sample rate. Depending on your\n"
	"         DOS extender there may be enough overhead to overwhelm the CPU\n"
	"         and possibly cause a crash.\n"
	"         Enable?";
#endif

static void change_param_menu() {
	unsigned char loop=1;
	unsigned char redraw=1;
	unsigned char uiredraw=1;
	unsigned char selector=change_param_idx;
#if TARGET_MSDOS == 32
	unsigned char oldmethod=sb_card->dsp_play_method;
#endif
	unsigned int cc,ra;
	VGA_ALPHA_PTR vga;
	char tmp[128];

	while (loop) {
		if (redraw || uiredraw) {
			_cli();
			if (redraw) {
				for (vga=vga_state.vga_alpha_ram+(80*2),cc=0;cc < (80*23);cc++) *vga++ = 0x1E00 | 177;
				ui_anim(1);
			}
			vga_moveto(0,4);

			vga_write_color(selector == 0 ? 0x70 : 0x1F);
			sprintf(tmp,"Sample rate:   %luHz",wav_sample_rate);
			vga_write(tmp);
			vga_write_until(30);
			vga_write("\n");

			vga_write_color(selector == 1 ? 0x70 : 0x1F);
			sprintf(tmp,"Channels:      %s",wav_stereo ? "stereo" : "mono");
			vga_write(tmp);
			vga_write_until(30);
			vga_write("\n");

			vga_write_color(selector == 2 ? 0x70 : 0x1F);
			sprintf(tmp,"Bits:          %u-bit",wav_16bit ? 16 : 8);
			vga_write(tmp);
			vga_write_until(30);
			vga_write("\n");

			vga_write_color(selector == 3 ? 0x70 : 0x1F);
			vga_write(  "Translation:   ");
			if (sb_card->dsp_adpcm > 0) {
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
				vga_write(sndsb_adpcm_mode_str[sb_card->dsp_adpcm]);
#endif
			}
			else if (sb_card->audio_data_flipped_sign)
				vga_write("Flip sign");
			else
				vga_write("None");
			vga_write_until(30);
			vga_write("\n");

			vga_write_color(selector == 4 ? 0x70 : 0x1F);
			vga_write(  "DSP mode:      ");
			if (sndsb_dsp_out_method_supported(sb_card,wav_sample_rate,wav_stereo,wav_16bit))
				vga_write_color(selector == 4 ? 0x70 : 0x1F);
			else
				vga_write_color(selector == 4 ? 0x74 : 0x1C);
			vga_write(sndsb_dspoutmethod_str[sb_card->dsp_play_method]);
			vga_write_until(30);
			vga_write("\n");

			sprintf(tmp,"%u",sb_card->dsp_direct_dac_read_after_command);
			vga_write_color(selector == 5 ? 0x70 : 0x1F);
			vga_write(  "DDAC read aft: ");
			if (sndsb_dsp_out_method_supported(sb_card,wav_sample_rate,wav_stereo,wav_16bit))
				vga_write_color(selector == 5 ? 0x70 : 0x1F);
			else
				vga_write_color(selector == 5 ? 0x74 : 0x1C);
			vga_write(tmp);
			vga_write_until(30);
			vga_write("\n");

			sprintf(tmp,"%u",sb_card->dsp_direct_dac_poll_retry_timeout);
			vga_write_color(selector == 6 ? 0x70 : 0x1F);
			vga_write(  "DDAC poll retry");
			if (sndsb_dsp_out_method_supported(sb_card,wav_sample_rate,wav_stereo,wav_16bit))
				vga_write_color(selector == 6 ? 0x70 : 0x1F);
			else
				vga_write_color(selector == 6 ? 0x74 : 0x1C);
			vga_write(tmp);
			vga_write_until(30);
			vga_write("\n");

			sprintf(tmp,"%s",sb_card->backwards?"Backwards":"Forwards");
			vga_write_color(selector == 7 ? 0x70 : 0x1F);
			vga_write(  "Direction:     ");
			if (sndsb_dsp_out_method_supported(sb_card,wav_sample_rate,wav_stereo,wav_16bit))
				vga_write_color(selector == 7 ? 0x70 : 0x1F);
			else
				vga_write_color(selector == 7 ? 0x74 : 0x1C);
			vga_write(tmp);
			vga_write_until(30);
			vga_write("\n");

			vga_moveto(0,13);
			vga_write_color(0x1F);
			vga_write_until(80);
			vga_write("\n");
			vga_write_until(80);
			vga_write("\n");
			vga_write_until(80);
			vga_write("\n");
			vga_moveto(0,13);
			if (sb_card->reason_not_supported) vga_write(sb_card->reason_not_supported);

			vga_moveto(0,16);
			vga_write("\n");
			vga_write_sync();
			_sti();
			redraw = 0;
			uiredraw = 0;
		}

		if (kbhit()) {
			int c = getch();
			if (c == 0) c = getch() << 8;

			if (c == 27 || c == 13)
				loop = 0;
			else if (isdigit(c)) {
				if (selector == 0) { /* sample rate, allow typing in sample rate */
					int i=0;
					VGA_ALPHA_PTR sco;
					struct vga_msg_box box;
					vga_msg_box_create(&box,"Custom sample rate",2,0);
					sco = vga_state.vga_alpha_ram + ((box.y+2) * vga_state.vga_width) + box.x + 2;
					sco[i] = c | 0x1E00;
					temp_str[i++] = c;
					while (1) {
						c = getch();
						if (c == 0) c = getch() << 8;

						if (c == 27)
							break;
						else if (c == 13) {
							if (i == 0) break;
							temp_str[i] = 0;
							wav_sample_rate = strtol(temp_str,NULL,0);
                            if (wav_sample_rate == 0UL) wav_sample_rate = 1UL;
							uiredraw=1;
							break;
						}
						else if (isdigit(c)) {
							if (i < 5) {
								sco[i] = c | 0x1E00;
								temp_str[i++] = c;
							}
						}
						else if (c == 8) {
							if (i > 0) i--;
							sco[i] = ' ' | 0x1E00;
						}
					}
					vga_msg_box_destroy(&box);
				}
			}
			else if (c == 0x4800) { /* up arrow */
				if (selector > 0) selector--;
				else selector=7;
				uiredraw=1;
			}
			else if (c == 0x5000) { /* down arrow */
				if (selector < 7) selector++;
				else selector=0;
				uiredraw=1;
			}
			else if (c == 0x4B00) { /* left arrow */
				switch (selector) {
					case 0:	/* sample rate */
						ra = param_preset_rates[0];
						for (cc=0;cc < (sizeof(param_preset_rates)/sizeof(param_preset_rates[0]));cc++) {
							if (param_preset_rates[cc] < wav_sample_rate)
								ra = param_preset_rates[cc];
						}
						wav_sample_rate = ra;
						break;
					case 1:	/* stereo/mono */
						wav_stereo = !wav_stereo;
						break;
					case 2: /* 8/16-bit */
						wav_16bit = !wav_16bit;
						break;
					case 3: /* translatin */
						if (sb_card->dsp_adpcm == ADPCM_2BIT) {
							sb_card->dsp_adpcm = ADPCM_2_6BIT;
						}
						else if (sb_card->dsp_adpcm == ADPCM_2_6BIT) {
							sb_card->dsp_adpcm = ADPCM_4BIT;
						}
						else if (sb_card->dsp_adpcm == ADPCM_4BIT) {
							sb_card->dsp_adpcm = 0;
						}
						else {
							sb_card->dsp_adpcm = ADPCM_2BIT;
						}
						break;
					case 4: /* DSP mode */
						if (sb_card->dsp_play_method == 0)
							sb_card->dsp_play_method = SNDSB_DSPOUTMETHOD_MAX - 1;
						else
							sb_card->dsp_play_method--;
						break;
					case 5: /* Direct DAC read after command/data */
						if (sb_card->dsp_direct_dac_read_after_command != 0)
							sb_card->dsp_direct_dac_read_after_command--;
						break;
					case 6: /* Direct DAC read poll retry timeout */
						if (sb_card->dsp_direct_dac_poll_retry_timeout != 0)
							sb_card->dsp_direct_dac_poll_retry_timeout--;
						break;
					case 7:
						sb_card->backwards ^= 1;
						break;
				};
				update_cfg();
				uiredraw=1;
			}
			else if (c == 0x4D00) { /* right arrow */
				switch (selector) {
					case 0:	/* sample rate */
						for (cc=0;cc < ((sizeof(param_preset_rates)/sizeof(param_preset_rates[0]))-1);) {
							if (param_preset_rates[cc] > wav_sample_rate) break;
							cc++;
						}
						wav_sample_rate = param_preset_rates[cc];
						break;
					case 1:	/* stereo/mono */
						wav_stereo = !wav_stereo;
						break;
					case 2: /* 8/16-bit */
						wav_16bit = !wav_16bit;
						break;
					case 3: /* translatin */
						if (sb_card->dsp_adpcm == ADPCM_2BIT) {
							sb_card->dsp_adpcm = 0;
						}
						else if (sb_card->dsp_adpcm == ADPCM_2_6BIT) {
							sb_card->dsp_adpcm = ADPCM_2BIT;
						}
						else if (sb_card->dsp_adpcm == ADPCM_4BIT) {
							sb_card->dsp_adpcm = ADPCM_2_6BIT;
						}
						else {
							sb_card->dsp_adpcm = ADPCM_4BIT;
						}
						break;
					case 4: /* DSP mode */
						if (++sb_card->dsp_play_method == SNDSB_DSPOUTMETHOD_MAX)
							sb_card->dsp_play_method = 0;
						break;
					case 5: /* Direct DAC read after command/data */
						if (sb_card->dsp_direct_dac_read_after_command < 255)
							sb_card->dsp_direct_dac_read_after_command++;
						break;
					case 6: /* Direct DAC read poll retry timeout */
						if (sb_card->dsp_direct_dac_poll_retry_timeout < 255)
							sb_card->dsp_direct_dac_poll_retry_timeout++;
						break;
					case 7:
						sb_card->backwards ^= 1;
						break;
				};
				update_cfg();
				uiredraw=1;
			}
		}

		ui_anim(0);
	}

#if TARGET_MSDOS == 32
	if (!irq_0_had_warned && sb_card->dsp_play_method == SNDSB_DSPOUTMETHOD_DIRECT) {
		/* NOTE TO SELF: It can overwhelm the UI in DOSBox too, but DOSBox seems able to
		   recover if you manage to hit CTRL+F12 to speed up the CPU cycles in the virtual machine.
		   On real hardware, even with the recovery method the machine remains hung :( */
		if (confirm_yes_no_dialog(dos32_irq_0_warning))
			irq_0_had_warned = 1;
		else
			sb_card->dsp_play_method = oldmethod;
	}
#endif

	change_param_idx = selector;
	wav_bytes_per_sample = (wav_stereo ? 2 : 1) * (wav_16bit ? 2 : 1);
}

static const struct vga_menu_item menu_separator =
	{(char*)1,		's',	0,	0};

static const struct vga_menu_item main_menu_file_set =
	{"Set file...",		's',	0,	0};
static const struct vga_menu_item main_menu_file_quit =
	{"Quit",		'q',	0,	0};

static const struct vga_menu_item* main_menu_file[] = {
	&main_menu_file_set,
	&main_menu_file_quit,
	NULL
};

static const struct vga_menu_item main_menu_playback_play =
	{"Play",		'p',	0,	0};
static const struct vga_menu_item main_menu_playback_stop =
	{"Stop",		's',	0,	0};
static const struct vga_menu_item main_menu_playback_params =
	{"Parameters",		'a',	0,	0};
static struct vga_menu_item main_menu_playback_reduced_irq =
	{"xxx",			'i',	0,	0};
static struct vga_menu_item main_menu_playback_dsp_autoinit_dma =
	{"xxx",			't',	0,	0};
static struct vga_menu_item main_menu_playback_dsp_autoinit_command =
	{"xxx",			'c',	0,	0};
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
static struct vga_menu_item main_menu_playback_noreset_adpcm =
	{"xxx",			'n',	0,	0};
static struct vga_menu_item main_menu_playback_timer_clamp =
	{"xxx",			0,	0,	0};
static struct vga_menu_item main_menu_playback_dsp4_fifo_autoinit =
	{"xxx",			'f',	0,	0};
#endif

static const struct vga_menu_item* main_menu_playback[] = {
	&main_menu_playback_play,
	&main_menu_playback_stop,
	&menu_separator,
	&main_menu_playback_params,
	&main_menu_playback_reduced_irq,
	&main_menu_playback_dsp_autoinit_dma,
	&main_menu_playback_dsp_autoinit_command,
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	&main_menu_playback_noreset_adpcm,
	&main_menu_playback_timer_clamp,
	&main_menu_playback_dsp4_fifo_autoinit,
#endif
	NULL
};

static const struct vga_menu_item main_menu_device_dsp_reset =
	{"DSP reset",		'r',	0,	0};
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
static const struct vga_menu_item main_menu_device_mixer_reset =
	{"Mixer reset",		'r',	0,	0};
#endif
static const struct vga_menu_item main_menu_device_trigger_irq =
	{"IRQ test",		't',	0,	0};
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
static const struct vga_menu_item main_menu_device_autoinit_stop =
	{"Exit auto-init",	'x',	0,	0};
static const struct vga_menu_item main_menu_device_autoinit_stopcont =
	{"Exit & continue auto-init",	'o',	0,	0};
static const struct vga_menu_item main_menu_device_haltcont_dma =
	{"Halt & continue DMA",	'h',	0,	0};
#endif
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
static struct vga_menu_item main_menu_device_srate_force =
	{"xxx",		        's',	0,	0};
static struct vga_menu_item main_menu_device_realloc_dma =
	{"Realloc DMA",		'd',	0,	0};
#endif

static const struct vga_menu_item* main_menu_device[] = {
	&main_menu_device_dsp_reset,
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	&main_menu_device_mixer_reset,
#endif
	&main_menu_device_trigger_irq,
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	&main_menu_device_autoinit_stop,
	&main_menu_device_autoinit_stopcont,
	&main_menu_device_haltcont_dma,
#endif
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
    &main_menu_device_srate_force,
    &main_menu_device_realloc_dma,
#endif
	NULL
};

static const struct vga_menu_item main_menu_help_about =
	{"About",		'r',	0,	0};



#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
static const struct vga_menu_item main_menu_help_dsp_modes =
	{"DSP modes",		'd',	0,	0};
#endif
static const struct vga_menu_item* main_menu_help[] = {
	&main_menu_help_about,
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	&menu_separator,
	&main_menu_help_dsp_modes,
#endif
	NULL
};

static const struct vga_menu_bar_item main_menu_bar[] = {
	/* name                 key     scan    x       w       id */
	{" File ",		'F',	0x21,	0,	6,	&main_menu_file}, /* ALT-F */
	{" Playback ",		'P',	0x19,	6,	10,	&main_menu_playback}, /* ALT-P */
	{" Device ",		'D',	0x20,	16,	8,	&main_menu_device}, /* ALT-D */
	{" Help ",		'H',	0x23,	24,	6,	&main_menu_help}, /* ALT-H */
	{NULL,			0,	0x00,	0,	0,	0}
};

static void my_vga_menu_idle() {
	ui_anim(0);
}

static int confirm_quit() {
	/* FIXME: Why does this cause Direct DSP playback to horrifically slow down? */
	return confirm_yes_no_dialog("Are you sure you want to exit to DOS?");
}

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
int adpcm_warning_prompt() {
	return confirm_yes_no_dialog("Most Sound Blaster clones do not support auto-init ADPCM playback.\nIf nothing plays when enabled, your sound card is one of them.\n\nEnable?");
}
#endif

static void update_cfg() {
	unsigned int r;

	sb_card->dsp_adpcm = sb_card->dsp_adpcm;
	r = wav_sample_rate;
	if (sb_card->dsp_adpcm == ADPCM_4BIT) r /= 2;
	else if (sb_card->dsp_adpcm == ADPCM_2_6BIT) r /= 3;
	else if (sb_card->dsp_adpcm == ADPCM_2BIT) r /= 4;
	if (sb_card->dsp_adpcm > 0) {
		if (sb_card->dsp_adpcm == ADPCM_4BIT)
			sb_card->buffer_irq_interval = wav_sample_rate / 2;
		else if (sb_card->dsp_adpcm == ADPCM_2_6BIT)
			sb_card->buffer_irq_interval = wav_sample_rate / 3;
		else if (sb_card->dsp_adpcm == ADPCM_2BIT)
			sb_card->buffer_irq_interval = wav_sample_rate / 4;

		if (reduced_irq_interval == 2)
			sb_card->buffer_irq_interval = sb_card->buffer_size;
		else if (reduced_irq_interval == 0)
			sb_card->buffer_irq_interval /= 15;
		else if (reduced_irq_interval == -1)
			sb_card->buffer_irq_interval /= 100;

		if (sb_card->dsp_adpcm == ADPCM_4BIT)
			sb_card->buffer_irq_interval &= ~1UL;
		else if (sb_card->dsp_adpcm == ADPCM_2_6BIT)
			sb_card->buffer_irq_interval -=
				sb_card->buffer_irq_interval % 3;
		else if (sb_card->dsp_adpcm == ADPCM_2BIT)
			sb_card->buffer_irq_interval &= ~3UL;
	}
	else {
		sb_card->buffer_irq_interval = r;
		if (reduced_irq_interval == 2)
			sb_card->buffer_irq_interval =
				sb_card->buffer_size / wav_bytes_per_sample;
		else if (reduced_irq_interval == 0)
			sb_card->buffer_irq_interval /= 15;
		else if (reduced_irq_interval == -1)
			sb_card->buffer_irq_interval /= 100;
	}

	if (reduced_irq_interval == 2)
		main_menu_playback_reduced_irq.text =
			"IRQ interval: full length";
	else if (reduced_irq_interval == 1)
		main_menu_playback_reduced_irq.text =
			"IRQ interval: large";
	else if (reduced_irq_interval == 0)
		main_menu_playback_reduced_irq.text =
			"IRQ interval: small";
	else /* -1 */
		main_menu_playback_reduced_irq.text =
			"IRQ interval: tiny";

	main_menu_playback_dsp_autoinit_dma.text =
		sb_card->dsp_autoinit_dma ? "DMA autoinit: On" : "DMA autoinit: Off";
	main_menu_playback_dsp_autoinit_command.text =
		sb_card->dsp_autoinit_command ? "DSP playback: auto-init" : "DSP playback: single-cycle";
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	main_menu_playback_dsp4_fifo_autoinit.text =
		sb_card->dsp_4xx_fifo_autoinit ? "DSP 4.xx autoinit FIFO: On" : "DSP 4.xx autoinit FIFO: Off";

    if (sb_card->srate_force_dsp_4xx)
        main_menu_device_srate_force.text = "Force srate cmd: Using 4.xx (switch to TC)";
    else if (sb_card->srate_force_dsp_tc)
        main_menu_device_srate_force.text = "Force srate cmd: Using TC (switch to Off)";
    else
        main_menu_device_srate_force.text = "Force srate cmd: Off (switch to 4.xx)";
#endif
}

static void prompt_play_wav(unsigned char rec) {
	unsigned char gredraw = 1;
#if TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__COMPACT__) || defined(__SMALL__))
#else
	struct find_t ft;
#endif

	{
		const char *rp;
		char temp[sizeof(wav_file)];
		int cursor = strlen(wav_file),i,c,redraw=1,ok=0;
		memcpy(temp,wav_file,strlen(wav_file)+1);
		while (!ok) {
			if (gredraw) {
#if TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__COMPACT__) || defined(__SMALL__))
#else
				char *cwd;
#endif

				gredraw = 0;
				vga_clear();
				vga_moveto(0,4);
				vga_write_color(0x07);
				vga_write("Enter WAV file path:\n");
				vga_write_sync();
				draw_irq_indicator();
				ui_anim(1);
				redraw = 1;

#if TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__COMPACT__) || defined(__SMALL__))
#else
				cwd = getcwd(NULL,0);
				if (cwd) {
					vga_moveto(0,6);
					vga_write_color(0x0B);
					vga_write(cwd);
					vga_write_sync();
				}

				if (_dos_findfirst("*.*",_A_NORMAL|_A_RDONLY,&ft) == 0) {
					int x=0,y=7,cw = 14,i;
					char *ex;

					do {
						ex = strrchr(ft.name,'.');
						if (!ex) ex = "";

						if (ft.attrib&_A_SUBDIR) {
							vga_write_color(0x0F);
						}
						else if (!strcasecmp(ex,".wav")) {
							vga_write_color(0x1E);
						}
						else {
							vga_write_color(0x07);
						}
						vga_moveto(x,y);
						for (i=0;i < 13 && ft.name[i] != 0;) vga_writec(ft.name[i++]);
						for (;i < 14;i++) vga_writec(' ');

						x += cw;
						if ((x+cw) > vga_state.vga_width) {
							x = 0;
							if (y >= vga_state.vga_height) break;
							y++;
						}
					} while (_dos_findnext(&ft) == 0);

					_dos_findclose(&ft);
				}
#endif
			}
			if (redraw) {
				rp = (const char*)temp;
				vga_moveto(0,5);
				vga_write_color(0x0E);
				for (i=0;i < 80;i++) {
					if (*rp != 0)	vga_writec(*rp++);
					else		vga_writec(' ');	
				}
				vga_moveto(cursor,5);
				vga_write_sync();
				redraw=0;
			}

			if (kbhit()) {
				c = getch();
				if (c == 0) c = getch() << 8;

				if (c == 27) {
					ok = -1;
				}
				else if (c == 13) {
#if TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__COMPACT__) || defined(__SMALL__))
					ok = 1;
#else
					struct stat st;

					if (isalpha(temp[0]) && temp[1] == ':' && temp[2] == 0) {
						unsigned int total;

						_dos_setdrive(tolower(temp[0])+1-'a',&total);
						temp[0] = 0;
						gredraw = 1;
						cursor = 0;
					}
					else if (stat(temp,&st) == 0) {
						if (S_ISDIR(st.st_mode)) {
							chdir(temp);
							temp[0] = 0;
							gredraw = 1;
							cursor = 0;
						}
						else {
							ok = 1;
						}
					}
					else {
						ok = 1;
					}
#endif
				}
				else if (c == 8) {
					if (cursor != 0) {
						temp[--cursor] = 0;
						redraw = 1;
					}
				}
				else if (c >= 32 && c < 256) {
					if (cursor < 79) {
						temp[cursor++] = (char)c;
						temp[cursor  ] = (char)0;
						redraw = 1;
					}
				}
			}
			else {
				ui_anim(0);
			}
		}

		if (ok == 1) {
			unsigned char wp = wav_playing;
			stop_play();
			close_wav();
			memcpy(wav_file,temp,strlen(temp)+1);
			open_wav();
			if (wp) begin_play();
		}
	}
}

static void help() {
#if TARGET_MSDOS == 16 && defined(__TINY__)
    printf("See source code for options.");
#else
	printf("test [options]\n");
# if !(TARGET_MSDOS == 16 && defined(__COMPACT__)) /* this is too much to cram into a small model EXE */
	printf(" /h /help             This help\n");
	printf(" /nopnp               Don't scan for ISA Plug & Play devices\n");
	printf(" /noprobe             Don't probe ISA I/O ports for non PnP devices\n");
	printf(" /noenv               Don't use BLASTER environment variable\n");
	printf(" /wav=<file>          Open with specified WAV file\n");
	printf(" /play                Automatically start playing WAV file\n");
	printf(" /sc=<N>              Automatically pick Nth sound card (first card=1)\n");
	printf(" /ddac                Force DSP Direct DAC output mode\n");
#  if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__)))
	printf(" /96k /64k /63k /32k  Limit DMA buffer to...\n");
#  else
	printf(" /64k /63k /32k       Limit DMA buffer to...\n");
#  endif
    printf(" /16k /8k /4k         Limit DMA buffer to...\n");
	printf(" /nomirqp             Disable 'manual' IRQ probing\n");
	printf(" /noairqp             Disable 'alt' IRQ probing\n");
	printf(" /nosb16cfg           Don't read configuration from SB16 config byte\n");
	printf(" /nodmap              Disable DMA probing\n");
	printf(" /nohdmap             Disable 16-bit DMA probing\n");
	printf(" /nowinvxd            don't try to identify Windows drivers\n");
	printf(" /nochain             Don't chain to previous IRQ (sound blaster IRQ)\n");
	printf(" /noidle              Don't use sndsb library idle function\n");
	printf(" /adma                Assume DMA controllers are present\n");
	printf(" /noess               Do not use/detect ESS 688/1869 extensions\n");
	printf(" /sbalias:dsp         Use DSP alias port 0x22D by default\n");
	printf(" /ex-ess              Experimentally use ESS extensions for ESS chips\n");
	printf("                      not directly supported.\n");
	printf(" /nif2                Don't use DSP command 0xF2 to probe IRQ\n");
	printf(" /ni80                Don't use DSP command 0x80 to probe IRQ\n");
	printf(" /nde2                Don't use DSP command 0xE2 to probe DMA\n");
	printf(" /nd14                Don't use DSP command 0x14 to probe DMA\n");
	printf(" /fip                 Force IRQ probe\n");
	printf(" /fdp                 Force DMA probe\n");
#  if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__)))
	printf(" /dmaaio              Allow DMA auto-init override (careful!)\n");
	printf(" /stopres             Always reset DSP to stop playback\n");
	printf(" /hinoblk             Assume DSP hispeed modes are non-blocking\n");
    printf(" /srf4xx              Force SB16 sample rate commands\n");
    printf(" /srftc               Force SB/SBPro time constant sample rate\n");
    printf(" /noasp               Don't probe SB16 ASP/CSP chip\n");
#  endif
# endif

# if TARGET_MSDOS == 32
	printf("The following option affects hooking the NMI interrupt. Hooking is\n");
	printf("required to work with Gravis Ultrasound SBOS/MEGA-EM SB emulation\n");
	printf("and to work around problems with common DOS extenders. If not specified,\n");
	printf("the program will only hook NMI if SBOS/MEGA-EM is resident.\n");
	printf(" /-nmi or /+nmi       Don't / Do hook NMI interrupt, reflect to real mode.\n");
# endif
#endif
}

int main(int argc,char **argv) {
	uint32_t sb_irq_pcounter = 0;
	int i,loop,redraw,bkgndredraw,cc;
	const struct vga_menu_item *mitem = NULL;
	unsigned char assume_dma = 0;
	int disable_probe = 0;
#ifdef ISAPNP
	int disable_pnp = 0;
#endif
	int disable_env = 0;
	int force_ddac = 0;
	VGA_ALPHA_PTR vga;
	int autoplay = 0;
	int sc_idx = -1;

	printf("Sound Blaster test program\n");
	for (i=1;i < argc;) {
		char *a = argv[i++];

		if (*a == '-' || *a == '/') {
			unsigned char m = *a++;
			while (*a == m) a++;

			if (!strcmp(a,"h") || !strcmp(a,"help")) {
				help();
				return 1;
			}
			else if (!strcmp(a,"ex-ess")) {
				sndsb_probe_options.experimental_ess = 1;
			}
			else if (!strcmp(a,"noess")) {
				sndsb_probe_options.disable_ess_extensions = 1;
			}
			else if (!strcmp(a,"adma")) {
				assume_dma = 1;
			}
			else if (!strcmp(a,"nochain")) {
				dont_chain_irq = 1;
			}
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__)))
			else if (!strcmp(a,"-nmi")) {
# if TARGET_MSDOS == 32
				sndsb_nmi_32_hook = 0;
# endif
			}
			else if (!strcmp(a,"+nmi")) {
# if TARGET_MSDOS == 32
				sndsb_nmi_32_hook = 1;
# endif
			}
#endif
#ifdef ISAPNP
			else if (!strcmp(a,"nopnp")) {
				disable_pnp = 1;
			}
#endif
			else if (!strncmp(a,"wav=",4)) {
				a += 4;
				strcpy(wav_file,a);
			}
			else if (!strcmp(a,"play")) {
				autoplay = 1;
			}
			else if (!strncmp(a,"sc=",3)) {
				a += 3;
				sc_idx = strtol(a,NULL,0);
			}
			else if (!strcmp(a,"noprobe")) {
				disable_probe = 1;
			}
			else if (!strcmp(a,"noenv")) {
				disable_env = 1;
			}
			else if (!strcmp(a,"ddac")) {
				force_ddac = 1;
			}
			else if (!strcmp(a,"nomirqp")) {
				sndsb_probe_options.disable_manual_irq_probing = 1;
			}
			else if (!strcmp(a,"noairqp")) {
				sndsb_probe_options.disable_alt_irq_probing = 1;
			}
			else if (!strcmp(a,"nosb16cfg")) {
				sndsb_probe_options.disable_sb16_read_config_byte = 1;
			}
			else if (!strcmp(a,"nodmap")) {
				sndsb_probe_options.disable_manual_dma_probing = 1;
			}
			else if (!strcmp(a,"nohdmap")) {
				sndsb_probe_options.disable_manual_high_dma_probing = 1;
			}
			else if (!strcmp(a,"nowinvxd")) {
				sndsb_probe_options.disable_windows_vxd_checks = 1;
			}
			else {
				help();
				return 1;
			}
		}
	}

	if (!probe_vga()) {
		printf("Cannot init VGA\n");
		return 1;
	}

	/* TODO: Make sure this code still runs reliably without DMA (Direct DAC etc) if DMA not available! */
	if (!probe_8237())
		printf("WARNING: Cannot init 8237 DMA\n");

	if (assume_dma)
		d8237_flags |= D8237_DMA_PRIMARY | D8237_DMA_SECONDARY;

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__)))
	printf("DMA available: 0-3=%s 4-7=%s mask8=0x%lx mask16=0x%lx 128k=%u\n",
		d8237_flags&D8237_DMA_PRIMARY?"yes":"no",
		d8237_flags&D8237_DMA_SECONDARY?"yes":"no",
        d8237_8bit_limit_mask(),
        d8237_16bit_limit_mask(),
        d8237_can_do_16bit_128k()?1:0);
#endif

	if (!probe_8259()) {
		printf("Cannot init 8259 PIC\n");
		return 1;
	}
	if (!probe_8254()) {
		printf("Cannot init 8254 timer\n");
		return 1;
	}
	if (!init_sndsb()) {
		printf("Cannot init library\n");
		return 1;
	}

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__)))
	/* it's up to us now to tell it certain minor things */
	sndsb_detect_virtualbox();		// whether or not we're running in VirtualBox
	/* sndsb now allows us to keep the EXE small by not referring to extra sound card support */
	sndsb_enable_sb16_support();		// SB16 support
	sndsb_enable_sc400_support();		// SC400 support
	sndsb_enable_ess_audiodrive_support();	// ESS AudioDrive support
#endif

#if TARGET_MSDOS == 32
	if (sndsb_nmi_32_hook > 0) /* it means the NMI hook is taken */
		printf("Sound Blaster NMI hook/reflection active\n");

	if (gravis_mega_em_detect(&megaem_info)) {
		/* let the user know we're a 32-bit program and MEGA-EM's emulation
		 * won't work with 32-bit DOS programs */
		printf("WARNING: Gravis MEGA-EM detected. Sound Blaster emulation doesn't work\n");
		printf("         with 32-bit protected mode programs (like myself). If you want\n");
		printf("         to test it's Sound Blaster emulation use the 16-bit real mode\n");
		printf("         builds instead.\n");
	}
	if (gravis_sbos_detect() >= 0) {
		printf("WARNING: Gravis SBOS emulation is not 100%% compatible with 32-bit builds.\n");
		printf("         It may work for awhile, but eventually the simulated IRQ will go\n");
		printf("         missing and playback will stall. Please consider using the 16-bit\n");
		printf("         real-mode builds instead. When a workaround is possible, it will\n");
		printf("         be implemented and this warning will be removed.\n");
	}
#elif TARGET_MSDOS == 16
# if defined(__LARGE__)
	if (gravis_sbos_detect() >= 0) {
		printf("WARNING: 16-bit large model builds of the SNDSB program have a known, but not\n");
		printf("         yet understood incompatability with Gravis SBOS emulation. Use the\n");
		printf("         dos86s, dos86m, or dos86c builds.\n");
	}
# endif
#endif
#ifdef ISAPNP
	if (!init_isa_pnp_bios()) {
		printf("Cannot init ISA PnP\n");
		return 1;
	}
	if (!disable_pnp) {
		if (find_isa_pnp_bios()) {
			int ret;
			char tmp[192];
			unsigned int j,nodesize=0;
			const char *whatis = NULL;
			unsigned char csn,node=0,numnodes=0xFF,data[192];

			memset(data,0,sizeof(data));
			if (isa_pnp_bios_get_pnp_isa_cfg(data) == 0) {
				struct isapnp_pnp_isa_cfg *nfo = (struct isapnp_pnp_isa_cfg*)data;
				isapnp_probe_next_csn = nfo->total_csn;
				isapnp_read_data = nfo->isa_pnp_port;
			}
			else {
				printf("  ISA PnP BIOS failed to return configuration info\n");
			}

			/* enumerate device nodes reported by the BIOS */
			if (isa_pnp_bios_number_of_sysdev_nodes(&numnodes,&nodesize) == 0 && numnodes != 0xFF && nodesize <= sizeof(devnode_raw)) {
				for (node=0;node != 0xFF;) {
					struct isa_pnp_device_node far *devn;
					unsigned char this_node;

					/* apparently, start with 0. call updates node to
					 * next node number, or 0xFF to signify end */
					this_node = node;
					if (isa_pnp_bios_get_sysdev_node(&node,devnode_raw,ISA_PNP_BIOS_GET_SYSDEV_NODE_CTRL_NOW) != 0) break;

					devn = (struct isa_pnp_device_node far*)devnode_raw;
					if (isa_pnp_is_sound_blaster_compatible_id(devn->product_id,&whatis)) {
						isa_pnp_product_id_to_str(tmp,devn->product_id);
						if ((ret = sndsb_try_isa_pnp_bios(devn->product_id,this_node,devn,sizeof(devnode_raw))) <= 0)
							printf("ISA PnP BIOS: error %d for %s '%s'\n",ret,tmp,whatis);
						else
							printf("ISA PnP BIOS: found %s '%s'\n",tmp,whatis);
					}
				}
			}

			/* enumerate the ISA bus directly */
			if (isapnp_read_data != 0) {
				printf("Scanning ISA PnP devices...\n");
				for (csn=1;csn < 255;csn++) {
					isa_pnp_init_key();
					isa_pnp_wake_csn(csn);

					isa_pnp_write_address(0x06); /* CSN */
					if (isa_pnp_read_data() == csn) {
						/* apparently doing this lets us read back the serial and vendor ID in addition to resource data */
						/* if we don't, then we only read back the resource data */
						isa_pnp_init_key();
						isa_pnp_wake_csn(csn);

						for (j=0;j < 9;j++) data[j] = isa_pnp_read_config();

						if (isa_pnp_is_sound_blaster_compatible_id(*((uint32_t*)data),&whatis)) {
							isa_pnp_product_id_to_str(tmp,*((uint32_t*)data));
							if ((ret = sndsb_try_isa_pnp(*((uint32_t*)data),csn)) <= 0)
								printf("ISA PnP: error %d for %s '%s'\n",ret,tmp,whatis);
							else
								printf("ISA PnP: found %s '%s'\n",tmp,whatis);
						}
					}

					/* return back to "wait for key" state */
					isa_pnp_write_data_register(0x02,0x02);	/* bit 1: set -> return to Wait For Key state (or else a Pentium Pro system I own eventually locks up and hangs) */
				}
			}
		}
	}
#endif
	/* Non-plug & play scan */
	if (!disable_env && sndsb_try_blaster_var() != NULL) {
		printf("Created card ent. for BLASTER variable. IO=%X MPU=%X DMA=%d DMA16=%d IRQ=%d\n",
			sndsb_card_blaster->baseio,
			sndsb_card_blaster->mpuio,
			sndsb_card_blaster->dma8,
			sndsb_card_blaster->dma16,
			sndsb_card_blaster->irq);
		if (!sndsb_init_card(sndsb_card_blaster)) {
			printf("Nope, didn't work\n");
			sndsb_free_card(sndsb_card_blaster);
		}
	}
	if (!disable_probe) {
		if (sndsb_try_base(0x220))
			printf("Also found one at 0x220\n");
		if (sndsb_try_base(0x240))
			printf("Also found one at 0x240\n");
	}

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	/* There is a known issue with NTVDM.EXE Sound Blaster emulation under Windows XP. Not only
	 * do we get stuttery audio, but at some random point (1-5 minutes of continuous playback)
	 * the DOS VM crashes up for some unknown reason (VM is hung). */
	if (windows_mode == WINDOWS_NT) {
		struct sndsb_ctx *cx = sndsb_index_to_ctx(0);
		if (cx != NULL && cx->baseio != 0) {
			if (cx->windows_emulation && cx->windows_xp_ntvdm) {
				printf("WARNING: Windows XP/Vista/7 NTVDM.EXE emulation detected.\n");
				printf("         There is a known issue with NTVDM.EXE emulation that causes\n");
				printf("         playback to stutter, and if left running long enough, causes\n");
				printf("         this program to lock up and freeze.\n");
				printf("         If you must use this program under Windows XP, please consider\n");
				printf("         installing VDMSOUND and running this program within the VDMSOUND\n");
				printf("         environment.\n");
			}
		}
	}
#endif

	/* init card no longer probes the mixer */
	for (i=0;i < SNDSB_MAX_CARDS;i++) {
		struct sndsb_ctx *cx = sndsb_index_to_ctx(i);
		if (cx->baseio == 0) continue;

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
		if (!cx->mixer_probed)
			sndsb_probe_mixer(cx);
#endif

		if (cx->irq < 0)
			sndsb_probe_irq_F2(cx);
		if (cx->irq < 0)
			sndsb_probe_irq_80(cx);
		if (cx->dma8 < 0)
			sndsb_probe_dma8_E2(cx);
		if (cx->dma8 < 0)
			sndsb_probe_dma8_14(cx);

		// having IRQ and DMA changes the ideal playback method and capabilities
		sndsb_update_capabilities(cx);
		sndsb_determine_ideal_dsp_play_method(cx);
	}

	if (sc_idx < 0) {
		int count=0;
		for (i=0;i < SNDSB_MAX_CARDS;i++) {
			const char *ess_str;
			const char *mixer_str;

			struct sndsb_ctx *cx = sndsb_index_to_ctx(i);
			if (cx->baseio == 0) continue;

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
			mixer_str = sndsb_mixer_chip_str(cx->mixer_chip);
			ess_str = sndsb_ess_chipset_str(cx->ess_chipset);
#else
			mixer_str = "";
			ess_str = "";
#endif

			printf("  [%u] base=%X mpu=%X dma=%d dma16=%d irq=%d DSP=%u 1.XXAI=%u\n",
					i+1,cx->baseio,cx->mpuio,cx->dma8,cx->dma16,cx->irq,cx->dsp_ok,cx->dsp_autoinit_dma);
			printf("      MIXER=%u[%s] DSPv=%u.%u SC6600=%u OPL=%X GAME=%X AWE=%X\n",
					cx->mixer_ok,mixer_str,(unsigned int)cx->dsp_vmaj,(unsigned int)cx->dsp_vmin,
					cx->is_gallant_sc6600,cx->oplio,cx->gameio,cx->aweio);
			printf("      ESS=%u[%s] use=%u wss=%X OPL3SAx=%X\n",
					cx->ess_chipset,ess_str,cx->ess_extensions,cx->wssio,cx->opl3sax_controlio);
#ifdef ISAPNP
			if (cx->pnp_name != NULL) {
				isa_pnp_product_id_to_str(temp_str,cx->pnp_id);
				printf("      ISA PnP[%u]: %s %s\n",cx->pnp_csn,temp_str,cx->pnp_name);
			}
#endif
			printf("      '%s'\n",cx->dsp_copyright);

			count++;
		}
		if (count == 0) {
			printf("No cards found.\n");
			return 1;
		}
		printf("-----------\n");
		printf("Select the card you wish to test: "); fflush(stdout);
		i = getch();
		printf("\n");
		if (i == 27) return 0;
		if (i == 13 || i == 10) i = '1';
		sc_idx = i - '0';
	}

	if (sc_idx < 1 || sc_idx > SNDSB_MAX_CARDS) {
		printf("Sound card index out of range\n");
		return 1;
	}

	sb_card = &sndsb_card[sc_idx-1];
	if (sb_card->baseio == 0) {
		printf("No such card\n");
		return 1;
	}

	printf("Allocating sound buffer..."); fflush(stdout);
    realloc_dma_buffer();

	i = int10_getmode();
	if (i != 3) int10_setmode(3);

	/* hook IRQ 0 */
	irq_0_count = 0;
	irq_0_adv = 1;
	irq_0_max = 1;
	old_irq_0 = _dos_getvect(irq2int(0));
	_dos_setvect(irq2int(0),irq_0);
	p8259_unmask(0);

	if (sb_card->irq != -1) {
		old_irq_masked = p8259_is_masked(sb_card->irq);
		if (vector_is_iret(irq2int(sb_card->irq)))
			old_irq_masked = 1;

		old_irq = _dos_getvect(irq2int(sb_card->irq));
		_dos_setvect(irq2int(sb_card->irq),sb_irq);
		p8259_unmask(sb_card->irq);
	}
	
	vga_write_color(0x07);
	vga_clear();

	loop=1;
	redraw=1;
	bkgndredraw=1;
	vga_menu_bar.bar = main_menu_bar;
	vga_menu_bar.sel = -1;
	vga_menu_bar.row = 3;
	vga_menu_idle = my_vga_menu_idle;
	if (force_ddac) sb_card->dsp_play_method = SNDSB_DSPOUTMETHOD_DIRECT;
	reduced_irq_interval=(sb_card->dsp_play_method == SNDSB_DSPOUTMETHOD_1xx);
	update_cfg();

	if (!sndsb_assign_dma_buffer(sb_card,sb_dma)) {
		printf("Cannot assign DMA buffer\n");
		return 1;
	}

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
	/* please let me know if the user attempts to close my DOS Box */
	if (dos_close_awareness_available()) {
		int d;

		printf("Windows is running, attempting to enable Windows close-awareness\n");

		/* counter-intuitive in typical Microsoft fashion.
		 * When they say "enable/disable" the close command what they apparently mean
		 * is that "enabling" it triggers immediate closing when the user clicks the close
		 * button, and "disabling" means it queues the event and hands it to the DOS
		 * program as a request to shutdown. If their documentation would simply
		 * explain that, I would not have wasted 30 minutes wondering why Windows 9x
		 * would immediately complain about not being able to close this program.
		 *
		 * Sadly, Windows XP, despite being the "merging of Windows NT and 98 codebases"
		 * doesn't provide us with close-awareness. */
		if ((d=dos_close_awareness_enable(0)) != 0)
			printf("Warning, cannot enable Windows 'close-awareness' ret=0x%X\n",d);
		else
			printf("Close-awareness enabled\n");
	}
#endif

	if (wav_file[0] != 0) open_wav();
	if (autoplay) begin_play();
	while (loop) {
		if ((mitem = vga_menu_bar_keymon()) != NULL) {
			/* act on it */
			if (mitem == &main_menu_file_quit) {
				if (confirm_quit()) {
					loop = 0;
					break;
				}
			}
			else if (mitem == &main_menu_file_set) {
				prompt_play_wav(0);
				bkgndredraw = 1;
				redraw = 1;
			}
			else if (mitem == &main_menu_playback_play) {
				if (!wav_playing) {
					begin_play();
					redraw = 1;
				}
			}
			else if (mitem == &main_menu_playback_stop) {
				if (wav_playing) {
					stop_play();
					redraw = 1;
				}
			}
			else if (mitem == &main_menu_device_dsp_reset) {
				struct vga_msg_box box;
				vga_msg_box_create(&box,"Resetting DSP...",0,0);
				stop_play();
				sndsb_reset_dsp(sb_card);
				t8254_wait(t8254_us2ticks(1000000));
				vga_msg_box_destroy(&box);
			}
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
			else if (mitem == &main_menu_device_mixer_reset) {
				struct vga_msg_box box;
				vga_msg_box_create(&box,"Resetting mixer...",0,0);
				sndsb_reset_mixer(sb_card);
				t8254_wait(t8254_us2ticks(1000000));
				vga_msg_box_destroy(&box);
			}
#endif
			else if (mitem == &main_menu_help_about) {
				struct vga_msg_box box;
				vga_msg_box_create(&box,"Sound Blaster test program v1.3 for DOS\n\n(C) 2008-2017 Jonathan Campbell\nALL RIGHTS RESERVED\n"
#if TARGET_MSDOS == 32
					"32-bit protected mode version"
#elif defined(__LARGE__)
					"16-bit real mode (large model) version"
#elif defined(__MEDIUM__)
					"16-bit real mode (medium model) version"
#elif defined(__COMPACT__)
					"16-bit real mode (compact model) version"
#elif defined(__HUGE__)
					"16-bit real mode (huge model) version"
#elif defined(__TINY__)
					"16-bit real mode (tiny model) version"
#else
					"16-bit real mode (small model) version"
#endif
					,0,0);
				while (1) {
					ui_anim(0);
					if (kbhit()) {
						i = getch();
						if (i == 0) i = getch() << 8;
						if (i == 13 || i == 27) break;
					}
				}
				vga_msg_box_destroy(&box);
			}
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
			else if (mitem == &main_menu_help_dsp_modes) {
				int quit = 0;
				struct vga_msg_box box;

				vga_msg_box_create(&box,
					"Explanation of DSP modes:\n"
					"\n"
					"4.xx      Sound Blaster 16 or compatible DSP operation. 16-bit audio.\n"
					"\n"
					"3.xx      Sound Blaster Pro or compatible. Most clones emulate this card.\n"
					"          Creative SB16 cards do not replicate Sound Blaster Pro stereo.\n"
					"\n"
					"2.01      Sound Blaster 2.01 or compatible with auto-init DMA and\n"
					"          high-speed DAC playback modes up to 44100Hz.\n"
					"\n"
					"2.00      Sound Blaster 2.0 or compatible with auto-init DMA.\n"
					"\n"
					"1.xx      Original Sound Blaster or compatible DSP operation.\n"
					"\n"
					"Direct    DSP command 0x10 (Direct DAC output) and system timer.\n"
					"          If DMA is not available, this is your only option. Emulators,\n"
					"          clones, some motherboard & SB16 combos have problems with it.\n"
					"\n"
					"Detailed explanations are available in README.TXT"
					,0,0);
				while (!quit) {
					ui_anim(0);
					if (kbhit()) {
						i = getch();
						if (i == 0) i = getch() << 8;
						if (i == 13 || i == 27) {
							quit = (i == 27);
							break;
						}
					}
				}
				vga_msg_box_destroy(&box);

				vga_msg_box_create(&box,
					"Additional playback modes:\n"
					"\n"
					"Flip sign    Flip sign bit before sending to audio, and instruct SB16 DSP\n"
					"             to play nonstandard format. Clones may produce loud static.\n"
					"\n"
					"ADPCM        Convert audio to Sound Blaster ADPCM format and instruct DSP\n"
					"             to play it. Clones generally do not support this.\n"
					"\n"
					"Auto-init    DSP 2.01 and higher support auto-init ADPCM playback. Clones\n"
					"ADPCM        definitely do not support this.\n"
					"\n"
					"ADPCM reset  On actual Creative SB hardware the DSP resets the ADPCM step\n"
					"per interval size per block. DOSBox, emulators, do not reset ADPCM state.\n"
					"\n"
					"Goldplay     A semi-popular music tracker library, SB support is hacky.\n"
					"             This program can use the same technique for testing purposes\n"
					"\n"
					"Detailed explanations are available in README.TXT"
					,0,0);
				while (!quit) {
					ui_anim(0);
					if (kbhit()) {
						i = getch();
						if (i == 0) i = getch() << 8;
						if (i == 13 || i == 27) {
							quit = (i == 27);
							break;
						}
					}
				}
				vga_msg_box_destroy(&box);

				vga_msg_box_create(&box,
					"Additional things you can play with:\n"
					"\n"
					"IRQ interval ..... DSP block size to play/record with.\n"
					"DMA autoinit ..... Use/don't use auto-init on the DMA controller side.\n"
					"DSP playback ..... Whether to use auto-init or single-cycle DSP commands.\n"
					"Force hispeed .... If set, always use hispeed DSP playback (SB 2.0 and Pro).\n"
					"\n"
					"DSP 4.xx autoinit FIFO . (SB16) use the FIFO for auto-init DSP 4.xx.\n"
					"DSP 4.xx single FIFO ... (SB16) use the FIFO for single cycle DSP 4.xx.\n"
					"DSP nag mode ........... Test 'nagging' the DSP, Crystal Dream style.\n"
					"                         Has no effect unless DSP in single-cycle mode.\n"
					"Poll ack when no IRQ ... If not assigned an IRQ, use polling of the status\n"
					"                         port to prevent halting on SB16.\n"
					"DSP alias port ......... Use alias port 22Dh instead of 22Ch.\n"
					"Backwards .............. Play file backwards by using DMA decrement mode.\n"
					"Wari hack alias ........ Use DSP command 0x15 instead of DSP command 0x14\n"
					"                         to play audio. 'Wari' uses this oddity for audio.\n"
					"\n"
					"Detailed explanations are available in README.TXT"
					,0,0);
				while (!quit) {
					ui_anim(0);
					if (kbhit()) {
						i = getch();
						if (i == 0) i = getch() << 8;
						if (i == 13 || i == 27) {
							quit = (i == 27);
							break;
						}
					}
				}
				vga_msg_box_destroy(&box);
			}
#endif
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
			else if (mitem == &main_menu_playback_dsp4_fifo_autoinit) {
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				sb_card->dsp_4xx_fifo_autoinit = !sb_card->dsp_4xx_fifo_autoinit;
				update_cfg();
				ui_anim(1);
				if (wp) begin_play();
			}
            else if (mitem == &main_menu_device_srate_force) {
				unsigned char wp = wav_playing;
				if (wp) stop_play();

                if (sb_card->srate_force_dsp_4xx) {
                    sb_card->srate_force_dsp_4xx = 0;
                    sb_card->srate_force_dsp_tc = 1;
                }
                else if (sb_card->srate_force_dsp_tc) {
                    sb_card->srate_force_dsp_4xx = 0;
                    sb_card->srate_force_dsp_tc = 0;
                }
                else {
                    sb_card->srate_force_dsp_4xx = 1;
                    sb_card->srate_force_dsp_tc = 0;
                }
				update_cfg();

				if (wp) begin_play();
				bkgndredraw = 1;
				redraw = 1;
            }
#endif
			else if (mitem == &main_menu_playback_dsp_autoinit_dma) {
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				sb_card->dsp_autoinit_dma = !sb_card->dsp_autoinit_dma;
				update_cfg();
				if (wp) begin_play();
			}
			else if (mitem == &main_menu_playback_dsp_autoinit_command) {
				/* NOTES:
				 *
				 *   - Pro Audio Spectrum 16 (PAS16): Despite reporting DSP v2.0, auto-init commands actually do work */
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				sb_card->dsp_autoinit_command = !sb_card->dsp_autoinit_command;
				update_cfg();
				if (wp) begin_play();
			}
			else if (mitem == &main_menu_playback_reduced_irq) {
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				if (++reduced_irq_interval >= 3) reduced_irq_interval = -1;
				update_cfg();
				if (wp) begin_play();
			}
			else if (mitem == &main_menu_playback_params) {
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				change_param_menu();
				if (wp) begin_play();
				bkgndredraw = 1;
				redraw = 1;
			}
			else if (mitem == &main_menu_device_trigger_irq) {
				unsigned char wp = wav_playing;
				struct vga_msg_box box;
				int res;

				if (wp) stop_play();

				vga_msg_box_create(&box,"Issuing DSP command to test IRQ",0,0);
				res = sndsb_irq_test(sb_card); // -1 test inconclusive 0 = failed 1 = pass */
				vga_msg_box_destroy(&box);

				if (wp) begin_play();

				vga_msg_box_create(&box,(res < 0) ? "IRQ test N/A" : ((res > 0) ? "IRQ test success" : "IRQ test failed"),0,0);
				while (1) {
					ui_anim(0);
					if (kbhit()) {
						i = getch();
						if (i == 0) i = getch() << 8;
						if (i == 13 || i == 27) break;
					}
				}
				vga_msg_box_destroy(&box);
			}
#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
			else if (mitem == &main_menu_device_haltcont_dma) {
				unsigned char wp = wav_playing;
				struct vga_msg_box box;
				int res;

				vga_msg_box_create(&box,"Asking to halt DMA. Hit enter to then ask to continue DMA",0,0);

				_cli(); // do not conflict with IRQ handler
				res = sndsb_halt_dma(sb_card);
				_sti();
				if (!res) {
					vga_msg_box_destroy(&box);
					vga_msg_box_create(&box,"Failed to halt DMA",0,0);
					stop_play();
					sndsb_reset_dsp(sb_card);
				}

				while (1) {
					ui_anim(0);
					if (kbhit()) {
						i = getch();
						if (i == 0) i = getch() << 8;
						if (i == 13 || i == 27) break;
					}
				}
				vga_msg_box_destroy(&box);

				_cli(); // do not conflict with IRQ handler
				res = sndsb_continue_dma(sb_card);
				_sti();
				if (!res) {
					vga_msg_box_create(&box,"Failed to continue DMA",0,0);
					stop_play();
					sndsb_reset_dsp(sb_card);

					while (1) {
						ui_anim(0);
						if (kbhit()) {
							i = getch();
							if (i == 0) i = getch() << 8;
							if (i == 13 || i == 27) break;
						}
					}

					vga_msg_box_destroy(&box);
				}

				if (wp) begin_play();
			}
			else if (mitem == &main_menu_device_autoinit_stop || mitem == &main_menu_device_autoinit_stopcont) {
				unsigned char wp = wav_playing;
				struct vga_msg_box box;
				int res;

				vga_msg_box_create(&box,"Asking sound card to exit auto-init mode\nAudio should stop after one IRQ block.\nContinuing auto-init will not work if playback stops",0,0);

				_cli(); // do not conflict with IRQ handler
				res = sndsb_exit_autoinit_mode(sb_card);
				_sti();
				if (!res) {
					vga_msg_box_destroy(&box);
					vga_msg_box_create(&box,"Failed to exit auto-init DMA",0,0);
					stop_play();
					sndsb_reset_dsp(sb_card);
				}

				while (1) {
					ui_anim(0);
					if (kbhit()) {
						i = getch();
						if (i == 0) i = getch() << 8;
						if (i == 13 || i == 27) break;
					}
				}
				vga_msg_box_destroy(&box);

				if (mitem == &main_menu_device_autoinit_stopcont) {
					vga_msg_box_create(&box,"Asking sound card to reenter auto-init mode\nIf audio already stopped, then this will not restart it.\nHit enter to next use the continue DMA commands.",0,0);

					_cli(); // do not conflict with IRQ handler
					res = sndsb_continue_autoinit_mode(sb_card);
					_sti();
					if (!res) {
						vga_msg_box_destroy(&box);
						vga_msg_box_create(&box,"Failed to continue auto-init DMA",0,0);
						stop_play();
						sndsb_reset_dsp(sb_card);
					}

					while (1) {
						ui_anim(0);
						if (kbhit()) {
							i = getch();
							if (i == 0) i = getch() << 8;
							if (i == 13 || i == 27) break;
						}
					}
					vga_msg_box_destroy(&box);

					vga_msg_box_create(&box,"I sent continue DMA commands. Did they work?",0,0);

					_cli(); // do not conflict with IRQ handler
					res = sndsb_continue_dma(sb_card);
					_sti();
					if (!res) {
						vga_msg_box_destroy(&box);
						vga_msg_box_create(&box,"Failed to continue DMA",0,0);
						stop_play();
						sndsb_reset_dsp(sb_card);
					}

					while (1) {
						ui_anim(0);
						if (kbhit()) {
							i = getch();
							if (i == 0) i = getch() << 8;
							if (i == 13 || i == 27) break;
						}
					}
					vga_msg_box_destroy(&box);
				}

				if (wp) {
					stop_play();
					begin_play();
				}
			}
#endif
		}

		if (sb_card->irq_counter != sb_irq_pcounter) {
			sb_irq_pcounter = sb_card->irq_counter;
			redraw = 1;
		}

		if (redraw || bkgndredraw) {
			if (!wav_playing) update_cfg();
			if (bkgndredraw) {
				for (vga=vga_state.vga_alpha_ram+(80*2),cc=0;cc < (80*23);cc++) *vga++ = 0x1E00 | 177;
				vga_menu_bar_draw();
				draw_irq_indicator();
			}
			ui_anim(bkgndredraw);
			_cli();
			vga_moveto(0,2);
			vga_write_color(0x1F);
			for (vga=vga_state.vga_alpha_ram+(80*2),cc=0;cc < 80;cc++) *vga++ = 0x1F20;
			vga_write("File: ");
			vga_write(wav_file);
			vga_write_sync();
			bkgndredraw = 0;
			redraw = 0;
			_sti();
		}

		if (kbhit()) {
			i = getch();
			if (i == 0) i = getch() << 8;

			if (i == 27) {
				if (confirm_quit()) {
					loop = 0;
					break;
				}
			}
			else if (i == '?') {
				if (sb_card->reason_not_supported) {
					struct vga_msg_box box;

					vga_msg_box_create(&box,sb_card->reason_not_supported,0,0);
					while (1) {
						ui_anim(0);
						if (kbhit()) {
							i = getch();
							if (i == 0) i = getch() << 8;
							if (i == 13 || i == 27) break;
						}
					}
					vga_msg_box_destroy(&box);
				}
			}
			else if (i == ' ') {
                if (wav_playing) stop_play();
                else begin_play();
            }
			else if (i == 0x4B00) {
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				wav_position -= wav_sample_rate * 5UL;
				if ((signed long)wav_position < 0) wav_position = 0;
				if (wp) begin_play();
				bkgndredraw = 1;
				redraw = 1;
			}
			else if (i == 0x4D00) {
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				wav_position += wav_sample_rate * 5UL;
				if (wp) begin_play();
				bkgndredraw = 1;
				redraw = 1;
			}
			else if (i == 0x4300) { /* F9 */
				unsigned char wp = wav_playing;
				if (wp) stop_play();
				sb_card->backwards ^= 1;
				if (wp) begin_play();
				bkgndredraw = 1;
				redraw = 1;
			}
			else if (i == 0x4200) { /* F8 */
				getch(); /* delibrate pause */
			}
		}

#if !(TARGET_MSDOS == 16 && (defined(__TINY__) || defined(__SMALL__) || defined(__COMPACT__))) /* this is too much to cram into a small model EXE */
		/* Windows "close-awareness".
		 * If the user attempts to close the DOSBox window, Windows will let us know */
		if (dos_close_awareness_available()) {
			int r = dos_close_awareness_query();

			if (r == DOS_CLOSE_AWARENESS_NOT_ACK) {
				/* then ack it, and act as if File -> Quit were selected */
				dos_close_awareness_ack();

				if (confirm_quit())
					break;
				else
					dos_close_awareness_cancel();
			}
			else if (r == DOS_CLOSE_AWARENESS_ACKED) {
				/* then we need to exit */
				break;
			}
		}
#endif

		ui_anim(0);
	}

	_sti();
	vga_write_sync();
	printf("Stopping playback...\n");
	stop_play();
	printf("Closing WAV...\n");
	close_wav();
	printf("Freeing buffer...\n");
    free_dma_buffer();

	if (sb_card->irq >= 0 && old_irq_masked)
		p8259_mask(sb_card->irq);

	printf("Releasing IRQ...\n");
	if (sb_card->irq != -1)
		_dos_setvect(irq2int(sb_card->irq),old_irq);

	sndsb_free_card(sb_card);
	free_sndsb(); /* will also de-ref/unhook the NMI reflection */
	_dos_setvect(irq2int(0),old_irq_0);
	return 0;
}


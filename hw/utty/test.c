
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include <hw/dos/dos.h>
#include <hw/utty/utty.h>

int main(int argc,char **argv) {
	probe_dos();

#ifdef TARGET_PC98
	if (!probe_nec_pc98()) {
		printf("Sorry, your system is not PC-98\n");
		return 1;
	}
#else
	if (!probe_vga()) {
        printf("VGA probe failed\n");
		return 1;
	}
#endif

    if (!utty_init()) {
        printf("utty init fail\n");
        return 1;
    }
#ifdef TARGET_PC98
    if (!utty_init_pc98()) {
        printf("utty init vga fail\n");
        return 1;
    }
#else
    if (!utty_init_vgalib()) {
        printf("utty init vga fail\n");
        return 1;
    }
#endif

#if TARGET_MSDOS == 32
    printf("Alpha ptr: %p\n",utty_funcs.vram);
#else
    printf("Alpha ptr: %Fp\n",utty_funcs.vram);
#endif
    printf("Size: %u x %u (stride=%u)\n",utty_funcs.w,utty_funcs.h,utty_funcs.stride);

    {
        const char *msg = "Hello world";
        utty_offset_t o = utty_offset_getofs(4/*row*/,4/*col*/);
        UTTY_ALPHA_CHAR uch;
        unsigned char c;

        uch.f.at = UTTY_COLOR_MAKE_INTENSITY_FG(UTTY_COLOR_FB(/*f*/UTTY_COLOR_RED,/*b*/UTTY_COLOR_BLACK));
        while ((c=(*msg++)) != 0) {
            uch.f.ch = c;
            o = utty_funcs.setchar(o,uch);
        }
    }

    {
        const char *msg = "Hello world";
        utty_offset_t o = utty_offset_getofs(5/*row*/,4/*col*/);
        UTTY_ALPHA_CHAR uach[12],uch;
        unsigned int i=0;
        unsigned char c;

        uch.f.at = UTTY_COLOR_MAKE_INTENSITY_FG(UTTY_COLOR_FB(/*f*/UTTY_COLOR_GREEN,/*b*/UTTY_COLOR_BLACK));
        while ((c=(*msg++)) != 0) {
            uch.f.ch = c;
            uach[i++] = uch;
        }
        assert(i < 12);
        utty_funcs.setcharblock(o,uach,i);
    }

    {
#ifdef TARGET_PC98
        const char *msg = "Hello world \x82\xb1\x82\xea\x82\xcd\x93\xfa\x96\x7b\x8c\xea\x82\xc5\x82\xb7";
#else
        const char *msg = "Hello world";
#endif
        utty_offset_t o = utty_offset_getofs(6/*row*/,4/*col*/);
        UTTY_ALPHA_CHAR uach[40],uch = UTTY_BLANK_CHAR;
        unsigned int i=0;

        uch.f.at = UTTY_COLOR_MAKE_INTENSITY_FG(UTTY_COLOR_FB(/*f*/UTTY_COLOR_BLUE,/*b*/UTTY_COLOR_BLACK));
        i = utty_string2ac_const(uach,UTTY_ARRAY_LEN(uach),msg,uch);
        assert(i < 40);
        utty_funcs.setcharblock(o,uach,i);
    }

    {
#ifdef TARGET_PC98
        const char *msg = "Hello world \x82\xb1\x82\xea\x82\xcd\x93\xfa\x96\x7b\x8c\xea\x82\xc5\x82\xb7";
#else
        const char *msg = "Hello world";
#endif
        utty_offset_t o = utty_offset_getofs(7/*row*/,4/*col*/);
        UTTY_ALPHA_CHAR uach[UTTY_MAX_CHAR_WIDTH],uch = UTTY_BLANK_CHAR;
        const char *scan = msg,*scanfence = msg + strlen(msg);
        unsigned int i=0;

        uch.f.at = UTTY_COLOR_MAKE_INTENSITY_FG(UTTY_COLOR_FB(/*f*/UTTY_COLOR_WHITE,/*b*/UTTY_COLOR_BLACK));
        while (*scan != 0) {
            i = utty_string2ac(uach,UTTY_ARRAY_LEN(uach),&scan,uch);
            assert(i <= UTTY_MAX_CHAR_WIDTH);
            utty_funcs.setcharblock(o,uach,i);
            o = utty_offset_advance(o,i);
            assert(scan <= scanfence);
        }
    }

    {
#ifdef TARGET_PC98
        const char *msg = "Hello world \x82\xb1\x82\xea\x82\xcd\x93\xfa\x96\x7b\x8c\xea\x82\xc5\x82\xb7";
#else
        const char *msg = "Hello world";
#endif
        utty_offset_t o = utty_offset_getofs(8/*row*/,4/*col*/);
        UTTY_ALPHA_CHAR uch = UTTY_BLANK_CHAR;

#ifdef TARGET_PC98
        uch.f.at = 0x00C1u;         // yellow
#else
        uch.f.at = 0x0Eu;           // yellow
#endif

        utty_printat_const(o,msg,uch);
    }

    {
#ifdef TARGET_PC98
        const char *msg = "Hello world \x82\xb1\x82\xea\x82\xcd\x93\xfa\x96\x7b\x8c\xea\x82\xc5\x82\xb7";
#else
        const char *msg = "Hello world";
#endif
        utty_offset_t o = utty_offset_getofs(9/*row*/,4/*col*/),ostp;
        UTTY_ALPHA_CHAR uch = UTTY_BLANK_CHAR;

        uch.f.at = UTTY_COLOR_FB(/*f*/UTTY_COLOR_BLACK,/*b*/UTTY_COLOR_YELLOW);
        ostp = utty_offset_advance(o,32);
        o = utty_printat_const(o,msg,uch);
        while (o < ostp) o = utty_funcs.setchar(o,uch);
    }

    return 0;
}


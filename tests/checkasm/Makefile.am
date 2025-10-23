check_PROGRAMS = checkasm

TESTS = checkasm

checkasm_CPPFLAGS = -I$(top_srcdir) -I$(top_srcdir)/include \
       -I$(top_builddir) -I$(top_builddir)/include \
       $(AVUTIL_CFLAGS) \
       $(NULL)

checkasm_LDADD = $(LDADD) $(AVUTIL_LIBS) \
    $(top_builddir)/lib/upipe-v210/libupipe_v210_la-v210dec.o \
    $(top_builddir)/lib/upipe-v210/libupipe_v210_la-v210enc.o \
    $(NULL)

checkasm_SOURCES = checkasm.c checkasm.h timer.h \
    planar10_input.c \
    planar8_input.c \
    sdi_input.c \
    uyvy_input.c \
    v210_input.c \
    $(NULL)

if HAVE_BITSTREAM
checkasm_LDADD += \
    $(top_builddir)/lib/upipe-hbrmt/libupipe_hbrmt_la-sdidec.o \
    $(top_builddir)/lib/upipe-hbrmt/libupipe_hbrmt_la-sdienc.o \
    $(NULL)
endif

if HAVE_X86ASM
checkasm_SOURCES += x86/checkasm.asm x86/timer.h
checkasm_LDADD += \
    $(top_builddir)/lib/upipe-v210/v210dec.o \
    $(top_builddir)/lib/upipe-v210/v210enc.o \
    $(NULL)
CLEANFILES = x86/checkasm.lo x86/.libs/checkasm.o

if HAVE_BITSTREAM
checkasm_LDADD += \
    $(top_builddir)/lib/upipe-hbrmt/sdidec.o \
    $(top_builddir)/lib/upipe-hbrmt/sdienc.o \
    $(NULL)
endif
endif

if ARCH_AARCH64
checkasm_SOURCES += aarch64/asm.S aarch64/checkasm.S aarch64/timer.h
endif

V_ASM = $(V_ASM_@AM_V@)
V_ASM_ = $(V_ASM_@AM_DEFAULT_VERBOSITY@)
V_ASM_0 = @echo "  ASM     " $@;

# NOTE: the extension is .o
.asm.o:
	$(V_ASM)$(LIBTOOL) $(AM_V_lt) --mode=compile --tag=CC $(NASM) $(NASMFLAGS) $< -o $@

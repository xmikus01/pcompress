#
# This file is a part of Pcompress, a chunked parallel multi-
# algorithm lossless compression and decompression program.
#
# Copyright (C) 2012 Moinak Ghosh. All rights reserved.
# Use is subject to license terms.
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# moinakg@belenix.org, http://moinakg.wordpress.com/
#
# This program includes partly-modified public domain source
# code from the LZMA SDK: http://www.7-zip.org/sdk.html
#

PROG= pcompress
MAINSRCS = main.c utils.c allocator.c zlib_compress.c bzip2_compress.c \
	lzma_compress.c ppmd_compress.c adaptive_compress.c
MAINHDRS = allocator.h  pcompress.h  utils.h
MAINOBJS = $(MAINSRCS:.c=.o)

RABINSRCS = rabin/rabin_polynomial.c
RABINHDRS = rabin/rabin_polynomial.h utils.h
RABINOBJS = $(RABINSRCS:.c=.o)

LZMASRCS = lzma/LzmaEnc.c lzma/LzFind.c lzma/LzmaDec.c
LZMAHDRS = lzma/CpuArch.h lzma/LzFind.h lzma/LzmaEnc.h lzma/Types.h \
	lzma/LzHash.h lzma/LzmaDec.h utils.h
LZMAOBJS = $(LZMASRCS:.c=.o)

PPMDSRCS = lzma/Ppmd8.c lzma/Ppmd8Enc.c lzma/Ppmd8Dec.c
PPMDHDRS = lzma/Ppmd.h lzma/Ppmd8.h
PPMDOBJS = $(PPMDSRCS:.c=.o)

CRCSRCS = lzma/crc64_fast.c lzma/crc64_table.c
CRCHDRS = lzma/crc64_table_le.h lzma/crc64_table_be.h lzma/crc_macros.h
CRCOBJS = $(CRCSRCS:.c=.o)

BAKFILES = *~ lzma/*~ rabin/*~

RM = rm -f
CPPFLAGS = -I. -I./lzma -I./rabin -D_7ZIP_ST -DNODEFAULT_PROPS -DFILE_OFFSET_BITS=64 \
	-D_REENTRANT -D__USE_SSE_INTRIN__ -D_LZMA_PROB32
VEC_FLAGS = -ftree-vectorize
LOOP_OPTFLAGS = $(VEC_FLAGS) -floop-interchange -floop-block
LDLIBS = -ldl -lbz2 $(ZLIB_DIR) -lz -lm

ifdef DEBUG
LINK = g++ -m64 -pthread -msse3
COMPILE = gcc -m64 -O -g -msse3 -c
COMPILE_cpp = g++ -m64 -O -g -msse3 -c
ifdef DEBUG_NO_SLAB
CPPFLAGS += -DDEBUG_NO_SLAB
endif
else
LINK = g++ -m64 -pthread -msse3
COMPILE = gcc -m64 -O3 -msse3 -c
COMPILE_cpp = g++ -m64 -O3 -msse3 -c
CPPFLAGS += -DNDEBUG
endif

all: $(PROG)

$(LZMAOBJS): $(LZMASRCS) $(LZMAHDRS)
	$(COMPILE) $(CPPFLAGS) $(@:.o=.c) -o $@

$(CRCOBJS): $(CRCSRCS) $(CRCHDRS)
	$(COMPILE) $(VEC_FLAGS) $(CPPFLAGS) $(@:.o=.c) -o $@

$(PPMDOBJS): $(PPMDSRCS) $(PPMDHDRS)
	$(COMPILE) $(VEC_FLAGS) $(CPPFLAGS) $(@:.o=.c) -o $@

$(RABINOBJS): $(RABINSRCS) $(RABINHDRS)
	$(COMPILE) $(VEC_FLAGS) $(CPPFLAGS) $(@:.o=.c) -o $@

$(MAINOBJS): $(MAINSRCS) $(MAINHDRS)
	$(COMPILE) $(LOOP_OPTFLAGS) $(CPPFLAGS) $(@:.o=.c) -o $@

$(PROG): $(MAINOBJS) $(LZMAOBJS) $(PPMDOBJS) $(CRCOBJS) $(RABINOBJS)
	$(LINK) -o $@ $(MAINOBJS) $(LZMAOBJS) $(PPMDOBJS) $(CRCOBJS) $(RABINOBJS) $(LDLIBS)

clean:
	$(RM) $(PROG) $(MAINOBJS) $(LZMAOBJS) $(PPMDOBJS) $(CRCOBJS) $(RABINOBJS) $(BAKFILES)


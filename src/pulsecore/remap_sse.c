/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include <pulse/sample.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "cpu-x86.h"
#include "remap.h"

#define LOAD_SAMPLES                                   \
                " movdqu (%1), %%xmm0           \n\t"  \
                " movdqu 16(%1), %%xmm2         \n\t"  \
                " movdqu 32(%1), %%xmm4         \n\t"  \
                " movdqu 48(%1), %%xmm6         \n\t"  \
                " movdqa %%xmm0, %%xmm1         \n\t"  \
                " movdqa %%xmm2, %%xmm3         \n\t"  \
                " movdqa %%xmm4, %%xmm5         \n\t"  \
                " movdqa %%xmm6, %%xmm7         \n\t"

#define UNPACK_SAMPLES(s)                              \
                " punpckl"#s" %%xmm0, %%xmm0    \n\t"  \
                " punpckh"#s" %%xmm1, %%xmm1    \n\t"  \
                " punpckl"#s" %%xmm2, %%xmm2    \n\t"  \
                " punpckh"#s" %%xmm3, %%xmm3    \n\t"  \
                " punpckl"#s" %%xmm4, %%xmm4    \n\t"  \
                " punpckh"#s" %%xmm5, %%xmm5    \n\t"  \
                " punpckl"#s" %%xmm6, %%xmm6    \n\t"  \
                " punpckh"#s" %%xmm7, %%xmm7    \n\t"  \

#define STORE_SAMPLES                                  \
                " movdqu %%xmm0, (%0)           \n\t"  \
                " movdqu %%xmm1, 16(%0)         \n\t"  \
                " movdqu %%xmm2, 32(%0)         \n\t"  \
                " movdqu %%xmm3, 48(%0)         \n\t"  \
                " movdqu %%xmm4, 64(%0)         \n\t"  \
                " movdqu %%xmm5, 80(%0)         \n\t"  \
                " movdqu %%xmm6, 96(%0)         \n\t"  \
                " movdqu %%xmm7, 112(%0)        \n\t"  \
                " add $64, %1                   \n\t"  \
                " add $128, %0                  \n\t"

#define HANDLE_SINGLE(s)                               \
                " movd (%1), %%mm0              \n\t"  \
                " movq %%mm0, %%mm1             \n\t"  \
                " punpckl"#s" %%mm0, %%mm0      \n\t"  \
                " movq %%mm0, (%0)              \n\t"  \
                " add $4, %1                    \n\t"  \
                " add $8, %0                    \n\t"

#define MONO_TO_STEREO(s)                               \
                " mov %3, %2                    \n\t"   \
                " sar $4, %2                    \n\t"   \
                " cmp $0, %2                    \n\t"   \
                " je 2f                         \n\t"   \
                "1:                             \n\t"   \
                LOAD_SAMPLES                            \
                UNPACK_SAMPLES(s)                       \
                STORE_SAMPLES                           \
                " dec %2                        \n\t"   \
                " jne 1b                        \n\t"   \
                "2:                             \n\t"   \
                " mov %3, %2                    \n\t"   \
                " and $15, %2                   \n\t"   \
                " je 4f                         \n\t"   \
                "3:                             \n\t"   \
                HANDLE_SINGLE(s)                        \
                " dec %2                        \n\t"   \
                " jne 3b                        \n\t"   \
                "4:                             \n\t"   \
                " emms                          \n\t"

static void remap_mono_to_stereo_sse (pa_remap_t *m, void *dst, const void *src, unsigned n) {
    pa_reg_x86 temp;

    switch (*m->format) {
        case PA_SAMPLE_FLOAT32NE:
        {
            __asm__ __volatile__ (
                MONO_TO_STEREO(dq) /* do doubles to quads */
                : "+r" (dst), "+r" (src), "=&r" (temp)
                : "r" ((pa_reg_x86)n)
                : "cc"
            );
            break;
        }
        case PA_SAMPLE_S16NE:
        {
            __asm__ __volatile__ (
                MONO_TO_STEREO(wd) /* do words to doubles */
                : "+r" (dst), "+r" (src), "=&r" (temp)
                : "r" ((pa_reg_x86)n)
                : "cc"
            );
            break;
        }
        default:
            pa_assert_not_reached();
    }
}

/* set the function that will execute the remapping based on the matrices */
static void init_remap_sse (pa_remap_t *m) {
    unsigned n_oc, n_ic;

    n_oc = m->o_ss->channels;
    n_ic = m->i_ss->channels;

    /* find some common channel remappings, fall back to full matrix operation. */
    if (n_ic == 1 && n_oc == 2 &&
            m->map_table_f[0][0] >= 1.0 && m->map_table_f[1][0] >= 1.0) {
        m->do_remap = (pa_do_remap_func_t) remap_mono_to_stereo_sse;
        pa_log_info("Using SSE mono to stereo remapping");
    }
}

void pa_remap_func_init_sse (pa_cpu_x86_flag_t flags) {
#if defined (__i386__) || defined (__amd64__)
    pa_log_info("Initialising SSE optimized remappers.");

    pa_set_init_remap_func ((pa_init_remap_func_t) init_remap_sse);
#endif /* defined (__i386__) || defined (__amd64__) */
}

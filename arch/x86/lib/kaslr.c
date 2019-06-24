// SPDX-License-Identifier: GPL-2.0
/*
 * Entropy functions used on early boot for KASLR base and memory
 * randomization. The base randomization is done in the compressed
 * kernel and memory randomization is done early when the regular
 * kernel starts. This file is included in the compressed kernel and
 * normally linked in the regular.
 */
#include <asm/asm.h>
#include <asm/kaslr.h>
#include <asm/msr.h>
#include <asm/archrandom.h>
#include <asm/e820/api.h>
#include <asm/io.h>

/*
 * When built for the regular kernel, several functions need to be stubbed out
 * or changed to their regular kernel equivalent.
 */
#ifndef KASLR_COMPRESSED_BOOT
#include <asm/cpufeature.h>
#include <asm/setup.h>

#define debug_putstr(v) early_printk("%s", v)
#define has_cpuflag(f) boot_cpu_has(f)
#define get_boot_seed() kaslr_offset()
#endif

#define I8254_PORT_CONTROL	0x43
#define I8254_PORT_COUNTER0	0x40
#define I8254_CMD_READBACK	0xC0
#define I8254_SELECT_COUNTER0	0x02
#define I8254_STATUS_NOTREADY	0x40
static inline u16 i8254(void)
{
	u16 status, timer;

	do {
		outb(I8254_CMD_READBACK | I8254_SELECT_COUNTER0,
		     I8254_PORT_CONTROL);
		status = inb(I8254_PORT_COUNTER0);
		timer  = inb(I8254_PORT_COUNTER0);
		timer |= inb(I8254_PORT_COUNTER0) << 8;
	} while (status & I8254_STATUS_NOTREADY);

	return timer;
}

unsigned long kaslr_get_random_seed(const char *purpose)
{
#ifdef CONFIG_X86_64
	const unsigned long mix_const = 0x5d6008cbf3848dd3UL;
#else
	const unsigned long mix_const = 0x3f39e593UL;
#endif
	unsigned long raw, random = get_boot_seed();
	bool use_i8254 = true;

	if (purpose) {
		debug_putstr(purpose);
		debug_putstr(" KASLR using");
	}

	if (has_cpuflag(X86_FEATURE_RDRAND)) {
		if (purpose)
			debug_putstr(" RDRAND");
		if (rdrand_long(&raw)) {
			random ^= raw;
			use_i8254 = false;
		}
	}

	if (has_cpuflag(X86_FEATURE_TSC)) {
		if (purpose)
			debug_putstr(" RDTSC");
		raw = rdtsc();

		random ^= raw;
		use_i8254 = false;
	}

	if (use_i8254) {
		if (purpose)
			debug_putstr(" i8254");
		random ^= i8254();
	}

	/* Circular multiply for better bit diffusion */
	asm(_ASM_MUL "%3"
	    : "=a" (random), "=d" (raw)
	    : "a" (random), "rm" (mix_const));
	random += raw;

	if (purpose)
		debug_putstr("...\n");

	return random;
}

/*
 * 64bit variant of Bob Jenkins' public domain PRNG
 * 256 bits of internal state
 */
struct prng_state {
	u64 a, b, c, d;
};

static struct prng_state state;
static bool initialized;

#define rot(x, k) (((x)<<(k))|((x)>>(64-(k))))
static u64 prng_u64(struct prng_state *x) {
	u64 e = x->a - rot(x->b, 7);
	x->a = x->b ^ rot(x->c, 13);
	x->b = x->c + rot(x->d, 37);
	x->c = x->d + e;
	x->d = e + x->a;

	return x->d;
}

static void prng_init(struct prng_state *state) {
	int i;

	state->a = kaslr_get_random_seed(NULL);
	state->b = kaslr_get_random_seed(NULL);
	state->c = kaslr_get_random_seed(NULL);
	state->d = kaslr_get_random_seed(NULL);

	for (i=0; i < 30; ++i)
		(void)prng_u64(state);

	initialized = true;
}

unsigned long kaslr_get_prandom_long(void)
{
	if (!initialized)
		prng_init(&state);

	return prng_u64(&state);
}

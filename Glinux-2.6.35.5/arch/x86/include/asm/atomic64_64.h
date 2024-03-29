#ifndef _ASM_X86_ATOMIC64_64_H
#define _ASM_X86_ATOMIC64_64_H

#include <linux/types.h>
#include <asm/alternative.h>
#include <asm/cmpxchg.h>

/* The 64-bit atomic type */

#define ATOMIC64_INIT(i)	{ (i) }

/**
 * atomic64_read - read atomic64 variable
 * @v: pointer of type atomic64_t
 *
 * Atomically reads the value of @v.
 * Doesn't imply a read memory barrier.
 */
static inline long atomic64_read(const atomic64_t *v)
{
	return (*(volatile long *)&(v)->counter);
}

/**
 * atomic64_read_unchecked - read atomic64 variable
 * @v: pointer of type atomic64_unchecked_t
 *
 * Atomically reads the value of @v.
 * Doesn't imply a read memory barrier.
 */
static inline long atomic64_read_unchecked(const atomic64_unchecked_t *v)
{
	return v->counter;
}

/**
 * atomic64_set - set atomic64 variable
 * @v: pointer to type atomic64_t
 * @i: required value
 *
 * Atomically sets the value of @v to @i.
 */
static inline void atomic64_set(atomic64_t *v, long i)
{
	v->counter = i;
}

/**
 * atomic64_set_unchecked - set atomic64 variable
 * @v: pointer to type atomic64_unchecked_t
 * @i: required value
 *
 * Atomically sets the value of @v to @i.
 */
static inline void atomic64_set_unchecked(atomic64_unchecked_t *v, long i)
{
	v->counter = i;
}

/**
 * atomic64_add - add integer to atomic64 variable
 * @i: integer value to add
 * @v: pointer to type atomic64_t
 *
 * Atomically adds @i to @v.
 */
static inline void atomic64_add(long i, atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "addq %1,%0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     LOCK_PREFIX "subq %1,%0\n"
		     "int $4\n0:\n"
		     _ASM_EXTABLE(0b, 0b)
#endif

		     : "=m" (v->counter)
		     : "er" (i), "m" (v->counter));
}

/**
 * atomic64_add_unchecked - add integer to atomic64 variable
 * @i: integer value to add
 * @v: pointer to type atomic64_unchecked_t
 *
 * Atomically adds @i to @v.
 */
static inline void atomic64_add_unchecked(long i, atomic64_unchecked_t *v)
{
	asm volatile(LOCK_PREFIX "addq %1,%0"
		     : "=m" (v->counter)
		     : "er" (i), "m" (v->counter));
}

/**
 * atomic64_sub - subtract the atomic64 variable
 * @i: integer value to subtract
 * @v: pointer to type atomic64_t
 *
 * Atomically subtracts @i from @v.
 */
static inline void atomic64_sub(long i, atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "subq %1,%0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     LOCK_PREFIX "addq %1,%0\n"
		     "int $4\n0:\n"
		     _ASM_EXTABLE(0b, 0b)
#endif

		     : "=m" (v->counter)
		     : "er" (i), "m" (v->counter));
}

/**
 * atomic64_sub_and_test - subtract value from variable and test result
 * @i: integer value to subtract
 * @v: pointer to type atomic64_t
 *
 * Atomically subtracts @i from @v and returns
 * true if the result is zero, or false for all
 * other cases.
 */
static inline int atomic64_sub_and_test(long i, atomic64_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "subq %2,%0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     LOCK_PREFIX "addq %2,%0\n"
		     "int $4\n0:\n"
		     _ASM_EXTABLE(0b, 0b)
#endif

		     "sete %1\n"
		     : "=m" (v->counter), "=qm" (c)
		     : "er" (i), "m" (v->counter) : "memory");
	return c;
}

/**
 * atomic64_inc - increment atomic64 variable
 * @v: pointer to type atomic64_t
 *
 * Atomically increments @v by 1.
 */
static inline void atomic64_inc(atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "incq %0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     "int $4\n0:\n"
		     ".pushsection .fixup,\"ax\"\n"
		     "1:\n"
		     LOCK_PREFIX "decq %0\n"
		     "jmp 0b\n"
		     ".popsection\n"
		     _ASM_EXTABLE(0b, 1b)
#endif

		     : "=m" (v->counter)
		     : "m" (v->counter));
}

/**
 * atomic64_inc_unchecked - increment atomic64 variable
 * @v: pointer to type atomic64_unchecked_t
 *
 * Atomically increments @v by 1.
 */
static inline void atomic64_inc_unchecked(atomic64_unchecked_t *v)
{
	asm volatile(LOCK_PREFIX "incq %0"
		     : "=m" (v->counter)
		     : "m" (v->counter));
}

/**
 * atomic64_dec - decrement atomic64 variable
 * @v: pointer to type atomic64_t
 *
 * Atomically decrements @v by 1.
 */
static inline void atomic64_dec(atomic64_t *v)
{
	asm volatile(LOCK_PREFIX "decq %0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     "int $4\n0:\n"
		     ".pushsection .fixup,\"ax\"\n"
		     "1: \n"
		     LOCK_PREFIX "incq %0\n"
		     "jmp 0b\n"
		     ".popsection\n"
		     _ASM_EXTABLE(0b, 1b)
#endif

		     : "=m" (v->counter)
		     : "m" (v->counter));
}

/**
 * atomic64_dec_unchecked - decrement atomic64 variable
 * @v: pointer to type atomic64_t
 *
 * Atomically decrements @v by 1.
 */
static inline void atomic64_dec_unchecked(atomic64_unchecked_t *v)
{
	asm volatile(LOCK_PREFIX "decq %0\n"
		     : "=m" (v->counter)
		     : "m" (v->counter));
}

/**
 * atomic64_dec_and_test - decrement and test
 * @v: pointer to type atomic64_t
 *
 * Atomically decrements @v by 1 and
 * returns true if the result is 0, or false for all other
 * cases.
 */
static inline int atomic64_dec_and_test(atomic64_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "decq %0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     "int $4\n0:\n"
		     ".pushsection .fixup,\"ax\"\n"
		     "1: \n"
		     LOCK_PREFIX "incq %0\n"
		     "jmp 0b\n"
		     ".popsection\n"
		     _ASM_EXTABLE(0b, 1b)
#endif

		     "sete %1\n"
		     : "=m" (v->counter), "=qm" (c)
		     : "m" (v->counter) : "memory");
	return c != 0;
}

/**
 * atomic64_inc_and_test - increment and test
 * @v: pointer to type atomic64_t
 *
 * Atomically increments @v by 1
 * and returns true if the result is zero, or false for all
 * other cases.
 */
static inline int atomic64_inc_and_test(atomic64_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "incq %0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     "int $4\n0:\n"
		     ".pushsection .fixup,\"ax\"\n"
		     "1: \n"
		     LOCK_PREFIX "decq %0\n"
		     "jmp 0b\n"
		     ".popsection\n"
		     _ASM_EXTABLE(0b, 1b)
#endif

		     "sete %1\n"
		     : "=m" (v->counter), "=qm" (c)
		     : "m" (v->counter) : "memory");
	return c != 0;
}

/**
 * atomic64_add_negative - add and test if negative
 * @i: integer value to add
 * @v: pointer to type atomic64_t
 *
 * Atomically adds @i to @v and returns true
 * if the result is negative, or false when
 * result is greater than or equal to zero.
 */
static inline int atomic64_add_negative(long i, atomic64_t *v)
{
	unsigned char c;

	asm volatile(LOCK_PREFIX "addq %2,%0\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     LOCK_PREFIX "subq %2,%0\n"
		     "int $4\n0:\n"
		     _ASM_EXTABLE(0b, 0b)
#endif

		     "sets %1\n"
		     : "=m" (v->counter), "=qm" (c)
		     : "er" (i), "m" (v->counter) : "memory");
	return c;
}

/**
 * atomic64_add_return - add and return
 * @i: integer value to add
 * @v: pointer to type atomic64_t
 *
 * Atomically adds @i to @v and returns @i + @v
 */
static inline long atomic64_add_return(long i, atomic64_t *v)
{
	long __i = i;
	asm volatile(LOCK_PREFIX "xaddq %0, %1\n"

#ifdef CONFIG_PAX_REFCOUNT
		     "jno 0f\n"
		     "movq %0, %1\n"
		     "int $4\n0:\n"
		     _ASM_EXTABLE(0b, 0b)
#endif

		     : "+r" (i), "+m" (v->counter)
		     : : "memory");
	return i + __i;
}

/**
 * atomic64_add_return_unchecked - add and return
 * @i: integer value to add
 * @v: pointer to type atomic64_unchecked_t
 *
 * Atomically adds @i to @v and returns @i + @v
 */
static inline long atomic64_add_return_unchecked(long i, atomic64_unchecked_t *v)
{
	long __i = i;
	asm volatile(LOCK_PREFIX "xaddq %0, %1"
		     : "+r" (i), "+m" (v->counter)
		     : : "memory");
	return i + __i;
}

static inline long atomic64_sub_return(long i, atomic64_t *v)
{
	return atomic64_add_return(-i, v);
}

#define atomic64_inc_return(v)  (atomic64_add_return(1, (v)))
static inline long atomic64_inc_return_unchecked(atomic64_unchecked_t *v)
{
	return atomic64_add_return_unchecked(1, v);
}
#define atomic64_dec_return(v)  (atomic64_sub_return(1, (v)))

static inline long atomic64_cmpxchg(atomic64_t *v, long old, long new)
{
	return cmpxchg(&v->counter, old, new);
}

static inline long atomic64_xchg(atomic64_t *v, long new)
{
	return xchg(&v->counter, new);
}

/**
 * atomic64_add_unless - add unless the number is a given value
 * @v: pointer of type atomic64_t
 * @a: the amount to add to v...
 * @u: ...unless v is equal to u.
 *
 * Atomically adds @a to @v, so long as it was not @u.
 * Returns non-zero if @v was not @u, and zero otherwise.
 */
static inline int atomic64_add_unless(atomic64_t *v, long a, long u)
{
	long c, old, new;
	c = atomic64_read(v);
	for (;;) {
		if (unlikely(c == u))
			break;

		asm volatile("add %2,%0\n"

#ifdef CONFIG_PAX_REFCOUNT
			     "jno 0f\n"
			     "int $4\n0:\n"
			     _ASM_EXTABLE(0b, 0b)
#endif

			     : "=r" (new)
			     : "0" (c), "ir" (a));

		old = atomic64_cmpxchg(v, c, new);
		if (likely(old == c))
			break;
		c = old;
	}
	return c != u;
}

#define atomic64_inc_not_zero(v) atomic64_add_unless((v), 1, 0)

/*
 * atomic64_dec_if_positive - decrement by 1 if old value positive
 * @v: pointer of type atomic_t
 *
 * The function returns the old value of *v minus 1, even if
 * the atomic variable, v, was not decremented.
 */
static inline long atomic64_dec_if_positive(atomic64_t *v)
{
	long c, old, dec;
	c = atomic64_read(v);
	for (;;) {
		dec = c - 1;
		if (unlikely(dec < 0))
			break;
		old = atomic64_cmpxchg((v), c, dec);
		if (likely(old == c))
			break;
		c = old;
	}
	return dec;
}

#endif /* _ASM_X86_ATOMIC64_64_H */

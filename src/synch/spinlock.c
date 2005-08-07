/*
 * Copyright (C) 2001-2004 Jakub Jermar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <arch.h>

#include <arch/atomic.h>
#include <arch/barrier.h>
#include <synch/spinlock.h>

#ifdef __SMP__

void spinlock_initialize(spinlock_t *sl)
{
	sl->val = 0;
}

#ifdef DEBUG_SPINLOCK
void spinlock_lock(spinlock_t *sl)
{
	int i = 0;
	__address caller = ((__u32 *) &sl)[-1];

	while (test_and_set(&sl->val)) {
		if (i++ > 300000) {
			printf("cpu%d: looping on spinlock %X, caller=%X\n", CPU->id, sl, caller);
			i = 0;
		}
	}
	CS_ENTER_BARRIER();

}
#else
void spinlock_lock(spinlock_t *sl)
{
	/*
	 * Each architecture has its own efficient/recommended
	 * implementation of spinlock.
	 */
	spinlock_arch(&sl->val);
	CS_ENTER_BARRIER();
}
#endif

int spinlock_trylock(spinlock_t *sl)
{
	int rc;
	
	rc = !test_and_set(&sl->val);
	CS_ENTER_BARRIER();
	
	return rc;
}

void spinlock_unlock(spinlock_t *sl)
{
	CS_LEAVE_BARRIER();
	sl->val = 0;
}

#endif

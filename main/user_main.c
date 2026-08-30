/*
 * user_main.c -- the user-space application.  Runs in U-mode.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everything in this file executes at the lowest privilege the ESP32-P4 core
 * offers.  That imposes three rules, and they are why the code looks the way
 * it does:
 *
 *  1. NO LIBRARY CALLS.  printf() would reach the console driver, which takes
 *     FreeRTOS locks, which execute csr instructions -- illegal in U-mode.  So
 *     the number formatting below is hand-rolled.  It also keeps libgcc out of
 *     the picture; the only helpers the compiler could want here are divu and
 *     remu, which the P4 has in hardware.
 *
 *  2. CODE IN IRAM.  ESP-IDF's PMP entry 4 grants U-mode read+execute over
 *     [SOC_IRAM_LOW, _iram_text_end).  IRAM_ATTR puts these functions inside
 *     it.  The same functions in flash would fault on the instruction fetch.
 *
 *  3. DATA IN DRAM.  Entry 5 grants U-mode read+write over the DRAM that
 *     follows.  String constants are therefore DRAM_ATTR rather than left in
 *     flash rodata: flash rodata does happen to be U-readable on this
 *     configuration, but relying on that would make the app depend on a PMP
 *     entry it has no reason to need.  Locals live on the user stack, which
 *     the kernel carves out of its user arena -- also DRAM.
 *
 * The one way out is u_syscall*(), and every externally visible thing this
 * application does goes through it.
 */

#include <stdint.h>

#include "esp_attr.h"

#include "syscall.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The application's timebase.  Both cadences below are whole multiples of it,
 * so a single sleeping loop covers both without a second thread.
 */

#define USER_TICK_MS 1000

#define USER_MSG_PERIOD 2 /* ticks -- "User y" every 2 s          */
#define SYSCALL_PERIOD 3  /* ticks -- "syscall z" every 3 s       */

#define USER_MSG_MAX 48

/****************************************************************************
 * Private Data
 ****************************************************************************/

static DRAM_ATTR const char g_user_prefix[] = "User ";
static DRAM_ATTR const char g_syscall_prefix[] = "syscall ";

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: u_append
 *
 * Description:
 *   Copy a NUL-terminated string into dst at pos.  Returns the new position.
 *   Hand-rolled rather than strcpy() for the reasons at the top of the file.
 *
 ****************************************************************************/
static IRAM_ATTR uint32_t u_append(char *dst, uint32_t pos, const char *src)
{
    while (*src != '\0')
    {
        dst[pos++] = *src++;
    }

    return pos;
}

/****************************************************************************
 * Name: u_append_u32
 *
 * Description:
 *   Append an unsigned decimal.  Digits come out least significant first, so
 *   they go via a small reversal buffer on the user stack.
 *
 ****************************************************************************/
static IRAM_ATTR uint32_t u_append_u32(char *dst, uint32_t pos, uint32_t val)
{
    char tmp[10];
    uint32_t n = 0;

    if (val == 0)
    {
        dst[pos++] = '0';
        return pos;
    }

    while (val != 0)
    {
        tmp[n++] = (char) ('0' + (val % 10));
        val /= 10;
    }

    while (n != 0)
    {
        dst[pos++] = tmp[--n];
    }

    return pos;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: user_main
 *
 * Description:
 *   The user application's entry point.  Never called: the kernel loads its
 *   address into the user context's pc and mrets to it, so the first
 *   instruction of this function is the first instruction ever executed in
 *   U-mode on this system.
 *
 *   It sleeps a tick at a time and acts on the two cadences it is responsible
 *   for.  The sleep is a syscall, so the kernel is free to schedule other
 *   tasks while U-mode is parked -- the user application is not a spin loop
 *   stealing a core.
 *
 ****************************************************************************/
void IRAM_ATTR user_main(void)
{
    char msg[USER_MSG_MAX];
    uint32_t tick = 0;
    uint32_t user_n = 1;
    uint32_t syscall_n = 1;
    uint32_t pos;

    for (;;)
    {
        u_syscall1(SYS_DELAY_MS, USER_TICK_MS);
        tick++;

        /* Every 2 s: the application's own output.  SYS_WRITE is the
         * counted-length console write -- U-mode's equivalent of writing to
         * stdout, since it cannot reach the console driver itself.
         */

        if ((tick % USER_MSG_PERIOD) == 0)
        {
            pos = u_append(msg, 0, g_user_prefix);
            pos = u_append_u32(msg, pos, user_n++);
            msg[pos++] = '\n';
            u_syscall2(SYS_WRITE, (uint32_t) (uintptr_t) msg, pos);
        }

        /* Every 3 s: hand a message to the kernel and let the kernel print it.
         * SYS_PUTS takes a NUL-terminated string in the user arena; the kernel
         * validates the pointer, copies the string out and emits it.
         */

        if ((tick % SYSCALL_PERIOD) == 0)
        {
            pos = u_append(msg, 0, g_syscall_prefix);
            pos = u_append_u32(msg, pos, syscall_n++);
            msg[pos] = '\0';
            u_syscall1(SYS_PUTS, (uint32_t) (uintptr_t) msg);
        }
    }
}

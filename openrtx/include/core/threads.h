/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef THREADS_H
#define THREADS_H

#include <stddef.h>

/**
 * Threads' stack sizes
 */
#ifndef __ZEPHYR__
#define UI_THREAD_STKSIZE 2048
#ifndef RTX_THREAD_STKSIZE
#define RTX_THREAD_STKSIZE 512
#endif
#define CODEC2_THREAD_STKSIZE 16384
#define AUDIO_THREAD_STKSIZE 512
#else
#define UI_THREAD_STKSIZE 2048
#ifndef RTX_THREAD_STKSIZE
#define RTX_THREAD_STKSIZE 1024
#endif
#define CODEC2_THREAD_STKSIZE 16384
#define AUDIO_THREAD_STKSIZE 512
#endif

/**
 * Thread priority levels, UNIX-like: lower level, higher thread priority
 */
#ifdef _MIOSIX
#define THREAD_PRIO_RT 0
#define THREAD_PRIO_HIGH 1
#define THREAD_PRIO_NORMAL 2
#define THREAD_PRIO_LOW 3
#endif

/**
 * Spawn all the threads for the various functionalities.
 */
void create_threads();

#endif /* THREADS_H */

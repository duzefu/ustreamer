/*****************************************************************************
#                                                                            #
#    uStreamer - Lightweight and fast MJPEG-HTTP streamer.                   #
#                                                                            #
#    Copyright (C) 2018-2024  Maxim Devaev <mdevaev@gmail.com>               #
#                                                                            #
#    This program is free software: you can redistribute it and/or modify    #
#    it under the terms of the GNU General Public License as published by    #
#    the Free Software Foundation, either version 3 of the License, or       #
#    (at your option) any later version.                                     #
#                                                                            #
#    This program is distributed in the hope that it will be useful,         #
#    but WITHOUT ANY WARRANTY; without even the implied warranty of          #
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           #
#    GNU General Public License for more details.                            #
#                                                                            #
#    You should have received a copy of the GNU General Public License       #
#    along with this program.  If not, see <https://www.gnu.org/licenses/>.  #
#                                                                            #
*****************************************************************************/


#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <locale.h> // Make C locale for strerror_l()
#include <errno.h>
#include <math.h>
#include <time.h>
#include <assert.h>

#include <sys/file.h>

#include "types.h"


#ifdef NDEBUG
#	error WTF dude? Asserts are good things!
#endif

#if CHAR_BIT != 8
#	error There are not 8 bits in a char!
#endif


#define RN "\r\n"

#define INLINE inline __attribute__((always_inline))

#define US_CALLOC(x_dest, x_nmemb)		assert(((x_dest) = calloc((x_nmemb), sizeof(*(x_dest)))) != NULL)
#define US_REALLOC(x_dest, x_nmemb)		assert(((x_dest) = realloc((x_dest), (x_nmemb) * sizeof(*(x_dest)))) != NULL)
#define US_DELETE(x_dest, x_free)		{ if (x_dest) { x_free(x_dest); x_dest = NULL; } }
#define US_CLOSE_FD(x_dest)				{ if (x_dest >= 0) { close(x_dest); x_dest = -1; } }
#define US_MEMSET_ZERO(x_obj)			memset(&(x_obj), 0, sizeof(x_obj))

#define US_SNPRINTF(x_dest, x_size, x_fmt, ...)	assert(snprintf((x_dest), (x_size), (x_fmt), ##__VA_ARGS__) > 0)
#define US_ASPRINTF(x_dest, x_fmt, ...)			assert(asprintf(&(x_dest), (x_fmt), ##__VA_ARGS__) > 0)

#define US_MIN(x_a, x_b) ({ \
		__typeof__(x_a) m_a = (x_a); \
		__typeof__(x_b) m_b = (x_b); \
		(m_a < m_b ? m_a : m_b); \
	})

#define US_MAX(x_a, x_b) ({ \
		__typeof__(x_a) m_a = (x_a); \
		__typeof__(x_b) m_b = (x_b); \
		(m_a > m_b ? m_a : m_b); \
	})

#define US_ONCE_FOR(x_once, x_value, ...) { \
		const int m_reported = (x_value); \
		if (m_reported != (x_once)) { \
			__VA_ARGS__; \
			(x_once) = m_reported; \
		} \
	}

#define US_ONCE(...) US_ONCE_FOR(once, __LINE__, ##__VA_ARGS__)


INLINE char *us_strdup(const char *str) {
	char *const new = strdup(str);
	assert(new != NULL);
	return new;
}

INLINE const char *us_bool_to_string(bool flag) {
	return (flag ? "true" : "false");
}

INLINE uz us_align_size(uz size, uz to) {
	return ((size + (to - 1)) & ~(to - 1));
}

INLINE sll us_floor_ms(ldf now) {
	return (sll)now - (now < (sll)now); // floor()
}

INLINE u32 us_triple_u32(u32 x) {
	// https://nullprogram.com/blog/2018/07/31/
	x ^= x >> 17;
	x *= UINT32_C(0xED5AD4BB);
	x ^= x >> 11;
	x *= UINT32_C(0xAC4C1B51);
	x ^= x >> 15;
	x *= UINT32_C(0x31848BAB);
	x ^= x >> 14;
	return x;
}

INLINE void us_get_now(clockid_t clk_id, time_t *sec, long *msec) {
	struct timespec ts;
	assert(!clock_gettime(clk_id, &ts));
	*sec = ts.tv_sec;
	*msec = round(ts.tv_nsec / 1.0e6);

	if (*msec > 999) {
		*sec += 1;
		*msec = 0;
	}
}

INLINE ldf us_get_now_monotonic(void) {
	time_t sec;
	long msec;
	us_get_now(CLOCK_MONOTONIC, &sec, &msec);
	return (ldf)sec + ((ldf)msec) / 1000;
}

INLINE u64 us_get_now_monotonic_u64(void) {
	struct timespec ts;
	assert(!clock_gettime(CLOCK_MONOTONIC, &ts));
	return (u64)(ts.tv_nsec / 1000) + (u64)ts.tv_sec * 1000000;
}

INLINE u64 us_get_now_id(void) {
	const u64 now = us_get_now_monotonic_u64();
	return (u64)us_triple_u32(now) | ((u64)us_triple_u32(now + 12345) << 32);
}

INLINE ldf us_get_now_real(void) {
	time_t sec;
	long msec;
	us_get_now(CLOCK_REALTIME, &sec, &msec);
	return (ldf)sec + ((ldf)msec) / 1000;
}

INLINE uint us_get_cores_available(void) {
	long cores_sysconf = sysconf(_SC_NPROCESSORS_ONLN);
	cores_sysconf = (cores_sysconf < 0 ? 0 : cores_sysconf);
	return US_MAX(US_MIN(cores_sysconf, 4), 1);
}

INLINE void us_ld_to_timespec(ldf ld, struct timespec *ts) {
	ts->tv_sec = (long)ld;
	ts->tv_nsec = (ld - ts->tv_sec) * 1000000000L;
	if (ts->tv_nsec > 999999999L) {
		ts->tv_sec += 1;
		ts->tv_nsec = 0;
	}
}

INLINE ldf us_timespec_to_ld(const struct timespec *ts) {
	return ts->tv_sec + ((ldf)ts->tv_nsec) / 1000000000;
}

INLINE int us_flock_timedwait_monotonic(int fd, ldf timeout) {
	// 计算截止时间戳
	const ldf deadline_ts = us_get_now_monotonic() + timeout;
	int retval = -1;

	// 循环尝试获取锁
	while (true) {
		retval = flock(fd, LOCK_EX | LOCK_NB);
		// 如果成功获取锁、遇到非阻塞错误或当前时间超过截止时间，则退出循环
		if (retval == 0 || errno != EWOULDBLOCK || us_get_now_monotonic() > deadline_ts) {
			break;
		}
		// 短暂休眠后重试
		if (usleep(1000) < 0) {
			break;
		}
	}
	return retval;
}

INLINE char *us_errno_to_string(int error) {
#	if (_POSIX_C_SOURCE >= 200112L) && !defined(_GNU_SOURCE) // XSI
	char buf[2048];
	const uz max_len = sizeof(buf) - 1;
	if (strerror_r(error, buf, max_len) != 0) {
		US_SNPRINTF(buf, max_len, "Errno = %d", error);
	}
	return us_strdup(buf);

#	elif defined(__GLIBC__) && defined(_GNU_SOURCE) // GNU
	char buf[2048];
	const uz max_len = sizeof(buf) - 1;
	return us_strdup(strerror_r(error, buf, max_len));

#	else // BSD
	locale_t locale = newlocale(LC_MESSAGES_MASK, "C", NULL);
	if (locale) {
		char *ptr = us_strdup(strerror_l(error, locale));
		freelocale(locale);
		return ptr;
	}
	return us_strdup("!!! newlocale() error !!!");
#	endif
}

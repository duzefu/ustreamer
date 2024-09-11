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


#include "memsink.h"

#include <stdatomic.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include <sys/file.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "types.h"
#include "errors.h"
#include "tools.h"
#include "logging.h"
#include "frame.h"
#include "memsinksh.h"


us_memsink_s *us_memsink_init_opened(
	const char *name, const char *obj, bool server,
	mode_t mode, bool rm, uint client_ttl, uint timeout) {

	us_memsink_s *sink;
	US_CALLOC(sink, 1);
	sink->name = name;
	sink->obj = obj;
	sink->server = server;
	sink->rm = rm;
	sink->client_ttl = client_ttl;
	sink->timeout = timeout;
	sink->fd = -1;
	atomic_init(&sink->has_clients, false);

	US_LOG_INFO("Using %s-sink: %s", name, obj);

	if ((sink->data_size = us_memsink_calculate_size(obj)) == 0) {
		US_LOG_ERROR("%s-sink: Invalid object suffix", name);
		goto error;
	}

	const mode_t mask = umask(0);
	sink->fd = shm_open(sink->obj, (server ? O_RDWR | O_CREAT : O_RDWR), mode);
	umask(mask);
	if (sink->fd == -1) {
		umask(mask);
		US_LOG_PERROR("%s-sink: Can't open shared memory", name);
		goto error;
	}

	if (sink->server && ftruncate(sink->fd, sizeof(us_memsink_shared_s) + sink->data_size) < 0) {
		US_LOG_PERROR("%s-sink: Can't truncate shared memory", name);
		goto error;
	}

	if ((sink->mem = us_memsink_shared_map(sink->fd, sink->data_size)) == NULL) {
		US_LOG_PERROR("%s-sink: Can't mmap shared memory", name);
		goto error;
	}
	return sink;

error:
	us_memsink_destroy(sink);
	return NULL;
}

void us_memsink_destroy(us_memsink_s *sink) {
	if (sink->mem != NULL) {
		if (us_memsink_shared_unmap(sink->mem, sink->data_size) < 0) {
			US_LOG_PERROR("%s-sink: Can't unmap shared memory", sink->name);
		}
	}
	if (sink->fd >= 0) {
		if (close(sink->fd) < 0) {
			US_LOG_PERROR("%s-sink: Can't close shared memory fd", sink->name);
		}
		if (sink->rm && shm_unlink(sink->obj) < 0) {
			if (errno != ENOENT) {
				US_LOG_PERROR("%s-sink: Can't remove shared memory", sink->name);
			}
		}
	}
	free(sink);
}

bool us_memsink_server_check(us_memsink_s *sink, const us_frame_s *frame) {
	// 如果 frame == NULL，则仅检查是否有客户端或是否需要初始化内存。

	assert(sink->server);

	if (sink->mem->magic != US_MEMSINK_MAGIC || sink->mem->version != US_MEMSINK_VERSION) {
		// 如果内存区域未初始化，则需要写入一些内容。
		// 不需要锁定，因为只有服务器会写入这些变量。
		return true;
	}

	const ldf unsafe_ts = sink->mem->last_client_ts;
	if (unsafe_ts != sink->unsafe_last_client_ts) {
		// 客户端在任何操作时都会将其时间戳写入 sink。
		// 我们在这里不加锁，只是检查这个数字是否与我们在前几次迭代中读取的相同。
		// 这个值不需要一致，即使我们在这里读取时与客户端写入时发生了内存竞争而读取了垃圾数据，
		// 我们仍然可以判断是否有客户端。
		// 如果数字发生变化，则肯定有客户端，不需要进一步检查。
		// 如果数字没有变化，则应加锁并检查是否需要写入内存以初始化帧。
		sink->unsafe_last_client_ts = unsafe_ts;
		atomic_store(&sink->has_clients, true);
		return true;
	}

	if (flock(sink->fd, LOCK_EX | LOCK_NB) < 0) {
		if (errno == EWOULDBLOCK) {
			// 有一个活跃的客户端，它现在正持有锁并从 sink 读取帧
			atomic_store(&sink->has_clients, true);
			return true;
		}
		US_LOG_PERROR("%s-sink: 无法锁定内存", sink->name);
		return false;
	}

	// 检查是否有活跃的客户端通过超时
	const bool has_clients = (sink->mem->last_client_ts + sink->client_ttl > us_get_now_monotonic());
	atomic_store(&sink->has_clients, has_clients);

	if (flock(sink->fd, LOCK_UN) < 0) {
		US_LOG_PERROR("%s-sink: 无法解锁内存", sink->name);
		return false;
	}
	if (has_clients) {
		return true;
	}
	if (frame != NULL && !US_FRAME_COMPARE_GEOMETRY(sink->mem, frame)) {
		// 如果帧的几何/格式发生变化，也需要立即写入 sink
		return true;
	}
	return false;
}

// 将帧数据放入内存sink的函数
int us_memsink_server_put(us_memsink_s *sink, const us_frame_s *frame, bool *key_requested) {
	assert(sink->server); // 确保是服务器端调用

	const ldf now = us_get_now_monotonic(); // 获取当前时间戳

	// 检查帧数据大小是否超过内存sink的容量
	if (frame->used > sink->data_size) {
		US_LOG_ERROR("%s-sink: Can't put frame: is too big (%zu > %zu)",
			sink->name, frame->used, sink->data_size);
		return 0;
	}

	// 尝试锁定内存区域，等待时间为1秒
	if (us_flock_timedwait_monotonic(sink->fd, 1) == 0) {
		US_LOG_VERBOSE("%s-sink: >>>>> Exposing new frame ...", sink->name);

		sink->mem->id = us_get_now_id(); // 更新帧ID
		if (sink->mem->key_requested && frame->key) {
			sink->mem->key_requested = false; // 如果请求关键帧且当前帧是关键帧，则清除请求
		}
		if (key_requested != NULL) { // 对于非H264 sink，不需要key_requested
			*key_requested = sink->mem->key_requested;
		}

		// 将帧数据复制到内存sink中
		memcpy(us_memsink_get_data(sink->mem), frame->data, frame->used);
		sink->mem->used = frame->used;
		US_FRAME_COPY_META(frame, sink->mem); // 复制帧元数据

		sink->mem->magic = US_MEMSINK_MAGIC; // 设置magic number以标识有效数据
		sink->mem->version = US_MEMSINK_VERSION; // 设置协议版本

		// 更新是否有客户端的标志
		atomic_store(&sink->has_clients, (sink->mem->last_client_ts + sink->client_ttl > us_get_now_monotonic()));

		// 解锁内存区域
		if (flock(sink->fd, LOCK_UN) < 0) {
			US_LOG_PERROR("%s-sink: Can't unlock memory", sink->name);
			return -1;
		}
		US_LOG_VERBOSE("%s-sink: Exposed new frame; full exposition time = %.3Lf",
			sink->name, us_get_now_monotonic() - now);

	} else if (errno == EWOULDBLOCK) { // 如果内存区域正忙
		US_LOG_VERBOSE("%s-sink: ===== Shared memory is busy now; frame skipped", sink->name);

	} else { // 其他锁定错误
		US_LOG_PERROR("%s-sink: Can't lock memory", sink->name);
		return -1;
	}
	return 0;
}

int us_memsink_client_get(us_memsink_s *sink, us_frame_s *frame, bool *key_requested, bool key_required) {
	assert(!sink->server); // Client only

	if (us_flock_timedwait_monotonic(sink->fd, sink->timeout) < 0) {
		if (errno == EWOULDBLOCK) {
			return US_ERROR_NO_DATA;
		}
		US_LOG_PERROR("%s-sink: Can't lock memory", sink->name);
		return -1;
	}

	int retval = 0;

	if (sink->mem->magic != US_MEMSINK_MAGIC) {
		retval = US_ERROR_NO_DATA; // Not updated
		goto done;
	}
	if (sink->mem->version != US_MEMSINK_VERSION) {
		US_LOG_ERROR("%s-sink: Protocol version mismatch: sink=%u, required=%u",
			sink->name, sink->mem->version, US_MEMSINK_VERSION);
		retval = -1;
		goto done;
	}

	// Let the sink know that the client is alive
	sink->mem->last_client_ts = us_get_now_monotonic();

	if (sink->mem->id == sink->last_readed_id) {
		retval = US_ERROR_NO_DATA; // Not updated
		goto done;
	}

	sink->last_readed_id = sink->mem->id;
	us_frame_set_data(frame, us_memsink_get_data(sink->mem), sink->mem->used);
	US_FRAME_COPY_META(sink->mem, frame);
	if (key_requested != NULL) { // We don't need it for non-H264 sinks
		*key_requested = sink->mem->key_requested;
	}
	if (key_required) {
		sink->mem->key_requested = true;
	}

done:
	if (flock(sink->fd, LOCK_UN) < 0) {
		US_LOG_PERROR("%s-sink: Can't unlock memory", sink->name);
		retval = -1;
	}
	return retval;
}

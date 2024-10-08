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


#include "encoder.h"

#include <stdlib.h>
#include <strings.h>
#include <assert.h>

#include <pthread.h>

#include "../libs/types.h"
#include "../libs/tools.h"
#include "../libs/array.h"
#include "../libs/threading.h"
#include "../libs/logging.h"
#include "../libs/frame.h"
#include "../libs/capture.h"

#include "workers.h"
#include "m2m.h"

#include "encoders/cpu/encoder.h"
#include "encoders/hw/encoder.h"


static const struct {
	const char *name;
	const us_encoder_type_e type; // cppcheck-suppress unusedStructMember
} _ENCODER_TYPES[] = {
	{"CPU",			US_ENCODER_TYPE_CPU},
	{"HW",			US_ENCODER_TYPE_HW},
	{"M2M-VIDEO",	US_ENCODER_TYPE_M2M_VIDEO},
	{"M2M-IMAGE",	US_ENCODER_TYPE_M2M_IMAGE},
	{"M2M-MJPEG",	US_ENCODER_TYPE_M2M_VIDEO},
	{"M2M-JPEG",	US_ENCODER_TYPE_M2M_IMAGE},
	{"RV1126-MJPEG",US_ENCODER_TYPE_RV1126_MJPEG},
	{"RV1126-H264",	US_ENCODER_TYPE_RV1126_H264},
	{"RV1126-H265",	US_ENCODER_TYPE_RV1126_H265},
	{"OMX",			US_ENCODER_TYPE_M2M_IMAGE},
	{"NOOP",		US_ENCODER_TYPE_CPU},
};


static void *_worker_job_init(void *v_enc);
static void _worker_job_destroy(void *v_job);
static bool _worker_run_job(us_worker_s *wr);


us_encoder_s *us_encoder_init(void) {
	us_encoder_runtime_s *run;
	US_CALLOC(run, 1);
	run->type = US_ENCODER_TYPE_RV1126_H264;
	run->quality = 0;
	US_MUTEX_INIT(run->mutex);

	us_encoder_s *enc;
	US_CALLOC(enc, 1);
	enc->type = run->type;
	enc->n_workers = us_get_cores_available();
	enc->run = run;
	return enc;
}

void us_encoder_destroy(us_encoder_s *enc) {
	us_encoder_runtime_s *const run = enc->run;
	if (run->m2ms != NULL) {
		for (uint index = 0; index < run->n_m2ms; ++index) {
			US_DELETE(run->m2ms[index], us_m2m_encoder_destroy);
		}
		free(run->m2ms);
	}
	US_MUTEX_DESTROY(run->mutex);
	free(run);
	free(enc);
}

int us_encoder_parse_type(const char *str) {
	US_ARRAY_ITERATE(_ENCODER_TYPES, 0, item, {
		if (!strcasecmp(item->name, str)) {
			return item->type;
		}
	});
	return -1;
}

const char *us_encoder_type_to_string(us_encoder_type_e type) {
	US_ARRAY_ITERATE(_ENCODER_TYPES, 0, item, {
		if (item->type == type) {
			return item->name;
		}
	});
	return _ENCODER_TYPES[0].name;
}

void us_encoder_open(us_encoder_s *enc, us_capture_s *cap) {
	us_encoder_runtime_s *const run = enc->run; // 获取编码器运行时信息
	us_capture_runtime_s *const cr = cap->run; // 获取捕获运行时信息

	assert(run->pool == NULL); // 确保线程池未初始化

	us_encoder_type_e type = enc->type; // 获取编码器类型
	uint quality = cap->jpeg_quality; // 获取JPEG质量
	uint n_workers = US_MIN(enc->n_workers, cr->n_bufs); // 计算工作线程数

	if (us_is_jpeg(cr->format) && type != US_ENCODER_TYPE_HW) {
		US_LOG_INFO("Switching to HW encoder: the input is (M)JPEG ..."); // 切换到硬件编码器
		type = US_ENCODER_TYPE_HW;
	}

	US_LOG_DEBUG("!!!!!!!!!!!!!type==%d ...", (type)); // 准备M2M编码器
	if (type == US_ENCODER_TYPE_HW) {
		if (us_is_jpeg(cr->format)) {
			quality = cr->jpeg_quality;
			n_workers = 1; // 硬件编码器只需要一个线程
		} else {
			US_LOG_INFO("Switching to CPU encoder: the input format is not (M)JPEG ..."); // 切换到CPU编码器
			type = US_ENCODER_TYPE_CPU;
			quality = cap->jpeg_quality;
		}

	} else if (type == US_ENCODER_TYPE_M2M_VIDEO || type == US_ENCODER_TYPE_M2M_IMAGE) {
		US_LOG_DEBUG("Preparing M2M-%s encoder ...", (type == US_ENCODER_TYPE_M2M_VIDEO ? "VIDEO" : "IMAGE")); // 准备M2M编码器
		if (run->m2ms == NULL) {
			US_CALLOC(run->m2ms, n_workers);
		}
		for (; run->n_m2ms < n_workers; ++run->n_m2ms) {
			// 从零开始，并在后续需要时进行进一步初始化
			char name[32];
			US_SNPRINTF(name, 31, "JPEG-%u", run->n_m2ms);
			if (type == US_ENCODER_TYPE_M2M_VIDEO) {
				run->m2ms[run->n_m2ms] = us_m2m_mjpeg_encoder_init(name, enc->m2m_path, quality);
			} else {
				run->m2ms[run->n_m2ms] = us_m2m_jpeg_encoder_init(name, enc->m2m_path, quality);
			}
		}
	} else if (type == US_ENCODER_TYPE_RV1126_H264 || type == US_ENCODER_TYPE_RV1126_H265 || type == US_ENCODER_TYPE_RV1126_MJPEG) {
		n_workers = 1; //1126不需要多个编码器
		// 这里应是在初始化单独编码某一帧的编码工具
		char name[64];
		switch (type) {
		case US_ENCODER_TYPE_RV1126_H264:
			sprintf(name, "rv1126_h264_encoder");
			break;
		case US_ENCODER_TYPE_RV1126_H265:
			sprintf(name, "rv1126_h265_encoder");
			break;
		case US_ENCODER_TYPE_RV1126_MJPEG:
			sprintf(name, "rv1126_mjpeg_encoder");
			break;
		default:
			break;
		}
		US_LOG_DEBUG("Preparing %s encoder ...", name); // 准备M2M编码器
		// 这里的代码不需要了,因为RV1126编码器是单线程的,不需要指定好几个线程,让他们使用不同的编码器来处理编码
		// 对应的代码在_worker_run_job
		// if (run->rv1126_encoder == NULL) {
		// 	US_CALLOC(run->rv1126_encoder, n_workers);
		// }
		// for (; run->n_rv1126_encoder < n_workers; ++run->n_rv1126_encoder) {
		// 	char name[32];
		// 	US_SNPRINTF(name, 31, "rv1126-%u", run->n_rv1126_encoder);
		// 	run->rv1126_encoder[run->n_rv1126_encoder] = us_rv1126_encoder_init(type, "/dev/video0");
		// }
	}

	if (quality == 0) {
		US_LOG_INFO("Using JPEG quality: encoder default"); // 使用默认JPEG质量
	} else {
		US_LOG_INFO("Using JPEG quality: %u%%", quality); // 使用指定JPEG质量
	}

	US_MUTEX_LOCK(run->mutex); // 加锁
	run->type = type;
	run->quality = quality;
	US_MUTEX_UNLOCK(run->mutex); // 解锁

	const ldf desired_interval = (
		cap->desired_fps > 0 && (cap->desired_fps < cap->run->hw_fps || cap->run->hw_fps == 0)
		? (ldf)1 / cap->desired_fps
		: 0
	); // 计算期望的帧间隔

	enc->run->pool = us_workers_pool_init(
		"JPEG", "jw", n_workers, desired_interval,
		_worker_job_init, (void*)enc,
		_worker_job_destroy,
		_worker_run_job); // 初始化工作线程池
}

void us_encoder_close(us_encoder_s *enc) {
	assert(enc->run->pool != NULL);
	US_DELETE(enc->run->pool, us_workers_pool_destroy);
}

void us_encoder_get_runtime_params(us_encoder_s *enc, us_encoder_type_e *type, uint *quality) {
	us_encoder_runtime_s *const run = enc->run;
	US_MUTEX_LOCK(run->mutex);
	*type = run->type;
	*quality = run->quality;
	US_MUTEX_UNLOCK(run->mutex);
}

static void *_worker_job_init(void *v_enc) {
	us_encoder_job_s *job;
	US_CALLOC(job, 1);
	job->enc = (us_encoder_s*)v_enc;
	job->dest = us_frame_init();
	return (void*)job;
}

static void _worker_job_destroy(void *v_job) {
	us_encoder_job_s *job = v_job;
	us_frame_destroy(job->dest);
	free(job);
}

static bool _worker_run_job(us_worker_s *wr) {
	us_encoder_job_s *const job = wr->job;
	us_encoder_runtime_s *const run = job->enc->run;
	const us_frame_s *const src = &job->hw->raw;
	us_frame_s *const dest = job->dest;

	if (run->type == US_ENCODER_TYPE_CPU) {
		US_LOG_VERBOSE("Compressing JPEG using CPU: worker=%s, buffer=%u",
			wr->name, job->hw->buf.index);
		us_cpu_encoder_compress(src, dest, run->quality);

	} else if (run->type == US_ENCODER_TYPE_HW) {
		US_LOG_VERBOSE("Compressing JPEG using HW (just copying): worker=%s, buffer=%u",
			wr->name, job->hw->buf.index);
		us_hw_encoder_compress(src, dest);

	} else if (run->type == US_ENCODER_TYPE_M2M_VIDEO || run->type == US_ENCODER_TYPE_M2M_IMAGE) {
		US_LOG_VERBOSE("Compressing JPEG using M2M-%s: worker=%s, buffer=%u",
			(run->type == US_ENCODER_TYPE_M2M_VIDEO ? "VIDEO" : "IMAGE"), wr->name, job->hw->buf.index);
		if (us_m2m_encoder_compress(run->m2ms[wr->number], src, dest, false) < 0) {
			goto error;
		}
	} else if (run->type == US_ENCODER_TYPE_RV1126_MJPEG || run->type == US_ENCODER_TYPE_RV1126_H264 || run->type == US_ENCODER_TYPE_RV1126_H265) {
		US_LOG_VERBOSE("Compressing JPEG using rv1126-%s: worker=%s, buffer=%u",
			(run->type == US_ENCODER_TYPE_RV1126_MJPEG ? "MJPEG" : (run->type == US_ENCODER_TYPE_RV1126_H264 ? US_ENCODER_TYPE_RV1126_H264 :US_ENCODER_TYPE_RV1126_H265)), wr->name, job->hw->buf.index);
		// TODO 等RK给我回复之后再看这里怎么处理单一图像
		// if (us_rv1126_encoder_compress(src, dest, false) < 0) {
		// 	goto error;
		// }

	} else {
		assert(0 && "Unknown encoder type");
	}

	US_LOG_VERBOSE("Compressed new JPEG: size=%zu, time=%0.3Lf, worker=%s, buffer=%u",
		job->dest->used,
		job->dest->encode_end_ts - job->dest->encode_begin_ts,
		wr->name,
		job->hw->buf.index);
	return true;

error:
	US_LOG_ERROR("Compression failed: worker=%s, buffer=%u", wr->name, job->hw->buf.index);
	return false;
}

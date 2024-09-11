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

#include "../libs/types.h"
#include "../libs/frame.h"
#include "../libs/fpsi.h"


typedef struct {
	u8	*data;
	uz	allocated;
} us_rv1126_buffer_s;

typedef struct {
	int				fd;
	uint			fps_limit;
	us_rv1126_buffer_s	*input_bufs;
	uint			n_input_bufs;
	us_rv1126_buffer_s	*output_bufs;
	uint			n_output_bufs;

	uint	p_width;
	uint	p_height;
	uint	p_input_format;
	uint	p_stride;

	bool	ready;
	int		last_online;
	ldf		last_encode_ts;
} us_rv1126_encoder_runtime_s;



enum RV1126_ENCODER_FORMAT{
	RV1126_ENCODER_FORMAT_H264=0,
	RV1126_ENCODER_FORMAT_H265,
	RV1126_ENCODER_FORMAT_MJPEG,
	RV1126_ENCODER_FORMAT_JPEG,
	RV1126_ENCODER_FORMAT_MAX,
};

enum RV1126_RAW_FORMAT{
	RV1126_RAW_FORMAT_YUYV422=0,
	RV1126_RAW_FORMAT_YUYV420,
	RV1126_RAW_FORMAT_MAX,
};

typedef struct {
	char	*name;
	char	*dev_path;
	enum RV1126_ENCODER_FORMAT	output_format;
	uint	bitrate;
	uint	gop;
	uint	quality;
	bool	allow_dma;

	us_rv1126_encoder_runtime_s *run;
} us_rv1126_encoder_s;

us_rv1126_encoder_s* us_rv1126_encoder_init(enum RV1126_ENCODER_FORMAT output_format, const char* capture_device);
int us_rv1126_encoder_deinit(us_rv1126_encoder_s* enc);

int us_rv1126_encoder_compress(us_rv1126_encoder_s *enc, const us_frame_s *src, us_frame_s *dest, bool force_key);
int us_rv1126_get_meta(us_fpsi_meta_s* meta);
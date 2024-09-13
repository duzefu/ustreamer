#include "rv1126.h"
// #include "sample_common.h"
#include "rkmedia/rkmedia_api.h"
#include "rkmedia/rkmedia_venc.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <assert.h>

#include <sys/mman.h>

#include <linux/videodev2.h>

#include "../libs/types.h"
#include "../libs/tools.h"
#include "../libs/logging.h"
#include "../libs/frame.h"
#include "../libs/xioctl.h"

RK_U32 vi_width = 1920;
RK_U32 vi_height = 1080;
RK_U32 venc_width = 1920;
RK_U32 venc_height = 1080;
RK_U32 vi_buffer_num = 3; // 增加可能会导致延迟增高,降低在cpu不行(或者不同帧处理时间有明显差异,典型情况就是跑模型)的情况下会丢帧
RK_U32 Media_framerate = 60;
RK_U32 Media_bitrate = 6*1024*1024*8;   // 3MBps (每秒产生3M数据)
RK_S32 gop = 30;
#define AV_BLOCK_TIME                       1000         // 1000毫秒
static const VI_PIPE ViPipe = 0;
static const VI_CHN ViChn = 0;
static const VENC_CHN VencChn = 0;
CODEC_TYPE_E  enc_type = RK_CODEC_TYPE_MJPEG;
MPP_CHN_S stSrcChn = {.enModId = RK_ID_VI , .s32ChnId = ViChn , .s32DevId = ViPipe};
MPP_CHN_S stDestChn = {.enModId = RK_ID_VENC , .s32ChnId = VencChn , .s32DevId = 0 };


// for test
bool g_stream_save = true;
char* g_stream_save_path = "/tmp/test.mjpeg";
FILE* g_stream_save_fd = NULL;


/**
 * 修改编码器参数
 *
 * @param bitrate 新的比特率（bps）
 * @return 成功返回 true，失败返回 false
 */
int rv1126_encoder_change_bitrate(int bitrate){

    VENC_CHN_STATUS_S status;

    Media_bitrate = bitrate;

    // use query to check if the venc is running
    if (RK_MPI_VENC_QueryStatus(VencChn,&status) != RK_SUCCESS){
        // venc not running, only change the bitrate
        return true;
    }
    // get para then change then set
    // TODO: RK文档里面有这个函数,但是SDK里面找不到任何定义
    // VENC_CHN_PARAM_S param;
    // if (RK_MPI_VENC_GetVencChnParam(VencChn,&param) == RK_SUCCESS){
    //     param.stFrameRate.s32DstFrmRate = Media_bitrate;
    //     if (RK_MPI_VENC_SetVencChnParam(VencChn,&param) == RK_SUCCESS){
    //         US_LOG_DEBUG("RV1126:SetVencChnParam bitrate = %d",Media_bitrate);
    //         return true;
    //     }else{
    //         US_LOG_ERROR("RV1126:SetVencChnParam failed");
    //         return false;
    //     }
    // }else
    //     US_LOG_ERROR("RV1126:GetVencChnParam failed");
    return false;
}

int us_rv1126_encoder_deinit(us_rv1126_encoder_s* enc){
    int ret = 0;
    if (g_stream_save){
        if(g_stream_save_fd){
            fclose(g_stream_save_fd);
            g_stream_save_fd = NULL;
        }
    }
	ret = RK_MPI_SYS_UnBind(&stSrcChn,&stDestChn);
	if (ret != RK_SUCCESS)
        US_LOG_ERROR("<<-ERR->>[%s:%d]:RK_MPI_SYS_UnBind[0]---ret=[%d]", __FUNCTION__, __LINE__, ret);
	US_LOG_INFO("RV1126:unbind vi[%d] and venc[%d]",stSrcChn.s32ChnId,stDestChn.s32ChnId);

	ret = RK_MPI_VENC_DestroyChn(VencChn);
	if (ret != RK_SUCCESS)
        US_LOG_ERROR("<<-ERR->>[%s:%d]:RK_MPI_VENC_DestroyChn[%d]---ret=[%d]", __FUNCTION__, __LINE__, ret,VencChn);
	US_LOG_INFO("RV1126:destory venc[%d]",VencChn);

	ret = RK_MPI_VI_DisableChn(ViPipe, ViChn);
	if (ret != RK_SUCCESS)
        US_LOG_ERROR("<<-ERR->>[%s:%d]:RK_MPI_VI_DisableChn[%d]---ret=[%d]", __FUNCTION__, __LINE__, ret,ViChn);
	US_LOG_INFO("RV1126:destroy vi %d %d",ViPipe,ViChn);

	return true;
}


us_rv1126_encoder_s* us_rv1126_encoder_init(enum RV1126_ENCODER_FORMAT output_format, const char* capture_device){
    int ret;
    us_rv1126_encoder_s* encoder = (us_rv1126_encoder_s*)malloc(sizeof(us_rv1126_encoder_s));
    encoder->allow_dma = false;
    encoder->bitrate = Media_bitrate;
    encoder->dev_path = capture_device;
    encoder->gop = 30;
    encoder->name = "rv1126";
    encoder->output_format = output_format;
    encoder->quality = 50;

    if (g_stream_save){
        g_stream_save_fd = fopen(g_stream_save_path,"w");
    }

    switch (output_format){
        case RV1126_ENCODER_FORMAT_H264:
            enc_type = RK_CODEC_TYPE_H264;
            break;
        case RV1126_ENCODER_FORMAT_MJPEG:
            enc_type = RK_CODEC_TYPE_MJPEG;
            break;
        case RV1126_ENCODER_FORMAT_H265:
            enc_type = RK_CODEC_TYPE_H265;
            break;
        default:
            break;
    }

	/************************************* VI -> VENC *********************************************/
	RK_MPI_SYS_Init();
	VI_CHN_ATTR_S vi_chn_attr;
	vi_chn_attr.pcVideoNode = capture_device == NULL ? "/dev/video0":capture_device;
	vi_chn_attr.u32BufCnt = vi_buffer_num;
	vi_chn_attr.u32Width = vi_width;
	vi_chn_attr.u32Height = vi_height;
	vi_chn_attr.enPixFmt = IMAGE_TYPE_YUV422P; //TODO: 根据input_format设置,也可能未必需要,因为可以配置6911输出固定的YUYV422
	vi_chn_attr.enWorkMode = VI_WORK_MODE_NORMAL;
    vi_chn_attr.enBufType = VI_CHN_BUF_TYPE_MMAP; // 这个好像是必须的,默认的DMA好像不支持
	ret = RK_MPI_VI_SetChnAttr(ViPipe, ViChn, &vi_chn_attr);
	ret |= RK_MPI_VI_EnableChn(ViPipe, ViChn);
	if (ret)
	{
		US_LOG_ERROR("Create vi[0] failed! ret=%d\n", ret);
		return NULL;
	}

	VENC_CHN_ATTR_S venc_chn_attr;
	memset(&venc_chn_attr, 0, sizeof(venc_chn_attr));
	venc_chn_attr.stVencAttr.enType = RK_CODEC_TYPE_H264;
	venc_chn_attr.stVencAttr.imageType = IMAGE_TYPE_YUYV422;
	venc_chn_attr.stVencAttr.u32PicWidth = vi_width;
	venc_chn_attr.stVencAttr.u32PicHeight = vi_height;
	venc_chn_attr.stVencAttr.u32VirWidth = venc_width;
	venc_chn_attr.stVencAttr.u32VirHeight = venc_height;
	venc_chn_attr.stVencAttr.enRotation = 0;

	if(enc_type == RK_CODEC_TYPE_H264)
	{
        venc_chn_attr.stVencAttr.u32Profile = 66; //use baseline because we want lowest latencency

		venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H264CBR;
		venc_chn_attr.stRcAttr.stH264Cbr.u32Gop = 30;
		venc_chn_attr.stRcAttr.stH264Cbr.u32BitRate = Media_bitrate;
		venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH264Cbr.fr32DstFrameRateNum = Media_framerate;
		venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH264Cbr.u32SrcFrameRateNum = 60;
	}
	else if (enc_type == RK_CODEC_TYPE_H265)
	{
		venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_H265CBR;
		venc_chn_attr.stRcAttr.stH265Cbr.u32Gop = 30;
		venc_chn_attr.stRcAttr.stH265Cbr.u32BitRate = Media_bitrate;
		venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH265Cbr.fr32DstFrameRateNum = Media_framerate;
		venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateDen = 1;
		venc_chn_attr.stRcAttr.stH265Cbr.u32SrcFrameRateNum = 60;
	}else if (enc_type == RK_CODEC_TYPE_MJPEG){
        venc_chn_attr.stRcAttr.enRcMode = VENC_RC_MODE_MJPEGCBR;
		venc_chn_attr.stRcAttr.stMjpegCbr.u32BitRate = Media_bitrate;
        venc_chn_attr.stRcAttr.stMjpegCbr.fr32DstFrameRateDen = 1;
        venc_chn_attr.stRcAttr.stMjpegCbr.fr32DstFrameRateNum = Media_framerate;
        venc_chn_attr.stRcAttr.stMjpegCbr.u32SrcFrameRateDen = 1;
        venc_chn_attr.stRcAttr.stMjpegCbr.u32SrcFrameRateNum = 60;

    }
	ret = RK_MPI_VENC_CreateChn(VencChn, &venc_chn_attr);//venc_chn:0
	if (ret) {
		US_LOG_ERROR("RV1126: Create venc[%d] error! code:%d\n", VencChn,ret);
		return NULL;
	}


    // ret = RK_MPI_VI_StartStream(s32CamId, 0);
    // if (ret) {
    //     printf("ERROR: RK_MPI_VI_StartStream error! ret=%d\n", ret);
    //     return -1;
    // }


	// Bind VI[0] to VENC[0]
    MPP_CHN_S stSrcChn;
    stSrcChn.enModId = RK_ID_VI;
    stSrcChn.s32DevId = ViPipe;
    stSrcChn.s32ChnId = ViChn;
    MPP_CHN_S stDestChn;
    stDestChn.enModId = RK_ID_VENC;
    stDestChn.s32DevId = 0;
    stDestChn.s32ChnId = VencChn;
    ret = RK_MPI_SYS_Bind(&stSrcChn, &stDestChn);
    if (ret) {
        US_LOG_ERROR("RV1126: Bind VI[%d] and VENC[%d] error! ret=%d\n", ViChn,VencChn,ret);
        return NULL;
    }

	return encoder;
}


// get frame from venc
int us_rv1126_get_frame(us_frame_s* frame){
    MEDIA_BUFFER mb = RK_MPI_SYS_GetMediaBuffer(RK_ID_VENC, VencChn, AV_BLOCK_TIME);
    if (!mb){
        US_LOG_ERROR("get VENC data faile");
        return false;
    }
    // US_LOG_DEBUG("get frame size %d", RK_MPI_MB_GetSize(mb));

    // fill frame
    RK_S32 flag = RK_MPI_MB_GetFlag(mb);
    int frame_size = RK_MPI_MB_GetSize(mb);
    us_frame_set_data(frame, RK_MPI_MB_GetPtr(mb), frame_size);
    if (g_stream_save) fwrite(RK_MPI_MB_GetPtr(mb), 1, frame_size, g_stream_save_fd);
    RK_MPI_MB_ReleaseBuffer(mb);
    frame->encode_begin_ts = us_get_now_monotonic();
    switch (enc_type)
    {
    case RK_CODEC_TYPE_H265:
        // TODO: 标准v4l2里面没有定义H265的fourcc，这里不知道要怎么搞,也不确定是不是必须要有
        // frame->format = V4L2_PIX_FMT_H265;
        break;
    case RK_CODEC_TYPE_H264:
        frame->format = V4L2_PIX_FMT_H264;
        break;
    case RK_CODEC_TYPE_MJPEG:
        frame->format = V4L2_PIX_FMT_MJPEG;
        break;
    default:
        US_LOG_ERROR("Unknown encoder type %d", enc_type);
        break;
    }
	frame->stride = 0;
	frame->used = frame_size;
    frame->encode_end_ts = us_get_now_monotonic();
    frame->key = flag == VENC_NALU_ISLICE ? true : false; 
    frame->gop = gop;
    frame->online = true;
    return true;
}

int us_rv1126_get_meta(us_fpsi_meta_s* meta){
    meta->width = vi_width;
    meta->height = vi_height;
    meta->online = true;
}

#include <stdio.h>
#include <unistd.h>

#include <vencoder.h>
#include <veInterface.h>
#include <memoryAdapter.h>

#include "cam.h"
#include "util.h"
#include "conf.h"

//add
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static VideoEncoder *gVideoEnc = NULL;
static VencBaseConfig baseConfig;
static int g_width = G_WIDTH;
static int g_height = G_HEIGHT;
static int g_pix_fmt = G_CEDARC_PIX_FMT;
static int g_fps = G_FPS;

int h264_init(int width, int height, int fps) {

	g_width = width;
	g_height = height;
	g_fps = fps;

	VencH264Param h264Param = {
		.bEntropyCodingCABAC = 1,
		.nBitrate = 4 * 1024 * 1024,
		.nFramerate = g_fps,
		.nCodingMode = VENC_FRAME_CODING,
		.nMaxKeyInterval = 10,
		.sProfileLevel.nProfile = VENC_H264ProfileHigh,
		.sProfileLevel.nLevel = VENC_H264Level42,
		.sQPRange.nMinqp = 20,
		.sQPRange.nMaxqp = 40,
	};

	CLEAR(baseConfig);
	baseConfig.memops = MemAdapterGetOpsS();
	if (baseConfig.memops == NULL) {
		dlog(DLOG_CRIT "Error: MemAdapterGetOpsS() failed\n");
		return -1;
	}
	CdcMemOpen(baseConfig.memops);

	baseConfig.nInputWidth = g_width;
	baseConfig.nInputHeight = g_height;
	baseConfig.nStride = g_width;
	baseConfig.nDstWidth = g_width;
	baseConfig.nDstHeight = g_height;
	baseConfig.eInputFormat = g_pix_fmt;

	gVideoEnc = VideoEncCreate(VENC_CODEC_H264);
	if (gVideoEnc == NULL) {
		dlog(DLOG_CRIT "Error: VideoEncCreate() failed\n");
		return -1;
	}

	{
		VideoEncSetParameter(gVideoEnc, VENC_IndexParamH264Param, &h264Param);
		int value = 0;
		VideoEncSetParameter(gVideoEnc, VENC_IndexParamIfilter, &value);
		value = 0;
		VideoEncSetParameter(gVideoEnc, VENC_IndexParamRotation, &value);
		value = 0;
		VideoEncSetParameter(gVideoEnc, VENC_IndexParamFastEnc, &value);
		value = 0;
		VideoEncSetParameter(gVideoEnc, VENC_IndexParamSetPSkip, &value);
		VencCyclicIntraRefresh sIntraRefresh;
		sIntraRefresh.bEnable = 1;
    	sIntraRefresh.nBlockNumber = g_fps/2;
		VideoEncSetParameter(gVideoEnc, VENC_IndexParamH264CyclicIntraRefresh, &sIntraRefresh);
	}
	VideoEncInit(gVideoEnc, &baseConfig);

	dlog(DLOG_INFO "Info: h264 encocder init OK\n");

	return 0;
}

int h264_encode(unsigned char *addrPhyY, unsigned char *addrPhyC, int socket_descriptor, struct sockaddr_in address) {
	// Prepare buffers
	VencInputBuffer inputBuffer;
	VencOutputBuffer outputBuffer;
	int ret = 0;
	CLEAR(inputBuffer);
	CLEAR(outputBuffer);
	// Pass pre-allocated buffer (from V4L2) to CedarVE
	// No need to use AllocInputBuffer() and copy data unnecessarily
	inputBuffer.pAddrPhyY = addrPhyY;
	inputBuffer.pAddrPhyC = addrPhyC;

	AddOneInputBuffer(gVideoEnc, &inputBuffer);
	ret = VideoEncodeOneFrame(gVideoEnc);
	if (ret != VENC_RESULT_OK) {
		dlog("Error: VideoEncodeOneFrame() failed %d\n",ret);
		return -1;
	}

	// Mark buffer as used, and get output H.264 bitstream
	AlreadyUsedInputBuffer(gVideoEnc, &inputBuffer);
	GetOneBitstreamFrame(gVideoEnc, &outputBuffer);

	if (outputBuffer.nSize0 > 0){
		if(outputBuffer.nFlag == 1)
		{
			VencHeaderData sps_pps_data;
			// Write SPS, PPS NAL units
			VideoEncGetParameter(gVideoEnc, VENC_IndexParamH264SPSPPS, &sps_pps_data);
			ret = sendto(socket_descriptor, sps_pps_data.pBuffer, sps_pps_data.nLength, 0, (struct sockaddr *)&address, sizeof(address));
			if(ret < 0){
				dlog("Error: sendto sps_pps_data failed %d %d\n",ret,sps_pps_data.nLength);
			}
		}
		ret = sendto(socket_descriptor, outputBuffer.pData0, outputBuffer.nSize0, 0, (struct sockaddr *)&address, sizeof(address));
		if(ret < 0){
			("Error: sendto pData0 failed %d %d\n",ret,outputBuffer.nSize0);
		}
	}
		
	if (outputBuffer.nSize1 > 0){
		ret = sendto(socket_descriptor, outputBuffer.pData1, outputBuffer.nSize1, 0, (struct sockaddr *)&address, sizeof(address));
		if(ret < 0){
			dlog("Error: sendto pData1 failed %d %d\n",ret,outputBuffer.nSize1);
		}
	}

	FreeOneBitStreamFrame(gVideoEnc, &outputBuffer);
	return 0;
}

void h264_deinit() {
	if (baseConfig.memops) {
		CdcMemClose(baseConfig.memops);
		baseConfig.memops = NULL;
	}
	if (gVideoEnc) {
		ReleaseAllocInputBuffer(gVideoEnc);
		VideoEncDestroy(gVideoEnc);
		gVideoEnc = NULL;
	}
}

// rtmppublisher.cpp : 定义 DLL 应用程序的导出函数。
//

#include "stdafx.h"
#include "rtmppublisher.h"
#include <conio.h>

// 这是导出变量的一个示例
RTMPPUBLISHER_API int nrtmppublisher = 0;

// 这是导出函数的一个示例。
RTMPPUBLISHER_API int fnrtmppublisher(void)
{
	return 42;
}

// 这是已导出类的构造函数。
// 有关类定义的信息，请参阅 rtmppublisher.h
Crtmppublisher::Crtmppublisher()
{
	return;
}

int init_rtmp(AVCodecContext *pCodecCtx)
{
	AllocConsole();
	const char *out_filename;//rtmp://localhost/live/livestream
	out_filename = "rtmp://localhost/live/livestream";//输出 URL（Output URL）[RTMP]	

	avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", out_filename); //RTMP
	//avformat_alloc_output_context2(&ofmt_ctx, NULL, "mpegts", out_filename);//UDP
	if (!ofmt_ctx) {
		printf("Could not create output context\n");//AVERROR(EINVAL)
		return -1;
	}

	AVCodec *pCodec;
	pCodec = avcodec_find_encoder(CODEC_ID_H264);
	if (pCodec == NULL) return -1;

	out_stream = avformat_new_stream(ofmt_ctx, pCodec);
	if (!out_stream) {
		printf("Failed allocating output stream\n");
		return -1;
	}

	int ret = avcodec_copy_context(out_stream->codec, pCodecCtx);
	if (ret < 0) {
		printf("Failed to copy context from input to output stream codec context\n");
		return -1;
	}
	/*out_stream->codec->bit_rate = pCodecCtx->bit_rate;	
	out_stream->codec->width = pCodecCtx->width;
	out_stream->codec->height = pCodecCtx->height;	
	out_stream->codec->time_base.num = pCodecCtx->time_base.num;
	out_stream->codec->time_base.den = pCodecCtx->time_base.den;		
	out_stream->codec->gop_size = pCodecCtx->gop_size;
	out_stream->codec->max_b_frames = pCodecCtx->max_b_frames;
	out_stream->codec->pix_fmt = pCodecCtx->pix_fmt;
	out_stream->codec->codec_id = pCodecCtx->codec_id;
	out_stream->codec->codec_type=pCodecCtx->codec_type;*/


	out_stream->codec->codec_tag = 0;
	if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
		out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;	
	//Open output URL
	ret = avio_open(&ofmt_ctx->pb, ofmt_ctx->filename, AVIO_FLAG_WRITE);
	if (ret < 0) {
		printf("Could not open output URL '%s'", ofmt_ctx->filename);
		return -1;
	}	
	//Write file header
	ret = avformat_write_header(ofmt_ctx, NULL);
	if (ret < 0) {
		printf("Error occurred when opening output URL\n");
		return -1;
	}
	return 0;
}


RTMPPUBLISHER_API int init_ffmpeg_video(int inWidth, int inHeight, PixelFormat infmt, int outWidth, int outHeight, int rate, int fps)
{			
	av_register_all();
	avformat_network_init();
	PixelFormat tofmt = PIX_FMT_YUV420P;//PIX_FMT_YUV420P	
	//find 264 encoder
	AVCodec *m_pCodec;
	m_pCodec = avcodec_find_encoder(CODEC_ID_H264);
	if (m_pCodec == NULL) return -1;

	//get code contect
	m_pCodecCtx = avcodec_alloc_context3(m_pCodec);
	if (m_pCodecCtx == NULL) return -1;

	//config 
	m_pCodecCtx->bit_rate = rate;
	m_pCodecCtx->width = outWidth;
	m_pCodecCtx->height = outHeight;
	m_pCodecCtx->time_base.num = 1;
	m_pCodecCtx->time_base.den = fps;
	m_pCodecCtx->gop_size = 15;
	m_pCodecCtx->max_b_frames = 0;
	m_pCodecCtx->pix_fmt = tofmt;
	m_pCodecCtx->codec_type = AVMEDIA_TYPE_VIDEO;

	av_opt_set(m_pCodecCtx->priv_data, "preset", "superfast", 0);//ultrafast superfast veryfast faster fast medium slow slower veryslow placebo
	av_opt_set(m_pCodecCtx->priv_data, "tune", "zerolatency", 0);//zerolatency fastdecode psnr stillimage grain animation film

	if (avcodec_open2(m_pCodecCtx, m_pCodec, NULL) < 0)
	{
		perror("avcodec_open2");
		return -1;
	}

	//init in AVframe
	m_pInFrame = avcodec_alloc_frame();
	m_pInFrame->format = infmt;
	m_pInFrame->width = inWidth;
	m_pInFrame->height = inHeight;

	//init out AVframe
	m_pOutFrame = avcodec_alloc_frame();
	m_pOutFrame->format = tofmt;
	m_pOutFrame->width = outWidth;
	m_pOutFrame->height = outHeight;
	int nBytes = avpicture_get_size(tofmt, m_pOutFrame->width, m_pOutFrame->height);
	uint8_t *outbuffer = (uint8_t*)av_malloc(nBytes);
	avpicture_fill((AVPicture*)m_pOutFrame, outbuffer, tofmt, m_pOutFrame->width, m_pOutFrame->height);
	fooCtx = sws_getContext(m_pInFrame->width, m_pInFrame->height, infmt,
		m_pOutFrame->width, m_pOutFrame->height, tofmt, SWS_POINT, NULL, NULL, NULL);	//SWS_FAST_BILINEAR SWS_POINT SWS_SPLINE SWS_BICUBIC

	frame_index = 0;
	init_rtmp(m_pCodecCtx);

	return 0;
}
//int64_t lasttime=0;
RTMPPUBLISHER_API uint8_t* encodetoH264(uint8_t *pData)
{
	if (m_video_pkt.data)
	{
		av_free_packet(&m_video_pkt);		
	}
	/*av_init_packet(&m_video_pkt);*/
	m_video_pkt.data=NULL;
	m_video_pkt.size=0;		

	avpicture_fill((AVPicture*)m_pInFrame,(uint8_t*)pData,(PixelFormat)m_pInFrame->format,m_pInFrame->width ,m_pInFrame->height);			
	//m_pInFrame->data[0] += m_pInFrame->linesize[0] * (m_pCodecCtx->height - 1);
	//m_pInFrame->linesize[0] = -m_pInFrame->linesize[0];	
	sws_scale(fooCtx, (const uint8_t* const*)m_pInFrame->data, m_pInFrame->linesize, 0, m_pCodecCtx->height, m_pOutFrame->data, m_pOutFrame->linesize);

	int nGotOutput;
	if (frame_index == 0)
	{
		start_time = av_gettime();		
	}	
	m_video_pkt.pts=m_video_pkt.dts=frame_index;
	int ret = avcodec_encode_video2(m_pCodecCtx, &m_video_pkt, m_pOutFrame, &nGotOutput);
	if (ret < 0)
	{
		//fail skip		
		nGotOutput = -1;
	}

	if (nGotOutput == 1)
	{				
		/*_cprintf("size=%d ",(m_video_pkt.size * sizeof(uint8_t)));
		_cprintf("flags=%d",m_video_pkt.flags);*/
		m_video_pkt.stream_index = 0;
		//if(m_pCodecCtx->coded_frame->key_frame) // 如果是关键帧
		//	m_video_pkt.flags |= AV_PKT_FLAG_KEY;
		//_cprintf("m_pCodecCtx->coded_frame->key_frame=%d\n",m_pCodecCtx->coded_frame->key_frame);
		
		AVRational time_base = out_stream->time_base;		
		AVRational time_base_q = { 1, AV_TIME_BASE };
		//Duration between 2 frames (us)
		int64_t calc_duration = (double)AV_TIME_BASE / (m_pCodecCtx->time_base.den);
		m_video_pkt.pts = av_rescale_q(frame_index*calc_duration, time_base_q, time_base);
		//m_video_pkt.dts = m_video_pkt.pts;
		m_video_pkt.duration = av_rescale_q(calc_duration, time_base_q, time_base);		
		m_video_pkt.pos = -1;
		
		//_cprintf("pts=%d \n",m_video_pkt.pts);
		
		int64_t pts_time = av_rescale_q(m_video_pkt.pts, time_base, time_base_q);
		int64_t now_time = av_gettime() - start_time;
		if (pts_time > now_time){
			int64_t sleeptime=pts_time - now_time;
			_cprintf("sleeptime=%d\n",sleeptime);
			av_usleep(sleeptime);			
		}
		/*int64_t now_time = av_gettime();		
		if (now_time-lasttime<40*1000){			
			int64_t sleeptime=40*1000-(now_time-lasttime);
			_cprintf("sleeptime=%d\n",sleeptime);
			av_usleep(sleeptime);			
		}
		lasttime=now_time;*/

		frame_index++;		
		int ret = av_interleaved_write_frame(ofmt_ctx, &m_video_pkt);
		if (ret < 0) {
			printf("Error muxing packet\n");
			return NULL;
		}		
	}
	return NULL;
}

RTMPPUBLISHER_API int getpktlen()
{
	return m_video_len;
}

int publisherRTMP(uint8_t *pData, int datalen);


RTMPPUBLISHER_API int desrtoyffmpeg()
{
	av_free(m_pOutFrame);
	av_free(m_pInFrame);
	av_free_packet(&m_video_pkt);
	sws_freeContext(fooCtx);
	avcodec_close(m_pCodecCtx);

	/* close output */
	avio_close(ofmt_ctx->pb);
	avformat_free_context(ofmt_ctx);
	return 0;
}

//int publisherRTMP(uint8_t *pData ,int datalen)
//{
//	AVOutputFormat *ofmt = NULL;
//	//Input AVFormatContext and Output AVFormatContext
//	AVFormatContext *ifmt_ctx = NULL, *ofmt_ctx = NULL;
//	AVPacket pkt;
//	const char *in_filename, *out_filename;
//	int ret, i;
//	int videoindex=-1;
//	int frame_index=0;
//	int64_t start_time=0;
//	//in_filename  = "cuc_ieschool.mov";
//	//in_filename  = "cuc_ieschool.mkv";
//	//in_filename  = "cuc_ieschool.ts";
//	//in_filename  = "udp_clip.mp4";
//	in_filename  = "video.h264";
//	//in_filename  = "cuc_ieschool.flv";//输入URL（Input file URL）
//	//in_filename  = "testfile.flv";
//
//	out_filename = "rtmp://localhost/live/livestream";//输出 URL（Output URL）[RTMP]
//	//out_filename = "rtp://233.233.233.233:6666";//输出 URL（Output URL）[UDP]
//
//	av_register_all();
//	//Network
//	avformat_network_init();	
//	//Input
//	if ((ret = avformat_open_input(&ifmt_ctx, in_filename, 0, 0)) < 0) {
//		printf( "Could not open input file.");
//		goto end;
//	}
//	if ((ret = avformat_find_stream_info(ifmt_ctx, 0)) < 0) {
//		printf( "Failed to retrieve input stream information");
//		goto end;
//	}
//
//	for(i=0; i<ifmt_ctx->nb_streams; i++) 
//		if(ifmt_ctx->streams[i]->codec->codec_type==AVMEDIA_TYPE_VIDEO){
//			videoindex=i;
//			break;
//		}
//
//		av_dump_format(ifmt_ctx, 0, in_filename, 0);
//
//		//Output
//
//		avformat_alloc_output_context2(&ofmt_ctx, NULL, "flv", out_filename); //RTMP
//		//avformat_alloc_output_context2(&ofmt_ctx, NULL, "mpegts", out_filename);//UDP
//
//		if (!ofmt_ctx) {
//			printf( "Could not create output context\n");
//			ret = AVERROR_UNKNOWN;
//			goto end;
//		}
//		ofmt = ofmt_ctx->oformat;
//		for (i = 0; i < ifmt_ctx->nb_streams; i++) {
//			//Create output AVStream according to input AVStream
//			AVStream *in_stream = ifmt_ctx->streams[i];
//			AVStream *out_stream = avformat_new_stream(ofmt_ctx, in_stream->codec->codec);
//			if (!out_stream) {
//				printf( "Failed allocating output stream\n");
//				ret = AVERROR_UNKNOWN;
//				goto end;
//			}
//			//Copy the settings of AVCodecContext
//			ret = avcodec_copy_context(out_stream->codec, in_stream->codec);
//			if (ret < 0) {
//				printf( "Failed to copy context from input to output stream codec context\n");
//				goto end;
//			}
//			out_stream->codec->codec_tag = 0;
//			if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
//				out_stream->codec->flags |= CODEC_FLAG_GLOBAL_HEADER;
//		}
//		//Dump Format------------------
//		av_dump_format(ofmt_ctx, 0, out_filename, 1);
//		//Open output URL
//		if (!(ofmt->flags & AVFMT_NOFILE)) {
//			ret = avio_open(&ofmt_ctx->pb, out_filename, AVIO_FLAG_WRITE);
//			if (ret < 0) {
//				printf( "Could not open output URL '%s'", out_filename);
//				goto end;
//			}
//		}
//		//Write file header
//		ret = avformat_write_header(ofmt_ctx, NULL);
//		if (ret < 0) {
//			printf( "Error occurred when opening output URL\n");
//			goto end;
//		}
//
//		start_time=av_gettime();
//		while (1) {
//			AVStream *in_stream, *out_stream;
//			//Get an AVPacket
//			ret = av_read_frame(ifmt_ctx, &pkt);
//			if (ret < 0)
//				break;
//			//FIX：No PTS (Example: Raw H.264)
//			//Simple Write PTS
//			if(pkt.pts==AV_NOPTS_VALUE){
//				//Write PTS
//				AVRational time_base1=ifmt_ctx->streams[videoindex]->time_base;
//				//Duration between 2 frames (us)
//				int64_t calc_duration=(double)AV_TIME_BASE/av_q2d(ifmt_ctx->streams[videoindex]->r_frame_rate);
//				//Parameters
//				pkt.pts=(double)(frame_index*calc_duration)/(double)(av_q2d(time_base1)*AV_TIME_BASE);
//				pkt.dts=pkt.pts;
//				pkt.duration=(double)calc_duration/(double)(av_q2d(time_base1)*AV_TIME_BASE);
//			}
//			//Important:Delay
//			if(pkt.stream_index==videoindex){
//				AVRational time_base=ifmt_ctx->streams[videoindex]->time_base;
//				AVRational time_base_q={1,AV_TIME_BASE};
//				int64_t pts_time = av_rescale_q(pkt.dts, time_base, time_base_q);
//				int64_t now_time = av_gettime() - start_time;
//				if (pts_time > now_time)
//					av_usleep(pts_time - now_time);
//
//			}
//
//			in_stream  = ifmt_ctx->streams[pkt.stream_index];
//			out_stream = ofmt_ctx->streams[pkt.stream_index];
//			/* copy packet */
//			//Convert PTS/DTS
//			pkt.pts = av_rescale_q_rnd(pkt.pts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
//			pkt.dts = av_rescale_q_rnd(pkt.dts, in_stream->time_base, out_stream->time_base, (AVRounding)(AV_ROUND_NEAR_INF|AV_ROUND_PASS_MINMAX));
//			pkt.duration = av_rescale_q(pkt.duration, in_stream->time_base, out_stream->time_base);
//			pkt.pos = -1;
//			//Print to Screen
//			if(pkt.stream_index==videoindex){
//				printf("Send %8d video frames to output URL\n",frame_index);
//				frame_index++;
//			}
//			//ret = av_write_frame(ofmt_ctx, &pkt);
//			ret = av_interleaved_write_frame(ofmt_ctx, &pkt);
//
//			if (ret < 0) {
//				printf( "Error muxing packet\n");
//				break;
//			}
//
//			av_free_packet(&pkt);
//
//		}
//		//Write file trailer
//		av_write_trailer(ofmt_ctx);
//end:
//		avformat_close_input(&ifmt_ctx);
//		/* close output */
//		if (ofmt_ctx && !(ofmt->flags & AVFMT_NOFILE))
//			avio_close(ofmt_ctx->pb);
//		avformat_free_context(ofmt_ctx);
//		if (ret < 0 && ret != AVERROR_EOF) {
//			printf( "Error occurred.\n");
//			return -1;
//		}
//		return 0;
//}

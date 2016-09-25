#include "librtmpSend.h"

#include <stdio.h>
#include <stdlib.h>
#include "rtmp.h"
#include "rtmp_sys.h"
#include "amf.h"
#include "sps_decode.h"

#ifdef WIN32     
#include <windows.h>  
#pragma comment(lib,"WS2_32.lib")   
#pragma comment(lib,"winmm.lib")  
#endif 

#include "android/log.h"
// log标签
#define LOG_TAG    "video"
#define printf(...)  __android_log_print(ANDROID_LOG_WARN,LOG_TAG, __VA_ARGS__)

//定义包头长度，RTMP_MAX_HEADER_SIZE=18
#define RTMP_HEAD_SIZE   (sizeof(RTMPPacket)+RTMP_MAX_HEADER_SIZE)
//存储Nal单元数据的buffer大小
#define BUFFER_SIZE 32768
//搜寻Nal单元时的一些标志
#define GOT_A_NAL_CROSS_BUFFER BUFFER_SIZE+1
#define GOT_A_NAL_INCLUDE_A_BUFFER BUFFER_SIZE+2
#define NO_MORE_BUFFER_TO_READ BUFFER_SIZE+3

#ifdef __cplusplus
extern "C" {
#endif

/**
 * _NaluUnit
 * 内部结构体。该结构体主要用于存储和传递Nal单元的类型、大小和数据
 */
typedef struct _NaluUnit {
	int type;
	int size;
	unsigned char *data;
} NaluUnit;

/**
 * _RTMPMetadata
 * 内部结构体。该结构体主要用于存储和传递元数据信息
 */
typedef struct _RTMPMetadata {
	// video, must be h264 type   
	unsigned int nWidth;
	unsigned int nHeight;
	unsigned int nFrameRate;
	unsigned int nSpsLen;
	unsigned char *Sps;
	unsigned int nPpsLen;
	unsigned char *Pps;
} RTMPMetadata, *LPRTMPMetadata;

/**
 * 初始化winsock
 *					
 * @成功则返回1 , 失败则返回相应错误代码
 */
int InitSockets() {
#ifdef WIN32
	WORD version;
	WSADATA wsaData;
	version = MAKEWORD(1, 1);
	return (WSAStartup(version, &wsaData) == 0);
#else
	return TRUE;
#endif
}

/**
 * 释放winsock
 *					
 * @成功则返回0 , 失败则返回相应错误代码
 */
inline void CleanupSockets() {
#ifdef WIN32
	WSACleanup();
#endif
}

unsigned int nalhead_pos; // m_pFileBuf中nal起始计算偏移
RTMP* m_pRtmp;
RTMPMetadata metaData;
unsigned char *m_pFileBuf;
unsigned int m_pFileBufSize; // m_pFileBuf有效长度
unsigned char *m_pFileBuf_tmp;
unsigned char* m_pFileBuf_tmp_old;	//used for realloc
unsigned char* readTemp; // 用于作为临时缓存
unsigned int tick = 0; // 帧时间标签 
unsigned int m_frameRate = 25; // 帧率，数字越大，发送间隔越短

/**
 * 初始化并连接到服务器
 *
 * @param url 服务器上对应webapp的地址
 * @param frameRate 帧率，数字越大，发送间隔越短
 *					
 * @成功则返回1 , 失败则返回0
 */
int RTMP264_Connect(const char* url, int frameRate) {
	nalhead_pos = 0;
	m_pFileBufSize = 0;
	m_pFileBuf = (unsigned char*) malloc(BUFFER_SIZE);
	m_pFileBuf_tmp = (unsigned char*) malloc(BUFFER_SIZE);
	readTemp = (unsigned char*) malloc(BUFFER_SIZE);
	m_frameRate = frameRate;
	InitSockets();

	m_pRtmp = RTMP_Alloc();
	RTMP_Init(m_pRtmp);
	/*设置URL*/
	if (RTMP_SetupURL(m_pRtmp, (char*) url) == FALSE) {
		RTMP_Free(m_pRtmp);
		return false;
	}
	/*设置可写,即发布流,这个函数必须在连接前使用,否则无效*/
	RTMP_EnableWrite(m_pRtmp);
	/*连接服务器*/
	if (RTMP_Connect(m_pRtmp, NULL) == FALSE) {
		RTMP_Free(m_pRtmp);
		return false;
	}

	/*连接流*/
	if (RTMP_ConnectStream(m_pRtmp, 0) == FALSE) {
		RTMP_Close(m_pRtmp);
		RTMP_Free(m_pRtmp);
		return false;
	}
	memset(&metaData, 0, sizeof(RTMPMetadata));
	memset(m_pFileBuf, 0, BUFFER_SIZE);
	tick = 0;
	return true;
}

/**
 * 断开连接，释放相关的资源。
 *
 */
void RTMP264_Close() {
	free(metaData.Sps);
	free(metaData.Pps);
	if (m_pRtmp) {
		RTMP_Close(m_pRtmp);
		RTMP_Free(m_pRtmp);
		m_pRtmp = NULL;
	}
	CleanupSockets();
	if (m_pFileBuf != NULL) {
		free(m_pFileBuf);
	}
	if (m_pFileBuf_tmp != NULL) {
		free(m_pFileBuf_tmp);
	}
	printf("RTMP264_Close.\n");
}

/**
 * 发送RTMP数据包
 *
 * @param nPacketType 数据类型
 * @param data 存储数据内容
 * @param size 数据大小
 * @param nTimestamp 当前包的时间戳
 *
 * @成功则返回 1 , 失败则返回一个小于0的数
 */
int SendPacket(unsigned int nPacketType, unsigned char *data, unsigned int size,
		unsigned int nTimestamp) {
	RTMPPacket* packet;
	/*分配包内存和初始化,len为包体长度*/
	packet = (RTMPPacket *) malloc(RTMP_HEAD_SIZE + size);
	memset(packet, 0, RTMP_HEAD_SIZE);
	/*包体内存*/
	packet->m_body = (char *) packet + RTMP_HEAD_SIZE;
	packet->m_nBodySize = size;
	memcpy(packet->m_body, data, size);
	packet->m_hasAbsTimestamp = 0;
	packet->m_packetType = nPacketType; /*此处为类型有两种一种是音频,一种是视频*/
	packet->m_nInfoField2 = m_pRtmp->m_stream_id;
	packet->m_nChannel = 0x04;

	packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
	if (RTMP_PACKET_TYPE_AUDIO == nPacketType && size != 4) {
		packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
	}
	packet->m_nTimeStamp = nTimestamp;
	/*发送*/
	int nRet = 0;
	if (RTMP_IsConnected(m_pRtmp)) {
		nRet = RTMP_SendPacket(m_pRtmp, packet, TRUE); /*TRUE为放进发送队列,FALSE是不放进发送队列,直接发送*/
	}
	/*释放内存*/
	free(packet);
	return nRet;
}

/**
 * 发送视频的sps和pps信息
 *
 * @param pps 存储视频的pps信息
 * @param pps_len 视频的pps信息长度
 * @param sps 存储视频的pps信息
 * @param sps_len 视频的sps信息长度
 *
 * @成功则返回 1 , 失败则返回0
 */
int SendVideoSpsPps(unsigned char *pps, int pps_len, unsigned char * sps,
		int sps_len) {
	RTMPPacket * packet = NULL; //rtmp包结构
	unsigned char * body = NULL;
	int i;
	packet = (RTMPPacket *) malloc(RTMP_HEAD_SIZE + 1024);
	//RTMPPacket_Reset(packet);//重置packet状态
	memset(packet, 0, RTMP_HEAD_SIZE + 1024);
	packet->m_body = (char *) packet + RTMP_HEAD_SIZE;
	body = (unsigned char *) packet->m_body;
	i = 0;
	body[i++] = 0x17;
	body[i++] = 0x00;

	body[i++] = 0x00;
	body[i++] = 0x00;
	body[i++] = 0x00;

	/*AVCDecoderConfigurationRecord*/
	body[i++] = 0x01;
	body[i++] = sps[1];
	body[i++] = sps[2];
	body[i++] = sps[3];
	body[i++] = 0xff;

	/*sps*/
	body[i++] = 0xe1;
	body[i++] = (sps_len >> 8) & 0xff;
	body[i++] = sps_len & 0xff;
	memcpy(&body[i], sps, sps_len);
	i += sps_len;

	/*pps*/
	body[i++] = 0x01;
	body[i++] = (pps_len >> 8) & 0xff;
	body[i++] = (pps_len) & 0xff;
	memcpy(&body[i], pps, pps_len);
	i += pps_len;

	packet->m_packetType = RTMP_PACKET_TYPE_VIDEO;
	packet->m_nBodySize = i;
	packet->m_nChannel = 0x04;
	packet->m_nTimeStamp = 0;
	packet->m_hasAbsTimestamp = 0;
	packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
	packet->m_nInfoField2 = m_pRtmp->m_stream_id;

	/*调用发送接口*/
	int nRet = RTMP_SendPacket(m_pRtmp, packet, TRUE);
	free(packet);    //释放内存
	return nRet;
}

/**
 * 发送H264数据帧
 *
 * @param data 存储数据帧内容
 * @param size 数据帧的大小
 * @param bIsKeyFrame 记录该帧是否为关键帧
 * @param nTimeStamp 当前帧的时间戳
 *
 * @成功则返回 1 , 失败则返回0
 */
int mySendH264Packet(unsigned char *data, unsigned int size, int bIsKeyFrame,
		unsigned int nTimeStamp) {
	if (data == NULL && size < 11) {
		return false;
	}

	unsigned char *body = (unsigned char*) malloc(size + 9);
	memset(body, 0, size + 9);

	int i = 0;
	if (bIsKeyFrame) {
		body[i++] = 0x17;    // 1:Iframe  7:AVC
		body[i++] = 0x01;    // AVC NALU
		body[i++] = 0x00;
		body[i++] = 0x00;
		body[i++] = 0x00;

		// NALU size   
		body[i++] = size >> 24 & 0xff;
		body[i++] = size >> 16 & 0xff;
		body[i++] = size >> 8 & 0xff;
		body[i++] = size & 0xff;
		// NALU data   
		memcpy(&body[i], data, size);
	} else {
		body[i++] = 0x27;    // 2:Pframe  7:AVC
		body[i++] = 0x01;    // AVC NALU
		body[i++] = 0x00;
		body[i++] = 0x00;
		body[i++] = 0x00;

		// NALU size   
		body[i++] = size >> 24 & 0xff;
		body[i++] = size >> 16 & 0xff;
		body[i++] = size >> 8 & 0xff;
		body[i++] = size & 0xff;
		// NALU data   
		memcpy(&body[i], data, size);
	}
	int bRet = SendPacket(RTMP_PACKET_TYPE_VIDEO, body, i + size, nTimeStamp);
//	printf("SendPacket\n");
	free(body);

	return bRet;
}
/**
 * 发送H264头部数据帧
 *
 * @param nTimeStamp 当前帧的时间戳
 *
 * @成功则返回 1 , 失败则返回0
 */
int mySendH264PacketMetaData(unsigned int nTimeStamp) {
	int bRet = SendVideoSpsPps(metaData.Pps, metaData.nPpsLen, metaData.Sps,
			metaData.nSpsLen);

	return bRet;
}

/**
 * 从内存中读取出第一个Nal单元
 *
 * @param nalu 存储nalu数据
 * @成功则返回 1 , 失败则返回0
 */
int myReadFirstNaluFromBufFile(NaluUnit &nalu) {
	int naltail_pos = nalhead_pos;
	memset(m_pFileBuf_tmp, 0, BUFFER_SIZE);
	while (nalhead_pos < m_pFileBufSize) {
		//search for nal header
		if (m_pFileBuf[nalhead_pos++] == 0x00
				&& m_pFileBuf[nalhead_pos++] == 0x00) {
			if (m_pFileBuf[nalhead_pos++] == 0x01)
				goto gotnal_head;
			else {
				//cuz we have done an i++ before,so we need to roll back now
				nalhead_pos--;
				if (m_pFileBuf[nalhead_pos++] == 0x00
						&& m_pFileBuf[nalhead_pos++] == 0x01)
					goto gotnal_head;
				else
					continue;
			}
		} else
			continue;

		//search for nal tail which is also the head of next nal
		gotnal_head:
		//normal case:the whole nal is in this m_pFileBuf
		naltail_pos = nalhead_pos;
		while (naltail_pos < m_pFileBufSize) {
			if (m_pFileBuf[naltail_pos++] == 0x00
					&& m_pFileBuf[naltail_pos++] == 0x00) {
				if (m_pFileBuf[naltail_pos++] == 0x01) {
					nalu.size = (naltail_pos - 3) - nalhead_pos;
					break;
				} else {
					naltail_pos--;
					if (m_pFileBuf[naltail_pos++] == 0x00
							&& m_pFileBuf[naltail_pos++] == 0x01) {
						nalu.size = (naltail_pos - 4) - nalhead_pos;
						break;
					}
				}
			}
		}

		nalu.type = m_pFileBuf[nalhead_pos] & 0x1f;
		memcpy(m_pFileBuf_tmp, m_pFileBuf + nalhead_pos, nalu.size);
		nalu.data = m_pFileBuf_tmp;
		nalhead_pos = naltail_pos;
		return TRUE;
	}
}

/************************************************************************/
/* 将字符串以十六进制方式打印出来                                       */
/************************************************************************/
void printBuf(unsigned char* buf, int bufSize) {
	for (int i = 0; i <= bufSize; i++) {
		printf("%.2x ", buf[i]);
		if (i % 16 == 0) {
			printf("\n");
		}
	}
	printf("\n");
}
/************************************************************************/
/* 衔接字符串                                                           */
/************************************************************************/
void strcatFileBuf(uint8_t *buf, int buf_size) {
	memcpy(m_pFileBuf + m_pFileBufSize, buf, buf_size);
	m_pFileBufSize += buf_size;
}
/************************************************************************/
/* 把m_pFileBuf中已经获取的nal节数据清除掉                              */
/************************************************************************/
void subtFileBufFromLeftPos(int pos) {
	memset(readTemp, 0, BUFFER_SIZE);
	memcpy(readTemp, m_pFileBuf + pos, m_pFileBufSize);
	m_pFileBufSize = m_pFileBufSize - pos;
	nalhead_pos = nalhead_pos - pos;
	memcpy(m_pFileBuf, readTemp, m_pFileBufSize);
}
/**
 * 从内存中读取出一个Nal单元
 *
 * @param nalu 存储nalu数据
 * @param uint8_t *buf：外部数据送至该地址
 * @param int buf_size：外部数据大小
 * @成功则返回 1 , 失败则返回0
 */
int myReadOneNaluFromBufFile(NaluUnit &nalu, uint8_t *buf, int buf_size) {
	// nal的偏移
	int naltail_pos = nalhead_pos;
	int ret;
	// nal节开始标识大小
	int nalustart;    //nal的开始标识符是几个00
	memset(m_pFileBuf_tmp, 0, BUFFER_SIZE);
	nalu.size = 0;
	while (1) {
		if (nalhead_pos == NO_MORE_BUFFER_TO_READ)
			return FALSE;
		while (naltail_pos < m_pFileBufSize) {
			//search for nal tail
			if (m_pFileBuf[naltail_pos++] == 0x00
					&& m_pFileBuf[naltail_pos++] == 0x00) {
				if (m_pFileBuf[naltail_pos++] == 0x01) {
					nalustart = 3;
					goto gotnal;
				} else {
					//cuz we have done an i++ before,so we need to roll back now
					naltail_pos--;
					if (m_pFileBuf[naltail_pos++] == 0x00
							&& m_pFileBuf[naltail_pos++] == 0x01) {
						nalustart = 4;
						goto gotnal;
					} else
						continue;
				}
			} else
				continue;

			gotnal:
			/**
			 *special case1:parts of the nal lies in a m_pFileBuf and we have to read from buffer
			 *again to get the rest part of this nal
			 */
			if (nalhead_pos == GOT_A_NAL_CROSS_BUFFER
					|| nalhead_pos == GOT_A_NAL_INCLUDE_A_BUFFER) {
//					printf("ReadOneNaluFromBuf：special case\n");
				nalu.size = nalu.size + naltail_pos - nalustart;
				if (nalu.size > BUFFER_SIZE) {
					m_pFileBuf_tmp_old = m_pFileBuf_tmp; //// save pointer in case realloc fails
					if ((m_pFileBuf_tmp = (unsigned char*) realloc(
							m_pFileBuf_tmp, nalu.size)) == NULL) {
						free(m_pFileBuf_tmp_old);  // free original block
						return FALSE;
					}
				}
				memcpy(m_pFileBuf_tmp + nalu.size + nalustart - naltail_pos,
						m_pFileBuf, naltail_pos - nalustart);
				nalu.data = m_pFileBuf_tmp;
				nalhead_pos = naltail_pos;
				return TRUE;
			}
			//normal case:the whole nal is in this m_pFileBuf
			// 正常情况，整个完整nal都在m_pFileBuf中
			else {
//					printf("ReadOneNaluFromBuf：normal case\n");
				nalu.type = m_pFileBuf[nalhead_pos] & 0x1f;
				// nal大小 = nal的偏移 - m_pFileBuf中nal起始计算偏移 - nal节开始标识大小
				nalu.size = naltail_pos - nalhead_pos - nalustart;
				if (nalu.type == 0x06) {
					// 如果nal内容偏移等于nal头部偏移，即无nal内容，跳过
					nalhead_pos = naltail_pos;
					continue;
				}
				memcpy(m_pFileBuf_tmp, m_pFileBuf + nalhead_pos, nalu.size);
				nalu.data = m_pFileBuf_tmp;
				nalhead_pos = naltail_pos;
				// 把m_pFileBuf中已经获取的nal节数据清除掉
				subtFileBufFromLeftPos(naltail_pos);
				return TRUE;
			}
		}

		if (naltail_pos
				>= m_pFileBufSize&& nalhead_pos!=GOT_A_NAL_CROSS_BUFFER && nalhead_pos != GOT_A_NAL_INCLUDE_A_BUFFER) {
			return FALSE;
		}
	}
	return FALSE;
}

/**
 * 将一段H.264编码的视频文件利用RTMP协议发送到服务器
 *
 * @param char *buf 要发送的数据
 * @param char *buf_size 要发送的数据长度
 * @发送成功则返回1 , 发送失败则返回0
 */
int myRTMP264_SendBuffer(unsigned char *buf, int buf_size) {
	// 先缓存要处理的数据，达到一定的数量才开始处理
	strcatFileBuf(buf, buf_size);
	printf("m_pFileBufSize:%d", m_pFileBufSize);

	if (m_pFileBufSize < 15000) {
		return TRUE;
	}

	NaluUnit naluUnit;
	memset(&naluUnit, 0, sizeof(NaluUnit));

	uint32_t now, last_update; // 用于帧频率的sleep休眠时间计算
	// 用于满足帧频率的sleep休眠时间
	unsigned int tick_gap = 1000 / m_frameRate; // 帧频率
	int bKeyframe = FALSE; // 是否是关键帧
	int sendResult = FALSE; // 数据发放结果
	int sendBuffFlag = false; // 第一次数据发送

	if (metaData.nSpsLen == 0) {
		// 读取SPS帧   
		myReadFirstNaluFromBufFile(naluUnit);
		printf("myReadFirstNaluFromBuf NALU size:%8d\n", naluUnit.size);
		metaData.nSpsLen = naluUnit.size;
		metaData.Sps = NULL;
		metaData.Sps = (unsigned char*) malloc(naluUnit.size);
		memcpy(metaData.Sps, naluUnit.data, naluUnit.size);

		// 读取PPS帧   
		myReadOneNaluFromBufFile(naluUnit, NULL, 0);
		printf("PPS NALU size:%8d\n", naluUnit.size);
		metaData.nPpsLen = naluUnit.size;
		metaData.Pps = NULL;
		metaData.Pps = (unsigned char*) malloc(naluUnit.size);
		memcpy(metaData.Pps, naluUnit.data, naluUnit.size);

		// 解码SPS,获取视频图像宽、高信息   
		int width = 0, height = 0, fps = 0;
		h264_decode_sps(metaData.Sps, metaData.nSpsLen, width, height, fps);
		metaData.nWidth = width;
		metaData.nHeight = height;
		printf(
				"===========================================metaData nWidth:%d    nHeight:%d\n",
				width, height);
//		if (fps)
//			metaData.nFrameRate = fps; // 没解析出来过
//		else
		metaData.nFrameRate = m_frameRate;

		// 读取第一个Nal节
		int firstNal = myReadOneNaluFromBufFile(naluUnit, NULL, 0);
//		printf("myReadOneNaluFromBuf NALU size:%8d\n", naluUnit.size);
		bKeyframe = (naluUnit.type == 0x05) ? TRUE : FALSE;
		// MetaData和第一Nal数据数据必须同时发，否则无法实现图像
		// 所以，实际上要回避firstNal=false的情况，保证第一次传递进来的数据，就已经包括完整的metaDate和Nal
		if (firstNal) {
			mySendH264PacketMetaData(tick);
			// 发送第一个Nal是同一个函数发送的
			printf(
					"mySendH264Packet:naluUnit.type=%d tick(%d) naluUnit.size=%d\n",
					naluUnit.type, tick, naluUnit.size);
			sendResult = mySendH264Packet(naluUnit.data, naluUnit.size,
					bKeyframe, tick);
		} else {
			printf("firstNal can not send!!!");
		}
	} else {
		sendResult = TRUE;
		sendBuffFlag = TRUE;
	}

	int ret;
	while (sendResult) {
		got_sps_pps: last_update = RTMP_GetTime();
		if (!sendBuffFlag) {
			ret = myReadOneNaluFromBufFile(naluUnit, NULL, 0);
			if (!ret) {
				return TRUE;
			}
		} else {
			ret = myReadOneNaluFromBufFile(naluUnit, buf, buf_size);
			if (!ret) {
				return TRUE;
			}
			sendBuffFlag = FALSE;
		}
		if (naluUnit.type == 0x07 || naluUnit.type == 0x08)
			goto got_sps_pps;
		bKeyframe = (naluUnit.type == 0x05) ? TRUE : FALSE;
		tick += tick_gap;
		now = RTMP_GetTime();
		// 用于维持帧率平衡的时间延迟
		int times = tick_gap - now + last_update;
		if (times > 0) {
			printf("while:times=%d\n", times);
			msleep(times);
		}
		//msleep(40);
		// adong:重复发关键帧还会画面抖动(大概是解析SPS和PPS引起的数据冗余)
		// adong:如果不重复发关键帧，在发送过程中好像视频播放无法中途切入播放，好像是通过判断关键帧在切入播放视频的
		// adong:所以在mySendH264Packet中分离了SPS和PPS的发送
//		if (bKeyframe==TRUE && tick>=30000) {
		// 会导致图像断裂，暂时屏蔽下
//			tick = 0;
//			mySendH264PacketMetaData(tick);
//		}
		printf("mySendH264Packet:naluUnit.type=%d tick(%d) naluUnit.size=%d\n",
				naluUnit.type, tick, naluUnit.size);
		sendResult = mySendH264Packet(naluUnit.data, naluUnit.size, bKeyframe,
				tick);

	}
	return FALSE;
}

#ifdef __cplusplus
}
#endif

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
// log��ǩ
#define LOG_TAG    "video"
#define printf(...)  __android_log_print(ANDROID_LOG_WARN,LOG_TAG, __VA_ARGS__)

//�����ͷ���ȣ�RTMP_MAX_HEADER_SIZE=18
#define RTMP_HEAD_SIZE   (sizeof(RTMPPacket)+RTMP_MAX_HEADER_SIZE)
//�洢Nal��Ԫ���ݵ�buffer��С
#define BUFFER_SIZE 32768
//��ѰNal��Ԫʱ��һЩ��־
#define GOT_A_NAL_CROSS_BUFFER BUFFER_SIZE+1
#define GOT_A_NAL_INCLUDE_A_BUFFER BUFFER_SIZE+2
#define NO_MORE_BUFFER_TO_READ BUFFER_SIZE+3

#ifdef __cplusplus
extern "C" {
#endif

/**
 * _NaluUnit
 * �ڲ��ṹ�塣�ýṹ����Ҫ���ڴ洢�ʹ���Nal��Ԫ�����͡���С������
 */
typedef struct _NaluUnit {
	int type;
	int size;
	unsigned char *data;
} NaluUnit;

/**
 * _RTMPMetadata
 * �ڲ��ṹ�塣�ýṹ����Ҫ���ڴ洢�ʹ���Ԫ������Ϣ
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
 * ��ʼ��winsock
 *					
 * @�ɹ��򷵻�1 , ʧ���򷵻���Ӧ�������
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
 * �ͷ�winsock
 *					
 * @�ɹ��򷵻�0 , ʧ���򷵻���Ӧ�������
 */
inline void CleanupSockets() {
#ifdef WIN32
	WSACleanup();
#endif
}

unsigned int nalhead_pos; // m_pFileBuf��nal��ʼ����ƫ��
RTMP* m_pRtmp;
RTMPMetadata metaData;
unsigned char *m_pFileBuf;
unsigned int m_pFileBufSize; // m_pFileBuf��Ч����
unsigned char *m_pFileBuf_tmp;
unsigned char* m_pFileBuf_tmp_old;	//used for realloc
unsigned char* readTemp; // ������Ϊ��ʱ����
unsigned int tick = 0; // ֡ʱ���ǩ 
unsigned int m_frameRate = 25; // ֡�ʣ�����Խ�󣬷��ͼ��Խ��

/**
 * ��ʼ�������ӵ�������
 *
 * @param url �������϶�Ӧwebapp�ĵ�ַ
 * @param frameRate ֡�ʣ�����Խ�󣬷��ͼ��Խ��
 *					
 * @�ɹ��򷵻�1 , ʧ���򷵻�0
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
	/*����URL*/
	if (RTMP_SetupURL(m_pRtmp, (char*) url) == FALSE) {
		RTMP_Free(m_pRtmp);
		return false;
	}
	/*���ÿ�д,��������,�����������������ǰʹ��,������Ч*/
	RTMP_EnableWrite(m_pRtmp);
	/*���ӷ�����*/
	if (RTMP_Connect(m_pRtmp, NULL) == FALSE) {
		RTMP_Free(m_pRtmp);
		return false;
	}

	/*������*/
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
 * �Ͽ����ӣ��ͷ���ص���Դ��
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
 * ����RTMP���ݰ�
 *
 * @param nPacketType ��������
 * @param data �洢��������
 * @param size ���ݴ�С
 * @param nTimestamp ��ǰ����ʱ���
 *
 * @�ɹ��򷵻� 1 , ʧ���򷵻�һ��С��0����
 */
int SendPacket(unsigned int nPacketType, unsigned char *data, unsigned int size,
		unsigned int nTimestamp) {
	RTMPPacket* packet;
	/*������ڴ�ͳ�ʼ��,lenΪ���峤��*/
	packet = (RTMPPacket *) malloc(RTMP_HEAD_SIZE + size);
	memset(packet, 0, RTMP_HEAD_SIZE);
	/*�����ڴ�*/
	packet->m_body = (char *) packet + RTMP_HEAD_SIZE;
	packet->m_nBodySize = size;
	memcpy(packet->m_body, data, size);
	packet->m_hasAbsTimestamp = 0;
	packet->m_packetType = nPacketType; /*�˴�Ϊ����������һ������Ƶ,һ������Ƶ*/
	packet->m_nInfoField2 = m_pRtmp->m_stream_id;
	packet->m_nChannel = 0x04;

	packet->m_headerType = RTMP_PACKET_SIZE_LARGE;
	if (RTMP_PACKET_TYPE_AUDIO == nPacketType && size != 4) {
		packet->m_headerType = RTMP_PACKET_SIZE_MEDIUM;
	}
	packet->m_nTimeStamp = nTimestamp;
	/*����*/
	int nRet = 0;
	if (RTMP_IsConnected(m_pRtmp)) {
		nRet = RTMP_SendPacket(m_pRtmp, packet, TRUE); /*TRUEΪ�Ž����Ͷ���,FALSE�ǲ��Ž����Ͷ���,ֱ�ӷ���*/
	}
	/*�ͷ��ڴ�*/
	free(packet);
	return nRet;
}

/**
 * ������Ƶ��sps��pps��Ϣ
 *
 * @param pps �洢��Ƶ��pps��Ϣ
 * @param pps_len ��Ƶ��pps��Ϣ����
 * @param sps �洢��Ƶ��pps��Ϣ
 * @param sps_len ��Ƶ��sps��Ϣ����
 *
 * @�ɹ��򷵻� 1 , ʧ���򷵻�0
 */
int SendVideoSpsPps(unsigned char *pps, int pps_len, unsigned char * sps,
		int sps_len) {
	RTMPPacket * packet = NULL; //rtmp���ṹ
	unsigned char * body = NULL;
	int i;
	packet = (RTMPPacket *) malloc(RTMP_HEAD_SIZE + 1024);
	//RTMPPacket_Reset(packet);//����packet״̬
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

	/*���÷��ͽӿ�*/
	int nRet = RTMP_SendPacket(m_pRtmp, packet, TRUE);
	free(packet);    //�ͷ��ڴ�
	return nRet;
}

/**
 * ����H264����֡
 *
 * @param data �洢����֡����
 * @param size ����֡�Ĵ�С
 * @param bIsKeyFrame ��¼��֡�Ƿ�Ϊ�ؼ�֡
 * @param nTimeStamp ��ǰ֡��ʱ���
 *
 * @�ɹ��򷵻� 1 , ʧ���򷵻�0
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
 * ����H264ͷ������֡
 *
 * @param nTimeStamp ��ǰ֡��ʱ���
 *
 * @�ɹ��򷵻� 1 , ʧ���򷵻�0
 */
int mySendH264PacketMetaData(unsigned int nTimeStamp) {
	int bRet = SendVideoSpsPps(metaData.Pps, metaData.nPpsLen, metaData.Sps,
			metaData.nSpsLen);

	return bRet;
}

/**
 * ���ڴ��ж�ȡ����һ��Nal��Ԫ
 *
 * @param nalu �洢nalu����
 * @�ɹ��򷵻� 1 , ʧ���򷵻�0
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
/* ���ַ�����ʮ�����Ʒ�ʽ��ӡ����                                       */
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
/* �ν��ַ���                                                           */
/************************************************************************/
void strcatFileBuf(uint8_t *buf, int buf_size) {
	memcpy(m_pFileBuf + m_pFileBufSize, buf, buf_size);
	m_pFileBufSize += buf_size;
}
/************************************************************************/
/* ��m_pFileBuf���Ѿ���ȡ��nal�����������                              */
/************************************************************************/
void subtFileBufFromLeftPos(int pos) {
	memset(readTemp, 0, BUFFER_SIZE);
	memcpy(readTemp, m_pFileBuf + pos, m_pFileBufSize);
	m_pFileBufSize = m_pFileBufSize - pos;
	nalhead_pos = nalhead_pos - pos;
	memcpy(m_pFileBuf, readTemp, m_pFileBufSize);
}
/**
 * ���ڴ��ж�ȡ��һ��Nal��Ԫ
 *
 * @param nalu �洢nalu����
 * @param uint8_t *buf���ⲿ���������õ�ַ
 * @param int buf_size���ⲿ���ݴ�С
 * @�ɹ��򷵻� 1 , ʧ���򷵻�0
 */
int myReadOneNaluFromBufFile(NaluUnit &nalu, uint8_t *buf, int buf_size) {
	// nal��ƫ��
	int naltail_pos = nalhead_pos;
	int ret;
	// nal�ڿ�ʼ��ʶ��С
	int nalustart;    //nal�Ŀ�ʼ��ʶ���Ǽ���00
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
//					printf("ReadOneNaluFromBuf��special case\n");
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
			// �����������������nal����m_pFileBuf��
			else {
//					printf("ReadOneNaluFromBuf��normal case\n");
				nalu.type = m_pFileBuf[nalhead_pos] & 0x1f;
				// nal��С = nal��ƫ�� - m_pFileBuf��nal��ʼ����ƫ�� - nal�ڿ�ʼ��ʶ��С
				nalu.size = naltail_pos - nalhead_pos - nalustart;
				if (nalu.type == 0x06) {
					// ���nal����ƫ�Ƶ���nalͷ��ƫ�ƣ�����nal���ݣ�����
					nalhead_pos = naltail_pos;
					continue;
				}
				memcpy(m_pFileBuf_tmp, m_pFileBuf + nalhead_pos, nalu.size);
				nalu.data = m_pFileBuf_tmp;
				nalhead_pos = naltail_pos;
				// ��m_pFileBuf���Ѿ���ȡ��nal�����������
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
 * ��һ��H.264�������Ƶ�ļ�����RTMPЭ�鷢�͵�������
 *
 * @param char *buf Ҫ���͵�����
 * @param char *buf_size Ҫ���͵����ݳ���
 * @���ͳɹ��򷵻�1 , ����ʧ���򷵻�0
 */
int myRTMP264_SendBuffer(unsigned char *buf, int buf_size) {
	// �Ȼ���Ҫ��������ݣ��ﵽһ���������ſ�ʼ����
	strcatFileBuf(buf, buf_size);
	printf("m_pFileBufSize:%d", m_pFileBufSize);

	if (m_pFileBufSize < 15000) {
		return TRUE;
	}

	NaluUnit naluUnit;
	memset(&naluUnit, 0, sizeof(NaluUnit));

	uint32_t now, last_update; // ����֡Ƶ�ʵ�sleep����ʱ�����
	// ��������֡Ƶ�ʵ�sleep����ʱ��
	unsigned int tick_gap = 1000 / m_frameRate; // ֡Ƶ��
	int bKeyframe = FALSE; // �Ƿ��ǹؼ�֡
	int sendResult = FALSE; // ���ݷ��Ž��
	int sendBuffFlag = false; // ��һ�����ݷ���

	if (metaData.nSpsLen == 0) {
		// ��ȡSPS֡   
		myReadFirstNaluFromBufFile(naluUnit);
		printf("myReadFirstNaluFromBuf NALU size:%8d\n", naluUnit.size);
		metaData.nSpsLen = naluUnit.size;
		metaData.Sps = NULL;
		metaData.Sps = (unsigned char*) malloc(naluUnit.size);
		memcpy(metaData.Sps, naluUnit.data, naluUnit.size);

		// ��ȡPPS֡   
		myReadOneNaluFromBufFile(naluUnit, NULL, 0);
		printf("PPS NALU size:%8d\n", naluUnit.size);
		metaData.nPpsLen = naluUnit.size;
		metaData.Pps = NULL;
		metaData.Pps = (unsigned char*) malloc(naluUnit.size);
		memcpy(metaData.Pps, naluUnit.data, naluUnit.size);

		// ����SPS,��ȡ��Ƶͼ�������Ϣ   
		int width = 0, height = 0, fps = 0;
		h264_decode_sps(metaData.Sps, metaData.nSpsLen, width, height, fps);
		metaData.nWidth = width;
		metaData.nHeight = height;
		printf(
				"===========================================metaData nWidth:%d    nHeight:%d\n",
				width, height);
//		if (fps)
//			metaData.nFrameRate = fps; // û����������
//		else
		metaData.nFrameRate = m_frameRate;

		// ��ȡ��һ��Nal��
		int firstNal = myReadOneNaluFromBufFile(naluUnit, NULL, 0);
//		printf("myReadOneNaluFromBuf NALU size:%8d\n", naluUnit.size);
		bKeyframe = (naluUnit.type == 0x05) ? TRUE : FALSE;
		// MetaData�͵�һNal�������ݱ���ͬʱ���������޷�ʵ��ͼ��
		// ���ԣ�ʵ����Ҫ�ر�firstNal=false���������֤��һ�δ��ݽ��������ݣ����Ѿ�����������metaDate��Nal
		if (firstNal) {
			mySendH264PacketMetaData(tick);
			// ���͵�һ��Nal��ͬһ���������͵�
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
		// ����ά��֡��ƽ���ʱ���ӳ�
		int times = tick_gap - now + last_update;
		if (times > 0) {
			printf("while:times=%d\n", times);
			msleep(times);
		}
		//msleep(40);
		// adong:�ظ����ؼ�֡���ử�涶��(����ǽ���SPS��PPS�������������)
		// adong:������ظ����ؼ�֡���ڷ��͹����к�����Ƶ�����޷���;���벥�ţ�������ͨ���жϹؼ�֡�����벥����Ƶ��
		// adong:������mySendH264Packet�з�����SPS��PPS�ķ���
//		if (bKeyframe==TRUE && tick>=30000) {
		// �ᵼ��ͼ����ѣ���ʱ������
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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * ��ʼ�������ӵ�������
 *
 * @param url �������϶�Ӧwebapp�ĵ�ַ
 * @param frameRate ֡�ʣ�����Խ�󣬷��ͼ��Խ��
 *					
 * @�ɹ��򷵻�1 , ʧ���򷵻�0
 */
int RTMP264_Connect(const char* url, int frameRate);

 /**
 * ��һ��H.264�������Ƶ�ļ�����RTMPЭ�鷢�͵�������
 *
 * @param char *buf Ҫ���͵�����
 * @param char *buf_size Ҫ���͵����ݳ���
 * @���ͳɹ��򷵻�1 , ����ʧ���򷵻�0
 */ 
int myRTMP264_SendBuffer(unsigned char *buf, int buf_size);

/**
 * �Ͽ����ӣ��ͷ���ص���Դ��
 *
 */    
void RTMP264_Close();  

#ifdef __cplusplus
}
#endif

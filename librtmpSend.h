#ifdef __cplusplus
extern "C" {
#endif

/**
 * 初始化并连接到服务器
 *
 * @param url 服务器上对应webapp的地址
 * @param frameRate 帧率，数字越大，发送间隔越短
 *					
 * @成功则返回1 , 失败则返回0
 */
int RTMP264_Connect(const char* url, int frameRate);

 /**
 * 将一段H.264编码的视频文件利用RTMP协议发送到服务器
 *
 * @param char *buf 要发送的数据
 * @param char *buf_size 要发送的数据长度
 * @发送成功则返回1 , 发送失败则返回0
 */ 
int myRTMP264_SendBuffer(unsigned char *buf, int buf_size);

/**
 * 断开连接，释放相关的资源。
 *
 */    
void RTMP264_Close();  

#ifdef __cplusplus
}
#endif

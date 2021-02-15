#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/file.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <string.h>
#include <stdarg.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <signal.h>
#include <sys/wait.h>
#include <regex.h> 
#include <alsa/asoundlib.h>

#define PCM_DEVICE "default"
#define TS_PACKET_SIZE 188
#define MTU 1500

struct rtp_header{  
    unsigned char cc:4;  
    unsigned char x:1;  
    unsigned char p:1;  
    unsigned char v:2;       
    unsigned char pt:7;  
    unsigned char m:1;  
    unsigned short sequence_number;  
    unsigned int timestamp;  
    unsigned int ssrc;  
};  
 
void main(int argc, char **argv)  
{  
  
    int socka;       
    int nPortA = 8000;  
    fd_set rfd;     // 读描述符集  
    struct timeval timeout;    // 定时变量  
    struct sockaddr_in addr; // 告诉sock 应该在什么地方licence  
    char recv_buff[MTU];    // 接收缓冲区  
    
      
    int nRecLen; // 客户端地址长度!!!!!!  
      
    struct sockaddr_in cli;    // 客户端地址  
    int nRet; // select返回值  
 
    socka = socket(AF_INET, SOCK_DGRAM, 0); // 创建数据报socka  
    if (socka == -1)  
    {  
        printf("socket()/n");  
        return;  
    }  
 
    memset(&addr, 0, sizeof(addr));  
      
    addr.sin_family = AF_INET;   // IP协议  
    addr.sin_port = htons(nPortA); // 端口  
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 在本机的所有ip上开始监听  
    if (bind(socka, (struct sockaddr*)&addr, sizeof(addr)) == -1)// bind socka  
    {  
        printf("bind()/n");  
        return;  
    }  
      
 
    // 设置超时时间为6s  
    timeout.tv_sec = 6;   
    timeout.tv_usec = 0;  
      
    memset(recv_buff, 0, sizeof(recv_buff)); // 清空接收缓冲区  

    //0.设置参数
    unsigned int pcm, tmp, dir;              
	int rate=44100, channels=2;
	snd_pcm_t *pcm_handle; //typedef struct _snd_pcm
	char *buff;
	int buff_size;
    int count=0;
	snd_pcm_uframes_t frames; //typedef unsigned long
	

    //1.打开音频播放设备
	snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0); // 这里是以音频播放模式打开
   

    //2.使用snd_pcm_hw_params_t配置硬件参数
    snd_pcm_hw_params_t *params;
	snd_pcm_hw_params_malloc(&params); // 或者 snd_pcm_hw_params_alloca(&hwparams); 
	snd_pcm_hw_params_any(pcm_handle, params); // 使用pcm设备初始化hwparams

	//snd_pcm_hw_params_set_xxx(pcm, hwparams, ...);  通过一系列的set函数来设置硬件参数中的基本参数
	//如， 通道数，采样率， 采样格式等等
    /* Set parameters */
	if (pcm = snd_pcm_hw_params_set_access(pcm_handle, params,
					SND_PCM_ACCESS_RW_INTERLEAVED) < 0) 
		printf("ERROR: Can't set interleaved mode. %s\n", snd_strerror(pcm));

	if (pcm = snd_pcm_hw_params_set_format(pcm_handle, params,
						SND_PCM_FORMAT_S16_LE) < 0) 
		printf("ERROR: Can't set format. %s\n", snd_strerror(pcm));

	if (pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, channels) < 0) 
		printf("ERROR: Can't set channels number. %s\n", snd_strerror(pcm));

	if (pcm = snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0) < 0) 
		printf("ERROR: Can't set rate. %s\n", snd_strerror(pcm));

	// write paras
	
    if (pcm = snd_pcm_hw_params(pcm_handle, params) < 0)
		printf("ERROR: Can't set harware parameters. %s\n", snd_strerror(pcm));
	//snd_pcm_hw_params_free(params); // 释放不再使用的hwparams空间

    //3. 使用snd_pcm_sw_params_t配置一些高级软件参数， 比如使用中断模式等等
    snd_pcm_hw_params_get_period_size(params, &frames, 0);
    buff_size = frames * channels * 2 /* 2 -> sample size */;
	buff = (char *) malloc(buff_size);
    printf("frames = %lu.\n",frames);
    snd_pcm_hw_params_get_period_time(params, &tmp, NULL);
    //4. 调用snd_pcm_prepare(pcm)来使音频设备准备好接收pcm数据
    if(snd_pcm_prepare(pcm_handle) < 0)  
	{
		snd_pcm_close(pcm_handle);
		exit(1);
	}
   
    while (1)  
    {  
        FD_ZERO(&rfd); // 在使用之前总是要清空  
          
        // 开始使用select  
        FD_SET(socka, &rfd); // 把socka放入要测试的描述符集中  
  
          
        nRet = select(socka+1, &rfd, NULL, NULL, &timeout);// 检测是否有套接口是否可读  
        if (nRet == -1)     
        {  
            printf("select()\n");  
            return;  
        }  
        else if (nRet == 0) // 超时  
        {  
            //printf("timeout\n");  
			continue; 
        }  
        else    // 检测到有套接口可读  
        {  
            if (FD_ISSET(socka, &rfd))  // socka可读  
            {  
                nRecLen = sizeof(cli);  
                int nRecEcho = recvfrom(socka, recv_buff, sizeof(recv_buff), 0, (struct sockaddr*)&cli, &nRecLen);  
                if (nRecEcho == -1)  
                {  
                    printf("recvfrom()\n");  
                    break;  
                }
                //start
                
                    //把数据从rece-buf 送入声卡buffer
                    //5. 循环调用snd_pcm_writei(pcm, buffer, frame)来往音频设备写数据

                    // if (pcm = read(0, buff, buff_size) == 0) {
                    //     printf("Early end of file.\n");
                    //     return 0;
                    // }

                    if (pcm = snd_pcm_writei(pcm_handle, recv_buff, frames) == -EPIPE) {
                        printf("XRUN.\n");
                        snd_pcm_prepare(pcm_handle);
                    } else if (pcm < 0) {
                        printf("ERROR. Can't write to PCM device. %s\n", snd_strerror(pcm));
                    }

                    
                
                
                
                struct rtp_header *p = (struct rtp_header *)recv_buff;
				printf("v = %d\n",p->v);
				printf("p = %d\n",p->p);
				printf("x = %d\n",p->x);
				printf("cc = %d\n",p->cc);
				printf("sequence_number = %d\n",ntohs(p->sequence_number));
				printf("ssrc = %d\n",ntohs(p->ssrc));
            }  
 
        }  
    }  
} 

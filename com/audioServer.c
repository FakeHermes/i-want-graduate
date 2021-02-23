#include <stdio.h>
#include <unistd.h> 
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include<arpa/inet.h>
 
#define TS_PACKET_SIZE 188
#define MTU 4284
 
 
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
 
void init_rtp_header(struct rtp_header *h){  
	h->v = 2;  
	h->p = 0;  
	h->x = 0;  
	h->cc = 0;  
	h->m = 0;  
	h->pt = 33;  
	h->sequence_number =0;  
	h->timestamp = 123;  
	h->ssrc =0;  
}  
 
 
void sequence_number_increase(struct rtp_header *header){  
	unsigned short sequence = ntohs(header->sequence_number);  
	sequence++;  
	header->sequence_number = htons(sequence);  
} 
 
 
 
int main(int argc,char *argv[]){  
	char buf[MTU];  
	unsigned int count = 0;  
	if(argc < 2){
		printf(" need wav path \n");
		return -1;
	}
 
	// Init RTP Header  
	init_rtp_header((struct rtp_header*)buf);  
	count = sizeof(struct rtp_header);  
 	
 	
	// Init socket  
	int sock = socket(AF_INET, SOCK_DGRAM, 0);  
	struct sockaddr_in dest_addr;  
 	
	dest_addr.sin_family=AF_INET;  
	dest_addr.sin_port = htons(8000);  
	//dest_addr.sin_addr.s_addr =inet_addr("10.46.169.189");
 	dest_addr.sin_addr.s_addr =inet_addr("127.0.0.1");
 	
	//	dest_addr.sin_addr.s_addr =INADDR_ANY;
	bzero(&(dest_addr.sin_zero),8);  
 
	// Open TS file  
	FILE *ts_file = fopen(argv[1], "r+"); 
        char h[44];
	//fread(h,1,44,ts_file);
        int n=0;
    
	while(!feof(ts_file)){  
		int read_len = fread(buf+count, 1, TS_PACKET_SIZE, ts_file);  
		//	if(*(buf+count) != 0x47){  
		//		fprintf(stderr, "Bad sync header!\n");  
		//		continue;  
		//	}  
		count += read_len;  
 
		//	printf("count = %d\n",count);
		if (count + TS_PACKET_SIZE > MTU){// We should send  
			printf("haha_count = %d\n",count);
			sequence_number_increase((struct rtp_header*)buf);  
			sendto(sock, buf, count, 0, (const struct sockaddr*)&dest_addr, sizeof(dest_addr));  
			count = sizeof(struct rtp_header);  
			usleep(10000);  
	       n++;	
		} 
	}  
	printf("numofcount:%d\n",n*1316);
	printf("end");
 
	fclose(ts_file); 
}

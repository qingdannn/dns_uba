// dns_uba.cpp : 此文件包含 "main" 函数。程序执行将在此处开始并结束。
//

#include "pch.h"

#define DNSPORT 53
using namespace std;

FILE* fp = NULL;
MYSQL *con;
char i_query[3000];//sql语句
char dbhost[30] = "localhost";
char dbuser[30] = "root";
char dbpasswd[30] = "123456";
void finish_with_error(MYSQL *con);//数据库报错
int num=0;

/* 6字节的MAC地址 */
typedef struct mac_address {
	u_char byte1;
	u_char byte2;
	u_char byte3;
	u_char byte4;
	u_char byte5;
	u_char byte6;
}mac_address;

/* 4字节的IP地址 */
typedef struct ip_address {
	u_char byte1;
	u_char byte2;
	u_char byte3;
	u_char byte4;
}ip_address;

/* MAC 首部 */
typedef struct mac_header {
	mac_address daddr;//目的MAC地址
	mac_address saddr;//源MAC地址
	u_short ty_len;//MAC首部长度或类型
}mac_header;

class MAC
{
public:
	mac_header * mh;
	u_char entype;//封装格式(encapsulation type)以太网为0；IEEE802.3为1
};

/* IPv4 首部 */
typedef struct ip_header {
	union ver_ihl //  版本号（4位）+ IP首部长度(4位)
	{
		struct {
			u_char ihl : 4;//IP首部长度(Datagram length)
			u_char ver : 4;//版本号（version）
		}half_byte;
		u_char word;
	} ver_ihl;
	u_char tos; // 服务类型(Type of service) 
	u_short tlen; // 总长(Total length) 
	u_short identification; // 标识(Identification) 
	u_short flags_fo; // 标志位(Flags) (3 bits) + 段偏移量(Fragment offset) (13 bits) 
	u_char ttl; // 存活时间(Time to live) 
	u_char proto; // 协议(Protocol) 
	u_short crc; // 首部校验和(Header checksum) 
	ip_address saddr; // 源地址(Source address) 
	ip_address daddr; // 目的地址(Destination address) 
	u_int op_pad; // 选项与填充(Option + Padding) 
}ip_header;

/* UDP 首部 / u_char占一个字节，u_short占2个字节*/
typedef struct udp_header {
	u_short sport; // 源端口(Source port) 
	u_short dport; // 目的端口(Destination port) 
	u_short len; // UDP数据包长度:UDP首部+UDP数据(Datagram length) 
	u_short crc; // 校验和(Checksum) 
}udp_header;

/* TCP 首部 / u_char占一个字节，u_short占2个字节*/
typedef struct tcp_header {
	u_short sport; // 源端口(Source port) 
	u_short dport; // 目的端口(Destination port) 
	u_int order;// 序号(Order)
	u_int confirmation;// 确认号（Confirmation order）
	union thl_retain //  TCP首部长度(4位)+ 保留字（4位）
	{
		struct {
			unsigned char a : 4;
			unsigned char thl : 4;//TCP首部长度(Datagram length)
		}half_byte;
		u_char word;
	} thl_retain;
	u_char symbol;//保留字（2位）+标志（6位）
	u_short window;//窗口大小
	u_short crc; // 校验和(Checksum)
	u_short up; //紧急指针（urgent pointer） 
}tcp_header;

typedef struct dns_packet //报文head+data
{
	u_short id;//每一个占2个字节，共12个字节
	union flags//标志第一个为0代表查询报文
	{
		struct {
			u_char rcode : 4;//0为无差错，3为名字差错
			u_char zero : 3;//0
			u_char RA : 1;//可用递归——支持递归查询
			u_char RD : 1;//期望递归——递归/迭代查询
			u_char TC : 1;//可截断（truncated）只返回前512字节
			u_char AA : 1;//授权回答（authoritative answer）
			u_char opcode : 4;//0为标准查询
			u_char QR : 1;//0为查询，1为响应
		}short_len;
		u_short flag_only;
	}flags;
	u_short quecount;
	u_short anscount;
	u_short authorcount;
	u_short additioncount;
	u_char dns_data; //内容头部
}dns_packet;

/* 回调函数原型 */
void packet_handler(u_char *param, const struct pcap_pkthdr *header, const u_char *pkt_data);

struct user_param
{
	pcap_dumper_t *user_dumpfile;
	int  max_num;//需要捕获的数据包个数
	int  cur_num;//当前数据包个数
	pcap_t *p;
};

int main(int argc, char **argv)
{
	pcap_if_t *alldevs;
	pcap_if_t *d;
	u_int netmask;
	int inum;
	int i = 0;
	char errbuf[PCAP_ERRBUF_SIZE];
	struct user_param uparam;
	pcap_t * &adhandle = uparam.p;
	struct bpf_program fcode;
	uparam.max_num = 10;//默认为10
	uparam.cur_num = 0;
	char packet_filter[] = "(port DNSPORT) and (ip or udp)"; //过滤

	/*设置需要捕获的数据包个数*/
	if (argc == 2)
	{
		uparam.max_num = atoi(argv[1]);
	}
	if (uparam.max_num <= 0)
	{
		fprintf(stderr, "Error in packet number: %d\n", uparam.max_num);
		exit(1);
	}

	/* 获取本机设备列表 */
	if (pcap_findalldevs_ex((char *)PCAP_SRC_IF_STRING, NULL, &alldevs, errbuf) == -1)
	{
		fprintf(stderr, "Error in pcap_findalldevs: %s\n", errbuf);
		exit(1);
	}

	/* 打印列表 */
	for (d = alldevs; d; d = d->next)
	{
		printf("%d. %s", ++i, d->name);
		if (d->description)
			printf(" (%s)\n", d->description);
		else
			printf(" (No description available)\n");
	}

	if (i == 0)
	{
		printf("\nNo interfaces found! Make sure WinPcap is installed.\n");
		return -1;
	}
	/*选择设备*/
	printf("Enter the interface number (1-%d):", i);
	scanf("%d", &inum);

	if (inum < 1 || inum > i)
	{
		printf("\nInterface number out of range.\n");
		/* 释放列表 */
		pcap_freealldevs(alldevs);
		return -1;
	}

	/* 跳转到选中的适配器 */
	for (d = alldevs, i = 0; i < inum - 1; d = d->next, i++);


	/* 打开适配器 */
	if ((uparam.p = pcap_open(d->name,          // 设备名
		65536,            // 要捕捉的数据包的部分 
						  // 65535保证能捕获到不同数据链路层上的每个数据包的全部内容
		PCAP_OPENFLAG_PROMISCUOUS,    // 混杂模式
		1000,             // 读取超时时间
		NULL,             // 远程机器验证
		errbuf            // 错误缓冲池
	)) == NULL)
	{
		fprintf(stderr, "\nUnable to open the adapter. %s is not supported by WinPcap\n", d->name);
		/* 释放设备列表 */
		pcap_freealldevs(alldevs);
		return -1;
	}

	if (d->addresses != NULL)
	{
		/* 获取接口第一个地址的掩码 */
		netmask = ((struct sockaddr_in *)(d->addresses->netmask))->sin_addr.S_un.S_addr;
	}
	else
		/* 如果这个接口没有地址，那么我们假设这个接口在C类网络中 */
		netmask = 0xffffff;

	/*编译过滤器为接收DNS协议的数据包*/
	if (pcap_compile(adhandle, &fcode, "(port 53) and (ip or udp)", 1, netmask) < 0)
	{
		fprintf(stderr, "\nUnable to compile the packet filter. Check the syntax.\n");
		pcap_close(adhandle);
		pcap_freealldevs(alldevs);
		return -1;
	}

	/*设置过滤器*/
	if (pcap_setfilter(adhandle, &fcode) < 0)
	{
		fprintf(stderr, "\nError setting the filter.\n");
		pcap_freecode(&fcode);
		pcap_close(adhandle);
		pcap_freealldevs(alldevs);
		return -1;
	}

	/* 打开数据库 */
	con = mysql_init(NULL);
	if (con == NULL)
	{
		fprintf(stderr, "%s\n", mysql_error(con));
		system("pause");
		return -1;
	}

	if (mysql_real_connect(con, dbhost, dbuser, dbpasswd, NULL, 0, NULL, 0) == NULL)
	{
		fprintf(stderr, "%s\n", mysql_error(con));
		mysql_close(con);
		system("pause");
		return -1;
	}

	printf("\nlistening on %s... Press Ctrl+C to stop...\n", d->description);

	/* 释放设备列表 */
	pcap_freealldevs(alldevs);

	/* 开始捕获 */
	pcap_loop(uparam.p, 0, packet_handler, (unsigned char *)&uparam);

	pcap_close(uparam.p);
	mysql_close(con);

	printf("%s个数据包已捕捉完成\n", argv[1]);
	printf("\n------------------------");
	printf("\n-----开始整合数据-----");
	exit(0);
}

/* 回调函数，用来处理数据包 */
void packet_handler(u_char *param, const struct pcap_pkthdr *header, const u_char *pkt_data)
{
	struct user_param *puser_param = (struct user_param*)param;
	struct tm *ltime; //定义时间
	char timestr[16];
	char timestr1[32];
	MAC mac;
	ip_header *ih = NULL;
	udp_header *uh = NULL;
	tcp_header *th = NULL;
	struct dns_packet *pdns = NULL;
	u_int ip_len;
	u_short sport, dport;
	time_t local_tv_sec;
	time_t local_time;

	puser_param->cur_num++;
	fprintf(stderr, "cur_num:%d\n", puser_param->cur_num);
	if (puser_param->cur_num + 1 > puser_param->max_num)
	{
		pcap_breakloop(puser_param->p);
	}

	/* 将时间戳转换成可识别的格式 */ //二进制转换
	local_tv_sec = header->ts.tv_sec;
	local_time = time(NULL); //获取当前日期
	strftime(timestr1, sizeof(timestr1), "%Y-%m-%d", localtime(&local_time));
	strftime(timestr, sizeof(timestr), "%H:%M:%S", localtime(&local_tv_sec));

	/* 打印数据包的时间戳和长度 */
	printf("%s.%d / len:%d /\n", timestr, header->ts.tv_usec, header->len); //显示时间和数据包长度

	/* 获得MAC数据包头部的位置 / 获得IP数据包头部的位置 */
	mac.mh = (mac_header *)(pkt_data); //MAC头部长度

	printf("%d-%d-%d-%d-%d-%d-> %d-%d-%d-%d-%d-%d\n",
		mac.mh->saddr.byte1, mac.mh->saddr.byte2, mac.mh->saddr.byte3, mac.mh->saddr.byte4, mac.mh->saddr.byte5, mac.mh->saddr.byte6,
		mac.mh->daddr.byte1, mac.mh->daddr.byte2, mac.mh->daddr.byte3, mac.mh->daddr.byte4, mac.mh->daddr.byte5, mac.mh->daddr.byte6);

	/*判断MAC报文格式及后续报文内容*/
	if (mac.mh->ty_len == 8)//ty IP报文 0800=8
	{
		mac.entype = 0;//以太网
		mac.mh->ty_len = 14;//len=14
	}
	else if ((mac.mh->ty_len == 264) || (mac.mh->ty_len == 13576))//ty 非IP报文 0806=264,0835=13576
		return;
	else
	{
		mac.entype = 1;//IEEE 802
		mac.mh->ty_len = *(pkt_data + 20);//ty
		if ((mac.mh->ty_len == 264) || (mac.mh->ty_len == 13576))//ty 非IP报文 0806=264,0835=13576
			return;
		else
			mac.mh->ty_len = 22;//len
	}

	/* 获得IP数据包头部的位置 */
	ih = (ip_header *)(pkt_data + mac.mh->ty_len); //以太网头部长度

	/*TCP/UDP*/
	if ((int)ih->proto == 17)
	{
		/* 获得UDP首部的位置 */
		uh = (udp_header *)(pkt_data + (4 * (int)ih->ver_ihl.half_byte.ihl) + mac.mh->ty_len);

		/* 将网络字节序列转换成主机字节序列 */
		sport = ntohs(uh->sport);
		dport = ntohs(uh->dport);

		/* 获得DNS首部的位置 */
		pdns = (struct dns_packet *)(pkt_data + (4 * (int)ih->ver_ihl.half_byte.ihl) + mac.mh->ty_len + 8); // sport+dport+length+checksum,DNS头指针
	}
	if ((int)ih->proto == 6)
	{
		/* 获得TCP首部的位置 */
		th = (tcp_header *)(pkt_data + (4 * (int)ih->ver_ihl.half_byte.ihl) + mac.mh->ty_len);

		/* 将网络字节序列转换成主机字节序列 */
		sport = ntohs(th->sport);
		dport = ntohs(th->dport);

		/* 获得DNS首部的位置 */
		pdns = (struct dns_packet *)(pkt_data + (4 * (int)ih->ver_ihl.half_byte.ihl) + mac.mh->ty_len + (int)th->thl_retain.half_byte.thl); // sport+dport+length+checksum,DNS头指针
		//printf("TCP:%p\n", pdns);
	}

	/*找到响应DNS包*/
	if (pdns->flags.short_len.QR == 1)//0查询 1响应
		return;

	/* 打印IP地址和UDP端口 */
	printf(" %d.%d.%d.%d:%d -> %d.%d.%d.%d:%d\n",
		ih->saddr.byte1,
		ih->saddr.byte2,
		ih->saddr.byte3,
		ih->saddr.byte4,
		sport,
		ih->daddr.byte1,
		ih->daddr.byte2,
		ih->daddr.byte3,
		ih->daddr.byte4,
		dport); //显示3

	/*DNS部分*/
	u_char *query = &(pdns->dns_data);//定位到问题部分
	u_char domainname[100] = { 0 };
	u_int i = 0;
	query++;//把点去了
	while (*query)
	{
		if (*query < 0x10)//48以后出现数字和英文字母
		{
			domainname[i] = '.';
		}
		else
		{
			domainname[i] = *query;
		}
		query++;
		i++;
	}

	/*写入数据库*/
	strcpy(i_query, "");
	sprintf(i_query, "INSERT INTO dns_uba.package (dns_domainname,ip_saddr,ip_daddr,mac_saddr,mac_daddr,request_time) VALUES ('%s','%d.%d.%d.%d','%d.%d.%d.%d','%d-%d-%d-%d-%d-%d','%d-%d-%d-%d-%d-%d','%s %s.%.2d')",
		domainname, ih->saddr.byte1, ih->saddr.byte2, ih->saddr.byte3, ih->saddr.byte4, ih->daddr.byte1, ih->daddr.byte2, ih->daddr.byte3, ih->daddr.byte4,
		mac.mh->saddr.byte1, mac.mh->saddr.byte2, mac.mh->saddr.byte3, mac.mh->saddr.byte4, mac.mh->saddr.byte5, mac.mh->saddr.byte6,
		mac.mh->daddr.byte1, mac.mh->daddr.byte2, mac.mh->daddr.byte3, mac.mh->daddr.byte4, mac.mh->daddr.byte5, mac.mh->daddr.byte6,
		timestr1, timestr, header->ts.tv_usec);
	if (mysql_query(con, i_query))
	{
		finish_with_error(con);
	}
}

void finish_with_error(MYSQL *con)
{
	fprintf(stderr, "%s\n", mysql_error(con));
	system("pause");
	mysql_close(con);
	exit(1);
}

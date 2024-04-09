// g++ client.cpp -o client.exe -lwsock32
#include<WinSock2.h>//Windows Sockets 或简称 Winsock,网络通信协议的开发接口
#include<iostream>
#include<ws2tcpip.h>//Windows 套接字 2 使用此标头
#include<cstdlib>
#include<time.h>
#include<cstdio>
#include<string.h>
#include<windows.h>
#include<WinBase.h>
#include <thread>
#pragma comment(lib,"ws2_32.lib")//链接Ws2_32.lib这个库
const int tries_limit = 10;
const int time_limit = 1000;//单位是ms
using namespace std;


int getUdpSocket() {//生成并初始化一个UDPsocket
    WORD ver = MAKEWORD(2, 2); // 指定使用Winsock 2.2版本
    WSADATA lpData;
    int err = WSAStartup(ver, &lpData); // 初始化Winsock库
    if (err != 0)
        return -1; // 初始化失败，返回错误码

    int udpsocket = socket(AF_INET, SOCK_DGRAM, 0); // 创建UDP socket
    if (udpsocket == INVALID_SOCKET) {
        WSACleanup(); // socket创建失败，清理Winsock资源
        return -2; // 返回错误码
    }

    return udpsocket; // 返回创建的UDP socket
}


sockaddr_in getAddr(const char* ip, int port) {//根据输入的IP地址和端口号构造出一个存储着地址的sockaddr_in类型
    sockaddr_in addr;
    addr.sin_family = AF_INET; // 协议族，在socket编程中只能是AF_INET
    addr.sin_port = htons(port); // 端口号，需要使用htons函数将主机字节序转换为网络字节序
    addr.sin_addr.S_un.S_addr = inet_addr(ip); // IP地址，使用inet_addr函数将点分十进制的IP地址转换为网络字节序的32位整数
    return addr;
}


char* RequestPack(char* content, int& datalen, int type, int choice) {//构造请求包
    // content: 被请求的文件名
    // datalen: 返回数据包的长度
    // type: 数据包类型，5表示"octet"，其他表示"netascii"
    // choice: 上传是2，下载是1

    int len = strlen(content);
    char* buf = new char[len + 2 + 2 + type]; // 开辟缓冲区buf，长度为文件名长度 + 2（0x00和choice） + 2（文件模式字符串长度） + type
    buf[0] = 0x00; // WRQ/RRQ数据包的第一个字节，总是0
    buf[1] = choice; // choice为1表示下载，为2表示上传

    // 将被请求的文件名放入WRQ/RRQ数据包
    memcpy(buf + 2, content, len);

    // 将字符串结束符"\0"放入WRQ/RRQ数据包
    memcpy(buf + 2 + len, "\0", 1);

    // 根据type选择文件模式，5表示"octet"，其他表示"netascii"
    if (type == 5)
        memcpy(buf + 2 + len + 1, "octet", 5);
    else
        memcpy(buf + 2 + len + 1, "netascii", 8);

    // 将字符串结束符"\0"放入WRQ/RRQ数据包
    memcpy(buf + 2 + len + 1 + type, "\0", 1);

    // 计算数据包长度
    datalen = len + 2 + 1 + type + 1;

    return buf; // 返回构造好的WRQ/RRQ数据包
}


char* AckPack(short& no) {//构造ACK数据包
    // no: ACK数据包的块编号
    char* ack = new char[4]; // 开辟缓冲区ack，长度为4（2字节0x00和0x04，2字节块编号）

    ack[0] = 0x00; // 第一个字节总是0
    ack[1] = 0x04; // 第二个字节表示ACK的操作码

    no = htons(no); // 将16位块编号从主机字节序转换为网络字节序
    memcpy(ack + 2, &no, 2); // 将转换后的块编号复制到ACK数据包中

    no = ntohs(no); // 将块编号从网络字节序转换回主机字节序

    return ack; // 返回构造好的ACK数据包
}


char* MakeData(short& no, FILE* f, int& datalen) {//制作DATA数据包
    // no: DATA数据包的块编号
    // f: 文件指针，指向待发送的文件
    // datalen: 返回数据包的长度

    char temp[512]; // 临时缓冲区，用于读取文件内容
    int sum = fread(temp, 1, 512, f); // 从文件读取最多512字节的内容

    if (!ferror(f)) { // 检查文件读取是否出错
        char* buf = new char[4 + sum]; // 开辟缓冲区buf，长度为4（2字节0x00和0x03，2字节块编号）+文件内容长度
        buf[0] = 0x00; // 第一个字节总是0
        buf[1] = 0x03; // 第二个字节表示DATA的操作码

        no = htons(no); // 将16位块编号从主机字节序转换为网络字节序
        memcpy(buf + 2, &no, 2); // 将转换后的块编号复制到DATA数据包中
        no = ntohs(no); // 将块编号从网络字节序转换回主机字节序

        memcpy(buf + 4, temp, sum); // 将文件内容复制到DATA数据包中

        datalen = sum + 4; // 计算数据包长度
        return buf; // 返回构造好的DATA数据包
    } else {
        return NULL; // 文件读取出错，返回NULL
    }
}


void print_time(FILE* fp) {//按照“年-月-日-时-分-秒”输出当前时间，用于构造日志文件
    // fp: 文件指针，指向日志文件

    time_t t;
    time(&t); // 获取当前时间

    char stime[100];
    strcpy(stime, ctime(&t)); // 将当前时间转化成人类易读的时间格式

    *(strchr(stime, '\n')) = '\0'; // 去掉ctime转化后结尾的回车符

    fprintf(fp, "[ %s ] ", stime); // 将格式化后的时间输出到日志文件中

    return;
}


int get_choice(sockaddr_in& addr, char* name, int& type) {//得到操作选择
    // addr: 用于存储服务器地址信息的结构
    // name: 用于存储文件名的字符数组
    // type: 用于存储文件传输方式的变量

    string s[] = {"", "下载", "上传"};
    int ch;

    printf("+-------------------------------------------+\n");
    printf("|  1.下载文件            2.上传文件         |\n");
    printf("|  0.关闭TFTP客户端                         |\n");
    printf("+-------------------------------------------+\n");

    scanf("%d", &ch);

    if (ch == 0)
        return 0; // 如果用户选择关闭TFTP客户端，则返回0

    cout << "请输入服务器ip：" << endl;
    char ip_s[50];
    cin >> ip_s;
    addr = getAddr(ip_s, 69); // 获取服务器地址信息

    cout << "请输入要" << s[ch] << "的文件全名：" << endl;
    scanf("%s", name); // 获取文件名

    cout << "请选择" << s[ch] << "文件的方式：1.netascii 2.octet" << endl;
    scanf("%d", &type);

    if (type == 1)
        type = 8; // 如果选择netascii，将type设为8
    else
        type = 5; // 如果选择octet，将type设为5

    return ch; // 返回用户的操作选择
}

void work(){
    FILE* fp = fopen("TFTP_client.log", "a");//日志
    char commonbuf[2048];
    int buflen;
    int Numbertokill;
    int Killtime;
    clock_t start, end; //记录传输开始和结束时间
    double runtime;
    SOCKET sock = getUdpSocket();
    sockaddr_in addr;
    int recvTimeout = time_limit; //单位是ms
    int sendTimeout = time_limit;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&recvTimeout, sizeof(int));//设置读取超时时间
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (char*)&sendTimeout, sizeof(int));//发送超时时间
    // printf("begin\n");
    while (1) {
        int type,datalen;
        char name[1000],op[]={'\0','R','W'};
        int choice = get_choice(addr,name,type);
        if (choice == 0) break;//选择0--结束
        char* sendData = RequestPack(name, datalen, type, choice);//构造请求包
        buflen = datalen;
        Numbertokill = 1;
        memcpy(commonbuf, sendData, datalen);
        int res = sendto(sock, sendData, datalen, 0, (sockaddr*)&addr, sizeof addr);//第一次发送RRQ/WRQ包
        start = clock();//计时开始
        print_time(fp);//日志
        fprintf(fp, "send %cRQ for file: %s\n", op[choice], name);//日志
        Killtime = 1;   //sendto超时的次数，与recv_from超时分开计算
        while (res != datalen) {//发送RRQ或者WRQ
            cout << "send "<<op[choice]<<"RQ failed: " << Killtime << "times" << endl;
            if (Killtime <= tries_limit) {   //失败10次自动结束传输
                res = sendto(sock, commonbuf, buflen, 0, (sockaddr*)&addr, sizeof addr);//重复发送请求包
                Killtime++;
            }
            else break;//结束
        }
        if (Killtime > tries_limit) continue;//没有continue说明发送成功
        delete[]sendData;
        //至此已经成功发送请求

        //选择1--下载文件
        if (choice == 1) {  
            //打开文件
            FILE* f = NULL;
            if(type == 5) f = fopen(name, "wb");
            else f = fopen(name, "w");
            if (f == NULL) {
                cout << "File " << name << " open failed!" << endl;
                continue;
            }
            int want_recv = 1;
            int RST = 0;
            int Fullsize = 0;
            //开始下载
            while (1) {
                char buf[1024];
                sockaddr_in server;
                int len = sizeof(server);
                res = recvfrom(sock, buf, 1024, 0, (sockaddr*)&server, &len); //从server反馈数据包中获得其分配的端口号信息
                if (res == -1) {//没收到
                    if (Numbertokill > tries_limit) {
                        printf("No block get.transmission failed.\n");
                        print_time(fp);//日志
                        fprintf(fp, "Download file: %s failed.\n", name);//日志
                        break;
                    }
                    int res = sendto(sock, commonbuf, buflen, 0, (sockaddr*)&addr, sizeof addr);
                    RST++;
                    cout << "resend last blk" << endl;
                    Killtime = 1;   //同上
                    while (res != buflen) {
                        cout << "resend last blk failed: " << Killtime << "times" << endl;
                        if (Killtime <= tries_limit) {
                            res = sendto(sock, commonbuf, buflen, 0, (sockaddr*)&addr, sizeof addr);
                            Killtime++;
                        }
                        else break;
                    }
                    if (Killtime > tries_limit) break;
                    Numbertokill++;
                }
                //收到，构造ack包
                if (res > 0) {
                    short flag;
                    memcpy(&flag, buf, 2);
                    flag = ntohs(flag);
                    if (flag == 3) {    //DATA数据包
                        addr = server;
                        short no;
                        memcpy(&no, buf + 2, 2);
                        no = ntohs(no);
                        cout << "Pack No=" << no << endl;
                        char* ack = AckPack(no);
                        int sendlen = sendto(sock, ack, 4, 0, (sockaddr*)&addr, sizeof addr);
                        Killtime = 1;
                        while (sendlen != 4) {
                            cout << "resend last ack failed: " << Killtime << " times" << endl;
                            if (Killtime <= tries_limit) {
                                sendlen = sendto(sock, ack, 4, 0, (sockaddr*)&addr, sizeof addr);
                                Killtime++;
                            }
                            else break;
                        }
                        if (Killtime > tries_limit)break;
                        if (no == want_recv) {
                            buflen = 4;
                            Numbertokill = 1;
                            memcpy(commonbuf, ack, 4);
                            fwrite(buf + 4, res - 4, 1, f);
                            Fullsize += res - 4;
                            if (res - 4 >= 0 && res - 4 < 512) {
                                cout << "download finished!" << endl;
                                end = clock();
                                runtime = (double)(end - start) / CLOCKS_PER_SEC;
                                print_time(fp);//日志
                                printf("Average transmission rate: %.2lf kb/s\n", Fullsize / runtime / 1000);
                                fprintf(fp, "Download file: %s finished.resent times: %d;Fullsize: %d\n", name, RST, Fullsize);//日志
                                break;
                            }
                            want_recv++;
                        }
                    }
                    if (flag == 5) {    //ERROR
                        short errorcode;
                        memcpy(&errorcode, buf + 2, 2);
                        errorcode = ntohs(errorcode);
                        char strError[1024];
                        int iter = 0;
                        while (*(buf + iter + 4) != 0) {
                            memcpy(strError + iter, buf + iter + 4, 1);
                            iter++;
                        }
                        *(strError + iter + 1) = '\0';
                        cout << "Error" << errorcode << " " << strError << endl;
                        print_time(fp);//日志
                        fprintf(fp, "Error %d %s\n", errorcode, strError);//日志
                        break;
                    }
                }
            }
            fclose(f);
        }
        //选择2--上传文件
        if (choice == 2) {
            //打开文件
            FILE* f = NULL;
            if(type == 5)f = fopen(name, "rb");
            else f = fopen(name, "r");
            if (f == NULL) {
                cout << "File " << name << " open failed!" << endl;
                continue;
            }
            //开始发送
            short block = 0;
            datalen = 0;
            int RST = 0;    //重传次数
            int Fullsize = 0;
            while (1) {
                char buf[1024];
                sockaddr_in server;   //从server反馈数据包中获得其分配的端口号信息
                int len = sizeof(server);
                res = recvfrom(sock, buf, 1024, 0, (sockaddr*)&server, &len);//接受信息
                //若没收到ack
                if (res == -1) {
                    printf("%d ", Numbertokill);
                    if (Numbertokill > tries_limit) { //连续10次没有收到回应
                        printf("No acks get.transmission failed.\n");
                        print_time(fp);//日志
                        fprintf(fp, "Upload file: %s failed.\n", name);//日志
                        break;
                    }
                    int res = sendto(sock, commonbuf, buflen, 0, (sockaddr*)&addr, sizeof addr);
                    RST++;
                    cout << "resend last blk" << endl;
                    Killtime = 1;   //同上
                    while (res != buflen) {
                        cout << "resend last blk failed: " << Killtime << "times" << endl;
                        if (Killtime <= tries_limit) {
                            res = sendto(sock, commonbuf, buflen, 0, (sockaddr*)&addr, sizeof addr);
                            Killtime++;
                        }
                        else break;
                    }
                    if (Killtime > tries_limit) break;
                    Numbertokill++;
                }
                //若收到ack
                if (res > 0) {   
                    short flag;
                    memcpy(&flag, buf, 2);
                    flag = ntohs(flag);
                    if (flag == 4) {    //ACK数据包
                        short no;
                        memcpy(&no, buf + 2, 2);
                        no = ntohs(no);
                        if (no == block) {
                            addr = server;
                            if (feof(f) && datalen != 516) {
                                cout << "upload finished!" << endl;
                                end = clock();
                                runtime = (double)(end - start) / CLOCKS_PER_SEC;
                                print_time(fp);//日志
                                printf("Average transmission rate: %.2lf kb/s\n", Fullsize / runtime / 1000);
                                fprintf(fp, "Upload file: %s finished.resent times: %d;Fullsize: %d\n", name, RST, Fullsize);//日志
                                break;
                            }
                            block++;
                            sendData = MakeData(block, f, datalen);
                            buflen = datalen;
                            Fullsize += datalen - 4;
                            Numbertokill = 1;
                            memcpy(commonbuf, sendData, datalen);
                            if (sendData == NULL) {
                                cout << "File reading mistakes!" << endl;
                                break;
                            }
                            int res = sendto(sock, sendData, datalen, 0, (sockaddr*)&addr, sizeof addr);
                            Killtime = 1;
                            while (res != datalen) {
                                cout << "send block " << block << "failed: " << Killtime << "times" << endl;
                                if (Killtime <= tries_limit) {
                                    res = sendto(sock, commonbuf, buflen, 0, (sockaddr*)&addr, sizeof addr);
                                    Killtime++;
                                }
                                else break;
                            }
                            if (Killtime > tries_limit) continue;
                            cout << "Pack No=" << block << endl;
                        }
                    }
                    if (flag == 5) {    //ERROR
                        short errorcode;
                        memcpy(&errorcode, buf + 2, 2);
                        errorcode = ntohs(errorcode);
                        char strError[1024];
                        int iter = 0;
                        while (*(buf + iter + 4) != 0) {
                            memcpy(strError + iter, buf + iter + 4, 1);
                            ++iter;
                        }
                        *(strError + iter + 1) = '\0';
                        cout << "Error" << errorcode << " " << strError << endl;
                        print_time(fp);//日志
                        fprintf(fp, "Error %d %s\n", errorcode, strError);//日志
                        break;
                    }
                }
            }
            fclose(f);//关闭文件
        }
        
        
    }
    fclose(fp);//关闭日志文件
}
int main() {
    cout<<"请输入要同时操作的个数"<<endl;
    cout<<"现在多线程还没有实现，只在主函数里稍作修改,所以此处请写1"<<endl;
    int tt;//线程数
    cin>>tt;
    if(tt == 1){
        work();
    }
    else{
        for (int i = 1; i <= tt; i++)
        {
            thread t(work);
            t.detach();
        }
    }
    return 0;
}


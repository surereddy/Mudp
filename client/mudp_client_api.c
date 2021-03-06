#include "mudpc.h"


/* Internal data structures for association management */

unsigned int aid = 0xFFFF;

int local_socks [MAX_BIND_ADDRS];

struct sockaddr_in remote_sockets [MAX_REMOTE_SOCKS];

unsigned char num_server_socks;

unsigned char num_socks;

unsigned char link_health[MAX_REMOTE_SOCKS * MAX_BIND_ADDRS][2];

unsigned char current_pref = 0;

int current_local_sock;

struct sockaddr_in current_remote_sock;

unsigned char mudp_state = MUDP_STATE_IDLE;

MUDPC_EVENT_CALLBACK client_cb;

void timer_thread (int sig);
void * read_thread (void * arg);

mudpc_socket_t mudp_socket_create
               (
                   unsigned char num_addrs,
                   char  (*bind_addrs)[20],
                   unsigned short port,
                   MUDPC_EVENT_CALLBACK cb
               )
{
    unsigned char i;

    if (MUDP_STATE_IDLE != mudp_state)
    {
        return 0;
    }

    /* Register the call back */
    if (NULL == cb)
    {
        return 0;
    }

    printf ("Callback PTR: %p\r\n", cb);
    client_cb = cb;
    printf ("Callback PTR: %p\r\n", client_cb);

    /* Bind the address provided address */
    for (i = 0; (i < num_addrs && i < MAX_BIND_ADDRS); i++)
    {
        struct sockaddr_in addr;
        int sockfd;
        sockfd = socket(PF_INET, SOCK_DGRAM, 0);
        perror("socket");
        if (sockfd < 0)
        {
            return 0;
        }
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr (bind_addrs[i]);
        memset(addr.sin_zero, 0x0, sizeof (addr.sin_zero));
        if (bind (sockfd,(struct sockaddr *)&addr, sizeof (addr )) < 0)
        {
            perror("bind");
            printf ("%s\r\n", bind_addrs[i]);
            return 0;
        }
        local_socks[i] = sockfd;
    }
    num_socks = i;
    mudp_state = MUDP_STATE_INIT;
    /* <TODO> Currently single instance allowed*/
    return 1;
}

int mudpc_connect
    (
        mudpc_socket_t sock,
        in_addr_t addr_local,
        char * addr,
        unsigned short port
    )
{
    unsigned char assoc_pkt[5];
    unsigned char rsp[512];
    int rsp_len = 512;
    struct sockaddr_in server;
    unsigned char offset = 0;
    socklen_t len = sizeof (struct sockaddr);
    int i;

    if (mudp_state != MUDP_STATE_INIT)
    {
        /* Invalid state */
        return -1;
    }
    /* Establish UDP association with server */
    server.sin_family = AF_INET;
    server.sin_port = htons (port);
    server.sin_addr.s_addr = inet_addr (addr);
    memset(server.sin_zero, 0x0, sizeof (server.sin_zero));

    /* Prepare the association request packet */
    assoc_pkt[0] = MUDP_ASSO_REQ;

    assoc_pkt [1] = ((addr_local >> 24) & 0xFF ) - 1 ;
    assoc_pkt [2] = 0xFF;

    assoc_pkt[3] = (unsigned char) num_socks;

    /* Send the association request to the server */

    if (sendto(local_socks[0], assoc_pkt, 0X04, 0, (struct sockaddr *)&server, len)==-1)
    {
        return -1;
    }

    len = sizeof (struct sockaddr);

    /* Wait for a Association response */
    if ((rsp_len = recvfrom(local_socks[0], rsp, rsp_len, 0, (struct sockaddr *)&server, &len))==-1)
    {
        return -1;
    }

        printf ("CONNETED!");

    /* Check of the response */
    if (rsp[0] != MUDP_ASSO_RSP)
    {
        return -1;
    }
        printf ("CONNETED!!");

    aid = rsp[1];
    aid <<= 8;
    aid |= rsp[2];

    /* num_socks = rsp[3]; */

    num_server_socks = rsp[3];

    port = *((unsigned short *)(rsp+4));
    
    offset = 6;

    for (i = 0; i < num_server_socks && i < MAX_REMOTE_SOCKS; i++)
    {
        server.sin_family = AF_INET;
        /* server.sin_port = htons (port); */
        server.sin_port = port;
        server.sin_addr.s_addr = *((unsigned int *)(rsp + offset));
        offset += 4;
        memset(server.sin_zero, 0x0, sizeof (server.sin_zero));
        remote_sockets[i] = server;
    }

    /* Association completed */
    printf ("CONNETED!!!");

    /* Create read thread for all the local sockets */
    for (i = 0; i < num_socks; i++)
    {
            pthread_t thread_id;

        if (pthread_create( &thread_id, NULL, read_thread, (void*) &(local_socks[i])) != 0)
        {
            printf ("Thread creation failed\r\n");
            return -1;
        }
    }

    current_local_sock = local_socks[0];
    current_remote_sock = remote_sockets[0];
    current_pref = 0;

    /* Start the heart beat timer */
    signal (SIGALRM, timer_thread);
    alarm (HEART_BEAT_INTERVAL);
    return 0;
}

void * read_thread (void * arg)
{
    int sock = *((int *)arg);
    unsigned char buf[MAX_BUFLEN];
    struct sockaddr_in addr;
    socklen_t len, from_len = sizeof (addr);
    unsigned short opcode;
    int i;

    while (1)
    {
        /* Read from the socket */
        if ((len = recvfrom(sock, buf, MAX_BUFLEN, 0, (struct sockaddr*)&addr, &from_len))==-1)
        {
            perror ("recvfrom");
            continue;
        }

        /* Find the server instance */

        for (i = 0; i < num_server_socks; i++)
        {
            if (addr.sin_addr.s_addr == remote_sockets[i].sin_addr.s_addr)
            {
                break;
            }
        }

        if (i == num_server_socks)
        {
            /* Its not from one of the registered hosts */
            continue;
        }

        /* Check for the type of the packet */
        opcode  = buf[0];

        if (opcode == MUDP_HEARTBEAT_RSP)
        {
            unsigned char idx = buf[3];
            /* Mark ack for heart beat received */
            link_health[idx][1] = 0;
        }
        else if (opcode == MUDP_DATA)
        {
            /* Send the data to the application thru  the callback */
            client_cb (0x00, (buf+3), len-3);
        }
        else
        {
            continue;
        }
    }
}


void timer_thread (int sig)
{
    unsigned char heart_beat[5];
    unsigned char pref = 0;
    unsigned char prev_pref = 255 ;
    unsigned char health;
    static unsigned char notify_pri_changed = 1;
    int i, j;

    //printf ("HEART BEAT SENDER\r\n");

    /* Prepare the heart beat packet */
    heart_beat [0] = MUDP_HEARTBEAT_REQ;
    heart_beat [1] = (unsigned char)(aid >> 8);
    heart_beat [2] = (unsigned char)(aid & 0xFF);

    /* Check on the health of the current link */
    health = link_health [current_pref][0];

    health += link_health [current_pref][1]?3:0;

    if ((health > 6) || (current_pref != 0))
    {
        /* Select a new link */
        health = link_health [current_pref][0];
        prev_pref = current_pref;
        for (i = 0; i < (num_server_socks * num_socks); i++)
        {
            if (link_health [i][0] < health)
            {
                current_pref = i;
                health = link_health [i][0];
            }
        }
        if (prev_pref != current_pref)
        {
            current_local_sock = local_socks[(current_pref/num_server_socks)];
            current_remote_sock = remote_sockets[(current_pref%num_server_socks)];
            notify_pri_changed = 1;
        }
    }

    /* Send Heartbeat messages to server */
    for (i = 0; i < num_socks; i++)
    {
        for (j = 0; j < num_server_socks; j++)
        {
            heart_beat [0] = MUDP_HEARTBEAT_REQ;
            pref = (i *  num_server_socks) + j;
            if (link_health [pref][1] == 1)
            {
                if (link_health [pref][0] < 15)
                {
                    link_health [pref][0] += 3;
                }
            }
            else
            {
                if (link_health [pref][0] > 0)
                {
                    link_health[pref][0]--;
                }
            }

            link_health [pref][1] = 1;
            heart_beat[3] = pref;

            if (pref == current_pref && notify_pri_changed)
            {
                heart_beat[0] = MUDP_HEARTBEAT_PRI;
                notify_pri_changed = 0;
            }

            //printf ("Sending Heartbeat\r\n");

            /* Send the heart beat */
            sendto(local_socks[i], heart_beat, 0x04, 0,
                    (struct sockaddr *)&remote_sockets[j], sizeof(struct sockaddr));
            perror ("sendto");
        }
    }
    /* Rset the alarm */
    signal (SIGALRM, timer_thread);
    alarm (HEART_BEAT_INTERVAL);

}

int mudpc_send (mudpc_socket_t sock, void * buf, unsigned int buf_len)
{
    unsigned char send_buffer [2000];
    int n;

    send_buffer [0] = MUDP_DATA;
    send_buffer [1] = (unsigned char)(aid >> 8);
    send_buffer [2] = (unsigned char)(aid & 0xFF);

    memcpy (&send_buffer[3], buf, buf_len);

    /* Send the buffer across */
    if ((n=sendto(current_local_sock, send_buffer, 0x03+buf_len, 0,
            (struct sockaddr *)&current_remote_sock, sizeof(struct sockaddr))) < 0)
    {
        perror ("sendto");
    }

    return n;

}


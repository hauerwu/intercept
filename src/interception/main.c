/*
 *  TCPCopy 1.0 series - An online replication tool for TCP based applications
 *
 *  Copyright 2014 Netease, Inc.  All rights reserved.
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      bin wang <wangbin579@gmail.com>
 */

#include <xcopy.h>
#include <intercept.h>

xcopy_srv_settings srv_settings;
static tc_event_loop_t s_evt_loop;

static void
server_release_resources()
{
    tc_log_info(LOG_WARN, 0, "sig %d received", tc_over); 
    tc_log_info(LOG_NOTICE, 0, "release_resources begin");
    release_tunnel_resources();
    server_over();

    finally_release_obsolete_events();

    tc_event_loop_finish(&s_evt_loop);

    tc_destroy_pool(srv_settings.pool);
    tc_destroy_pool(srv_settings.cpool);
    tc_log_info(LOG_NOTICE, 0, "release_resources end except log file");
    tc_log_end();
}


static void
signal_handler(int sig)
{
    tc_over = sig;
}

static signal_t signals[] = {
    { "SIGTERM", SIGTERM, 0,    signal_handler },
    { "SIGINT",  SIGINT, 0,    signal_handler },
    { "SIGPIPE", SIGPIPE, 0,    SIG_IGN },
    { NULL, 0, 0, NULL }
};


#if (!TC_ADVANCED)
/* retrieve ip addresses */
static int
retrieve_ip_addr()
{
    int          count = 0;
    char         tmp[32];
    size_t       len;
    uint32_t     address;
    const char  *split, *p;

    tc_memzero(tmp, 32);
    p = srv_settings.raw_ip_list;

    while (true) {
        split = strchr(p, ',');
        if (split != NULL) {
            len = (size_t) (split - p);
        } else {
            len = strlen(p);
        }

        strncpy(tmp, p, len);
        address = inet_addr(tmp);
        srv_settings.passed_ips.ips[count++] = address;

        if (count == MAX_ALLOWED_IP_NUM) {
            tc_log_info(LOG_WARN, 0, "reach the limit for passing firewall");
            break;
        }

        if (split == NULL) {
            break;
        } else {
            p = split + 1;
        }

        tc_memzero(tmp, 32);
    }

    srv_settings.passed_ips.num = count;

    return 1;
}

#else

static void
parse_target(ip_port_pair_t *pair, char *addr)
{
    char    *seq, *ip_s, *port_s;
    uint16_t tmp_port;

    if ((seq = strchr(addr, ':')) == NULL) {
        pair->ip = 0;
        port_s = addr;
    } else {
        ip_s = addr;
        port_s = seq + 1;

        *seq = '\0';
        pair->ip = inet_addr(ip_s);
        *seq = ':';
    }

    tmp_port = atoi(port_s);
    pair->port = htons(tmp_port);
}


/*
 * retrieve target addresses
 * format
 * 192.168.0.1:80,192.168.0.1:8080
 */
static int
retr_target_addrs(char *raw_tf, ip_port_pairs_t *tf)
{
    int   i;
    char *p, *seq;

    if (raw_tf == NULL) {
        tc_log_info(LOG_ERR, 0, "it must have -o argument");
        fprintf(stderr, "no -o argument\n");
        return -1;
    }

    for (tf->num = 1, p = raw_tf; *p; p++) {
        if (*p == ',') {
            tf->num++;
        }
    }

    tf->map = tc_palloc(srv_settings.pool, tf->num *
                                sizeof(ip_port_pair_t *));
    if (tf->map == NULL) {
        return -1;
    }
    tc_memzero(tf->map, tf->num * sizeof(ip_port_pair_t *));

    for (i = 0; i < tf->num; i++) {
        tf->map[i] = tc_palloc(srv_settings.pool, sizeof(ip_port_pair_t));
        if (tf->map[i] == NULL) {
            return -1;
        }
        tc_memzero(tf->map[i], sizeof(ip_port_pair_t));
    }

    p = raw_tf;
    i = 0;
    for ( ;; ) {
        if ((seq = strchr(p, ',')) == NULL) {
            parse_target(tf->map[i++], p);
            break;
        } else {
            *seq = '\0';
            parse_target(tf->map[i++], p);
            *seq = ',';

            p = seq + 1;
        }
    }

    return 0;
}
#endif

#if (TC_COMBINED)
static void 
set_combined_num(int num) 
{
    if (num >=0 && num < COMB_MAX_NUM) {
        srv_settings.cur_combined_num = num;
    }
}
#endif


static void
usage(void)
{
    printf("intercept " VERSION "\n");
#if (!TC_ADVANCED)
    printf("-x <passlist,> passed IP list through firewall\n"
           "               Format:\n"
           "               ip_addr1, ip_addr2 ...\n");
#endif
#if (TC_COMBINED)
    printf("-n <num>       set the maximal num of combined packets.\n");
#endif
    printf("-p <num>       set the TCP port number to listen on. The default number is 36524.\n"
           "-s <num>       set the hash table size for intercept. The default value is 65536.\n"
           "-l <file>      save log information in <file>\n");
    printf("-P <file>      save PID in <file>, only used with -d option\n"
           "-b <ip_addr>   interface to listen on (default: INADDR_ANY, all addresses)\n");
#if (TC_NFQUEUE) 
    printf("-q <num>       set the maximal length of the nfnetlink queue if the kernel\n"
           "               supports it.\n");
#endif
#if (TC_ADVANCED)
#if (TC_PCAP)
    printf("-i <device,>   The name of the interface to listen on.  This is usually a driver\n"
           "               name followed by a unit number,for example eth0 for the first\n"
           "               Ethernet interface.\n");
    printf("-F <filter>    user filter(same as pcap filter)\n");
#endif
    printf("-o <target>    set the target for capturing response packets.\n");
#endif
#if (TC_SINGLE)
    printf("-c             set connections protected\n");
#endif
    printf("-v             intercept version\n"
           "-h             print this help and exit\n"
           "-d             run as a daemon\n");
}


static int
read_args(int argc, char **argv) {
    int  c;

    opterr = 0;
    while (-1 != (c = getopt(argc, argv,
#if (!TC_ADVANCED)
         "x:" /* ip list passed through ip firewall */
#endif
#if (TC_COMBINED)
         "n:"
#endif
         "p:" /* TCP port number to listen on */
         "t:" /* router item timeout */
         "s:" /* hash table size for intercept */
         "b:" /* binded ip address */
#if (TC_NFQUEUE) 
         "q:" /* max queue length for nfqueue */
#endif
#if (TC_ADVANCED)
#if (TC_PCAP)
         "i:" /* <device,> */
         "F:" /* <filter> */
#endif
         "o:" /* target addresses */
#endif
         "h"  /* print this help and exit */
         "l:" /* error log file path */
#if (TC_SINGLE)
         "c"
#endif
         "P:" /* save PID in file */
         "v"  /* print version and exit*/
         "d"  /* daemon mode */
        )))
    {
        switch (c) {
#if (!TC_ADVANCED)
            case 'x':
                srv_settings.raw_ip_list = optarg;
                break;
#endif
#if (TC_COMBINED)
            case 'n':
                set_combined_num(atoi(optarg));
                break;
#endif
            case 'p':
                srv_settings.port = (uint16_t) atoi(optarg);
                break;
#if (TC_NFQUEUE) 
            case 'q':
                srv_settings.max_queue_len = atoi(optarg);
                break;
#endif
#if (TC_ADVANCED)
#if (TC_PCAP)
            case 'i':
                srv_settings.raw_device = optarg;
                break;
            case 'F':
                srv_settings.user_filter = optarg;
                break;
#endif
            case 'o':
                srv_settings.raw_tf = optarg;
                break;
#endif
            case 's':
                srv_settings.hash_size = (uint32_t) atoi(optarg);
                break;
            case 'b':
                srv_settings.bound_ip = optarg;
                break;
#if (TC_SINGLE)
            case 'c':
                srv_settings.conn_protected = true;
                break;
#endif
            case 'h':
                usage();
                return -1;
            case 'l':
                srv_settings.log_path = optarg;
                break;
            case 'P':
                srv_settings.pid_file = optarg;
                break;
            case 'v':
                printf ("intercept version:%s\n", VERSION);
                return -1;
            case 'd':
                srv_settings.do_daemonize = 1;
                break;
            case '?':
                switch (optopt) {    
                    case 'x':
                        fprintf(stderr, "intercept: option -%c require an ip address list\n",
                                optopt);
                        break;
                    case 'b':
                        fprintf(stderr, "intercept: option -%c require an ip address\n", 
                                optopt);
                        break;
                    case 'l':
                    case 'P':
                        fprintf(stderr, "intercept: option -%c require a file name\n", 
                                optopt);
                        break;

                    case 'n':
                    case 'p':
#if (TC_NFQUEUE)
                    case 'q':
#endif
                    case 's':
                        fprintf(stderr, "intercept: option -%c require a number\n", 
                                optopt);
                        break;

                    default:
                        fprintf(stderr, "intercept: illegal argument \"%c\"\n", 
                                optopt);
                        break;
                }
                return -1;

            default:
                fprintf(stderr, "intercept: illegal argument \"%c\"\n", optopt);
                return -1;
        }

    }

    return 0;
}


#if (TC_ADVANCED && TC_PCAP)
static void 
extract_filter()
{
    int              i, cnt = 0;
    char            *pt;
    ip_port_pair_t  *pair, **map;

    map = srv_settings.targets.map;

    pt = srv_settings.filter;
#if (TC_UDP)
    strcpy(pt, "udp and (");
#else
    strcpy(pt, "tcp and (");
#endif
    pt = pt + strlen(pt);
 
    for (i = 0; i < srv_settings.targets.num; i++) {

        pair = map[i];

        if (pair->port == 0 && pair->ip == 0) {
            continue;
        }

        if (cnt >= MAX_FILTER_ITEMS) {
            break;
        }

        cnt++; 

        if (i > 0) {
            strcpy(pt, " or ");
        }
        pt = pt + strlen(pt);

        pt = construct_filter(SRC_DIRECTION, pair->ip, pair->port, pt);
    }

    strcpy(pt, ")");

    if (cnt == 0) {
        tc_log_info(LOG_WARN, 0, "filter is not set");
    }

    tc_log_info(LOG_NOTICE, 0, "intercept filter = %s", srv_settings.filter);

    return;

}
#endif


static int  
set_details()
{
    int  n;
#if (TC_ADVANCED && TC_PCAP)
    int  len;
#endif

    tc_pagesize = getpagesize();
    tc_cacheline_size = TC_CPU_CACHE_LINE; 
    for (n = tc_pagesize; n >>= 1; tc_pagesize_shift++) { /* void */ }

#if (!TC_ADVANCED)
    /* retrieve ip address */
    if (srv_settings.raw_ip_list != NULL) {
        tc_log_info(LOG_NOTICE, 0, "-x para:%s", srv_settings.raw_ip_list);
        retrieve_ip_addr();
    }
#endif
    
#if (TC_ADVANCED)
    if (srv_settings.raw_tf != NULL) {
        tc_log_info(LOG_NOTICE, 0, "-o parameter:%s", srv_settings.raw_tf);
        retr_target_addrs(srv_settings.raw_tf, &(srv_settings.targets));
    } else {
#if (!TC_PCAP)
        tc_log_info(LOG_WARN, 0, "no raw targets for advanced mode");
        return -1;
#else
        tc_log_info(LOG_NOTICE, 0, "no raw targets for advanced mode");
#endif
    }

#if (TC_PCAP)
    if (srv_settings.raw_device != NULL) {
        tc_log_info(LOG_NOTICE, 0, "device:%s", srv_settings.raw_device);
        if (strcmp(srv_settings.raw_device, DEFAULT_DEVICE) == 0) {
            srv_settings.raw_device = NULL; 
        } else {
            retrieve_devices(srv_settings.raw_device, &(srv_settings.devices));
        }
    }

    if (srv_settings.user_filter != NULL) {
        tc_log_info(LOG_NOTICE, 0, "user filter:%s", srv_settings.user_filter);
        len = strlen(srv_settings.user_filter);
        if (len >= MAX_FILTER_LENGH) {
            tc_log_info(LOG_ERR, 0, "user filter is too long");
            return -1;
        }
        memcpy(srv_settings.filter, srv_settings.user_filter, len);
    } else {
        extract_filter();
    }
#endif

#endif

#if (TC_NFQUEUE)
    if (srv_settings.max_queue_len <= 1024) {
        srv_settings.max_queue_len = -1;
    }
#endif

    /* daemonize */
    if (srv_settings.do_daemonize) {
        if (sigignore(SIGHUP) == -1) {
            tc_log_info(LOG_ERR, errno, "failed to ignore SIGHUP");
        }
        if (daemonize() == -1) {
            fprintf(stderr, "failed to daemonize() in order to daemonize\n");
            return -1;
        }
    }

    return 0;
}


/* set default values for intercept */
static void
settings_init(void)
{
    srv_settings.port = SERVER_PORT;
    srv_settings.hash_size = 65536;
    srv_settings.bound_ip = NULL;
#if (TC_COMBINED)
    srv_settings.cur_combined_num = COMB_MAX_NUM;
#endif
#if (TC_SINGLE)
    srv_settings.conn_protected = false;
#endif
#if (TC_NFQUEUE)
    srv_settings.max_queue_len = -1;
#endif

}


static void
output_for_debug()
{
    /* print out intercept version */
    tc_log_info(LOG_NOTICE, 0, "intercept version:%s", VERSION);
    tc_log_info(LOG_NOTICE, 0, "intercept internal version:%d", 
            INTERNAL_VERSION);
    /* print out intercept working mode */
#if (TC_NFQUEUE)
    tc_log_info(LOG_NOTICE, 0, "TC_NFQUEUE mode");
#endif
#if (TC_SINGLE)
    tc_log_info(LOG_NOTICE, 0, "TC_SINGLE mode");
#endif
#if (TC_COMBINED)
    tc_log_info(LOG_NOTICE, 0, "TC_COMBINED mode");
#endif
#if (TC_ADVANCED)
    tc_log_info(LOG_NOTICE, 0, "TC_ADVANCED mode");
#endif
#if (TC_PAYLOAD)
    tc_log_info(LOG_NOTICE, 0, "TC_PAYLOAD mode");
#endif
#if (TC_DNAT)
    tc_log_info(LOG_NOTICE, 0, "TC_DNAT mode");
#endif
#if (TC_MILLION_SUPPORT)
    tc_log_info(LOG_NOTICE, 0, "TC_MILLION_SUPPORT mode");
#endif
#if (HAVE_PCAP_CREATE)
    tc_log_info(LOG_NOTICE, 0, "HAVE_PCAP_CREATE is true,new pcap");
#endif


}


int
main(int argc, char **argv)
{
    int ret;

    settings_init();

    if (set_signal_handler(signals) == -1) {
        return -1;
    }

    tc_time_init();

    if (read_args(argc, argv) == -1) {
        return -1;
    }

    if (srv_settings.log_path == NULL) {
        srv_settings.log_path = "error_intercept.log";  
    }

    if (tc_log_init(srv_settings.log_path) == -1) {
        return -1;
    }

    srv_settings.pool = tc_create_pool(TC_DEFAULT_POOL_SIZE, 0);
    if (srv_settings.pool == NULL) {
        return -1;
    }
    srv_settings.cpool = tc_create_pool(TC_DEFAULT_POOL_SIZE, 0);
    if (srv_settings.cpool == NULL) {
        tc_destroy_pool(srv_settings.pool);
        return -1;
    }

    ret = tc_event_loop_init(&s_evt_loop, MAX_FD_NUM);
    if (ret == TC_EVENT_ERROR) {
        tc_log_info(LOG_ERR, 0, "event loop init failed");
        return -1;
    }

    /* output debug info */
    output_for_debug();
    if (set_details() == -1) {
        return -1;
    }

    tc_event_timer_init();

    if (server_init(&s_evt_loop, srv_settings.bound_ip, srv_settings.port) == 
            TC_ERR)
    {
        return -1;
    }

#if (TC_COMBINED)
    tc_event_add_timer(s_evt_loop.pool, CHECK_INTERVAL, NULL, server_push);
#endif
    tc_event_add_timer(s_evt_loop.pool, OUTPUT_INTERVAL, NULL, server_stat);

    /* run now */
    tc_event_proc_cycle(&s_evt_loop);

    server_release_resources();

    return 0;
}

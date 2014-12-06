/*
    Apache 2.2 mod_myfixip -- Author: G.Grandes
    
    v0.1 - 2011.05.07, Init version (SSL)
    v0.2 - 2011.05.28, Mix version (SSL & Non-SSL)
    v0.3 - 2014.12.06, Support for PROXY protocol v1 (haproxy)
    
    == TODO ==
    Security: Detectar automaticamente si es una conexion SSL e ignorar 
              la cabecera de los headers y usar solo el HELO 
              Quiza: r->notes("ssl-secure-reneg"=>0/1) ???
              Workarround: Usar el parametro RewriteIPResetHeader
    
    = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
    In HTTP (no SSL): this will fix "remote_ip" field if the request 
    contains an "X-Cluster-Client-Ip" header field, and the request came 
    directly from a one of the IP Addresses specified in the configuration 
    file (RewriteIPAllow directive).

    In HTTPS (SSL): this will fix "remote_ip" field if any of:
    1) the connection buffer begins with "HELOxxxx" (there xxxx is IPv4 in 
       binary format -netorder-)
    2) buffer follow PROXY protocol v1

       - TCP/IPv4 :
         "PROXY TCP4 255.255.255.255 255.255.255.255 65535 65535\r\n"
         => 5 + 1 + 4 + 1 + 15 + 1 + 15 + 1 + 5 + 1 + 5 + 2 = 56 chars

       - TCP/IPv6 :
         "PROXY TCP6 ffff:f...f:ffff ffff:f...f:ffff 65535 65535\r\n"
         => 5 + 1 + 4 + 1 + 39 + 1 + 39 + 1 + 5 + 1 + 5 + 2 = 104 chars

       - unknown connection (short form) :
         "PROXY UNKNOWN\r\n"
         => 5 + 1 + 7 + 2 = 15 chars

       - worst case (optional fields set to 0xff) :
         "PROXY UNKNOWN ffff:f...f:ffff ffff:f...f:ffff 65535 65535\r\n"
         => 5 + 1 + 7 + 1 + 39 + 1 + 39 + 1 + 5 + 1 + 5 + 2 = 107 chars

       Complete Proxy-Protocol: 
         http://haproxy.1wt.eu/download/1.5/doc/proxy-protocol.txt

    The rewrite address of request is allowed from a one of the IP Addresses 
    specified in the configuration file (RewriteIPAllow directive).
    
    
    Usage:
    
    # Global
    <IfModule mod_myfixip.c>
      RewriteIPResetHeader off
      RewriteIPHookPortSSL 442
      RewriteIPAllow 192.168.0.0/16 127.0.0.1
    </IfModule>
    
    # VirtualHost
    <VirtualHost *:442>
      <IfModule mod_myfixip.c>
        RewriteIPResetHeader on
      </IfModule>
    </VirtualHost>
    
    To play with this module first compile it into a
    DSO file and install it into Apache's modules directory
    by running:
    
    $ apxs2 -c -i mod_myfixip.c

    = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = = =
    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at
    
    http://www.apache.org/licenses/LICENSE-2.0
    
    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
*/

// References:
// http://ci.apache.org/projects/httpd/trunk/doxygen/
// http://apr.apache.org/docs/apr/1.4/
// http://httpd.apache.org/docs/2.3/developer/
// http://onlamp.com/pub/ct/38
// http://svn.apache.org/repos/asf/httpd/httpd/branches/2.2.x/modules/aaa/mod_authz_host.c
// http://svn.apache.org/repos/asf/httpd/httpd/branches/2.2.x/modules/experimental/mod_example.c
// http://svn.apache.org/repos/asf/httpd/httpd/branches/2.2.x/modules/filters/mod_reqtimeout.c
// http://svn.apache.org/repos/asf/httpd/httpd/tags/2.4.0/modules/metadata/mod_remoteip.c
// http://docs.aws.amazon.com/ElasticLoadBalancing/latest/DeveloperGuide/elb-listener-config.html
// http://docs.aws.amazon.com/ElasticLoadBalancing/latest/DeveloperGuide/enable-proxy-protocol.html
// http://haproxy.1wt.eu/download/1.5/doc/proxy-protocol.txt
// http://blog.haproxy.com/haproxy/proxy-protocol/

#define CORE_PRIVATE
#include "httpd.h"
#include "http_config.h"
#include "http_connection.h"
#include "http_protocol.h"
#include "http_log.h"
#include "ap_mpm.h"
#include "apr_strings.h"
#include "scoreboard.h"
#include "http_core.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MODULE_NAME "mod_myfixip"
#define MODULE_VERSION "0.3"

module AP_MODULE_DECLARE_DATA myfixip_module;

#define DEFAULT_PORT    442
#define PROXY           "PROXY"
#define HELO            "HELO"
#define TEST            "TEST"
#define TEST_RES_OK     "OK" "\n"

#define NOTE_ORIGINAL_REMOTE_IP         "ORIGINAL_REMOTE_IP"
#define NOTE_BALANCER_ADDR              "LOAD_BALANCER_ADDR"
#define NOTE_BALANCER_TRUST             "LOAD_BALANCER_TRUSTED"
#define NOTE_FIXIP_NAME                 "X-FIXIP-REMOTE-IP"

#define HDR_CLIENTIP_NAME               "X-Cluster-Client-Ip"

//#define DEBUG
#define DIRECT_REWRITE

typedef struct
{
    apr_port_t port;
    apr_array_header_t *allows;
    int resetHeader;
} my_config;

typedef struct {
    apr_ipsubnet_t *ip;
} accesslist;

typedef struct
{
    int iter;
} my_ctx;

static const char *const myfixip_filter_name = "myfixip_filter_name";

// Create per-server configuration structure
static void *create_config(apr_pool_t *p, server_rec *s)
{
    my_config *conf = apr_palloc(p, sizeof(my_config));

    conf->port = DEFAULT_PORT;
    conf->allows = apr_array_make(p, 1, sizeof(accesslist));
    conf->resetHeader = 0;

    return conf;
}

// Merge per-server configuration structure
static void *merge_config(apr_pool_t *p, void *parent_server1_conf, void *add_server2_conf)
{
    my_config *merged_config = (my_config *) apr_pcalloc(p, sizeof(my_config));
    memcpy(merged_config, parent_server1_conf, sizeof(my_config));
    my_config *s1conf = (my_config *) parent_server1_conf;
    my_config *s2conf = (my_config *) add_server2_conf;

    //merged_config->port = (s1conf->port == s2conf->port) ? s1conf->port : s2conf->port;
    //merged_config->allows = (s1conf->allows == s2conf->allows) ? s1conf->allows : s2conf->allows;
    merged_config->resetHeader = (s1conf->resetHeader == s2conf->resetHeader) ? s1conf->resetHeader : s2conf->resetHeader;

    return (void *) merged_config;
}
                                                       
// Parse the RewriteIPResetHeader directive
static const char *reset_header_config_cmd(cmd_parms *parms, void *mconfig, int flag)
{
    my_config *conf = ap_get_module_config(parms->server->module_config, &myfixip_module);
    const char *err = ap_check_cmd_context (parms, NOT_IN_DIR_LOC_FILE|NOT_IN_LIMIT);

    if (err != NULL) {
        return err;
    }

    conf->resetHeader = flag ? TRUE : FALSE;
    return NULL;
}

// Parse the RewriteIPHookPortSSL directive
static const char *port_config_cmd(cmd_parms *parms, void *mconfig, const char *arg)
{
    my_config *conf = ap_get_module_config(parms->server->module_config, &myfixip_module);
    const char *err = ap_check_cmd_context (parms, GLOBAL_ONLY);
    
    if (err != NULL) {
        return err;
    }
    
    unsigned long int port = strtol(arg, (char **) NULL, 10);

    if ((port > 65535) || (port < 1)) {
        return "Integer overflow or invalid number";
    }

    conf->port = port;
    return NULL;
}

// Parse the RewriteIPAllow directive
static const char *allow_config_cmd(cmd_parms *cmd, void *dv, const char *where_c)
{
    my_config *d = ap_get_module_config(cmd->server->module_config, &myfixip_module);
    accesslist *a;
    char *where = apr_pstrdup(cmd->pool, where_c);
    char *s;
    char msgbuf[120];
    apr_status_t rv;

    a = (accesslist *) apr_array_push(d->allows);

    if ((s = ap_strchr(where, '/'))) {
        *s++ = '\0';
        rv = apr_ipsubnet_create(&a->ip, where, s, cmd->pool);
        if(APR_STATUS_IS_EINVAL(rv)) {
            /* looked nothing like an IP address */
            return "An IP address was expected";
        }
        else if (rv != APR_SUCCESS) {
            apr_strerror(rv, msgbuf, sizeof msgbuf);
            return apr_pstrdup(cmd->pool, msgbuf);
        }
    }
    else if (!APR_STATUS_IS_EINVAL(rv = apr_ipsubnet_create(&a->ip, where, NULL, cmd->pool))) {
        if (rv != APR_SUCCESS) {
            apr_strerror(rv, msgbuf, sizeof msgbuf);
            return apr_pstrdup(cmd->pool, msgbuf);
        }
    }
    else { /* no slash, didn't look like an IP address => must be a host */
        return "An IP address was expected";
    }

    return NULL;
}

// Array describing structure of configuration directives
static command_rec cmds[] = {
    AP_INIT_FLAG("RewriteIPResetHeader", reset_header_config_cmd, NULL, RSRC_CONF, "Reset HTTP-Header in this SSL vhost?"),
    AP_INIT_TAKE1("RewriteIPHookPortSSL", port_config_cmd, NULL, RSRC_CONF, "TCP Port where hack"),
    AP_INIT_ITERATE("RewriteIPAllow", allow_config_cmd, NULL, RSRC_CONF, "IP-address wildcards"),
    {NULL}
};

// Set up startup-time initialization
static int post_config(apr_pool_t *p, apr_pool_t *plog, apr_pool_t *ptemp, server_rec *s)
{
    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, NULL, MODULE_NAME " " MODULE_VERSION " started");
    return OK;
}

static int find_accesslist(apr_array_header_t *a, apr_sockaddr_t *remote_addr)
{
    accesslist *ap = (accesslist *) a->elts;
    int i;

    for (i = 0; i < a->nelts; ++i) {
        if (apr_ipsubnet_test(ap[i].ip, remote_addr)) {
            return 1;
        }
    }

    return 0;
}

static int check_trusted( conn_rec *c, my_config *conf )
{
    const char *trusted;

    trusted = apr_table_get( c->notes, NOTE_BALANCER_TRUST);
    if (trusted) return (trusted[0] == 'Y');

    // Find Access List & Permit/Deny rewrite IP of Client
    if (find_accesslist(conf->allows, c->remote_addr)) {
        apr_table_setn(c->notes, NOTE_BALANCER_TRUST, "Y");
        // Save Original IP
        apr_table_set(c->notes, NOTE_BALANCER_ADDR, c->remote_ip);
        apr_table_set(c->notes, NOTE_ORIGINAL_REMOTE_IP, c->remote_ip);
        return 1;
    }

    apr_table_setn(c->notes, NOTE_BALANCER_TRUST, "N");
    apr_table_set(c->notes, NOTE_ORIGINAL_REMOTE_IP, c->remote_ip);
    return 0;
}


static int process_connection(conn_rec *c)
{
    my_config *conf = ap_get_module_config (c->base_server->module_config, &myfixip_module);
    
#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::process_connection IP Connection from: %s to port=%d (1)", c->remote_ip, c->local_addr->port);
#endif

    if (!check_trusted(c, conf)) { // Not Trusted
        return DECLINED;
    }

    if (c->local_addr->port != conf->port) {
        return DECLINED;
    }

    my_ctx *cctx = apr_palloc(c->pool, sizeof(my_ctx));
    cctx->iter = 0;

    ap_add_input_filter(myfixip_filter_name, cctx, NULL, c);

    return DECLINED;
}

static const char *fromBinIPtoString(apr_pool_t *p, const char *binip)
{
    // Rewrite IP
    struct in_addr inp;
    memcpy((char *)&inp, binip, 4);
    char *str_ip = inet_ntoa(inp);
    if (inet_aton(str_ip, &inp) == 0) {
        return NULL;
    }
    return apr_pstrdup( p, str_ip );
}

static int rewrite_conn_ip(conn_rec *c, my_config *conf, const char *new_ip)
{
    // Find Access List & Permit/Deny rewrite IP of Client
    if (!check_trusted(c, conf)) { // NOT FOUND - REWRITE IP DENIED
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::rewrite_conn_ip ERROR: Rewrite IP from balancer=%s denied", c->remote_ip);
        return 1;
    } else {
        // Rewrite IP
#ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::rewrite_conn_ip DEBUG: ORIG IP=<%s>", c->remote_ip);
#endif

#ifdef DIRECT_REWRITE
        c->remote_ip = (char *)new_ip;
        inet_aton(new_ip, &(c->remote_addr->sa.sin.sin_addr));
        c->remote_host = NULL; // Force DNS re-resolution
    #ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::rewrite_conn_ip DEBUG: CHANGED IP=<%s> (DIRECT-REWRITE)", new_ip);
    #endif
#else
    #ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::rewrite_conn_ip DEBUG: CHANGED IP=<%s> (CONNECTION-NOTE)", new_ip);
    #endif
#endif
        // Set connection Note for mod_fixip
        apr_table_set(c->notes, NOTE_FIXIP_NAME, new_ip);
    }
    return 0;
}

static apr_status_t helocon_filter_in(ap_filter_t *f, apr_bucket_brigade *b, ap_input_mode_t mode, apr_read_type_e block, apr_off_t readbytes)
{
    conn_rec *c = f->c;
    my_ctx *ctx = f->ctx;
    my_config *conf = ap_get_module_config (c->base_server->module_config, &myfixip_module);
    const char *str = NULL;
    apr_size_t length = 8;
    apr_bucket *e = NULL, *d = NULL;

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in IP Connection from: %s to port=%d (1)", c->remote_ip, c->local_addr->port);
#endif

    // Fail quickly if the connection has already been aborted.
    if (c->aborted) {
        apr_brigade_cleanup(b);
        return APR_ECONNABORTED;
    }

    //if (c->local_addr->port != conf->port) {
    //    return APR_SUCCESS;
    //}

    ap_get_brigade(f->next, b, mode, block, readbytes);
    e = APR_BRIGADE_FIRST(b);

    if (e->type == NULL) {
        return APR_SUCCESS;
    }

    if (ctx->iter) {
        return APR_SUCCESS;
    } else {
        ctx->iter = 1;
    }

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in IP Connection from: %s to port=%d (2)", c->remote_ip, c->local_addr->port);
#endif

    // Read fisrt bucket
    apr_bucket_read(e, &str, &length, APR_BLOCK_READ);

    // TEST Command
    if (strncmp(TEST, str, 4) == 0) {
        apr_socket_t *csd = ap_get_module_config(c->conn_config, &core_module);

        length = strlen(TEST_RES_OK);
        apr_socket_send(csd, TEST_RES_OK, &length);
        apr_socket_shutdown(csd, APR_SHUTDOWN_WRITE);
        apr_socket_close(csd);

#ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in DEBUG: CMD=TEST OK");
#endif

        // No need to check for SUCCESS, we did that above
        c->aborted = 1;
        return APR_ECONNABORTED;
    }
    // HELO Command
    if (strncmp(HELO, str, 4) == 0) {
#ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in DEBUG: CMD=HELO OK");
#endif
        // delete HELO header
        apr_bucket_split(e, 8);
        d = e;
        e = APR_BUCKET_NEXT(e);
        APR_BUCKET_REMOVE(d);
        d = NULL;

        // REWRITE CLIENT IP
        const char *new_ip = fromBinIPtoString(c->pool, str+4);
        if (new_ip == NULL) {
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in ERROR: HELO+IP invalid");
            return APR_SUCCESS;
        }

        rewrite_conn_ip(c, conf, new_ip);
        return APR_SUCCESS;
    }
    // PROXY Command
    if (strncmp(PROXY, str, 5) == 0) {
#ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in DEBUG: CMD=PROXY OK");
#endif
        char *end = memchr(str, '\r', length - 1);
        if (!end || end[1] != '\n') {
            goto ABORT_CONN;
        }
        end[0] = ' '; // for next split
        end[1] = 0;
#ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in DEBUG: CMD=PROXY header=%s", str);
#endif
        length = (end + 2 - str);
        int size = length - 1;
        char *ptr = (char *) str;
        int tok = 0;
        char *srcip = NULL, *dstip = NULL, *srcport = NULL, *dstport = NULL;
        while (ptr) {
            char *f = memchr(ptr, ' ', size);
            if (!f) {
                break;
            }
            *f = '\0';
#ifdef DEBUG
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in DEBUG: CMD=PROXY token=%s [%d]", ptr, tok);
#endif
            // PROXY TCP4 255.255.255.255 255.255.255.255 65535 65535
            switch (tok) {
                case 0: // PROXY
                    break;
                case 2: // SRCIP
                    srcip = ptr;
                    break;
                case 3: // DSTIP
                    dstip = ptr;
                    break;
                case 4: // SRCPORT
                    srcport = ptr;
                    break;
                case 5: // DSTPORT
                    dstport = ptr;
                    break;
                case 1: // PROTO
                    if (strncmp("TCP", ptr, 3) == 0) {
                        if ((ptr[3] == '4') ||
                            (ptr[3] == '6')) {
                            break;
                        }
                    }
                default:
                    srcip = dstip = srcport = dstport = NULL;
                    goto ABORT_CONN;
            }
            size -= (f + 1 - ptr);
            ptr = f + 1;
            tok++;
        }
        if (!dstport) {
            goto ABORT_CONN;
        }
        // delete PROXY protocol header
        apr_bucket_split(e, length);
        d = e;
        e = APR_BUCKET_NEXT(e);
        APR_BUCKET_REMOVE(d);
        d = NULL;

        rewrite_conn_ip(c, conf, srcip);
        return APR_SUCCESS;

    ABORT_CONN:
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::helocon_filter_in ERROR: PROXY protocol header invalid from=%s", c->remote_ip);
        c->aborted = 1;
        return APR_ECONNABORTED;
    }

    //ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME " DEBUG-ED!!");

    return APR_SUCCESS;
}

static int post_read_handler(request_rec *r)
{
    conn_rec *c = r->connection;
    my_config *conf = ap_get_module_config (c->base_server->module_config, &myfixip_module);

    const char *remote_ip = apr_table_get(c->notes, NOTE_ORIGINAL_REMOTE_IP);
    const char *new_ip = NULL;

    if (conf->resetHeader) {
        apr_table_unset(r->headers_in, HDR_CLIENTIP_NAME);
    }

    // Get the Client Ip from the headers
    new_ip = apr_table_get(c->notes, NOTE_FIXIP_NAME);
    if (!new_ip) {
#ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::post_read_handler IP Connection from: %s (NOTE=NULL)", remote_ip);
#endif
        new_ip = apr_table_get(r->headers_in, HDR_CLIENTIP_NAME);
        if (!new_ip) {
#ifdef DEBUG
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::post_read_handler IP Connection from: %s (HEADER=NULL)", remote_ip);
#endif
            return DECLINED;
        }
    }


    if (!check_trusted(c, conf)) { // Not Trusted
#ifdef DEBUG
        ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::post_read_handler IP Connection from: %s newip=%s (DENIED)", remote_ip, new_ip);
#endif
        return DECLINED;
    }

#ifdef DEBUG
    ap_log_error(APLOG_MARK, APLOG_WARNING, 0, NULL, MODULE_NAME "::post_read_handler IP Connection from: %s remoteip=%s newip=%s (OK)", remote_ip, c->remote_ip, new_ip);
#endif
    if (strcmp(c->remote_ip, new_ip)) { // Change
        rewrite_conn_ip(c, conf, new_ip);
    }

    return DECLINED;
}

static void child_init(apr_pool_t *p, server_rec *s)
{
    ap_add_version_component(p, MODULE_NAME "/" MODULE_VERSION);
}

static void register_hooks(apr_pool_t *p)
{
    /*
     * mod_ssl is AP_FTYPE_CONNECTION + 5 and mod_myfixip needs to
     * be called before mod_ssl.
     */
    ap_register_input_filter(myfixip_filter_name, helocon_filter_in, NULL, AP_FTYPE_CONNECTION + 9);
    ap_hook_post_config(post_config, NULL, NULL, APR_HOOK_MIDDLE);
    ap_hook_child_init(child_init, NULL, NULL, APR_HOOK_MIDDLE);    
    ap_hook_process_connection(process_connection, NULL, NULL, APR_HOOK_FIRST);
    ap_hook_post_read_request(post_read_handler, NULL, NULL, APR_HOOK_FIRST);
}

module AP_MODULE_DECLARE_DATA myfixip_module = {
    STANDARD20_MODULE_STUFF,
    NULL,                       // create per-dir config structures
    NULL,                       // merge  per-dir    config structures
    create_config,              // create per-server config structures
    merge_config,               // merge  per-server config structures
    cmds,                       // table of config file commands
    register_hooks              // register hooks
};

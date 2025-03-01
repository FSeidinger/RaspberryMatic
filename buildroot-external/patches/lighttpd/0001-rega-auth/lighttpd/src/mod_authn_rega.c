/*
 * mod_authn_rega.c
 *
 * Copyright (C) 2018-2021 eQ-3 Entwicklung GmbH
 * Author: Christian Niclaus
 *
 * Licensed under Apache-2.0
 *
 * 1.0: initial release
 * 1.1: lighttpd 1.4.58+ compatibility <mail@jens-maus.de>
 * 1.2: lighttpd 1.4.61+ compatibility <mail@jens-maus.de>
 */

#include "base.h"
#include "plugin.h"
#include "mod_auth_api.h"
#include "log.h"
#include "response.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct {
    int auth_rega_port;
} plugin_config;

typedef struct {
    PLUGIN_DATA;
    plugin_config defaults;
    plugin_config conf;
} plugin_data;

static handler_t mod_authn_rega_basic(request_st *r, void *p_d, const http_auth_require_t *require, const buffer *username, const char *pw);
static int mod_authn_rega_checkAuth(int port, const char* user, const char* pass, log_error_st *errh);
static void mod_authn_rega_sendMsg(const char* msg, const int msgLength, const int regaPort, char* answer, int* answerLength, log_error_st *errh);

INIT_FUNC(mod_authn_rega_init) {
    static http_auth_backend_t http_auth_backend_rega =
      { "rega", mod_authn_rega_basic, NULL/*mod_authn_rega_digest*/, NULL };
    plugin_data *p = calloc(1, sizeof(*p));

    /* register http_auth_backend_rega */
    http_auth_backend_rega.p_d = p;
    http_auth_backend_set(&http_auth_backend_rega);

    return p;
}

static void mod_authn_rega_merge_config_cpv(plugin_config * const pconf, const config_plugin_value_t * const cpv) {
    switch (cpv->k_id) { /* index into static config_plugin_keys_t cpk[] */
      case 0: /* auth.backend.rega.port */
        pconf->auth_rega_port = (int)cpv->v.shrt;
        break;
      default:/* should not happen */
        return;
    }
}

static void mod_authn_rega_merge_config(plugin_config * const pconf, const config_plugin_value_t *cpv) {
    do {
        mod_authn_rega_merge_config_cpv(pconf, cpv);
    } while ((++cpv)->k_id != -1);
}

static void mod_authn_rega_patch_config(request_st * const r, plugin_data * const p) {
    p->conf = p->defaults; /* copy small struct instead of memcpy() */
    /*memcpy(&p->conf, &p->defaults, sizeof(plugin_config));*/
    for (int i = 1, used = p->nconfig; i < used; ++i) {
        if (config_check_cond(r, (uint32_t)p->cvlist[i].k_id))
            mod_authn_rega_merge_config(&p->conf,
                                        p->cvlist + p->cvlist[i].v.u2[0]);
    }
}

SETDEFAULTS_FUNC(mod_authn_rega_set_defaults) {
    static const config_plugin_keys_t cpk[] = {
      { CONST_STR_LEN("auth.backend.rega.port"),
        T_CONFIG_SHORT,
        T_CONFIG_SCOPE_CONNECTION }
     ,{ NULL, 0,
        T_CONFIG_UNSET,
        T_CONFIG_SCOPE_UNSET }
    };

    plugin_data * const p = p_d;
    if (!config_plugin_values_init(srv, p, cpk, "mod_authn_rega"))
        return HANDLER_ERROR;

    /* initialize p->defaults from global config context */
    if (p->nconfig > 0 && p->cvlist->v.u2[1]) {
        const config_plugin_value_t *cpv = p->cvlist + p->cvlist->v.u2[0];
        if (-1 != cpv->k_id)
            mod_authn_rega_merge_config(&p->defaults, cpv);
    }

    return HANDLER_GO_ON;
}

static handler_t mod_authn_rega_basic(request_st * const r, void *p_d, const http_auth_require_t * const require, const buffer * const username, const char * const pw) {
    plugin_data *p = (plugin_data *)p_d;

    UNUSED(require);

    mod_authn_rega_patch_config(r, p);

    if(1 == mod_authn_rega_checkAuth(p->conf.auth_rega_port, username->ptr, pw, r->conf.errh)) {
      //auth ok
        return HANDLER_GO_ON;
    } else {
        return HANDLER_ERROR;
    }
}

static int mod_authn_rega_checkAuth(int port, const char* user, const char* pass, log_error_st *errh) {

    //assemble message

    char msg[1024];
    memset(msg, 0, sizeof(char)*1024);

    const int lengthUser = strlen(user);
    const int lengthPass = strlen(pass);
    if((lengthUser + lengthPass + 1)  > 1023) {
        return 0;
    }

    int m = 0;
    //user
    for(int i = 0 ; i < lengthUser && m <= 1022; i++) {
        const char c = user[i];
        if(c == '\\') {
            msg[m] = '\\';
            m++;
            msg[m] = '\\';
        } else if(c == ':') {
            msg[m] = '\\';
            m++;
            msg[m] = ':';
        } else {
            msg[m] = c;
        }
        m++;
    }

    //delimiter
    msg[m] = ':';
    m++;
    //pass
    for(int i = 0; i < lengthPass && m <= 1022; i++) {
        const char c = pass[i];
        if(c == '\\') {
            msg[m] = '\\';
            m++;
            msg[m] = '\\';
        }
        else if(c == ':') {
            msg[m] = '\\';
            m++;
            msg[m] = ':';
        }
        else {
            msg[m] = c;
        }
        m++;
    }

    //Create socket and send
    int answerLength = 128;
    char answer[answerLength];
    memset(answer, 0, sizeof(char)*answerLength);
    mod_authn_rega_sendMsg(msg, m, port, answer, &answerLength, errh);
    if(strcmp("1", answer) == 0) {
      return 1;
    }

    return 0;
}

static void mod_authn_rega_sendMsg(const char* msg, const int msgLength, const int regaPort, char* answer, int* answerLength, log_error_st *errh) {
    const int answerBufferLength = *answerLength;
    *answerLength = 0;

    //create socket
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if(sock == -1) {
        return;
    }

    // define a timeout for recvfrom()/send() or otherwise these calls
    // can block
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
       setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        close(sock);
        return;
    }

    struct sockaddr_in addrRega, addrMe;

    memset(&addrRega, 0, sizeof(addrRega));
    addrRega.sin_family = AF_INET;
    addrRega.sin_addr.s_addr = inet_addr("127.0.0.1");
    addrRega.sin_port = htons(regaPort);
    memset(&addrMe, 0, sizeof(addrMe));
    addrMe.sin_family = AF_INET;
    addrMe.sin_addr.s_addr = inet_addr("127.0.0.1");
    //addrMe.sin_port = htons(0);

    if(bind(sock, &addrMe, sizeof(addrMe)) == -1) {
        close(sock);
        return;
    }

    //send msg
    if(sendto(sock, msg, msgLength, 0, &addrRega, sizeof(addrRega)) == -1 ) {
        log_error(errh, __FILE__, __LINE__, "could not send auth request to rega");
        close(sock);
        return;
    }

    //receive answer
    memset(&addrRega, 0, sizeof(addrRega));
    unsigned int sLength = 0;
    int result = -1;
    if((result = recvfrom(sock, answer, answerBufferLength, 0, &addrRega, &sLength)) == -1) {
        log_error(errh, __FILE__, __LINE__, "could not receive auth answer from rega");
        close(sock);
        return;
    }

    *answerLength = result;

    //check if the answer comes from correct port....
    if(sLength != 0) {
        char* name = inet_ntoa(addrRega.sin_addr);
        int port = ntohs(addrRega.sin_port);

        if( ! (port == regaPort && (strcmp(name, "127.0.0.1") == 0))) {
          *answerLength = 0;
        }
    }

    close(sock);
}

int mod_authn_rega_plugin_init(plugin *p);
int mod_authn_rega_plugin_init(plugin *p) {
    p->version     = LIGHTTPD_VERSION_ID;
    p->name        = "authn_rega";
    p->init        = mod_authn_rega_init;
    p->set_defaults= mod_authn_rega_set_defaults;

    return 0;
}

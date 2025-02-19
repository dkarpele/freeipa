/* Authors: Rob Crittenden <rcritten@redhat.com>
 *
 * Copyright (C) 2009  Red Hat
 * see file 'COPYING' for use and warranty information
 *
 * This program is free software you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include "config.h"
#include <stdbool.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <sys/utsname.h>
#include <krb5.h>
/* Doesn't work w/mozldap */
#include <ldap.h>
#include <popt.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <limits.h>

#ifdef WITH_IPA_JOIN_XML
#include "xmlrpc-c/base.h"
#include "xmlrpc-c/client.h"
#else
#include <curl/curl.h>
#include <jansson.h>
#endif

#include "ipa-client-common.h"
#include "ipa_ldap.h"
#include "ipa_hostname.h"

#define NAME "ipa-join"

#define JOIN_OID "2.16.840.1.113730.3.8.10.3"

#define IPA_CONFIG "/etc/ipa/default.conf"

char * read_config_file(const char *filename);
char * get_config_entry(char * data, const char *section, const char *key);

static int debug = 0;

#define ASPRINTF(strp, fmt...) \
    if (asprintf(strp, fmt) == -1) { \
        fprintf(stderr, _("Out of memory!\n")); \
        rval = 3; \
        goto cleanup; \
    }

/*
 * Translate some IPA exceptions into specific errors in this context.
 */
#ifdef WITH_IPA_JOIN_XML
static int
handle_fault(xmlrpc_env * const envP) {
    if (envP->fault_occurred) {
        switch(envP->fault_code) {
        case 2100: /* unable to add new host entry or write objectClass */
            fprintf(stderr,
                    _("No permission to join this host to the IPA domain.\n"));
            break;
        default:
            fprintf(stderr, "%s\n", envP->fault_string);
        }
        return 1;
    }
    return 0;
}
#endif

/* Get the IPA server from the configuration file.
 * The caller is responsible for freeing this value
 */
static char *
getIPAserver(char * data) {
    return get_config_entry(data, "global", "server");
}

/* Make sure that the keytab is writable before doing anything */
static int check_perms(const char *keytab)
{
    int ret;
    int fd;

    ret = access(keytab, W_OK);
    if (ret == -1) {
        switch(errno) {
            case EACCES:
                fprintf(stderr,
                        _("No write permissions on keytab file '%s'\n"),
                        keytab);
                break;
            case ENOENT:
                /* file doesn't exist, lets touch it and see if writable */
                fd = open(keytab, O_WRONLY | O_CREAT, 0600);
                if (fd != -1) {
                    close(fd);
                    unlink(keytab);
                    return 0;
                }
                fprintf(stderr,
                        _("No write permissions on keytab file '%s'\n"),
                        keytab);
                break;
            default:
                fprintf(stderr,
                        _("access() on %1$s failed: errno = %2$d\n"),
                        keytab, errno);
                break;
        }
        return 1;
    }

    return 0;
}

/*
 * There is no API in xmlrpc-c to set arbitrary headers but we can fake it
 * by using a specially-crafted User-Agent string.
 *
 * The caller is responsible for freeing the return value.
 */
 #ifdef WITH_IPA_JOIN_XML
char *
set_user_agent(const char *ipaserver) {
    int ret;
    char *user_agent = NULL;

    ret = asprintf(&user_agent, "%s/%s\r\nReferer: https://%s/ipa/xml\r\nX-Original-User-Agent:", NAME, VERSION, ipaserver);
    if (ret == -1) {
        fprintf(stderr, _("Out of memory!"));
        return NULL;
    }
    return user_agent;
}

/*
 * Make an XML-RPC call to methodName. This uses the curl client to make
 * a connection over SSL using the CA cert that should have been installed
 * by ipa-client-install.
 */
static void
callRPC(char * user_agent,
     xmlrpc_env *            const envP,
     xmlrpc_server_info * const serverInfoP,
     const char *               const methodName,
     xmlrpc_value *             const paramArrayP,
     xmlrpc_value **            const resultPP) {

    struct xmlrpc_clientparms clientparms;
    struct xmlrpc_curl_xportparms * curlXportParmsP = NULL;
    xmlrpc_client * clientP = NULL;

    memset(&clientparms, 0, sizeof(clientparms));

    XMLRPC_ASSERT(xmlrpc_value_type(paramArrayP) == XMLRPC_TYPE_ARRAY);

    curlXportParmsP = malloc(sizeof(*curlXportParmsP));
    if (curlXportParmsP == NULL) {
        xmlrpc_env_set_fault(envP, XMLRPC_INTERNAL_ERROR, _("Out of memory!"));
        return;
    }
    memset(curlXportParmsP, 0, sizeof(*curlXportParmsP));

    /* Have curl do SSL certificate validation */
    curlXportParmsP->no_ssl_verifypeer = 0;
    curlXportParmsP->no_ssl_verifyhost = 0;
    curlXportParmsP->cainfo = DEFAULT_CA_CERT_FILE;
    curlXportParmsP->user_agent = user_agent;

    clientparms.transport = "curl";
    clientparms.transportparmsP = (struct xmlrpc_xportparms *)
            curlXportParmsP;
    clientparms.transportparm_size = XMLRPC_CXPSIZE(cainfo);
    xmlrpc_client_create(envP, XMLRPC_CLIENT_NO_FLAGS, NAME, VERSION,
                         &clientparms, sizeof(clientparms),
                         &clientP);

    /* Set up kerberos negotiate authentication in curl. */
    xmlrpc_server_info_set_user(envP, serverInfoP, ":", "");
    xmlrpc_server_info_allow_auth_negotiate(envP, serverInfoP);

    /* Perform the XML-RPC call */
    if (!envP->fault_occurred) {
        xmlrpc_client_call2(envP, clientP, serverInfoP, methodName, paramArrayP, resultPP);
    }

    /* Cleanup */
    xmlrpc_server_info_free(serverInfoP);
    xmlrpc_client_destroy(clientP);
    free((void*)clientparms.transportparmsP);
}
#endif

/* The caller is responsible for unbinding the connection if ld is not NULL */
static LDAP *
connect_ldap(const char *hostname, const char *binddn, const char *bindpw,
             int *ret) {
    LDAP *ld = NULL;
    int ldapdebug = 2;
    char *uri = NULL;
    struct berval bindpw_bv;

    *ret = ldap_set_option(NULL, LDAP_OPT_DEBUG_LEVEL, &ldapdebug);
    if (*ret != LDAP_OPT_SUCCESS) {
        goto fail;
    }

    *ret = asprintf(&uri, "ldaps://%s:636", hostname);
    if (*ret == -1) {
        fprintf(stderr, _("Out of memory!"));
        *ret = LDAP_NO_MEMORY;
        goto fail;
    }

    *ret = ipa_ldap_init(&ld, uri);
    if (*ret != LDAP_SUCCESS) {
        goto fail;
    }
    *ret = ipa_tls_ssl_init(ld, uri, DEFAULT_CA_CERT_FILE);
    if (*ret != LDAP_SUCCESS) {
        fprintf(stderr, _("Unable to enable SSL in LDAP\n"));
        goto fail;
    }
    free(uri);
    uri = NULL;

    if (bindpw) {
        bindpw_bv.bv_val = discard_const(bindpw);
        bindpw_bv.bv_len = strlen(bindpw);
    } else {
        bindpw_bv.bv_val = NULL;
        bindpw_bv.bv_len = 0;
    }

    *ret = ldap_sasl_bind_s(ld, binddn, LDAP_SASL_SIMPLE, &bindpw_bv,
                            NULL, NULL, NULL);

    if (*ret != LDAP_SUCCESS) {
        ipa_ldap_error(ld, *ret, _("SASL Bind failed\n"));
        goto fail;
    }

    return ld;

fail:
    if (ld != NULL) {
        ldap_unbind_ext(ld, NULL, NULL);
    }
    if (uri != NULL) {
        free(uri);
    }
    return NULL;
}

/*
 * Given a list of naming contexts check each one to see if it has
 * an IPA v2 server in it. The first one we find wins.
 */
static int
check_ipa_server(LDAP *ld, char **ldap_base, struct berval **vals)
{
    struct berval **infovals;
    LDAPMessage *entry, *res = NULL;
    char *info_attrs[] = {"info", NULL};
    int i, ret = 0;

    for (i = 0; !*ldap_base && vals[i]; i++) {
        ret = ldap_search_ext_s(ld, vals[i]->bv_val,
                                LDAP_SCOPE_BASE, "(info=IPA*)", info_attrs,
                                0, NULL, NULL, NULL, 0, &res);

        if (ret != LDAP_SUCCESS) {
            break;
        }

        entry = ldap_first_entry(ld, res);
        infovals = ldap_get_values_len(ld, entry, info_attrs[0]);
        if (strcmp(infovals[0]->bv_val, "IPA V2.0") == 0)
            *ldap_base = strdup(vals[i]->bv_val);
        ldap_msgfree(res);
        res = NULL;
    }

    return ret;
}

/*
 * Determine the baseDN of the remote server. Look first for a
 * defaultNamingContext, otherwise fall back to reviewing each
 * namingContext.
 */
static int
get_root_dn(const char *ipaserver, char **ldap_base)
{
    LDAP *ld = NULL;
    char *root_attrs[] = {"namingContexts", "defaultNamingContext", NULL};
    LDAPMessage *entry, *res = NULL;
    struct berval **ncvals;
    struct berval **defvals;
    int ret, rval = 0;

    ld = connect_ldap(ipaserver, NULL, NULL, &ret);
    if (!ld) {
        rval = 14;
        goto done;
    }

    ret = ldap_search_ext_s(ld, "", LDAP_SCOPE_BASE,
                            "objectclass=*", root_attrs, 0,
                            NULL, NULL, NULL, 0, &res);

    if (ret != LDAP_SUCCESS) {
        fprintf(stderr, _("Search for %1$s on rootdse failed with error %2$d\n"),
                root_attrs[0], ret);
        rval = 14;
        goto done;
    }

   *ldap_base = NULL;

    entry = ldap_first_entry(ld, res);

    defvals = ldap_get_values_len(ld, entry, root_attrs[1]);
    if (defvals) {
        ret = check_ipa_server(ld, ldap_base, defvals);
    }
    ldap_value_free_len(defvals);

    /* loop through to find the IPA context */
    if (ret == LDAP_SUCCESS &&  !*ldap_base) {
        ncvals = ldap_get_values_len(ld, entry, root_attrs[0]);
        if (!ncvals) {
            fprintf(stderr, _("No values for %s"), root_attrs[0]);
            rval = 14;
            ldap_value_free_len(ncvals);
            goto done;
        }
        ret = check_ipa_server(ld, ldap_base, ncvals);
        ldap_value_free_len(ncvals);
    }

    if (ret != LDAP_SUCCESS) {
        fprintf(stderr, _("Search for IPA namingContext failed with error %d\n"), ret);
        rval = 14;
        goto done;
    }

    if (!*ldap_base) {
        fprintf(stderr, _("IPA namingContext not found\n"));
        rval = 14;
        goto done;
    }


done:
    if (res) ldap_msgfree(res);
    if (ld != NULL) {
        ldap_unbind_ext(ld, NULL, NULL);
    }

    return rval;
}

/* Join a host to the current IPA realm.
 *
 * There are several scenarios for this:
 * 1. You are an IPA admin user with fullrights to add hosts and generate
 *    keytabs.
 * 2. You are an IPA admin user with rights to generate keytabs but not
 *    write hosts.
 * 3. You are a regular IPA user with a password that can be used to
 *   generate the host keytab.
 *
 * If a password is presented it will be used regardless of the rights of
 * the user.
 */

/* If we only have a bindpw then try to join in a bit of a degraded mode.
 * This is going to duplicate some of the server-side code to determine
 * the state of the entry.
 */
static int
join_ldap(const char *ipaserver, const char *hostname, char ** binddn, const char *bindpw, const char *basedn, const char **princ, bool quiet)
{
    LDAP *ld;
    int rval = 0;
    char *oidresult = NULL;
    struct berval valrequest;
    struct berval *valresult = NULL;
    int rc, ret;
    char *ldap_base = NULL;

    *binddn = NULL;
    *princ = NULL;

    if (NULL != basedn) {
        ldap_base = strdup(basedn);
        if (!ldap_base) {
            fprintf(stderr, _("Out of memory!\n"));
            rval = 3;
            goto done;
        }
    } else {
        if (get_root_dn(ipaserver, &ldap_base) != 0) {
            fprintf(stderr, _("Unable to determine root DN of %s\n"),
                              ipaserver);
            rval = 14;
            goto done;
        } else {
            if (debug) {
                fprintf(stderr, "root DN %s\n", ldap_base);
            }
        }
    }

    ret = asprintf(binddn, "fqdn=%s,cn=computers,cn=accounts,%s", hostname, ldap_base);
    if (ret == -1)
    {
        fprintf(stderr, _("Out of memory!\n"));
        rval = 3;
        goto done;
    }
    if (debug) {
        fprintf(stderr, "Connecting to %s as %s\n", ipaserver, *binddn);
    }
    ld = connect_ldap(ipaserver, *binddn, bindpw, &ret);
    if (ld == NULL) {
        switch(ret) {
            case LDAP_NO_MEMORY:
                rval = 3;
                break;
            case LDAP_INVALID_CREDENTIALS: /* incorrect password */
            case LDAP_INAPPROPRIATE_AUTH: /* no password set */
                rval = 15;
                break;
            default: /* LDAP connection error catch-all */
                rval = 14;
                break;
        }
        goto done;
    }

    valrequest.bv_val = (char *)hostname;
    valrequest.bv_len = strlen(hostname);

    if ((rc = ldap_extended_operation_s(ld, JOIN_OID, &valrequest, NULL, NULL, &oidresult, &valresult)) != LDAP_SUCCESS) {
        char *s = NULL;
#ifdef LDAP_OPT_DIAGNOSTIC_MESSAGE
        ldap_get_option(ld, LDAP_OPT_DIAGNOSTIC_MESSAGE, &s);
#else
        ldap_get_option(ld, LDAP_OPT_ERROR_STRING, &s);
#endif
        fprintf(stderr, _("Enrollment failed. %s\n"), s);
        if (debug) {
            fprintf(stderr, "ldap_extended_operation_s failed: %s",
                            ldap_err2string(rc));
        }
        rval = 13;
        goto ldap_done;
    }

    /* Get the value from the result returned by the server. */
    *princ = strdup(valresult->bv_val);

ldap_done:
    if (ld != NULL) {
        ldap_unbind_ext(ld, NULL, NULL);
    }

done:
    free(ldap_base);
    if (valresult) ber_bvfree(valresult);
    if (oidresult) free(oidresult);
    return rval;
}

#ifdef WITH_IPA_JOIN_XML
static int
join_krb5_xmlrpc(const char *ipaserver, const char *hostname, char **hostdn, const char **princ, bool force, bool quiet) {
    xmlrpc_env env;
    xmlrpc_value * argArrayP = NULL;
    xmlrpc_value * paramArrayP = NULL;
    xmlrpc_value * paramP = NULL;
    xmlrpc_value * optionsP = NULL;
    xmlrpc_value * resultP = NULL;
    xmlrpc_value * structP = NULL;
    xmlrpc_server_info * serverInfoP = NULL;
    struct utsname uinfo;
    xmlrpc_value *princP = NULL;
    xmlrpc_value *krblastpwdchangeP = NULL;
    xmlrpc_value *hostdnP = NULL;
    const char *krblastpwdchange = NULL;
    char * url = NULL;
    char * user_agent = NULL;
    int rval = 0;
    int ret;

    *hostdn = NULL;
    *princ = NULL;

    /* Start up our XML-RPC client library. */
    xmlrpc_client_init(XMLRPC_CLIENT_NO_FLAGS, NAME, VERSION);

    uname(&uinfo);

    xmlrpc_env_init(&env);

    xmlrpc_client_setup_global_const(&env);

#if 1
    ret = asprintf(&url, "https://%s:443/ipa/xml", ipaserver);
#else
    ret = asprintf(&url, "http://%s:8888/", ipaserver);
#endif
    if (ret == -1)
    {
        fprintf(stderr, _("Out of memory!\n"));
        rval = 3;
        goto cleanup;
    }

    serverInfoP = xmlrpc_server_info_new(&env, url);

    argArrayP = xmlrpc_array_new(&env);
    paramArrayP = xmlrpc_array_new(&env);
    paramP = xmlrpc_string_new(&env, hostname);
    xmlrpc_array_append_item(&env, argArrayP, paramP);
#ifdef REALM
    if (!quiet)
        printf("Joining %s to IPA realm %s\n", hostname, iparealm);
#endif
    xmlrpc_array_append_item(&env, paramArrayP, argArrayP);
    xmlrpc_DECREF(paramP);

    optionsP = xmlrpc_build_value(&env, "{s:s,s:s}",
                                  "nsosversion", uinfo.release,
                                  "nshardwareplatform", uinfo.machine);
    xmlrpc_array_append_item(&env, paramArrayP, optionsP);
    xmlrpc_DECREF(optionsP);

    if ((user_agent = set_user_agent(ipaserver)) == NULL) {
        rval = 3;
        goto cleanup;
    }
    callRPC(user_agent, &env, serverInfoP, "join", paramArrayP, &resultP);
    if (handle_fault(&env)) {
        rval = 17;
        goto cleanup_xmlrpc;
    }

    /* Return value is the form of an array. The first value is the
     * DN, the second a struct of attribute values
     */
    xmlrpc_array_read_item(&env, resultP, 0, &hostdnP);
    xmlrpc_read_string(&env, hostdnP, (const char **)hostdn);
    xmlrpc_DECREF(hostdnP);
    xmlrpc_array_read_item(&env, resultP, 1, &structP);

    xmlrpc_struct_find_value(&env, structP, "krbprincipalname", &princP);
    if (princP) {
        xmlrpc_value * singleprincP = NULL;

        /* FIXME: all values are returned as lists currently. Once this is
         * fixed we can read the string directly.
         */
        xmlrpc_array_read_item(&env, princP, 0, &singleprincP);
        xmlrpc_read_string(&env, singleprincP, &*princ);
        xmlrpc_DECREF(princP);
        xmlrpc_DECREF(singleprincP);
    } else {
        fprintf(stderr, _("principal not found in XML-RPC response\n"));
        rval = 12;
        goto cleanup;
    }
    xmlrpc_struct_find_value(&env, structP, "krblastpwdchange", &krblastpwdchangeP);
    if (krblastpwdchangeP && !force) {
        xmlrpc_value * singleprincP = NULL;

        /* FIXME: all values are returned as lists currently. Once this is
         * fixed we can read the string directly.
         */
        xmlrpc_array_read_item(&env, krblastpwdchangeP, 0, &singleprincP);
        xmlrpc_read_string(&env, singleprincP, &krblastpwdchange);
        xmlrpc_DECREF(krblastpwdchangeP);
        fprintf(stderr, _("Host is already joined.\n"));
        rval = 13;
        goto cleanup;
    }

cleanup:
    if (argArrayP) xmlrpc_DECREF(argArrayP);
    if (paramArrayP) xmlrpc_DECREF(paramArrayP);
    if (resultP) xmlrpc_DECREF(resultP);

cleanup_xmlrpc:
    free(user_agent);
    free(url);
    free((char *)krblastpwdchange);
    xmlrpc_env_clean(&env);
    xmlrpc_client_cleanup();

    return rval;
}

#else // ifdef WITH_IPA_JOIN_XML

static inline struct curl_slist *
curl_slist_append_log(struct curl_slist *list, char *string, bool quiet) {
    list = curl_slist_append(list, string);
    if (!list) {
        fprintf(stderr, _("curl_slist_append() failed for value: '%s'\n"), string);
        return NULL;
    }
    return list;
}

#define CURL_SETOPT(curl, opt, val) \
    if (curl_easy_setopt(curl, opt, val) != CURLE_OK) { \
        fprintf(stderr, _("curl_easy_setopt() failed\n")); \
        rval = 17; \
        goto cleanup; \
    }

size_t
jsonrpc_handle_response(char *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t realsize = size * nmemb;
    curl_buffer *cb = (curl_buffer *) userdata;

    char *buf = (char *) realloc(cb->payload, cb->size + realsize + 1);
    if (!buf) {
        fprintf(stderr, _("Expanding buffer in jsonrpc_handle_response failed"));
        free(cb->payload);
        cb->payload = NULL;
        return 0;
    }
    cb->payload = buf;
    memcpy(&(cb->payload[cb->size]), ptr, realsize);

    cb->size += realsize;
    cb->payload[cb->size] = 0;

    return realsize;
}

static int
jsonrpc_request(const char *ipaserver, const json_t *json, curl_buffer *response, bool quiet) {
    int rval = 0;

    CURL *curl = NULL;

    char *url = NULL;
    char *referer = NULL;
    char *user_agent = NULL;
    struct curl_slist *headers = NULL;

    char *json_str = NULL;

    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, _("curl_global_init() failed\n"));
        rval = 17;
        goto cleanup;
    }

    curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, _("curl_easy_init() failed\n"));
        rval = 17;
        goto cleanup;
    }

    /* setting endpoint and custom headers */
    ASPRINTF(&url, "https://%s/ipa/json", ipaserver);
    CURL_SETOPT(curl, CURLOPT_URL, url);

    ASPRINTF(&referer, "referer: https://%s/ipa", ipaserver);
    headers = curl_slist_append_log(headers, referer, quiet);
    if (!headers) {
        rval = 17;
        goto cleanup;
    }

    ASPRINTF(&user_agent, "User-Agent: %s/%s", NAME, VERSION);
    headers = curl_slist_append_log(headers, user_agent, quiet);
    if (!headers) {
        rval = 17;
        goto cleanup;
    }

    headers = curl_slist_append_log(headers, "Accept: application/json", quiet);
    if (!headers) {
        rval = 17;
        goto cleanup;
    }

    headers = curl_slist_append_log(headers, "Content-Type: application/json", quiet);
    if (!headers) {
        rval = 17;
        goto cleanup;
    }
    CURL_SETOPT(curl, CURLOPT_HTTPHEADER, headers);

    CURL_SETOPT(curl, CURLOPT_CAINFO, DEFAULT_CA_CERT_FILE);

    CURL_SETOPT(curl, CURLOPT_WRITEFUNCTION, &jsonrpc_handle_response);
    CURL_SETOPT(curl, CURLOPT_WRITEDATA, response);

    CURL_SETOPT(curl, CURLOPT_HTTPAUTH, CURLAUTH_NEGOTIATE);
    CURL_SETOPT(curl, CURLOPT_USERPWD, ":");

    if (debug)
        CURL_SETOPT(curl, CURLOPT_VERBOSE, 1L);

    json_str = json_dumps(json, 0);
    if (!json_str) {
        fprintf(stderr, _("json_dumps() failed\n"));
        rval = 17;
        goto cleanup;
    }
    CURL_SETOPT(curl, CURLOPT_POSTFIELDS, json_str);

    if (debug)
        fprintf(stderr, _("JSON-RPC request:\n%s\n"), json_str);

    /* Perform the call and check for errors */
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK)
    {
        fprintf(stderr, _("JSON-RPC call failed: %s\n"), curl_easy_strerror(res));

        rval = 17;
        goto cleanup;
    }

    long resp_code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp_code);

    if (resp_code != 200) {
        if (resp_code == 401)
            fprintf(stderr, _("JSON-RPC call was unauthorized. Check your credentials.\n"));
        else
            fprintf(stderr, _("JSON-RPC call failed with status code: %li\n"), resp_code);

        rval = 17;
        goto cleanup;
    }

    if (debug && response->payload) {
        fprintf(stderr, _("JSON-RPC response:\n%s\n"), response->payload);
    }

cleanup:
    curl_slist_free_all(headers);

    if (curl)
        curl_easy_cleanup(curl);
    curl_global_cleanup();

    if (url)
        free(url);
    if (referer)
        free(referer);
    if (user_agent)
        free(user_agent);

    if (json_str)
        free(json_str);

    return rval;
}

static int
jsonrpc_parse_error(json_t *j_error_obj) {
    int rval = 0;

    json_error_t j_error;

    int error_code = 0;
    char *error_message = NULL;
    if (json_unpack_ex(j_error_obj, &j_error, 0, "{s:i, s:s}",
                       "code", &error_code,
                       "message", &error_message) != 0) {
        if (debug)
            fprintf(stderr, _("Extracting the error from the JSON-RPC response failed: %s\n"), j_error.text);

        rval = 17;
        goto cleanup;
    }

    switch (error_code) {
    case 2100:
        fprintf(stderr, _("No permission to join this host to the IPA domain.\n"));
        rval = 1;
        break;
    default:
        if (error_message)
            fprintf(stderr, "%s\n", error_message);
        rval = 1;
        break;
    }

cleanup:
    return rval;
}

static int
jsonrpc_parse_response(const char *payload, json_t** j_result_obj, bool quiet) {
    int rval = 0;

    json_error_t j_error;

    json_t *j_root = NULL;
    json_t *j_error_obj = NULL;

    j_root = json_loads(payload, 0, &j_error);
    if (!j_root) {
        fprintf(stderr, _("Parsing JSON-RPC response failed: %s\n"), j_error.text);

        rval = 17;
        goto cleanup;
    }

    j_error_obj = json_object_get(j_root, "error");
    if (j_error_obj && !json_is_null(j_error_obj))
    {
        rval = jsonrpc_parse_error(j_error_obj);
        goto cleanup;
    }

    *j_result_obj = json_object_get(j_root, "result");
    if (!*j_result_obj) {
        fprintf(stderr, _("Parsing JSON-RPC response failed: no 'result' value found.\n"));

        rval = 17;
        goto cleanup;
    }
    json_incref(*j_result_obj);

cleanup:
    json_decref(j_root);

    return rval;
}

static int
jsonrpc_parse_join_response(const char *payload, join_info *join_i, bool quiet) {
    int rval = 0;

    json_error_t j_error;

    json_t *j_result_obj = NULL;

    rval = jsonrpc_parse_response(payload, &j_result_obj, quiet);
    if (rval)
        goto cleanup;

    char *tmp_hostdn = NULL;
    char *tmp_princ = NULL;
    char *tmp_pwdch = NULL;
    if (json_unpack_ex(j_result_obj, &j_error, 0, "[s, {s:[s], s?:[s]}]",
                       &tmp_hostdn,
                       "krbprincipalname", &tmp_princ,
                       "krblastpwdchange", &tmp_pwdch) != 0) {
        fprintf(stderr, _("Extracting the data from the JSON-RPC response failed: %s\n"), j_error.text);

        rval = 17;
        goto cleanup;
    }
    ASPRINTF(&join_i->dn, "%s", tmp_hostdn);
    ASPRINTF(&join_i->krb_principal, "%s", tmp_princ);

    join_i->is_provisioned = tmp_pwdch != NULL;

cleanup:
    json_decref(j_result_obj);

    return rval;
}

static int
join_krb5_jsonrpc(const char *ipaserver, const char *hostname, char **hostdn, const char **princ, bool force, bool quiet) {
    int rval = 0;

    struct utsname uinfo;

    curl_buffer cb = {0};

    json_error_t j_error;
    json_t *json_req = NULL;

    join_info join_i = {0};

    *hostdn = NULL;
    *princ = NULL;

    uname(&uinfo);

    /* create the JSON-RPC payload */
    json_req = json_pack_ex(&j_error, 0, "{s:s, s:[[s], {s:s, s:s}]}",
                             "method", "join",
                             "params",
                             hostname,
                             "nsosversion", uinfo.release,
                             "nshardwareplatform", uinfo.machine);

    if (!json_req) {
        fprintf(stderr, _("json_pack_ex() failed: %s\n"), j_error.text);

        rval = 17;
        goto cleanup;
    }

    rval = jsonrpc_request(ipaserver, json_req, &cb, quiet);
    if (rval != 0)
        goto cleanup;

    rval = jsonrpc_parse_join_response(cb.payload, &join_i, quiet);
    if (rval != 0)
        goto cleanup;

    *hostdn = join_i.dn;
    *princ = join_i.krb_principal;

    if (!force && join_i.is_provisioned) {
        fprintf(stderr, _("Host is already joined.\n"));
        rval = 13;
        goto cleanup;
    }

cleanup:
    json_decref(json_req);

    if (cb.payload)
        free(cb.payload);

    return rval;
}

static int
jsonrpc_parse_unenroll_response(const char *payload, bool* result, bool quiet) {
    int rval = 0;

    json_error_t j_error;

    json_t *j_result_obj = NULL;

    rval = jsonrpc_parse_response(payload, &j_result_obj, quiet);
    if (rval)
        goto cleanup;

    if (json_unpack_ex(j_result_obj, &j_error, 0, "{s:b}",
                       "result", result) != 0) {
        fprintf(stderr, _("Extracting the data from the JSON-RPC response failed: %s\n"), j_error.text);

        rval = 20;
        goto cleanup;
    }

cleanup:
    json_decref(j_result_obj);

    return rval;
}

static int
jsonrpc_unenroll_host(const char *ipaserver, const char *host, bool quiet) {
    int rval = 0;

    curl_buffer cb = {0};

    json_error_t j_error;
    json_t *json_req = NULL;

    bool result = false;

    /* create the JSON-RPC payload */
    json_req = json_pack_ex(&j_error, 0, "{s:s, s:[[s], {}]}",
                            "method", "host_disable",
                            "params",
                            host);

    if (!json_req) {
        fprintf(stderr, _("json_pack_ex() failed: %s\n"), j_error.text);

        rval = 17;
        goto cleanup;
    }

    rval = jsonrpc_request(ipaserver, json_req, &cb, quiet);
    if (rval != 0)
        goto cleanup;

    rval = jsonrpc_parse_unenroll_response(cb.payload, &result, quiet);
    if (rval != 0)
        goto cleanup;

    if (result == true) {
        if (!quiet)
            fprintf(stderr, _("Unenrollment successful.\n"));
    } else {
        fprintf(stderr, _("Unenrollment failed.\n"));
    }

cleanup:
    json_decref(json_req);

    if (cb.payload)
        free(cb.payload);

    return rval;
}
#endif

#ifdef WITH_IPA_JOIN_XML
static int
xmlrpc_unenroll_host(const char *ipaserver, const char *host, bool quiet)
{
    int rval = 0;
    int ret;

    xmlrpc_env env;
    xmlrpc_value * argArrayP = NULL;
    xmlrpc_value * paramArrayP = NULL;
    xmlrpc_value * paramP = NULL;
    xmlrpc_value * resultP = NULL;
    xmlrpc_server_info * serverInfoP = NULL;
    xmlrpc_value *princP = NULL;
    char * url = NULL;
    char * user_agent = NULL;

    /* Start up our XML-RPC client library. */
    xmlrpc_client_init(XMLRPC_CLIENT_NO_FLAGS, NAME, VERSION);

    xmlrpc_env_init(&env);

    xmlrpc_client_setup_global_const(&env);

#if 1
    ret = asprintf(&url, "https://%s:443/ipa/xml", ipaserver);
#else
    ret = asprintf(&url, "http://%s:8888/", ipaserver);
#endif
    if (ret == -1)
    {
        fprintf(stderr, _("Out of memory!\n"));
        rval = 3;
        goto cleanup;
    }
    serverInfoP = xmlrpc_server_info_new(&env, url);

    argArrayP = xmlrpc_array_new(&env);
    paramArrayP = xmlrpc_array_new(&env);

    paramP = xmlrpc_string_new(&env, host);
    xmlrpc_array_append_item(&env, argArrayP, paramP);
    xmlrpc_array_append_item(&env, paramArrayP, argArrayP);
    xmlrpc_DECREF(paramP);

    if ((user_agent = set_user_agent(ipaserver)) == NULL) {
        rval = 3;
        goto cleanup;
    }
    callRPC(user_agent, &env, serverInfoP, "host_disable", paramArrayP, &resultP);
    if (handle_fault(&env)) {
        rval = 17;
        goto cleanup;
    }

    xmlrpc_struct_find_value(&env, resultP, "result", &princP);
    if (princP) {
        xmlrpc_bool result;

        xmlrpc_read_bool(&env, princP, &result);
        if (result == 1) {
            if (!quiet)
                fprintf(stderr, _("Unenrollment successful.\n"));
        } else {
            fprintf(stderr, _("Unenrollment failed.\n"));
        }

        xmlrpc_DECREF(princP);
    } else {
        fprintf(stderr, _("result not found in XML-RPC response\n"));
        rval = 20;
        goto cleanup;
    }

cleanup:
    free(user_agent);
    free(url);

    if (argArrayP)
        xmlrpc_DECREF(argArrayP);
    if (paramArrayP)
        xmlrpc_DECREF(paramArrayP);

    xmlrpc_env_clean(&env);
    xmlrpc_client_cleanup();

    return rval;
}
#endif

static int
join(const char *server, const char *hostname, const char *bindpw, const char *basedn, const char *keytab, bool force, bool quiet)
{
    int rval = 0;
    pid_t childpid = 0;
    int status = 0;
    char *ipaserver = NULL;
    char *iparealm = NULL;
    const char * princ = NULL;
    char * hostdn = NULL;

    krb5_context krbctx = NULL;
    krb5_ccache ccache = NULL;
    krb5_principal uprinc = NULL;
    krb5_error_code krberr;

    if (server) {
        ipaserver = strdup(server);
    } else {
        char * conf_data = read_config_file(IPA_CONFIG);
        if ((ipaserver = getIPAserver(conf_data)) == NULL) {
            fprintf(stderr, _("Unable to determine IPA server from %s\n"),
                    IPA_CONFIG);
            exit(1);
        }
        free(conf_data);
    }

    if (bindpw)
        rval = join_ldap(ipaserver, hostname, &hostdn, bindpw, basedn, &princ, quiet);
    else {
        krberr = krb5_init_context(&krbctx);
        if (krberr) {
            fprintf(stderr, _("Unable to join host: "
                              "Kerberos context initialization failed\n"));
            rval = 1;
            goto cleanup;
        }
        krberr = krb5_cc_default(krbctx, &ccache);
        if (krberr) {
            fprintf(stderr, _("Unable to join host:"
                              " Kerberos Credential Cache not found\n"));
            rval = 5;
            goto cleanup;
        }

        krberr = krb5_cc_get_principal(krbctx, ccache, &uprinc);
        if (krberr) {
            fprintf(stderr, _("Unable to join host: Kerberos User Principal "
                              "not found and host password not provided.\n"));
            rval = 6;
            goto cleanup;
        }

#ifdef WITH_IPA_JOIN_XML
        rval = join_krb5_xmlrpc(ipaserver, hostname, &hostdn, &princ, force, quiet);
#else
        rval = join_krb5_jsonrpc(ipaserver, hostname, &hostdn, &princ, force, quiet);
#endif
    }

    if (rval) goto cleanup;

    /* Fork off and let ipa-getkeytab generate the keytab for us */
    childpid = fork();

    if (childpid < 0) {
        fprintf(stderr, _("fork() failed\n"));
        rval = 1;
        goto cleanup;
    }

    if (childpid == 0) {
        char *argv[12];
        char *path = "/usr/sbin/ipa-getkeytab";
        int arg = 0;
        int err;

        argv[arg++] = path;
        argv[arg++] = "-s";
        argv[arg++] = ipaserver;
        argv[arg++] = "-p";
        argv[arg++] = (char *)princ;
        argv[arg++] = "-k";
        argv[arg++] = (char *)keytab;
        if (bindpw) {
            argv[arg++] = "-D";
            argv[arg++] = (char *)hostdn;
            argv[arg++] = "-w";
            argv[arg++] = (char *)bindpw;
        }
        if (quiet) {
            argv[arg++] = "-q";
        }
        argv[arg++] = NULL;
        err = execv(path, argv);
        if (err == -1) {
            switch(errno) {
            case ENOENT:
                fprintf(stderr, _("ipa-getkeytab not found\n"));
                break;
            case EACCES:
                fprintf(stderr, _("ipa-getkeytab has bad permissions?\n"));
                break;
            default:
                fprintf(stderr, _("executing ipa-getkeytab failed, "
                                  "errno %d\n"), errno);
                break;
            }
        }
    } else {
        wait(&status);
    }

    if WIFEXITED(status) {
        rval = WEXITSTATUS(status);
        if (rval != 0) {
            fprintf(stderr, _("child exited with %d\n"), rval);
        }
    }

cleanup:
    free((char *)princ);

    if (bindpw)
        ldap_memfree((void *)hostdn);
    else
        free((char *)hostdn);

    free((char *)ipaserver);
    free((char *)iparealm);
    if (uprinc) krb5_free_principal(krbctx, uprinc);
    if (ccache) krb5_cc_close(krbctx, ccache);
    if (krbctx) krb5_free_context(krbctx);

    return rval;
}

static int
unenroll_host(const char *server, const char *hostname, const char *ktname, bool quiet)
{
    int rval = 0;

    char *ipaserver = NULL;
    char *principal = NULL;
    char *realm = NULL;

    krb5_context krbctx = NULL;
    krb5_keytab keytab = NULL;
    krb5_ccache ccache = NULL;
    krb5_principal princ = NULL;
    krb5_error_code krberr;
    krb5_creds creds;
    krb5_get_init_creds_opt gicopts;
    char tgs[LINE_MAX];

    memset(&creds, 0, sizeof(creds));

    if (server) {
        ipaserver = strdup(server);
    } else {
        char * conf_data = read_config_file(IPA_CONFIG);
        if ((ipaserver = getIPAserver(conf_data)) == NULL) {
            fprintf(stderr, _("Unable to determine IPA server from %s\n"),
                    IPA_CONFIG);
            exit(1);
        }
        free(conf_data);
    }

    krberr = krb5_init_context(&krbctx);
    if (krberr) {
        fprintf(stderr, _("Unable to join host: "
                          "Kerberos context initialization failed\n"));
        rval = 1;
        goto cleanup;
    }

    krberr = krb5_kt_resolve(krbctx, ktname, &keytab);
    if (krberr != 0) {
        fprintf(stderr, _("Error resolving keytab: %s.\n"),
                error_message(krberr));
        rval = 7;
        goto cleanup;
    }

    krberr = krb5_get_default_realm(krbctx, &realm);
    if (krberr != 0) {
        fprintf(stderr, _("Error getting default Kerberos realm: %s.\n"),
                error_message(krberr));
        rval = 21;
        goto cleanup;
    }

    ASPRINTF(&principal, "host/%s@%s", hostname,  realm);

    krberr = krb5_parse_name(krbctx, principal, &princ);
    if (krberr != 0) {
        fprintf(stderr, _("Error parsing \"%1$s\": %2$s.\n"),
                principal, error_message(krberr));
        rval = 4;
        goto cleanup;
    }
    strcpy(tgs, KRB5_TGS_NAME);
    snprintf(tgs + strlen(tgs), sizeof(tgs) - strlen(tgs), "/%.*s",
             (krb5_princ_realm(krbctx, princ))->length,
             (krb5_princ_realm(krbctx, princ))->data);
    snprintf(tgs + strlen(tgs), sizeof(tgs) - strlen(tgs), "@%.*s",
             (krb5_princ_realm(krbctx, princ))->length,
             (krb5_princ_realm(krbctx, princ))->data);

    krb5_get_init_creds_opt_init(&gicopts);
    krb5_get_init_creds_opt_set_forwardable(&gicopts, 1);
    krberr = krb5_get_init_creds_keytab(krbctx, &creds, princ, keytab,
                                        0, tgs, &gicopts);
    if (krberr != 0) {
        fprintf(stderr, _("Error obtaining initial credentials: %s.\n"),
                error_message(krberr));
        rval = 19;
        goto cleanup;
    }

    krberr = krb5_cc_resolve(krbctx, "MEMORY:ipa-join", &ccache);
    if (krberr == 0) {
        krberr = krb5_cc_initialize(krbctx, ccache, creds.client);
    } else {
        fprintf(stderr,
                _("Unable to generate Kerberos Credential Cache\n"));
        rval = 19;
        goto cleanup;
    }

    if (krberr != 0) {
        fprintf(stderr,
                _("Unable to generate Kerberos Credential Cache\n"));
        rval = 19;
        goto cleanup;
    }

    krberr = krb5_cc_store_cred(krbctx, ccache, &creds);
    if (krberr != 0) {
        fprintf(stderr,
                _("Error storing creds in credential cache: %s.\n"),
                error_message(krberr));
        rval = 19;
        goto cleanup;
    }
    krb5_cc_close(krbctx, ccache);
    ccache = NULL;
    putenv("KRB5CCNAME=MEMORY:ipa-join");

#ifdef WITH_IPA_JOIN_XML
    rval = xmlrpc_unenroll_host(ipaserver, hostname, quiet);
#else
    rval = jsonrpc_unenroll_host(ipaserver, hostname, quiet);
#endif

cleanup:
    if (principal)
        free(principal);
    if (ipaserver)
        free(ipaserver);
    if (realm)
        krb5_free_default_realm(krbctx, realm);

    if (keytab)
        krb5_kt_close(krbctx, keytab);
    if (princ)
        krb5_free_principal(krbctx, princ);
    if (ccache)
        krb5_cc_close(krbctx, ccache);

    krb5_free_cred_contents(krbctx, &creds);

    if (krbctx)
        krb5_free_context(krbctx);

    return rval;
}

/*
 * Note, an intention with return values is so that this is compatible with
 * ipa-getkeytab. This is so based on the return value you can distinguish
 * between errors common between the two (no kerbeors ccache) and those
 * unique (host already added).
 */
int
main(int argc, const char **argv) {
    static const char *hostname = NULL;
    static const char *server = NULL;
    static const char *keytab = NULL;
    static const char *bindpw = NULL;
    static const char *basedn = NULL;
    int quiet = 0;
    int unenroll = 0;
    int force = 0;
    struct poptOption options[] = {
        { "debug", 'd', POPT_ARG_NONE, &debug, 0,
          _("Print the raw XML-RPC output in GSSAPI mode"), NULL },
        { "quiet", 'q', POPT_ARG_NONE, &quiet, 0,
          _("Quiet mode. Only errors are displayed."), NULL },
        { "unenroll", 'u', POPT_ARG_NONE, &unenroll, 0,
          _("Unenroll this host from IPA server"), NULL },
        { "hostname", 'h', POPT_ARG_STRING, &hostname, 0,
          _("Hostname of this server"), _("hostname") },
        { "server", 's', POPT_ARG_STRING, &server, 0,
          _("IPA Server to use"), _("hostname") },
        { "keytab", 'k', POPT_ARG_STRING, &keytab, 0,
          _("Specifies where to store keytab information."), _("filename") },
        { "force", 'f', POPT_ARG_NONE, &force, 0,
          _("Force the host join. Rejoin even if already joined."), NULL },
        { "bindpw", 'w', POPT_ARG_STRING, &bindpw, 0,
          _("LDAP password (if not using Kerberos)"), _("password") },
        { "basedn", 'b', POPT_ARG_STRING, &basedn, 0,
          _("LDAP basedn"), _("basedn") },
        POPT_AUTOHELP
        POPT_TABLEEND
    };
    poptContext pc;
    int ret;

    ret = init_gettext();
    if (ret) {
        fprintf(stderr, "Failed to load translations\n");
    }

    pc = poptGetContext("ipa-join", argc, (const char **)argv, options, 0);
    ret = poptGetNextOpt(pc);
    if (ret != -1) {
        if (!quiet) {
            poptPrintUsage(pc, stderr, 0);
        }
        poptFreeContext(pc);
        exit(2);
    }
    poptFreeContext(pc);

    if (debug)
        setenv("XMLRPC_TRACE_XML", "1", 1);

    if (!keytab)
        keytab = "/etc/krb5.keytab";

    /* auto-detect and verify hostname */
    if (!hostname) {
        hostname = ipa_gethostfqdn();
        if (hostname == NULL) {
            fprintf(stderr, _("Cannot get host's FQDN!\n"));
            exit(22);
        }
    }
    if (NULL == strstr(hostname, ".")) {
        fprintf(stderr, _("The hostname must be fully-qualified: %s\n"), hostname);
        exit(16);
    }
    if ((strcmp(hostname, "localhost") == 0) || (strcmp(hostname, "localhost.localdomain") == 0)){
        fprintf(stderr, _("The hostname must not be: %s\n"), hostname);
        exit(16);
    }

    if (unenroll) {
        ret = unenroll_host(server, hostname, keytab, quiet);
    } else {
        ret = check_perms(keytab);
        if (ret == 0)
            ret = join(server, hostname, bindpw, basedn, keytab, force, quiet);
    }

    exit(ret);
}

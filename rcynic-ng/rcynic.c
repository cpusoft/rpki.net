/*
 * Copyright (C) 2009--2011  Internet Systems Consortium ("ISC")
 * 
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * Portions copyright (C) 2006--2008  American Registry for Internet Numbers ("ARIN")
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND ARIN DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS.  IN NO EVENT SHALL ARIN BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/* $Id$ */

/**
 * @mainpage
 *
 * "Cynical rsync": Recursively walk RPKI tree using rsync to pull
 * data from remote sites, validating certificates and CRLs as we go.
 *
 * Doxygen doesn't quite know what to make of a one-file C program,
 * and ends up putting most of the interesting data @link rcynic.c
 * here. @endlink
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/file.h>
#include <errno.h>
#include <sys/signal.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <fcntl.h>
#include <signal.h>

#define SYSLOG_NAMES		/* defines CODE prioritynames[], facilitynames[] */
#include <syslog.h>

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/safestack.h>
#include <openssl/conf.h>
#include <openssl/rand.h>
#include <openssl/asn1t.h>
#include <openssl/cms.h>

#include "defstack.h"
#include "defasn1.h"

#ifndef FILENAME_MAX
#define	FILENAME_MAX	1024
#endif

#define	SCHEME_RSYNC	("rsync://")
#define	SIZEOF_RSYNC	(sizeof(SCHEME_RSYNC) - 1)

/**
 * Maximum length of an URI.
 */
#define	URI_MAX		(FILENAME_MAX + SIZEOF_RSYNC)

/**
 * Maximum number of times we try to kill an inferior process before
 * giving up.
 */
#define	KILL_MAX	10

#ifndef	HOSTNAME_MAX
#define	HOSTNAME_MAX	256
#endif

/**
 * Version number of XML summary output.
 */
#define	XML_SUMMARY_VERSION	1

/**
 * How much buffer space do we need for a raw address?
 */
#define ADDR_RAW_BUF_LEN	16

/**
 * Logging levels.  Same general idea as syslog(), but our own
 * catagories based on what makes sense for this program.  Default
 * mappings to syslog() priorities are here because it's the easiest
 * way to make sure that we assign a syslog level to each of ours.
 */

#define LOG_LEVELS							\
  QQ(log_sys_err,	LOG_ERR)	/* Error from OS or library  */	\
  QQ(log_usage_err,	LOG_ERR)	/* Bad usage (local error)   */	\
  QQ(log_data_err,	LOG_NOTICE)	/* Bad data, no biscuit      */	\
  QQ(log_telemetry,	LOG_INFO)	/* Normal progress chatter   */	\
  QQ(log_verbose,	LOG_INFO)	/* Extra chatter             */ \
  QQ(log_debug,		LOG_DEBUG)	/* Only useful when debugging */

#define QQ(x,y)	x ,
typedef enum log_level { LOG_LEVELS LOG_LEVEL_T_MAX } log_level_t;
#undef	QQ

#define	QQ(x,y)	{ #x , x },
static const struct {
  const char *name;
  log_level_t value;
} log_levels[] = {
  LOG_LEVELS
};
#undef	QQ

/**
 * MIB counters derived from OpenSSL.  Long list of validation failure
 * codes from OpenSSL (crypto/x509/x509_vfy.h).
 */

#define	MIB_COUNTERS_FROM_OPENSSL			\
  QV(X509_V_ERR_UNABLE_TO_GET_CRL)			\
  QV(X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE)	\
  QV(X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE)	\
  QV(X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY)	\
  QV(X509_V_ERR_CERT_SIGNATURE_FAILURE)			\
  QV(X509_V_ERR_CRL_SIGNATURE_FAILURE)			\
  QV(X509_V_ERR_CERT_NOT_YET_VALID)			\
  QV(X509_V_ERR_CERT_HAS_EXPIRED)			\
  QV(X509_V_ERR_CRL_NOT_YET_VALID)			\
  QV(X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD)		\
  QV(X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD)		\
  QV(X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD)		\
  QV(X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD)		\
  QV(X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT)		\
  QV(X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN)		\
  QV(X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY)	\
  QV(X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE)	\
  QV(X509_V_ERR_CERT_CHAIN_TOO_LONG)			\
  QV(X509_V_ERR_CERT_REVOKED)				\
  QV(X509_V_ERR_INVALID_CA)				\
  QV(X509_V_ERR_PATH_LENGTH_EXCEEDED)			\
  QV(X509_V_ERR_INVALID_PURPOSE)			\
  QV(X509_V_ERR_CERT_UNTRUSTED)				\
  QV(X509_V_ERR_CERT_REJECTED)				\
  QV(X509_V_ERR_AKID_SKID_MISMATCH)			\
  QV(X509_V_ERR_AKID_ISSUER_SERIAL_MISMATCH)		\
  QV(X509_V_ERR_KEYUSAGE_NO_CERTSIGN)			\
  QV(X509_V_ERR_UNABLE_TO_GET_CRL_ISSUER)		\
  QV(X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION)		\
  QV(X509_V_ERR_KEYUSAGE_NO_CRL_SIGN)			\
  QV(X509_V_ERR_UNHANDLED_CRITICAL_CRL_EXTENSION)	\
  QV(X509_V_ERR_INVALID_NON_CA)				\
  QV(X509_V_ERR_PROXY_PATH_LENGTH_EXCEEDED)		\
  QV(X509_V_ERR_KEYUSAGE_NO_DIGITAL_SIGNATURE)		\
  QV(X509_V_ERR_PROXY_CERTIFICATES_NOT_ALLOWED)		\
  QV(X509_V_ERR_INVALID_EXTENSION)			\
  QV(X509_V_ERR_INVALID_POLICY_EXTENSION)		\
  QV(X509_V_ERR_NO_EXPLICIT_POLICY)			\
  QV(X509_V_ERR_UNNESTED_RESOURCE)

/**
 * MIB counters specific to rcynic.  "validation_ok" is not used as a
 * counter, but is used as a validation status code.
 */

#define MIB_COUNTERS							    \
  QG(validation_ok,			"OK")				    \
  QG(backup_cert_accepted,		"Backup certificates accepted")	    \
  QB(backup_cert_rejected,		"Backup certificates rejected")	    \
  QG(backup_crl_accepted,		"Backup CRLs accepted")		    \
  QB(backup_crl_rejected,		"Backup CRLs rejected")		    \
  QG(current_cert_accepted,		"Current certificates accepted")    \
  QB(current_cert_rejected,		"Current certificates rejected")    \
  QG(current_crl_accepted,		"Current CRLs accepted")	    \
  QB(current_crl_rejected,		"Current CRLs rejected")	    \
  QG(current_manifest_accepted,		"Current Manifests accepted")	    \
  QB(current_manifest_rejected,		"Current Manifests rejected")	    \
  QG(backup_manifest_accepted,		"Backup Manifests accepted")	    \
  QB(backup_manifest_rejected,		"Backup Manifests rejected")	    \
  QB(rsync_failed,			"rsync transfers failed")	    \
  QG(rsync_succeeded,			"rsync transfers succeeded")	    \
  QB(rsync_timed_out,			"rsync transfers timed out")	    \
  QW(stale_crl,				"Stale CRLs")			    \
  QB(malformed_sia,			"Malformed SIA extensions")	    \
  QB(sia_missing,			"SIA extensions missing")	    \
  QB(aia_missing,			"AIA extensions missing")	    \
  QB(crldp_missing,			"CRLDP extensions missing")	    \
  QB(aia_mismatch,			"Mismatched AIA extensions")	    \
  QB(unknown_verify_error,		"Unknown OpenSSL verify error")	    \
  QG(current_cert_recheck,		"Certificates rechecked")	    \
  QB(manifest_invalid_ee,		"Invalid manifest certificates")    \
  QB(manifest_invalid_cms,		"Manifest validation failures")	    \
  QB(manifest_decode_error,		"Manifest decode errors")	    \
  QW(stale_manifest,			"Stale manifests")		    \
  QB(manifest_not_yet_valid,		"Manifests not yet valid")	    \
  QB(manifest_bad_econtenttype,		"Bad manifest eContentType")	    \
  QB(manifest_missing_signer,		"Missing manifest signers")	    \
  QB(manifest_missing_crldp,            "Missing manifest CRLDP")	    \
  QB(manifest_malformed_crldp,          "Malformed manifest CRLDP")	    \
  QB(certificate_digest_mismatch,	"Certificate digest mismatches")    \
  QB(crl_digest_mismatch,		"CRL digest mismatches")	    \
  QB(crl_not_in_manifest,               "CRL not listed in manifest")	    \
  QB(roa_invalid_ee,			"Invalid ROA certificates")	    \
  QB(roa_invalid_cms,			"ROA validation failures")	    \
  QB(roa_decode_error,			"ROA decode errors")		    \
  QB(roa_bad_econtenttype,		"Bad ROA eContentType")		    \
  QB(roa_missing_signer,		"Missing ROA signers")		    \
  QB(roa_digest_mismatch,		"ROA digest mismatches")	    \
  QG(current_roa_accepted,		"Current ROAs accepted")	    \
  QB(current_roa_rejected,		"Current ROAs rejected")	    \
  QG(backup_roa_accepted,		"Backup ROAs accepted")		    \
  QB(backup_roa_rejected,		"Backup ROAs rejected")		    \
  QB(malformed_roa_addressfamily,       "Malformed ROA addressFamilys")	    \
  QB(manifest_wrong_version,            "Wrong manifest versions")	    \
  QB(roa_wrong_version,			"Wrong ROA versions")		    \
  QW(trust_anchor_not_self_signed,	"Trust anchor not self-signed")	    \
  QB(uri_too_long,			"URI too long")			    \
  QB(malformed_crldp,			"Malformed CRDLP extension")	    \
  QB(certificate_bad_signature,		"Bad certificate signature")	    \
  QB(certificate_bad_crl,		"Bad certificate CRL")		    \
  QB(manifest_bad_crl,			"Manifest has bad CRL")		    \
  QB(roa_resources_malformed,		"ROA resources malformed")	    \
  QB(roa_bad_afi,			"ROA contains bad AFI value")	    \
  QB(roa_not_nested,			"ROA resources not in EE")	    \
  QB(roa_bad_crl,			"ROA EE has bad CRL")		    \
  QB(ghostbuster_digest_mismatch,	"Ghostbuster digest mismatches")    \
  QB(ghostbuster_bad_econtenttype,	"Bad Ghostbuster eContentType")	    \
  QB(ghostbuster_invalid_cms,		"Ghostbuster validation failures")  \
  QB(ghostbuster_missing_signer,	"Missing Ghostbuster signers")	    \
  QB(ghostbuster_bad_crl,		"Ghostbuster EE has bad CRL")	    \
  QB(ghostbuster_invalid_ee,		"Invalid Ghostbuster certificates") \
  QG(current_ghostbuster_accepted,	"Current Ghostbusters accepted")    \
  QB(current_ghostbuster_rejected,	"Current Ghostbusters rejected")    \
  QG(backup_ghostbuster_accepted,	"Backup Ghostbusters accepted")	    \
  QB(backup_ghostbuster_rejected,	"Backup Ghostbusters rejected")	    \
  QB(disallowed_extension,		"Disallowed X.509v3 extension")     \
  QB(crldp_mismatch,			"CRLDP doesn't match issuer's SIA") \
  QB(manifest_missing,			"Manifest pointer missing")	    \
  QB(manifest_mismatch,			"Manifest doesn't match SIA")	    \
  QB(trust_anchor_with_crldp,		"Trust anchor can't have CRLDP")    \
  QW(object_not_in_manifest,		"Object not in manifest")	    \
  QB(hash_too_long,			"Hash value is too long")	    \
  MIB_COUNTERS_FROM_OPENSSL

#define QV(x) QB(mib_openssl_##x, 0)

static const char
  mib_counter_kind_good[] = "good",
  mib_counter_kind_warn[] = "warn",
  mib_counter_kind_bad[]  = "bad";

#define QG(x,y)	mib_counter_kind_good ,
#define QW(x,y) mib_counter_kind_warn ,
#define QB(x,y) mib_counter_kind_bad ,
static const char * const mib_counter_kind[] = { MIB_COUNTERS NULL };
#undef QB
#undef QW
#undef QG

#define	QG(x,y) QQ(x,y)
#define	QW(x,y) QQ(x,y)
#define QB(x,y) QQ(x,y)

#define QQ(x,y) x ,
typedef enum mib_counter { MIB_COUNTERS MIB_COUNTER_T_MAX } mib_counter_t;
#undef	QQ

#define QQ(x,y) y ,
static const char * const mib_counter_desc[] = { MIB_COUNTERS NULL };
#undef	QQ

#define QQ(x,y) #x ,
static const char * const mib_counter_label[] = { MIB_COUNTERS NULL };
#undef	QQ

#undef	QV

#define	QQ(x,y)	0 ,
#define	QV(x)   x ,
static const long mib_counter_openssl[] = { MIB_COUNTERS 0 };
#undef	QV
#undef	QQ

/**
 * Type-safe string wrapper for URIs.
 */
typedef struct { char s[URI_MAX]; } uri_t;

/**
 * Type-safe string wrapper for filename paths.
 */
typedef struct { char s[FILENAME_MAX]; } path_t;

/**
 * Type-safe string wrapper for hostnames.
 */
typedef struct { char s[HOSTNAME_MAX]; } hostname_t;

/**
 * Type-safe wrapper for hash buffers.
 */
typedef struct { unsigned char h[EVP_MAX_MD_SIZE]; } hashbuf_t;

/**
 * Per-host MIB counter object.
 * hostname[] must be first element.
 */
typedef struct host_mib_counter {
  path_t hostname;		/* uri_to_filename() wants path_t */
  unsigned long counters[MIB_COUNTER_T_MAX];
} HOST_MIB_COUNTER;

DECLARE_STACK_OF(HOST_MIB_COUNTER)

/**
 * Per-URI validation status object.
 */
typedef struct validation_status {
  uri_t uri;
  time_t timestamp;
  mib_counter_t code;
} VALIDATION_STATUS;

DECLARE_STACK_OF(VALIDATION_STATUS)

/**
 * Structure to hold data parsed out of a certificate.
 */
typedef struct certinfo {
  int ca, ta;
  uri_t uri, sia, aia, crldp, manifest;
} certinfo_t;

/**
 * Program context that would otherwise be a mess of global variables.
 */
typedef struct rcynic_ctx {
  path_t authenticated, old_authenticated, unauthenticated;
  char *jane, *rsync_program;
  STACK_OF(OPENSSL_STRING) *rsync_cache, *backup_cache, *stale_cache;
  STACK_OF(HOST_MIB_COUNTER) *host_counters;
  STACK_OF(VALIDATION_STATUS) *validation_status;
  int indent, use_syslog, allow_stale_crl, allow_stale_manifest, use_links;
  int require_crl_in_manifest, rsync_timeout, priority[LOG_LEVEL_T_MAX];
  int allow_non_self_signed_trust_anchor, allow_object_not_in_manifest;
  log_level_t log_level;
  X509_STORE *x509_store;
} rcynic_ctx_t;

/**
 * Context for certificate tree walks.  This includes all the stuff
 * that we would keep as automatic variables on the call stack if we
 * didn't have to use callbacks to support multiple rsync processes.
 *
 * Mapping between fields here and automatic variables in the older
 * code is still in flux, names (and anything else) may change.
 */
typedef enum {
  walk_state_initial,		/* Initial state */
  walk_state_rsync,		/* rsyncing certinfo.sia */
  walk_state_ready,		/* Ready to traverse outputs */
  walk_state_current,		/* prefix = rc->unauthenticated */
  walk_state_backup,		/* prefix = rc->old_authenticated */
  walk_state_done		/* Done walking this cert's outputs */
} walk_state_t;

typedef struct walk_ctx {
  unsigned refcount;
  certinfo_t certinfo;
  X509 *cert;
  Manifest *manifest;
  STACK_OF(OPENSSL_STRING) *filenames;
  int manifest_iteration, filename_iteration;
  walk_state_t state;
} walk_ctx_t;

DECLARE_STACK_OF(walk_ctx_t)

/**
 * Extended context for verify callbacks.  This is a wrapper around
 * OpenSSL's X509_STORE_CTX, and the embedded X509_STORE_CTX @em must be
 * the first element of this structure in order for the evil cast to
 * do the right thing.  This is ugly but safe, as the C language
 * promises us that the address of the first element of a structure is
 * the same as the address of the structure.
 */
typedef struct rcynic_x509_store_ctx {
  X509_STORE_CTX ctx;		/* Must be first */
  const rcynic_ctx_t *rc;
  const certinfo_t *subject;
} rcynic_x509_store_ctx_t;



/**
 * Subversion ID data.
 */
static const char svn_id[] = "$Id$";

/*
 * ASN.1 Object identifiers in form suitable for use with oid_cmp()
 */

/** 1.3.6.1.5.5.7.48.2 */
static const unsigned char id_ad_caIssuers[] =
  {0x2b, 0x6, 0x1, 0x5, 0x5, 0x7, 0x30, 0x2};

/** 1.3.6.1.5.5.7.48.5 */
static const unsigned char id_ad_caRepository[] =
  {0x2b, 0x6, 0x1, 0x5, 0x5, 0x7, 0x30, 0x5};

/** 1.3.6.1.5.5.7.48.10 */
static const unsigned char id_ad_rpkiManifest[] =
  {0x2b, 0x6, 0x1, 0x5, 0x5, 0x7, 0x30, 0xa};

/** 1.2.840.113549.1.9.16.1.24 */
static const unsigned char id_ct_routeOriginAttestation[] =
  {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x10, 0x01, 0x18};

/** 1.2.840.113549.1.9.16.1.26 */
static const unsigned char id_ct_rpkiManifest[] =
  {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x10, 0x01, 0x1a};

/** 1.2.840.113549.1.9.16.1.35 */
static const unsigned char id_ct_rpkiGhostbusters[] =
  {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x01, 0x09, 0x10, 0x01, 0x23};

/** 2.16.840.1.101.3.4.2.1 */
static const unsigned char id_sha256[] =
  {0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01};

/**
 * RPKI certificate policy OID in form suitable for use with
 * X509_VERIFY_PARAM_add0_policy().
 */
static const char rpki_policy_oid[] = "1.3.6.1.5.5.7.14.2";



/**
 * Type-safe wrapper around free() to keep safestack macros happy.
 */
static void OPENSSL_STRING_free(OPENSSL_STRING s)
{
  if (s)
    free(s);
}

/**
 * Wrapper around an idiom we use with OPENSSL_STRING stacks.  There's
 * a bug in the current sk_OPENSSL_STRING_delete() macro that casts
 * the return value to the wrong type, so we cast it to something
 * innocuous here and avoid using that macro elsewhere.
 */
static void sk_OPENSSL_STRING_remove(STACK_OF(OPENSSL_STRING) *sk, const char *str)
{
  OPENSSL_STRING_free((void *) sk_OPENSSL_STRING_delete(sk, sk_OPENSSL_STRING_find(sk, str)));
}

/**
 * Allocate a new HOST_MIB_COUNTER object.
 */
static HOST_MIB_COUNTER *HOST_MIB_COUNTER_new(void)
{
  HOST_MIB_COUNTER *h = malloc(sizeof(*h));
  if (h)
    memset(h, 0, sizeof(*h));
  return h;
}

/**
 * Allocate a new VALIDATION_STATUS object.
 */
static VALIDATION_STATUS *VALIDATION_STATUS_new(void)
{
  VALIDATION_STATUS *v = malloc(sizeof(*v));
  if (v)
    memset(v, 0, sizeof(*v));
  return v;
}

/**
 * Type-safe wrapper around free() to keep safestack macros happy.
 */
static void HOST_MIB_COUNTER_free(HOST_MIB_COUNTER *h)
{
  if (h)
    free(h);
}

/**
 * Type-safe wrapper around free() to keep safestack macros happy.
 */
static void VALIDATION_STATUS_free(VALIDATION_STATUS *v)
{
  if (v)
    free(v);
}



/*
 * GCC attributes to help catch format string errors.
 */

#ifdef __GNUC__

static void logmsg(const rcynic_ctx_t *rc, 
		   const log_level_t level, 
		   const char *fmt, ...)
     __attribute__ ((format (printf, 3, 4)));

static void reject(const rcynic_ctx_t *rc,
		   const uri_t *uri,
		   const mib_counter_t code,
		   const char *fmt, ...)
     __attribute__ ((format (printf, 4, 5)));

#endif

/**
 * Logging.
 */
static void vlogmsg(const rcynic_ctx_t *rc, 
		    const log_level_t level, 
		    const char *fmt,
		    va_list ap)
{
  char tad[sizeof("00:00:00")+1];
  time_t tad_time;

  assert(rc && fmt);

  if (rc->log_level < level)
    return;

  if (rc->use_syslog) {
    vsyslog(rc->priority[level], fmt, ap);
  } else {
    time(&tad_time);
    strftime(tad, sizeof(tad), "%H:%M:%S", localtime(&tad_time));
    fprintf(stderr, "%s: ", tad);
    if (rc->jane)
      fprintf(stderr, "%s: ", rc->jane);
    if (rc->indent)
      fprintf(stderr, "%*s", rc->indent, " ");
    vfprintf(stderr, fmt, ap);
    putc('\n', stderr);
  }
}

/**
 * Logging.
 */
static void logmsg(const rcynic_ctx_t *rc, 
		   const log_level_t level, 
		   const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  vlogmsg(rc, level, fmt, ap);
  va_end(ap);
}

/**
 * Print OpenSSL library errors.
 */
static void log_openssl_errors(const rcynic_ctx_t *rc)
{
  const char *data, *file;
  unsigned long code;
  char error[256];
  int flags, line;

  if (!rc->log_level < log_verbose)
    return;

  while ((code = ERR_get_error_line_data(&file, &line, &data, &flags))) {
    ERR_error_string_n(code, error, sizeof(error));
    if (data && (flags & ERR_TXT_STRING))
      logmsg(rc, log_sys_err, "OpenSSL error %s:%d: %s: %s", file, line, error, data);
    else
      logmsg(rc, log_sys_err, "OpenSSL error %s:%d: %s", file, line, error);
    }
}

/**
 * Configure logging.
 */
static int configure_logmsg(rcynic_ctx_t *rc, const char *name)
{
  int i;

  assert(rc && name);

  for (i = 0; i < sizeof(log_levels)/sizeof(*log_levels); i++) {
    if (!strcmp(name, log_levels[i].name)) {
      rc->log_level = log_levels[i].value;
      return 1;
    }
  }

  logmsg(rc, log_usage_err, "Bad log level %s", name);
  return 0;
}

/**
 * Configure syslog.
 */
static int configure_syslog(const rcynic_ctx_t *rc, 
			    int *result,
			    const CODE *table,
			    const char *name)
{
  assert(result && table && name);

  while (table->c_name && strcmp(table->c_name, name))
    table++;

  if (table->c_name) {
    *result = table->c_val;
    return 1;
  } else {
    logmsg(rc, log_usage_err, "Bad syslog code %s", name);
    return 0;
  }
}

/**
 * Configure boolean variable.
 */
static int configure_boolean(const rcynic_ctx_t *rc,
			     int *result,
			     const char *val)
{
  assert(rc && result && val);

  switch (*val) {
  case 'y': case 'Y': case 't': case 'T': case '1':
    *result = 1;
    return 1;
  case 'n': case 'N': case 'f': case 'F': case '0':
    *result = 0;
    return 1;
  default:
    logmsg(rc, log_usage_err, "Bad boolean value %s", val);
    return 0;
  }
}

/**
 * Configure integer variable.
 */
static int configure_integer(const rcynic_ctx_t *rc,
			     int *result,
			     const char *val)
{
  long res;
  char *p;

  assert(rc && result && val);

  res = strtol(val, &p, 10);
  
  if (*val != '\0' && *p == '\0') {
    *result = (int) res;
    return 1;
  } else {
    logmsg(rc, log_usage_err, "Bad integer value %s", val);
    return 0;
  }
}



/**
 * Make a directory if it doesn't already exist.
 */
static int mkdir_maybe(const rcynic_ctx_t *rc, const path_t *name)
{
  path_t path;
  char *s;

  assert(name != NULL);
  if (strlen(name->s) >= sizeof(path.s)) {
    logmsg(rc, log_data_err, "Pathname %s too long", name->s);
    return 0;
  }
  strcpy(path.s, name->s);
  s = path.s[0] == '/' ? path.s + 1 : path.s;
  if ((s = strrchr(s, '/')) == NULL)
    return 1;
  *s = '\0';
  if (!mkdir_maybe(rc, &path)) {
    logmsg(rc, log_sys_err, "Failed to make directory %s", path.s);
    return 0;
  }
  if (!access(path.s, F_OK))
    return 1;
  logmsg(rc, log_verbose, "Creating directory %s", path.s);
  return mkdir(path.s, 0777) == 0;
}

/**
 * strdup() a string and push it onto a stack.
 */
static int sk_OPENSSL_STRING_push_strdup(STACK_OF(OPENSSL_STRING) *sk, const char *str)
{
  OPENSSL_STRING s = strdup(str);

  if (s && sk_OPENSSL_STRING_push(sk, s))
    return 1;
  if (s)
    free(s);
  return 0;
}

/**
 * Compare two URI strings, for OpenSSL STACK operations.
 */

static int uri_cmp(const char * const *a, const char * const *b)
{
  return strcmp(*a, *b);
}

/**
 * Is string an rsync URI?
 */
static int is_rsync(const char *uri)
{
  return uri && !strncmp(uri, SCHEME_RSYNC, SIZEOF_RSYNC);
}

/**
 * Convert an rsync URI to a filename, checking for evil character
 * sequences.  NB: This routine can't call mib_increment(), because
 * mib_increment() calls it, so errors detected here only go into
 * the log, not the MIB.
 */
static int uri_to_filename(const rcynic_ctx_t *rc,
			   const uri_t *uri,
			   path_t *path,
			   const path_t *prefix)
{
  const char *u;
  size_t n;

  path->s[0] = '\0';

  if (!is_rsync(uri->s)) {
    logmsg(rc, log_telemetry, "%s is not an rsync URI, not converting to filename", uri->s);
    return 0;
  }

  u = uri->s + SIZEOF_RSYNC;
  n = strlen(u);
  
  if (u[0] == '/' || u[0] == '.' || strstr(u, "/../") ||
      (n >= 3 && !strcmp(u + n - 3, "/.."))) {
    logmsg(rc, log_data_err, "Dangerous URI %s, not converting to filename", uri->s);
    return 0;
  }

  if (prefix)
    n += strlen(prefix->s);

  if (n >= sizeof(path->s)) {
    logmsg(rc, log_data_err, "URI %s too long, not converting to filename", uri->s);
    return 0;
  }

  if (prefix) {
    strcpy(path->s, prefix->s);
    strcat(path->s, u);
  } else {
    strcpy(path->s, u);
  }

  return 1;
}

/**
 * OID comparison.
 */
static int oid_cmp(const ASN1_OBJECT *obj, const unsigned char *oid, const size_t oidlen)
{
  assert(obj != NULL && oid != NULL);
  if (obj->length != oidlen)
    return obj->length - oidlen;
  else
    return memcmp(obj->data, oid, oidlen);
}

/**
 * Host MIB counter comparision.
 */
static int host_mib_counter_cmp(const HOST_MIB_COUNTER * const *a, const HOST_MIB_COUNTER * const *b)
{
  return strcasecmp((*a)->hostname.s, (*b)->hostname.s);
}

/**
 * MIB counter manipulation.
 */
static void mib_increment(const rcynic_ctx_t *rc,
			  const uri_t *uri,
			  const mib_counter_t counter)
{
  HOST_MIB_COUNTER *h = NULL, hn;
  char *s;

  assert(rc && uri);

  if (!rc->host_counters)
    return;

  memset(&hn, 0, sizeof(hn));

  if (!uri_to_filename(rc, uri, &hn.hostname, NULL)) {
    logmsg(rc, log_data_err, "Couldn't convert URI %s to hostname", uri->s);
    return;
  }

  if ((s = strchr(hn.hostname.s, '/')) != NULL)
    *s = '\0';

  h = sk_HOST_MIB_COUNTER_value(rc->host_counters,
				sk_HOST_MIB_COUNTER_find(rc->host_counters,
							 &hn));
  if (!h) {
    if ((h = HOST_MIB_COUNTER_new()) == NULL) {
      logmsg(rc, log_sys_err, "Couldn't allocate MIB counters for %s", uri->s);
      return;
    }
    strcpy(h->hostname.s, hn.hostname.s);
    if (!sk_HOST_MIB_COUNTER_push(rc->host_counters, h)) {
      logmsg(rc, log_sys_err, "Couldn't store MIB counters for %s", uri->s);
      free(h);
      return;
    }
  }

  h->counters[counter]++;
}

/**
 * Add a validation status entry to internal log.
 */
static void log_validation_status(const rcynic_ctx_t *rc,
				  const uri_t *uri,
				  const mib_counter_t code)
{
  VALIDATION_STATUS *v = NULL;

  assert(rc && uri);

  if (!rc->validation_status)
    return;

  if ((v = VALIDATION_STATUS_new()) == NULL) {
    logmsg(rc, log_sys_err, "Couldn't allocate validation status entry for %s", uri->s);
    goto punt;
  }

  strcpy(v->uri.s, uri->s);
  v->timestamp = time(0);
  v->code = code;

  if (!sk_VALIDATION_STATUS_push(rc->validation_status, v)) {
    logmsg(rc, log_sys_err, "Couldn't store validation status entry for %s", uri->s);
    goto punt;
  }

  v = NULL;

 punt:
  if (v)
    free(v);
}

/**
 * Reject an object.
 */
static void reject(const rcynic_ctx_t *rc,
		   const uri_t *uri,
		   const mib_counter_t code,
		   const char *fmt, ...)
{
  char format[URI_MAX * 2];
  va_list ap;

  assert(fmt && strlen(fmt) + sizeof("Rejected %s") < sizeof(format));
  snprintf(format, sizeof(format), "Rejected %s %s", uri->s, fmt);
  log_validation_status(rc, uri, code);
  va_start(ap, fmt);
  vlogmsg(rc, log_data_err, format, ap);
  va_end(ap);
}

/**
 * Copy a file
 */
static int cp(const path_t *source, const path_t *target)
{
  FILE *in = NULL, *out = NULL;
  int c, ret = 0;

  if ((in = fopen(source->s, "rb")) == NULL ||
      (out = fopen(target->s, "wb")) == NULL)
    goto done;

  while ((c = getc(in)) != EOF)
    if (putc(c, out) == EOF)
      goto done;

  ret = 1;

 done:
  ret &= !(in  != NULL && fclose(in)  == EOF);
  ret &= !(out != NULL && fclose(out) == EOF);
  return ret;
}

/**
 * Link a file
 */
static int ln(const path_t *source, const path_t *target)
{
  unlink(target->s);
  return link(source->s, target->s) == 0;
}

/**
 * Install an object.  It'd be nice if we could just use link(), but
 * that would require us to trust rsync never to do anything bad.  For
 * now we just copy in the simplest way possible.  Come back to this
 * if profiling shows a hotspot here.
 *
 * Well, ok, profiling didn't show an issue, but inode exhaustion did.
 * So we now make copy vs link a configuration choice.
 */
static int install_object(const rcynic_ctx_t *rc,
			  const uri_t *uri,
			  const path_t *source)
{
  path_t target;

  if (!uri_to_filename(rc, uri, &target, &rc->authenticated)) {
    logmsg(rc, log_data_err, "Couldn't generate installation name for %s", uri->s);
    return 0;
  }

  if (!mkdir_maybe(rc, &target)) {
    logmsg(rc, log_sys_err, "Couldn't create directory for %s", target.s);
    return 0;
  }

  if (rc->use_links ? !ln(source, &target) : !cp(source, &target)) {
    logmsg(rc, log_sys_err, "Couldn't %s %s to %s",
	   (rc->use_links ? "link" : "copy"), source->s, target.s);
    return 0;
  }
  log_validation_status(rc, uri, validation_ok);
  logmsg(rc, log_telemetry, "Accepted     %s", uri->s);
  return 1;
}

/**
 * Check str for a trailing suffix.
 */
static int endswith(const char *str, const char *suffix)
{
  size_t len_str, len_suffix;
  assert(str != NULL && suffix != NULL);
  len_str = strlen(str);
  len_suffix = strlen(suffix);
  return len_str >= len_suffix && !strcmp(str + len_str - len_suffix, suffix);
}

/**
 * Check str for a prefix.
 */
static int startswith(const char *str, const char *prefix)
{
  size_t len_str, len_prefix;
  assert(str != NULL && prefix != NULL);
  len_str = strlen(str);
  len_prefix = strlen(prefix);
  return len_str >= len_prefix && !strncmp(str, prefix, len_prefix);
}


/**
 * Set a directory name, making sure it has the trailing slash we
 * require in various other routines.
 */
static int set_directory(const rcynic_ctx_t *rc, path_t *out, const char *in)
{
  int need_slash;
  size_t n;

  assert(rc && in && out);
  n = strlen(in);
  assert(n > 0);
  need_slash = in[n - 1] != '/';

  if (n + need_slash + 1 > sizeof(out->s)) {
    logmsg(rc, log_usage_err, "Path \"%s\" too long", in);
    return 0;
  }

  strcpy(out->s, in);
  if (need_slash)
    strcat(out->s, "/");
  return 1;
}

/**
 * Remove a directory tree, like rm -rf.
 */
static int rm_rf(const path_t *name)
{
  path_t path;
  struct dirent *d;
  size_t len;
  DIR *dir;
  int ret = 0, need_slash;

  assert(name);
  len = strlen(name->s);
  assert(len > 0 && len < sizeof(path.s));
  need_slash = name->s[len - 1] != '/';

  if (rmdir(name->s) == 0)
    return 1;

  switch (errno) {
  case ENOENT:
    return 1;
  case ENOTEMPTY:
    break;
  default:
    return 0;
  }

  if ((dir = opendir(name->s)) == NULL)
    return 0;

  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] == '.' && (d->d_name[1] == '\0' || (d->d_name[1] == '.' && d->d_name[2] == '\0')))
      continue;
    if (len + strlen(d->d_name) + need_slash >= sizeof(path.s))
      goto done;
    strcpy(path.s, name->s);
    if (need_slash)
      strcat(path.s, "/");
    strcat(path.s, d->d_name);
    switch (d->d_type) {
    case DT_DIR:
      if (!rm_rf(&path))
	goto done;
      continue;
    default:
      if (unlink(path.s) < 0)
	goto done;
      continue;
    }
  }

  ret = rmdir(name->s) == 0;

 done:
  closedir(dir);
  return ret;
}



/**
 * Read non-directory filenames from a directory, so we can check to
 * see what's missing from a manifest.
 */
static STACK_OF(OPENSSL_STRING) *directory_filenames(const rcynic_ctx_t *rc,
						     const walk_state_t state,
						     const uri_t *uri)
{
  STACK_OF(OPENSSL_STRING) *result = NULL;
  path_t path;
  const path_t *prefix = NULL;
  DIR *dir = NULL;
  struct dirent *d;
  int ok = 0;

  assert(rc && uri);

  switch (state) {
  case walk_state_current:
    prefix = &rc->unauthenticated;
    break;
  case walk_state_backup:
    prefix = &rc->old_authenticated;
    break;
  default:
    goto done;
  }

  if (!uri_to_filename(rc, uri, &path, prefix) ||
      (dir = opendir(path.s)) == NULL || 
      (result = sk_OPENSSL_STRING_new(uri_cmp)) == NULL)
    goto done;

  while ((d = readdir(dir)) != NULL)
    if (d->d_type != DT_DIR && !sk_OPENSSL_STRING_push_strdup(result, d->d_name))
      goto done;

  ok = 1;

 done:
  if (dir != NULL)
    closedir(dir);

  if (ok)
    return result;

  sk_OPENSSL_STRING_pop_free(result, OPENSSL_STRING_free);
  return NULL;
}



/**
 * Increment walk context reference count.
 */
static void walk_ctx_attach(walk_ctx_t *w)
{
  if (w != NULL) {
    w->refcount++;
    assert(w->refcount != 0);
  }
}

/**
 * Decrement walk context reference count; freeing the context if the
 * reference count is now zero.
 */
static void walk_ctx_detach(walk_ctx_t *w)
{
  if (w != NULL && --(w->refcount) == 0) {
    assert(w->refcount == 0);
    X509_free(w->cert);
    Manifest_free(w->manifest);
    sk_OPENSSL_STRING_pop_free(w->filenames, OPENSSL_STRING_free);
    free(w);
  }
}

/**
 * Return top context of a walk context stack.
 */
static walk_ctx_t *walk_ctx_stack_head(STACK_OF(walk_ctx_t) *wsk)
{
  return sk_walk_ctx_t_value(wsk, sk_walk_ctx_t_num(wsk) - 1);
}

/**
 * Walk context iterator.  Think of this as the thing you call in the
 * third clause of a conceptual "for" loop: this reinitializes as
 * necessary for the next pass through the loop.
 *
 * This is still under construction, but general idea is that we have
 * several state variables in a walk context which collectively define
 * the current pass, product URI, etc, and we want to be able to
 * iterate through this sequence via the event system.  So we need a
 * function which steps to the next state.
 */
static void walk_ctx_loop_next(const rcynic_ctx_t *rc, STACK_OF(walk_ctx_t) *wsk)
{
  walk_ctx_t *w = walk_ctx_stack_head(wsk);

  assert(rc && wsk && w);

  if (w->manifest && w->manifest_iteration + 1 < sk_FileAndHash_num(w->manifest->fileList)) {
    w->manifest_iteration++;
    return;
  }

  if (w->filenames && w->filename_iteration + 1 < sk_OPENSSL_STRING_num(w->filenames)) {
    w->filename_iteration++;
    return;
  }

  if (w->state < walk_state_done) {
    w->state++;
    w->manifest_iteration = 0;
    w->filename_iteration = 0;
    sk_OPENSSL_STRING_pop_free(w->filenames, OPENSSL_STRING_free);
    w->filenames = directory_filenames(rc, w->state, &w->certinfo.sia);
  }
}

/**
 * Whether we're done iterating over a walk context.  Think of this as
 * the thing you call (negated) in the second clause of a conceptual
 * "for" loop.
 */
static int walk_ctx_loop_done(STACK_OF(walk_ctx_t) *wsk)
{
  walk_ctx_t *w = walk_ctx_stack_head(wsk);
  return wsk == NULL || w == NULL || w->state >= walk_state_done;
}

static Manifest *check_manifest(const rcynic_ctx_t *rc,
				STACK_OF(walk_ctx_t) *wsk);

/**
 * Loop initializer for walk context.  Think of this as the thing you
 * call in the first clause of a conceptual "for" loop.
 */
static void walk_ctx_loop_init(const rcynic_ctx_t *rc, STACK_OF(walk_ctx_t) *wsk)
{
  walk_ctx_t *w = walk_ctx_stack_head(wsk);

  assert(rc && wsk && w && w->state == walk_state_ready);

  assert(w->manifest == NULL);
  if ((w->manifest = check_manifest(rc, wsk)) == NULL)
    logmsg(rc, log_data_err, "Couldn't get manifest %s, blundering onward", w->certinfo.manifest.s);

  assert(w->filenames == NULL);
  w->filenames = directory_filenames(rc, w->state, &w->certinfo.sia);

  w->manifest_iteration = 0;
  w->filename_iteration = 0;
  w->state++;

  assert(w->state == walk_state_current);

  while (!walk_ctx_loop_done(wsk) &&
	 (w->manifest == NULL  || w->manifest_iteration >= sk_FileAndHash_num(w->manifest->fileList)) &&
	 (w->filenames == NULL || w->filename_iteration >= sk_OPENSSL_STRING_num(w->filenames)))
    walk_ctx_loop_next(rc, wsk);
}

/**
 * Extract URI and hash values from walk context.
 */
static int walk_ctx_loop_this(const rcynic_ctx_t *rc,
			      STACK_OF(walk_ctx_t) *wsk,
			      uri_t *uri,
			      const unsigned char **hash,
			      size_t *hashlen)
{
  const walk_ctx_t *w = walk_ctx_stack_head(wsk);
  const char *name = NULL;
  FileAndHash *fah = NULL;

  assert(rc && wsk && w && uri && hash && hashlen);

  if (w->manifest != NULL && w->manifest_iteration < sk_FileAndHash_num(w->manifest->fileList)) {
    fah = sk_FileAndHash_value(w->manifest->fileList, w->manifest_iteration);
    name = (const char *) fah->file->data;
  } else if (w->filenames != NULL && w->filename_iteration < sk_OPENSSL_STRING_num(w->filenames)) {
    name = sk_OPENSSL_STRING_value(w->filenames, w->filename_iteration);
  }

  if (name == NULL) {
    logmsg(rc, log_sys_err, "Can't find a URI in walk context, this shouldn't happen: state %d, manifest_iteration %d, filename_iteration %d",
	   (int) w->state, w->manifest_iteration, w->filename_iteration);
    return 0;
  }

  if (strlen(w->certinfo.sia.s) + strlen(name) >= sizeof(uri->s)) {
    logmsg(rc, log_data_err, "URI %s%s too long, skipping", w->certinfo.sia.s, uri->s);
    return 0;
  }

  strcpy(uri->s, w->certinfo.sia.s);
  strcat(uri->s, name);

  if (fah != NULL) {
    sk_OPENSSL_STRING_remove(w->filenames, name);
    *hash = fah->hash->data;
    *hashlen = fah->hash->length;
  } else {
    *hash = NULL;
    *hashlen = 0;
  }

  return 1;
}

/**
 * Create a new walk context stack.
 */
static STACK_OF(walk_ctx_t) *walk_ctx_stack_new(void)
{
  return sk_walk_ctx_t_new_null();
}

/**
 * Push a walk context onto a walk context stack, return the new context.
 */
static walk_ctx_t *walk_ctx_stack_push(STACK_OF(walk_ctx_t) *wsk,
				       X509 *x,
				       const certinfo_t *certinfo)
{
  walk_ctx_t *w;

  if (x == NULL || certinfo == NULL)
    return NULL;

  if ((w = malloc(sizeof(*w))) == NULL)
    return NULL;

  memset(w, 0, sizeof(*w));
  w->cert = x;
  w->certinfo = *certinfo;

  if (!sk_walk_ctx_t_push(wsk, w)) {
    free(w);
    return NULL;
  }

  walk_ctx_attach(w);
  return w;
}

/**
 * Pop and discard a walk context from a walk context stack.
 */
static void walk_ctx_stack_pop(STACK_OF(walk_ctx_t) *wsk)
{
  walk_ctx_detach(sk_walk_ctx_t_pop(wsk));
}

/**
 * Clone a stack of walk contexts.
 */
static STACK_OF(walk_ctx_t) *walk_ctx_stack_clone(STACK_OF(walk_ctx_t) *old_wsk)
{
  STACK_OF(walk_ctx_t) *new_wsk;
  int i;
  if (old_wsk == NULL || (new_wsk = sk_walk_ctx_t_dup(old_wsk)) == NULL)
    return NULL;
  for (i = 0; i < sk_walk_ctx_t_num(new_wsk); i++)
    walk_ctx_attach(sk_walk_ctx_t_value(new_wsk, i));
  return new_wsk;
}

/**
 * Extract certificate stack from walk context stack.  Returns a newly
 * created STACK_OF(X509) pointing to the existing cert objects (ie,
 * this is a shallow copy, so only free the STACK_OF(X509), not the
 * certificates themselves).
 */
static STACK_OF(X509) *walk_ctx_stack_certs(STACK_OF(walk_ctx_t) *wsk)
{
  STACK_OF(X509) *xsk = sk_X509_new_null();
  walk_ctx_t *w;
  int i;

  for (i = 0; i < sk_walk_ctx_t_num(wsk); i++)
    if ((w = sk_walk_ctx_t_value(wsk, i)) == NULL ||
	(w->cert != NULL && !sk_X509_push(xsk, w->cert)))
      goto fail;

  return xsk;

 fail:
  sk_X509_free(xsk);
  return NULL;
}

/**
 * Free a walk context stack, decrementing reference counts of each
 * frame on it.
 */
static void walk_ctx_stack_free(STACK_OF(walk_ctx_t) *wsk)
{
  sk_walk_ctx_t_pop_free(wsk, walk_ctx_detach);
}



/**
 * Maintain a cache of URIs we've already fetched.
 */
static int rsync_cached_string(const rcynic_ctx_t *rc,
			       const char *string)
{
  char *s, buffer[URI_MAX];

  assert(rc && rc->rsync_cache && strlen(string) < sizeof(buffer));
  strcpy(buffer, string);
  if ((s = strrchr(buffer, '/')) != NULL && s[1] == '\0')
    *s = '\0';
  while (sk_OPENSSL_STRING_find(rc->rsync_cache, buffer) < 0) {
    if ((s = strrchr(buffer, '/')) == NULL)
      return 0;
    *s = '\0';
  }
  return 1;
}

/**
 * Check whether a particular URI has been cached.
 */
static int rsync_cached_uri(const rcynic_ctx_t *rc,
			    const uri_t *uri)
{
  return is_rsync(uri->s) && rsync_cached_string(rc, uri->s + SIZEOF_RSYNC);
}


/**
 * Run rsync.  This is fairly nasty, because we need to:
 *
 * @li Construct the argument list for rsync;
 *
 * @li Run rsync in a child process;
 *
 * @li Sit listening to rsync's output, logging whatever we get;
 *
 * @li Impose an optional time limit on rsync's execution time
 *
 * @li Clean up from (b), (c), and (d); and
 *
 * @li Keep track of which URIs we've already fetched, so we don't
 *     have to do it again.
 *
 * Taken all together, this is pretty icky.  Breaking it into separate
 * functions wouldn't help much.  Don't read this on a full stomach.
 */
static int rsync(const rcynic_ctx_t *rc,
		 const char * const *args,
		 const uri_t *uri)
{
  static const char *rsync_cmd[] = {
    "rsync", "--update", "--times", "--copy-links", "--itemize-changes", NULL
  };

  const char *argv[100];
  char *s, *b, buffer[URI_MAX * 4];
  path_t path;
  int i, n, ret, pipe_fds[2], argc = 0, pid_status = -1;
  time_t now, deadline;
  struct timeval tv;
  pid_t pid, wpid;
  fd_set rfds;

  assert(rc && uri);

  memset(argv, 0, sizeof(argv));

  for (i = 0; rsync_cmd[i]; i++) {
    assert(argc < sizeof(argv)/sizeof(*argv));
    argv[argc++] = rsync_cmd[i];
  }
  if (args) {
    for (i = 0; args[i]; i++) {
      assert(argc < sizeof(argv)/sizeof(*argv));
      argv[argc++] = args[i];
    }
  }

  if (rc->rsync_program)
    argv[0] = rc->rsync_program;

  if (!uri_to_filename(rc, uri, &path, &rc->unauthenticated)) {
    logmsg(rc, log_data_err, "Couldn't extract filename from URI: %s", uri->s);
    return 0;
  }

  assert(argc < sizeof(argv)/sizeof(*argv));
  argv[argc++] = uri->s;

  assert(argc < sizeof(argv)/sizeof(*argv));
  argv[argc++] = path.s;

  assert(strlen(uri->s) > SIZEOF_RSYNC);
  if (rsync_cached_uri(rc, uri)) {
    logmsg(rc, log_verbose, "rsync cache hit for %s", uri->s);
    return 1;
  }

  if (!mkdir_maybe(rc, &path)) {
    logmsg(rc, log_sys_err, "Couldn't make target directory: %s", path.s);
    return 0;
  }

  logmsg(rc, log_telemetry, "Fetching %s", uri->s);

  for (i = 0; i < argc; i++)
    logmsg(rc, log_verbose, "rsync argv[%d]: %s", i, argv[i]);

  if (pipe(pipe_fds) < 0) {
    logmsg(rc, log_sys_err, "pipe() failed: %s", strerror(errno));
    return 0;
  }

  if ((i = fcntl(pipe_fds[0], F_GETFL, 0)) == -1 ||
      fcntl(pipe_fds[0], F_SETFL, i | O_NONBLOCK) == -1) {
    logmsg(rc, log_sys_err,
	   "Couldn't set rsync's output stream non-blocking: %s",
	   strerror(errno));
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    return 0;
  }

  switch ((pid = vfork())) {
  case -1:
     logmsg(rc, log_sys_err, "vfork() failed: %s", strerror(errno));
     close(pipe_fds[0]);
     close(pipe_fds[1]);
     return 0;
  case 0:
#define whine(msg) write(2, msg, sizeof(msg) - 1)
    close(pipe_fds[0]);
    if (dup2(pipe_fds[1], 1) < 0)
      whine("dup2(1) failed\n");
    else if (dup2(pipe_fds[1], 2) < 0)
      whine("dup2(2) failed\n");
    else if (execvp(argv[0], (char * const *) argv) < 0)
      whine("execvp() failed\n");
    whine("last system error: ");
    write(2, strerror(errno), strlen(strerror(errno)));
    whine("\n");
    _exit(1);
#undef whine
  }

  close(pipe_fds[1]);

  now = time(0);
  deadline = now + rc->rsync_timeout;

  n = -1;
  i = 0;
  while ((wpid = waitpid(pid, &pid_status, WNOHANG)) == 0 &&
	 (!rc->rsync_timeout || (now = time(0)) < deadline)) {
    FD_ZERO(&rfds);
    FD_SET(pipe_fds[0], &rfds);
    if (rc->rsync_timeout) {
      tv.tv_sec = deadline - now;
      tv.tv_usec = 0;
      n = select(pipe_fds[0] + 1, &rfds, NULL, NULL, &tv);
    } else {
      n = select(pipe_fds[0] + 1, &rfds, NULL, NULL, NULL);
    }
    if (n == 0 || (n < 0 && errno == EINTR))
      continue;
    if (n < 0)
      break;
    while ((n = read(pipe_fds[0], buffer + i, sizeof(buffer) - i - 1)) > 0) {
      n += i;
      assert(n < sizeof(buffer));
      buffer[n] = '\0';
      for (b = buffer; (s = strchr(b, '\n')) != NULL; b = s) {
	*s++ = '\0';
	logmsg(rc, log_telemetry, "%s", b);
      }
      i = strlen(b);
      assert(i < sizeof(buffer) && b + i < buffer + sizeof(buffer));
      if (b == buffer && i == sizeof(buffer) - 1) {
	logmsg(rc, log_telemetry, "%s\\", b);
	i = 0;
      }
      if (i > 0) {
	memmove(buffer, b, i);
      }
    }
    if (n == 0 || (n < 0 && errno != EAGAIN))
      break;
  }
  
  close(pipe_fds[0]);

  assert(i >= 0 && i < sizeof(buffer));
  if (i) {
    buffer[i] = '\0';
    logmsg(rc, log_telemetry, "%s", buffer);
  }

  if (n < 0 && errno != EAGAIN)
    logmsg(rc, log_sys_err, "Problem reading rsync's output: %s",
	   strerror(errno));

  if (rc->rsync_timeout && now >= deadline)
    logmsg(rc, log_data_err,
	   "Fetch of %s took longer than %d seconds, terminating fetch",
	   uri->s, rc->rsync_timeout);

  assert(pid > 0);
  for (i = 0; i < KILL_MAX && wpid == 0; i++) {
    if ((wpid = waitpid(pid, &pid_status, 0)) != 0 && WIFEXITED(pid_status))
      break;
    kill(pid, SIGTERM);
  }

  if (WEXITSTATUS(pid_status)) {
    logmsg(rc, log_data_err, "rsync exited with status %d fetching %s",
	   WEXITSTATUS(pid_status), uri->s);
    ret = 0;
    mib_increment(rc, uri, (rc->rsync_timeout && now >= deadline
			    ? rsync_timed_out
			    : rsync_failed));
  } else {
    ret = 1;
    mib_increment(rc, uri, rsync_succeeded);
  }

  assert(strlen(uri->s) > SIZEOF_RSYNC);
  strcpy(buffer, uri->s + SIZEOF_RSYNC);
  if ((s = strrchr(buffer, '/')) != NULL && s[1] == '\0')
    *s = '\0';
  if (!sk_OPENSSL_STRING_push_strdup(rc->rsync_cache, buffer))
    logmsg(rc, log_sys_err, "Couldn't cache URI %s, blundering onward", uri->s);

  return ret;
}

/**
 * rsync a single file (CRL, manifest, ROA, whatever).
 */
static int rsync_file(const rcynic_ctx_t *rc, const uri_t *uri)
{
  return rsync(rc, NULL, uri);
}

/**
 * rsync an entire subtree, generally rooted at a SIA collection.
 */
static int rsync_tree(const rcynic_ctx_t *rc, const uri_t *uri)
{
  static const char * const rsync_args[] = { "--recursive", "--delete", NULL };
  return rsync(rc, rsync_args, uri);
}



/**
 * Clean up old stuff from previous rsync runs.  --delete doesn't help
 * if the URI changes and we never visit the old URI again.
 */
static int prune_unauthenticated(const rcynic_ctx_t *rc,
				 const path_t *name,
				 const size_t baselen)
{
  path_t path;
  struct dirent *d;
  size_t len;
  DIR *dir;
  int need_slash;

  assert(rc && name && baselen > 0);
  len = strlen(name->s);
  assert(len >= baselen && len < sizeof(path.s));
  need_slash = name->s[len - 1] != '/';

  if (rsync_cached_string(rc, name->s + baselen)) {
    logmsg(rc, log_debug, "prune: cache hit for %s, not cleaning", name->s);
    return 1;
  }

  if (rmdir(name->s) == 0) {
    logmsg(rc, log_debug, "prune: removed %s", name->s);
    return 1;
  }

  switch (errno) {
  case ENOENT:
    logmsg(rc, log_debug, "prune: nonexistant %s", name->s);
    return 1;
  case ENOTEMPTY:
    break;
  default:
    logmsg(rc, log_debug, "prune: other error %s: %s", name->s, strerror(errno));
    return 0;
  }

  if ((dir = opendir(name->s)) == NULL)
    return 0;

  while ((d = readdir(dir)) != NULL) {
    if (d->d_name[0] == '.' && (d->d_name[1] == '\0' || (d->d_name[1] == '.' && d->d_name[2] == '\0')))
      continue;
    if (len + strlen(d->d_name) + need_slash >= sizeof(path)) {
      logmsg(rc, log_debug, "prune: %s%s%s too long", name->s, (need_slash ? "/" : ""), d->d_name);
      goto done;
    }
    strcpy(path.s, name->s);
    if (need_slash)
      strcat(path.s, "/");
    strcat(path.s, d->d_name);
    switch (d->d_type) {
    case DT_DIR:
      if (!prune_unauthenticated(rc, &path, baselen))
	goto done;
      continue;
    default:
      if (rsync_cached_string(rc, path.s + baselen)) {
	logmsg(rc, log_debug, "prune: cache hit %s", path.s);
	continue;
      }
      if (unlink(path.s) < 0) {
	logmsg(rc, log_debug, "prune: removing %s failed: %s", path.s, strerror(errno));
	goto done;
      }
      logmsg(rc, log_debug, "prune: removed %s", path.s);
      continue;
    }
  }

  if (rmdir(name->s) < 0 && errno != ENOTEMPTY)
    logmsg(rc, log_debug, "prune: couldn't remove %s: %s", name->s, strerror(errno));

 done:
  closedir(dir);
  return !d;
}



/**
 * Read a DER object using a BIO pipeline that hashes the file content
 * as we read it.  Returns the internal form of the parsed DER object,
 * sets the hash buffer (if specified) as a side effect.  The default
 * hash algorithm is SHA-256.
 */
static void *read_file_with_hash(const path_t *filename,
				 const ASN1_ITEM *it,
				 const EVP_MD *md,
				 hashbuf_t *hash)
{
  void *result = NULL;
  BIO *b;

  if ((b = BIO_new_file(filename->s, "rb")) == NULL)
    goto error;

  if (hash != NULL) {
    BIO *b2 = BIO_new(BIO_f_md());
    if (b2 == NULL)
      goto error;
    if (md == NULL)
      md = EVP_sha256();
    if (!BIO_set_md(b2, md)) {
      BIO_free(b2);
      goto error;
    }
    BIO_push(b2, b);
    b = b2;
  }

  if ((result = ASN1_item_d2i_bio(it, b, NULL)) == NULL)
    goto error;

  if (hash != NULL) {
    memset(hash, 0, sizeof(*hash));
    BIO_gets(b, (char *) hash, sizeof(hash->h));
  }    

 error:
  BIO_free_all(b);
  return result;
}

/**
 * Read and hash a certificate.
 */
static X509 *read_cert(const path_t *filename, hashbuf_t *hash)
{
  return read_file_with_hash(filename, ASN1_ITEM_rptr(X509), NULL, hash);
}

/**
 * Read and hash a CRL.
 */
static X509_CRL *read_crl(const path_t *filename, hashbuf_t *hash)
{
  return read_file_with_hash(filename, ASN1_ITEM_rptr(X509_CRL), NULL, hash);
}

/**
 * Read and hash a CMS message.
 */
static CMS_ContentInfo *read_cms(const path_t *filename, hashbuf_t *hash)
{
  return read_file_with_hash(filename, ASN1_ITEM_rptr(CMS_ContentInfo), NULL, hash);
}



/**
 * Extract CRLDP data from a certificate.
 */
static void extract_crldp_uri(const rcynic_ctx_t *rc,
			      const uri_t *uri,
			      const STACK_OF(DIST_POINT) *crldp,
			      uri_t *result)
{
  DIST_POINT *d;
  int i;

  assert(crldp);

  if (sk_DIST_POINT_num(crldp) != 1) {
    logmsg(rc, log_data_err, "CRLDistributionPoints sequence length is %d (should be 1) for %s",
	   sk_DIST_POINT_num(crldp), uri->s);
    mib_increment(rc, uri, malformed_crldp);
    return;
  }

  d = sk_DIST_POINT_value(crldp, 0);

  if (d->reasons || d->CRLissuer || !d->distpoint || d->distpoint->type != 0) {
    logmsg(rc, log_data_err, "CRLDP does not match RPKI certificate profile for %s", uri->s);
    mib_increment(rc, uri, malformed_crldp);
    return;
  }

  for (i = 0; i < sk_GENERAL_NAME_num(d->distpoint->name.fullname); i++) {
    GENERAL_NAME *n = sk_GENERAL_NAME_value(d->distpoint->name.fullname, i);
    assert(n != NULL);
    if (n->type != GEN_URI) {
      logmsg(rc, log_data_err, "CRLDP contains non-URI GeneralName for %s", uri->s);
      mib_increment(rc, uri, malformed_crldp);
      return;
    }
    if (!is_rsync((char *) n->d.uniformResourceIdentifier->data)) {
      logmsg(rc, log_verbose, "Skipping non-rsync URI %s for %s",
	     (char *) n->d.uniformResourceIdentifier->data, uri->s);
      continue;
    }
    if (sizeof(result->s) <= n->d.uniformResourceIdentifier->length) {
      logmsg(rc, log_data_err, "Skipping improbably long URI %s for %s",
	     (char *) n->d.uniformResourceIdentifier->data, uri->s);
      mib_increment(rc, uri, uri_too_long);
      continue;
    }
    strcpy(result->s, (char *) n->d.uniformResourceIdentifier->data);
    return;
  }
}

/**
 * Extract SIA or AIA data from a certificate.
 */
static void extract_access_uri(const rcynic_ctx_t *rc,
			       const uri_t *uri,
			       const AUTHORITY_INFO_ACCESS *xia,
			       const unsigned char *oid,
			       const int oidlen,
			       uri_t *result)
{
  int i;

  if (!xia)
    return;

  for (i = 0; i < sk_ACCESS_DESCRIPTION_num(xia); i++) {
    ACCESS_DESCRIPTION *a = sk_ACCESS_DESCRIPTION_value(xia, i);
    assert(a != NULL);
    if (a->location->type != GEN_URI)
      return;
    if (oid_cmp(a->method, oid, oidlen))
      continue;
    if (!is_rsync((char *) a->location->d.uniformResourceIdentifier->data)) {
      logmsg(rc, log_verbose, "Skipping non-rsync URI %s for %s",
	     a->location->d.uniformResourceIdentifier->data, uri->s);
      continue;
    }
    if (sizeof(result->s) <= a->location->d.uniformResourceIdentifier->length) {
      logmsg(rc, log_data_err, "Skipping improbably long URI %s for %s",
	     a->location->d.uniformResourceIdentifier->data, uri->s);
      mib_increment(rc, uri, uri_too_long);
      continue;
    }
    strcpy(result->s, (char *) a->location->d.uniformResourceIdentifier->data);
    return;
  }
}

/**
 * Parse interesting stuff from a certificate.
 */
static void parse_cert(const rcynic_ctx_t *rc, X509 *x, certinfo_t *c, const uri_t *uri)
{
  STACK_OF(DIST_POINT) *crldp;
  AUTHORITY_INFO_ACCESS *xia;

  assert(x != NULL && c != NULL && uri != NULL);
  memset(c, 0, sizeof(*c));

  c->ca = X509_check_ca(x) == 1;

  assert(strlen(uri->s) < sizeof(c->uri));
  strcpy(c->uri.s, uri->s);

  if ((xia = X509_get_ext_d2i(x, NID_info_access, NULL, NULL)) != NULL) {
    extract_access_uri(rc, uri, xia, id_ad_caIssuers, sizeof(id_ad_caIssuers), &c->aia);
    sk_ACCESS_DESCRIPTION_pop_free(xia, ACCESS_DESCRIPTION_free);
  }

  if ((xia = X509_get_ext_d2i(x, NID_sinfo_access, NULL, NULL)) != NULL) {
    extract_access_uri(rc, uri, xia, id_ad_caRepository, sizeof(id_ad_caRepository), &c->sia);
    extract_access_uri(rc, uri, xia, id_ad_rpkiManifest, sizeof(id_ad_rpkiManifest), &c->manifest);
    sk_ACCESS_DESCRIPTION_pop_free(xia, ACCESS_DESCRIPTION_free);
  }

  if ((crldp = X509_get_ext_d2i(x, NID_crl_distribution_points, NULL, NULL)) != NULL) {
    extract_crldp_uri(rc, uri, crldp, &c->crldp);
    sk_DIST_POINT_pop_free(crldp, DIST_POINT_free);
  }
}



/**
 * Attempt to read and check one CRL from disk.
 */

static X509_CRL *check_crl_1(const rcynic_ctx_t *rc,
			     const uri_t *uri,
			     path_t *path,
			     const path_t *prefix,
			     X509 *issuer,
			     const unsigned char *hash,
			     const size_t hashlen)
{
  hashbuf_t hashbuf;
  X509_CRL *crl = NULL;
  EVP_PKEY *pkey;
  int ret;

  assert(uri && path && issuer);

  if (!uri_to_filename(rc, uri, path, prefix))
    goto punt;

  if (hashlen > sizeof(hashbuf.h)) {
    reject(rc, uri, hash_too_long,
	   "because supplied hash is too long (%lu, our max is %lu)",
	   (unsigned long) hashlen, (unsigned long) sizeof(hashbuf.h));
    goto punt;
  }

  if (hash)
    crl = read_crl(path, &hashbuf);
  else
    crl = read_crl(path, NULL);

  if (!crl)
    goto punt;

  if (hash && memcmp(hashbuf.h, hash, hashlen)) {
    reject(rc, uri, crl_digest_mismatch,
	   "because digest of CRL did not match value from manifest");
    goto punt;
  }

  if ((pkey = X509_get_pubkey(issuer)) == NULL)
    goto punt;
  ret = X509_CRL_verify(crl, pkey);
  EVP_PKEY_free(pkey);

  if (ret > 0)
    return crl;

 punt:
  X509_CRL_free(crl);
  return NULL;
}

/**
 * Check whether we already have a particular CRL, attempt to fetch it
 * and check issuer's signature if we don't.
 */
static X509_CRL *check_crl(const rcynic_ctx_t *rc,
			   const uri_t *uri,
			   X509 *issuer,
			   const unsigned char *hash,
			   const size_t hashlen)
{
  path_t path;
  X509_CRL *crl;

  if (uri_to_filename(rc, uri, &path, &rc->authenticated) &&
      (crl = read_crl(&path, NULL)) != NULL)
    return crl;

  logmsg(rc, log_telemetry, "Checking CRL %s", uri->s);

  assert(rsync_cached_uri(rc, uri));

  if ((crl = check_crl_1(rc, uri, &path, &rc->unauthenticated,
			 issuer, hash, hashlen))) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, current_crl_accepted);
    return crl;
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, current_crl_rejected);
  }

  if ((crl = check_crl_1(rc, uri, &path, &rc->old_authenticated,
			 issuer, hash, hashlen))) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, backup_crl_accepted);
    return crl;
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, backup_crl_rejected);
  }

  return NULL;
}



/**
 * Check whether extensions in a certificate are allowed by profile.
 * Also returns failure in a few null-pointer cases that can't
 * possibly conform to profile.
 */
static int check_allowed_extensions(const X509 *x, const int allow_eku)
{
  int i;

  if (x == NULL || x->cert_info == NULL || x->cert_info->extensions == NULL)
    return 0;

  for (i = 0; i < sk_X509_EXTENSION_num(x->cert_info->extensions); i++) {
    switch (OBJ_obj2nid(sk_X509_EXTENSION_value(x->cert_info->extensions,
						i)->object)) {
    case NID_basic_constraints:
    case NID_subject_key_identifier:
    case NID_authority_key_identifier:
    case NID_key_usage:
    case NID_crl_distribution_points:
    case NID_info_access:
    case NID_sinfo_access:
    case NID_certificate_policies:
    case NID_sbgp_ipAddrBlock:
    case NID_sbgp_autonomousSysNum:
      continue;
    case NID_ext_key_usage:
      if (allow_eku)
	continue;
      else
	return 0;
    default:
      return 0;
    }
  }

  return 1;
}



/**
 * Validation callback function for use with x509_verify_cert().
 */
static int check_x509_cb(int ok, X509_STORE_CTX *ctx)
{
  rcynic_x509_store_ctx_t *rctx = (rcynic_x509_store_ctx_t *) ctx;
  mib_counter_t counter;

  assert(rctx != NULL);

  switch (ctx->error) {
  case X509_V_OK:
    return ok;

  case X509_V_ERR_SUBJECT_ISSUER_MISMATCH:
    /*
     * Informational events, not really errors.  ctx->check_issued()
     * is called in many places where failure to find an issuer is not
     * a failure for the calling function.  Just leave these alone.
     */
    return ok;

  case X509_V_ERR_CRL_HAS_EXPIRED:
    /*
     * This may not be an error at all.  CRLs don't really "expire",
     * although the signatures over them do.  What OpenSSL really
     * means by this error is just "it's now later than this source
     * said it intended to publish a new CRL.  Unclear whether this
     * should be an error; current theory is that it should not be.
     */
    if (rctx->rc->allow_stale_crl) {
      ok = 1;
      if (sk_OPENSSL_STRING_find(rctx->rc->stale_cache, rctx->subject->crldp.s) >= 0)
	return ok;
      if (!sk_OPENSSL_STRING_push_strdup(rctx->rc->stale_cache, rctx->subject->crldp.s))
	logmsg(rctx->rc, log_sys_err,
	       "Couldn't cache stale CRLDP %s, blundering onward", rctx->subject->crldp.s);
    }
    logmsg(rctx->rc, log_data_err, "Stale CRL %s", rctx->subject->crldp.s);
    if (ok)
      mib_increment(rctx->rc, &rctx->subject->uri, stale_crl);
    else
      reject(rctx->rc, &rctx->subject->uri, stale_crl, "due to stale CRL %s", rctx->subject->crldp.s);
    return ok;

  case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
    /*
     * This is another error that's only an error in the strange world
     * of OpenSSL, but a more serious one.  By default, OpenSSL
     * expects all trust anchors to be self-signed.  This is not a
     * PKIX requirement, it's just an OpenSSL thing, but one violates
     * it at one's peril, because the only way to convince OpenSSL to
     * allow a non-self-signed trust anchor is to intercept this
     * "error" in the verify callback handler.
     *
     * So this program supports non-self-signed trust anchors, but be
     * warned that enabling this feature may cause this program's
     * output not to work with other OpenSSL-based applications.
     */
    if (rctx->rc->allow_non_self_signed_trust_anchor)
      ok = 1;
    if (ok)
      mib_increment(rctx->rc, &rctx->subject->uri, trust_anchor_not_self_signed);
    else
      reject(rctx->rc, &rctx->subject->uri, trust_anchor_not_self_signed, 
	     "because trust anchor was not self-signed");
    return ok;

  /*
   * Select correct MIB counter for every known OpenSSL verify errors
   * except the ones we handle explicitly above, then fall through to
   * common handling for all of these.
   */
#define QV(x)							\
  case x:							\
    counter = mib_openssl_##x;					\
    break;
    MIB_COUNTERS_FROM_OPENSSL;
#undef	QV

  default:
    counter = unknown_verify_error;
    break;
  }

  if (ok)
    mib_increment(rctx->rc, &rctx->subject->uri, counter);
  else
    reject(rctx->rc, &rctx->subject->uri, counter,
	   "due to validation failure at depth %d: %s",
	   ctx->error_depth, 
	   X509_verify_cert_error_string(ctx->error));

  return ok;
}

/**
 * Check crypto aspects of a certificate, policy OID, RFC 3779 path
 * validation, and conformance to the RPKI certificate profile.
 */
static int check_x509(const rcynic_ctx_t *rc,
		      STACK_OF(X509) *certs,
		      X509 *x,
		      const certinfo_t *subject,
		      const certinfo_t *issuer_certinfo)
{
  rcynic_x509_store_ctx_t rctx;
  STACK_OF(X509_CRL) *crls = NULL;
  EVP_PKEY *pkey = NULL;
  X509_CRL *crl = NULL;
  unsigned long flags = (X509_V_FLAG_POLICY_CHECK | X509_V_FLAG_EXPLICIT_POLICY | X509_V_FLAG_X509_STRICT);
  X509 *issuer;
  int ret = 0;

  assert(rc && certs && x && subject);

  if (!X509_STORE_CTX_init(&rctx.ctx, rc->x509_store, x, NULL))
    return 0;
  rctx.rc = rc;
  rctx.subject = subject;

  issuer = sk_X509_value(certs, sk_X509_num(certs) - 1);
  assert(issuer != NULL);

  if (subject->sia.s[0] && subject->sia.s[strlen(subject->sia.s) - 1] != '/') {
    reject(rc, &subject->uri, malformed_sia,
	   "due to malformed SIA %s", subject->sia.s);
    goto done;
  }

  if (!subject->ta && !subject->aia.s[0]) {
    reject(rc, &subject->uri, aia_missing, "due to missing AIA extension");
    goto done;
  }

  if (!issuer_certinfo->ta && strcmp(issuer_certinfo->uri.s, subject->aia.s)) {
    reject(rc, &subject->uri, aia_mismatch,
	   "because AIA %s doesn't match parent", subject->aia.s);
    goto done;
  }

  if (subject->ca && !subject->sia.s[0]) {
    reject(rc, &subject->uri, sia_missing,
	   "because SIA extension repository pointer is missing");
    goto done;
  }

  if (subject->ca && !subject->manifest.s[0]) {
    reject(rc, &subject->uri, manifest_missing,
	   "because SIA extension manifest pointer is missing");
    goto done;
  }

  if (subject->ca && !startswith(subject->manifest.s, subject->sia.s)) {
    reject(rc, &subject->uri, manifest_mismatch,
	   "because SIA manifest %s points outside publication point %s",
	   subject->manifest.s, subject->sia.s);
    goto done;
  }

  if (!check_allowed_extensions(x, !subject->ca)) {
    reject(rc, &subject->uri, disallowed_extension,
	   "due to disallowed X.509v3 extension");
    goto done;
  }

  if (subject->ta) {

    if (subject->crldp.s[0]) {
      reject(rc, &subject->uri, trust_anchor_with_crldp,
	     "because it's a trust anchor but has a CRLDP extension");
      goto done;
    }

  } else {

    if (!subject->crldp.s[0]) {
      reject(rc, &subject->uri, crldp_missing, "because CRLDP extension is missing");
      goto done;
    }

    if (!subject->ca && !startswith(subject->crldp.s, issuer_certinfo->sia.s)) {
      reject(rc, &subject->uri, crldp_mismatch,
	     "because CRLDP %s points outside issuer's publication point %s",
	     subject->crldp.s, issuer_certinfo->sia.s);
      goto done;
    }
 
    flags |= X509_V_FLAG_CRL_CHECK;

    if ((pkey = X509_get_pubkey(issuer)) == NULL || X509_verify(x, pkey) <= 0) {
      reject(rc, &subject->uri, certificate_bad_signature,
	     "because it failed signature check prior to CRL fetch");
      goto done;
    }

    if ((crl = check_crl(rc, &subject->crldp, issuer, NULL, 0)) == NULL) {
      reject(rc, &subject->uri, certificate_bad_crl,
	     "due to bad CRL %s", subject->crldp.s);
      goto done;
    }

    if ((crls = sk_X509_CRL_new_null()) == NULL || !sk_X509_CRL_push(crls, crl)) {
      logmsg(rc, log_sys_err,
	     "Internal allocation error setting up CRL for validation");
      goto done;
    }
    crl = NULL;

    X509_STORE_CTX_set0_crls(&rctx.ctx, crls);

  }

  X509_STORE_CTX_trusted_stack(&rctx.ctx, certs);
  X509_STORE_CTX_set_verify_cb(&rctx.ctx, check_x509_cb);

  X509_VERIFY_PARAM_set_flags(rctx.ctx.param, flags);

  X509_VERIFY_PARAM_add0_policy(rctx.ctx.param, OBJ_txt2obj(rpki_policy_oid, 1));

 if (X509_verify_cert(&rctx.ctx) <= 0) {
   /*
    * Redundant error message?
    */
    logmsg(rc, log_data_err, "Validation failure for %s",
	   subject->uri.s[0] ? subject->uri.s : subject->ta ? "[Trust anchor]" : "[???]");
    goto done;
  }

 ret = 1;

 done:
  sk_X509_CRL_pop_free(crls, X509_CRL_free);
  X509_STORE_CTX_cleanup(&rctx.ctx);
  EVP_PKEY_free(pkey);
  X509_CRL_free(crl);

  return ret;
}

/**
 * Load certificate, check against manifest, then run it through all
 * the check_x509() tests.
 */
static X509 *check_cert_1(const rcynic_ctx_t *rc,
			  const uri_t *uri,
			  path_t *path,
			  const path_t *prefix,
			  STACK_OF(X509) *certs,
			  const certinfo_t *issuer,
			  certinfo_t *subject,
			  const unsigned char *hash,
			  const size_t hashlen)
{
  hashbuf_t hashbuf;
  X509 *x = NULL;

  assert(uri && path && certs && issuer && subject);

  if (!uri_to_filename(rc, uri, path, prefix))
    return NULL;

  if (access(path->s, R_OK))
    return NULL;

  if (hashlen > sizeof(hashbuf.h)) {
    reject(rc, uri, hash_too_long,
	   "because supplied hash is too long (%lu, our max is %lu)",
	   (unsigned long) hashlen, (unsigned long) sizeof(hashbuf.h));
    goto punt;
  }

  if (hash)
    x = read_cert(path, &hashbuf);
  else
    x = read_cert(path, NULL);

  if (!x) {
    logmsg(rc, log_sys_err, "Can't read certificate %s", path->s);
    goto punt;
  }

  if (hash && memcmp(hashbuf.h, hash, hashlen)) {
    reject(rc, uri, certificate_digest_mismatch,
	   "because digest did not match value in manifest");
    goto punt;
  }

  parse_cert(rc, x, subject, uri);

  if (check_x509(rc, certs, x, subject, issuer))
    return x;

 punt:
  X509_free(x);
  return NULL;
}

/**
 * Try to find a good copy of a certificate either in fresh data or in
 * backup data from a previous run of this program.
 */
static X509 *check_cert(rcynic_ctx_t *rc,
			uri_t *uri,
			STACK_OF(walk_ctx_t) *wsk,
			certinfo_t *subject,
			const unsigned char *hash,
			const size_t hashlen)
{
  walk_ctx_t *w = walk_ctx_stack_head(wsk);
  mib_counter_t accept_code, reject_code;
  const certinfo_t *issuer = NULL;
  STACK_OF(X509) *certs = NULL;
  const path_t *prefix = NULL;
  path_t path;
  X509 *x;

  assert(rc && uri && wsk && w && subject);

  issuer = &w->certinfo;

  switch (w->state) {
  case walk_state_current:
    prefix = &rc->unauthenticated;
    accept_code = current_cert_accepted;
    reject_code = current_cert_rejected;
    break;
  case walk_state_backup:
    prefix = &rc->old_authenticated;
    accept_code = backup_cert_accepted;
    reject_code = backup_cert_rejected;
    break;
  default:
    return NULL;
  }

  /*
   * If target file already exists and we're not here to recheck with
   * better data, just get out now.
   */

  if (uri_to_filename(rc, uri, &path, &rc->authenticated) && 
      !access(path.s, R_OK)) {
    if (w->state == walk_state_backup || sk_OPENSSL_STRING_find(rc->backup_cache, uri->s) < 0)
      return NULL;
    mib_increment(rc, uri, current_cert_recheck);
    logmsg(rc, log_telemetry, "Rechecking %s", uri->s);
  } else {
    logmsg(rc, log_telemetry, "Checking %s", uri->s);
  }

  if ((certs = walk_ctx_stack_certs(wsk)) == NULL)
    return NULL;

  rc->indent++;

  if ((x = check_cert_1(rc, uri, &path, prefix, certs, issuer, subject, hash, hashlen)) != NULL) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, accept_code);
    if (w->state == walk_state_current)
      sk_OPENSSL_STRING_remove(rc->backup_cache, uri->s);
    else if (!sk_OPENSSL_STRING_push_strdup(rc->backup_cache, uri->s))
      logmsg(rc, log_sys_err, "Couldn't cache URI %s, blundering onward", uri->s);
      
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, reject_code);
  }

  rc->indent--;

  sk_X509_free(certs);
  certs = NULL;

  return x;
}



/**
 * Read and check one manifest from disk.
 */
static Manifest *check_manifest_1(const rcynic_ctx_t *rc,
				  const uri_t *uri,
				  path_t *path,
				  const path_t *prefix,
				  STACK_OF(X509) *certs)
{
  CMS_ContentInfo *cms = NULL;
  const ASN1_OBJECT *eContentType = NULL;
  STACK_OF(X509) *signers = NULL;
  STACK_OF(X509_CRL) *crls = NULL;
  X509_CRL *crl = NULL;
  Manifest *manifest = NULL, *result = NULL;
  BIO *bio = NULL;
  rcynic_x509_store_ctx_t rctx;
  certinfo_t certinfo;
  int i, initialized_store_ctx = 0;
  FileAndHash *fah = NULL;
  char *crl_tail;

  assert(rc && uri && path && prefix && certs && sk_X509_num(certs));

  if (!uri_to_filename(rc, uri, path, prefix) ||
      (cms = read_cms(path, NULL)) == NULL)
    goto done;

  if ((eContentType = CMS_get0_eContentType(cms)) == NULL ||
      oid_cmp(eContentType, id_ct_rpkiManifest, sizeof(id_ct_rpkiManifest))) {
    reject(rc, uri, manifest_bad_econtenttype,
	   "due to bad manifest eContentType");
    goto done;
  }

  if ((bio = BIO_new(BIO_s_mem())) == NULL) {
    logmsg(rc, log_sys_err, "Couldn't allocate BIO for manifest %s", uri->s);
    goto done;
  }

  if (CMS_verify(cms, NULL, NULL, NULL, bio, CMS_NO_SIGNER_CERT_VERIFY) <= 0) {
    reject(rc, uri, manifest_invalid_cms,
	   "due to validation failure for manifest CMS message");
    goto done;
  }

  if ((signers = CMS_get0_signers(cms)) == NULL || sk_X509_num(signers) != 1) {
    reject(rc, uri, manifest_missing_signer,
	   "because could not couldn't extract manifest EE certificate from CMS");
    goto done;
  }

  parse_cert(rc, sk_X509_value(signers, 0), &certinfo, uri);

  if (!certinfo.crldp.s[0]) {
    reject(rc, uri, manifest_missing_crldp,
	   "due to missing CRLDP in manifest EE certificate");
    goto done;
  }

  if ((crl_tail = strrchr(certinfo.crldp.s, '/')) == NULL) {
    reject(rc, uri, manifest_malformed_crldp,
	   "due to malformed CRLDP %s in manifest EE certificate",
	   certinfo.crldp.s);
    goto done;
  }
  crl_tail++;

  if ((manifest = ASN1_item_d2i_bio(ASN1_ITEM_rptr(Manifest), bio, NULL)) == NULL) {
    reject(rc, uri, manifest_decode_error, "because unable to decode manifest");
    goto done;
  }

  if (manifest->version) {
    reject(rc, uri, manifest_wrong_version,
	   "because manifest version should be defaulted zero, not %ld",
	   ASN1_INTEGER_get(manifest->version));
    goto done;
  }

  if (X509_cmp_current_time(manifest->thisUpdate) > 0) {
    reject(rc, uri, manifest_not_yet_valid, "because manifest not yet valid");
    goto done;
  }

  if (X509_cmp_current_time(manifest->nextUpdate) < 0 &&
      sk_OPENSSL_STRING_find(rc->stale_cache, uri->s) < 0) {
    if (!sk_OPENSSL_STRING_push_strdup(rc->stale_cache, uri->s))
      logmsg(rc, log_sys_err, "Couldn't cache stale manifest %s, blundering onward", uri->s);
    if (!rc->allow_stale_manifest) {
      reject(rc, uri, stale_manifest,
	     "because it is a stale manifest");
      goto done;
    }
    logmsg(rc, log_data_err, "Stale manifest %s", uri->s);
    mib_increment(rc, uri, stale_manifest);
  }

  if (manifest->fileHashAlg == NULL ||
      oid_cmp(manifest->fileHashAlg, id_sha256, sizeof(id_sha256)))
    goto done;

  for (i = 0; (fah = sk_FileAndHash_value(manifest->fileList, i)) != NULL; i++)
    if (!strcmp((char *) fah->file->data, crl_tail))
      break;

  if (fah) {
    crl = check_crl(rc, &certinfo.crldp,
		    sk_X509_value(certs, sk_X509_num(certs) - 1),
		    fah->hash->data, fah->hash->length);
  } else if (rc->require_crl_in_manifest) {
    reject(rc, uri, crl_not_in_manifest,
	   "because CRL %s missing from manifest", certinfo.crldp.s);
    goto done;
  } else {
    logmsg(rc, log_data_err, "Manifest %s is missing entry for CRL %s", uri->s, certinfo.crldp.s);
    mib_increment(rc, uri, crl_not_in_manifest);
    crl = check_crl(rc, &certinfo.crldp,
		    sk_X509_value(certs, sk_X509_num(certs) - 1),
		    NULL, 0);
  }

  if (!crl) {
    reject(rc, uri, manifest_bad_crl, "due to bad manifest CRL %s", certinfo.crldp.s);
    goto done;
  }

  if ((crls = sk_X509_CRL_new_null()) == NULL || !sk_X509_CRL_push(crls, crl))
    goto done;
  crl = NULL;

  if (!(initialized_store_ctx = X509_STORE_CTX_init(&rctx.ctx, rc->x509_store, sk_X509_value(signers, 0), NULL)))
    goto done;
  
  rctx.rc = rc;
  rctx.subject = &certinfo;

  X509_STORE_CTX_trusted_stack(&rctx.ctx, certs);
  X509_STORE_CTX_set0_crls(&rctx.ctx, crls);
  X509_STORE_CTX_set_verify_cb(&rctx.ctx, check_x509_cb);

  X509_VERIFY_PARAM_set_flags(rctx.ctx.param,
			      X509_V_FLAG_CRL_CHECK |
			      X509_V_FLAG_POLICY_CHECK |
			      X509_V_FLAG_EXPLICIT_POLICY |
			      X509_V_FLAG_X509_STRICT);

  X509_VERIFY_PARAM_add0_policy(rctx.ctx.param, OBJ_txt2obj(rpki_policy_oid, 1));

  if (X509_verify_cert(&rctx.ctx) <= 0) {
    /*
     * Redundant error message?
     */
    logmsg(rc, log_data_err, "Validation failure for manifest %s EE certificate", uri->s);
    mib_increment(rc, uri, manifest_invalid_ee);
    goto done;
  }

  result = manifest;
  manifest = NULL;

 done:
  if (initialized_store_ctx)
    X509_STORE_CTX_cleanup(&rctx.ctx);
  BIO_free(bio);
  Manifest_free(manifest);
  CMS_ContentInfo_free(cms);
  sk_X509_free(signers);
  sk_X509_CRL_pop_free(crls, X509_CRL_free);

  return result;
}

/**
 * Check whether we already have a particular manifest, attempt to fetch it
 * and check issuer's signature if we don't.
 */
static Manifest *check_manifest(const rcynic_ctx_t *rc,
				STACK_OF(walk_ctx_t) *wsk)
{
  walk_ctx_t *w = walk_ctx_stack_head(wsk);
  CMS_ContentInfo *cms = NULL;
  Manifest *manifest = NULL;
  STACK_OF(X509) *certs = NULL;
  BIO *bio = NULL;
  path_t path;
  uri_t *uri;

  assert(rc && wsk && w);

  uri = &w->certinfo.manifest;

  if (uri_to_filename(rc, uri, &path, &rc->authenticated) &&
      (cms = read_cms(&path, NULL)) != NULL &&
      (bio = BIO_new(BIO_s_mem()))!= NULL &&
      CMS_verify(cms, NULL, NULL, NULL, bio,
		 CMS_NO_SIGNER_CERT_VERIFY |
		 CMS_NO_ATTR_VERIFY |
		 CMS_NO_CONTENT_VERIFY) > 0)
    manifest = ASN1_item_d2i_bio(ASN1_ITEM_rptr(Manifest), bio, NULL);

  CMS_ContentInfo_free(cms);
  BIO_free(bio);

  if (manifest != NULL)
    return manifest;

  logmsg(rc, log_telemetry, "Checking manifest %s", uri->s);

  assert(rsync_cached_uri(rc, uri));

  if ((certs = walk_ctx_stack_certs(wsk)) == NULL)
    return NULL;

  if (manifest == NULL &&
      (manifest = check_manifest_1(rc, uri, &path,
				   &rc->unauthenticated, certs))) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, current_manifest_accepted);
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, current_manifest_rejected);
  }

  if (manifest == NULL &&
      (manifest = check_manifest_1(rc, uri, &path,
				   &rc->old_authenticated, certs))) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, backup_manifest_accepted);
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, backup_manifest_rejected);
  }

  sk_X509_free(certs);
  certs = NULL;

  return manifest;
}



/**
 * Extract a ROA prefix from the ASN.1 bitstring encoding.
 */
static int extract_roa_prefix(unsigned char *addr,
			      unsigned *prefixlen,
			      const ASN1_BIT_STRING *bs,
			      const unsigned afi)
{
  unsigned length;

  switch (afi) {
  case IANA_AFI_IPV4: length =  4; break;
  case IANA_AFI_IPV6: length = 16; break;
  default: return 0;
  }

  if (bs->length < 0 || bs->length > length)
    return 0;

  if (bs->length > 0) {
    memcpy(addr, bs->data, bs->length);
    if ((bs->flags & 7) != 0) {
      unsigned char mask = 0xFF >> (8 - (bs->flags & 7));
      addr[bs->length - 1] &= ~mask;
    }
  }

  memset(addr + bs->length, 0, length - bs->length);

  *prefixlen = (bs->length * 8) - (bs->flags & 7);

  return 1;
}

/**
 * Read and check one ROA from disk.
 */
static int check_roa_1(const rcynic_ctx_t *rc,
		       const uri_t *uri,
		       path_t *path,
		       const path_t *prefix,
		       STACK_OF(X509) *certs,
		       const unsigned char *hash,
		       const size_t hashlen)
{
  unsigned char addrbuf[ADDR_RAW_BUF_LEN];
  const ASN1_OBJECT *eContentType = NULL;
  STACK_OF(IPAddressFamily) *roa_resources = NULL, *ee_resources = NULL;
  STACK_OF(X509_CRL) *crls = NULL;
  STACK_OF(X509) *signers = NULL;
  CMS_ContentInfo *cms = NULL;
  X509_CRL *crl = NULL;
  hashbuf_t hashbuf;
  ROA *roa = NULL;
  BIO *bio = NULL;
  rcynic_x509_store_ctx_t rctx;
  certinfo_t certinfo;
  int i, j, initialized_store_ctx = 0, result = 0;
  unsigned afi, *safi = NULL, safi_, prefixlen;
  ROAIPAddressFamily *rf;
  ROAIPAddress *ra;

  assert(rc && uri && path && prefix && certs && sk_X509_num(certs));

  if (!uri_to_filename(rc, uri, path, prefix))
    goto error;

  if (hashlen > sizeof(hashbuf.h)) {
    reject(rc, uri, hash_too_long,
	   "because supplied hash is too long (%lu, our max is %lu)",
	   (unsigned long) hashlen, (unsigned long) sizeof(hashbuf.h));
    goto error;
  }

  if (hash)
    cms = read_cms(path, &hashbuf);
  else
    cms = read_cms(path, NULL);

  if (!cms)
    goto error;

  if (hash && memcmp(hashbuf.h, hash, hashlen)) {
    reject(rc, uri, roa_digest_mismatch,
	   "because ROA does not match manifest digest");
    goto error;
  }

  if (!(eContentType = CMS_get0_eContentType(cms)) ||
      oid_cmp(eContentType, id_ct_routeOriginAttestation,
	      sizeof(id_ct_routeOriginAttestation))) {
    reject(rc, uri, roa_bad_econtenttype,
	   "because ROA has bad eContentType");
    goto error;
  }

  if ((bio = BIO_new(BIO_s_mem())) == NULL) {
    logmsg(rc, log_sys_err, "Couldn't allocate BIO for ROA %s", uri->s);
    goto error;
  }

  if (CMS_verify(cms, NULL, NULL, NULL, bio, CMS_NO_SIGNER_CERT_VERIFY) <= 0) {
    reject(rc, uri, roa_invalid_cms, "because ROA CMS failed validation");
    goto error;
  }

  if (!(signers = CMS_get0_signers(cms)) || sk_X509_num(signers) != 1) {
    reject(rc, uri, roa_missing_signer,
	   "because couldn't extract CMS signer from ROA");
    goto error;
  }

  parse_cert(rc, sk_X509_value(signers, 0), &certinfo, uri);

  if (!(roa = ASN1_item_d2i_bio(ASN1_ITEM_rptr(ROA), bio, NULL))) {
    reject(rc, uri, roa_decode_error, "because could not decode ROA");
    goto error;
  }

  if (roa->version) {
    reject(rc, uri, roa_wrong_version,
	   "because ROA version should be defaulted zero, not %ld",
	   ASN1_INTEGER_get(roa->version));
    goto error;
  }

  /*
   * ROA issuer doesn't need rights to the ASN, so we don't need to
   * check the asID field.
   */

  ee_resources = X509_get_ext_d2i(sk_X509_value(signers, 0), NID_sbgp_ipAddrBlock, NULL, NULL);

  /*
   * Extract prefixes from ROA and convert them into a resource set.
   */

  if (!(roa_resources = sk_IPAddressFamily_new_null()))
    goto error;

  for (i = 0; i < sk_ROAIPAddressFamily_num(roa->ipAddrBlocks); i++) {
    rf = sk_ROAIPAddressFamily_value(roa->ipAddrBlocks, i);
    if (!rf || !rf->addressFamily || rf->addressFamily->length < 2 || rf->addressFamily->length > 3) {
      reject(rc, uri, malformed_roa_addressfamily,
	     "because ROA addressFamily length should be 2 or 3, not %lu",
	     (unsigned long) rf->addressFamily->length);
      goto error;
    }
    afi = (rf->addressFamily->data[0] << 8) | (rf->addressFamily->data[1]);
    if (rf->addressFamily->length == 3)
      *(safi = &safi_) = rf->addressFamily->data[2];
    for (j = 0; j < sk_ROAIPAddress_num(rf->addresses); j++) {
      ra = sk_ROAIPAddress_value(rf->addresses, j);
      if (!ra ||
	  !extract_roa_prefix(addrbuf, &prefixlen, ra->IPAddress, afi) ||
	  !v3_addr_add_prefix(roa_resources, afi, safi, addrbuf, prefixlen)) {
	reject(rc, uri, roa_resources_malformed,
	       "because ROA resources appear malformed");
	goto error;
      }
    }
  }

  /*
   * ROAs can include nested prefixes, so direct translation to
   * resource sets could include overlapping ranges, which is illegal.
   * So we have to remove nested stuff before whacking into canonical
   * form.  Fortunately, this is relatively easy, since we know these
   * are just prefixes, not ranges: in a list of prefixes sorted by
   * the RFC 3779 rules, the first element of a set of nested prefixes
   * will always be the least specific.
   */

  for (i = 0; i < sk_IPAddressFamily_num(roa_resources); i++) {
    IPAddressFamily *f = sk_IPAddressFamily_value(roa_resources, i);

    if ((afi = v3_addr_get_afi(f)) == 0) {
      reject(rc, uri, roa_bad_afi,
	     "because found bad AFI while extracting data from ROA");
      goto error;
    }

    if (f->ipAddressChoice->type == IPAddressChoice_addressesOrRanges) {
      IPAddressOrRanges *aors = f->ipAddressChoice->u.addressesOrRanges;

      sk_IPAddressOrRange_sort(aors);

      for (j = 0; j < sk_IPAddressOrRange_num(aors) - 1; j++) {
	IPAddressOrRange *a = sk_IPAddressOrRange_value(aors, j);
	IPAddressOrRange *b = sk_IPAddressOrRange_value(aors, j + 1);
	unsigned char a_min[ADDR_RAW_BUF_LEN], a_max[ADDR_RAW_BUF_LEN];
	unsigned char b_min[ADDR_RAW_BUF_LEN], b_max[ADDR_RAW_BUF_LEN];
	int length;

	if ((length = v3_addr_get_range(a, afi, a_min, a_max, ADDR_RAW_BUF_LEN)) == 0 ||
	    (length = v3_addr_get_range(b, afi, b_min, b_max, ADDR_RAW_BUF_LEN)) == 0) {
	  reject(rc, uri, roa_resources_malformed, "because ROA resources appear malformed");
	  goto error;
	}

	if (memcmp(a_max, b_max, length) >= 0) {
	  (void) sk_IPAddressOrRange_delete(aors, j + 1);
	  IPAddressOrRange_free(b);
	  --j;
	}
      }
    }
  }

  if (!v3_addr_canonize(roa_resources)) {
    reject(rc, uri, roa_resources_malformed, "because ROA resources appear malformed");
    goto error;
  }

  if (!v3_addr_subset(roa_resources, ee_resources)) {
    reject(rc, uri, roa_not_nested,
	   "because ROA's resources are not a subset of its signing EE certificate's resources");
    goto error;
  }

  if (!(crl = check_crl(rc, &certinfo.crldp, sk_X509_value(certs, sk_X509_num(certs) - 1), NULL, 0))) {
    reject(rc, uri, roa_bad_crl, "because ROA EE certificate has bad CRL %s", certinfo.crldp.s);
    goto error;
  }

  if (!(crls = sk_X509_CRL_new_null()) || !sk_X509_CRL_push(crls, crl))
    goto error;
  crl = NULL;

  if (!(initialized_store_ctx = X509_STORE_CTX_init(&rctx.ctx, rc->x509_store, sk_X509_value(signers, 0), NULL)))
    goto error;
  
  rctx.rc = rc;
  rctx.subject = &certinfo;

  X509_STORE_CTX_trusted_stack(&rctx.ctx, certs);
  X509_STORE_CTX_set0_crls(&rctx.ctx, crls);
  X509_STORE_CTX_set_verify_cb(&rctx.ctx, check_x509_cb);

  X509_VERIFY_PARAM_set_flags(rctx.ctx.param,
			      X509_V_FLAG_CRL_CHECK |
			      X509_V_FLAG_POLICY_CHECK |
			      X509_V_FLAG_EXPLICIT_POLICY |
			      X509_V_FLAG_X509_STRICT);

  X509_VERIFY_PARAM_add0_policy(rctx.ctx.param, OBJ_txt2obj(rpki_policy_oid, 1));

  if (X509_verify_cert(&rctx.ctx) <= 0) {
    /*
     * Redundant error message?
     */
    logmsg(rc, log_data_err, "Validation failure for ROA %s EE certificate", uri->s);
    mib_increment(rc, uri, roa_invalid_ee);
    goto error;
  }

  result = 1;

 error:
  if (initialized_store_ctx)
    X509_STORE_CTX_cleanup(&rctx.ctx);
  BIO_free(bio);
  ROA_free(roa);
  CMS_ContentInfo_free(cms);
  sk_X509_free(signers);
  sk_X509_CRL_pop_free(crls, X509_CRL_free);
  sk_IPAddressFamily_pop_free(roa_resources, IPAddressFamily_free);
  sk_IPAddressFamily_pop_free(ee_resources, IPAddressFamily_free);

  return result;
}

/**
 * Check whether we already have a particular ROA, attempt to fetch it
 * and check issuer's signature if we don't.
 */
static void check_roa(const rcynic_ctx_t *rc,
		      const uri_t *uri,
		      STACK_OF(walk_ctx_t) *wsk,
		      const unsigned char *hash,
		      const size_t hashlen)
{
  STACK_OF(X509) *certs = NULL;
  path_t path;

  assert(rc && uri && wsk);

  if (uri_to_filename(rc, uri, &path, &rc->authenticated) &&
      !access(path.s, F_OK))
    return;

  logmsg(rc, log_telemetry, "Checking ROA %s", uri->s);

  assert(rsync_cached_uri(rc, uri));

  if ((certs = walk_ctx_stack_certs(wsk)) == NULL)
    return;

  if (check_roa_1(rc, uri, &path, &rc->unauthenticated,
		  certs, hash, hashlen)) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, current_roa_accepted);
    goto done;
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, current_roa_rejected);
  }

  if (check_roa_1(rc, uri, &path, &rc->old_authenticated,
		  certs, hash, hashlen)) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, backup_roa_accepted);
    goto done;
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, backup_roa_rejected);
  }

 done:
  sk_X509_free(certs);
}



/**
 * Read and check one Ghostbuster record from disk.
 */
static int check_ghostbuster_1(const rcynic_ctx_t *rc,
			       const uri_t *uri,
			       path_t *path,
			       const path_t *prefix,
			       STACK_OF(X509) *certs,
			       const unsigned char *hash,
			       const size_t hashlen)
{
  const ASN1_OBJECT *eContentType = NULL;
  STACK_OF(X509_CRL) *crls = NULL;
  STACK_OF(X509) *signers = NULL;
  CMS_ContentInfo *cms = NULL;
  X509_CRL *crl = NULL;
  hashbuf_t hashbuf;
  BIO *bio = NULL;
  rcynic_x509_store_ctx_t rctx;
  certinfo_t certinfo;
  int initialized_store_ctx = 0, result = 0;

  assert(rc && uri && path && prefix && certs && sk_X509_num(certs));

  if (!uri_to_filename(rc, uri, path, prefix))
    goto error;

  if (hashlen > sizeof(hashbuf.h)) {
    reject(rc, uri, hash_too_long,
	   "because supplied hash is too long (%lu, our max is %lu)",
	   (unsigned long) hashlen, (unsigned long) sizeof(hashbuf.h));
    goto error;
  }

  if (hash)
    cms = read_cms(path, &hashbuf);
  else
    cms = read_cms(path, NULL);

  if (!cms)
    goto error;

  if (hash && memcmp(hashbuf.h, hash, hashlen)) {
    reject(rc, uri, ghostbuster_digest_mismatch,
	   "because Ghostbuster record does not match manifest digest");
    goto error;
  }

  if (!(eContentType = CMS_get0_eContentType(cms)) ||
      oid_cmp(eContentType, id_ct_rpkiGhostbusters,
	      sizeof(id_ct_rpkiGhostbusters))) {
    reject(rc, uri, ghostbuster_bad_econtenttype,
	   "because Ghostbuster record has bad eContentType");
    goto error;
  }

#if 0
  /*
   * May want this later if we're going to inspect the VCard.  For now,
   * just leave this NULL and the right thing should happen.
   */
  if ((bio = BIO_new(BIO_s_mem())) == NULL) {
    logmsg(rc, log_sys_err, "Couldn't allocate BIO for Ghostbuster record %s", uri->s);
    goto error;
  }
#endif

  if (CMS_verify(cms, NULL, NULL, NULL, bio, CMS_NO_SIGNER_CERT_VERIFY) <= 0) {
    reject(rc, uri, ghostbuster_invalid_cms, "because Ghostbuster record CMS failed validation");
    goto error;
  }

  if (!(signers = CMS_get0_signers(cms)) || sk_X509_num(signers) != 1) {
    reject(rc, uri, ghostbuster_missing_signer,
	   "because couldn't extract CMS signer from Ghostbuster record");
    goto error;
  }

  parse_cert(rc, sk_X509_value(signers, 0), &certinfo, uri);

#if 0
  /*
   * Here is where we would read the VCard from the bio returned by
   * CMS_verify() so that we could check the VCard.
   */
#endif

  if (!(crl = check_crl(rc, &certinfo.crldp, sk_X509_value(certs, sk_X509_num(certs) - 1), NULL, 0))) {
    reject(rc, uri, ghostbuster_bad_crl, "because Ghostbuster record EE certificate has bad CRL %s", certinfo.crldp.s);
    goto error;
  }

  if (!(crls = sk_X509_CRL_new_null()) || !sk_X509_CRL_push(crls, crl))
    goto error;
  crl = NULL;

  if (!(initialized_store_ctx = X509_STORE_CTX_init(&rctx.ctx, rc->x509_store, sk_X509_value(signers, 0), NULL)))
    goto error;
  
  rctx.rc = rc;
  rctx.subject = &certinfo;

  X509_STORE_CTX_trusted_stack(&rctx.ctx, certs);
  X509_STORE_CTX_set0_crls(&rctx.ctx, crls);
  X509_STORE_CTX_set_verify_cb(&rctx.ctx, check_x509_cb);

  X509_VERIFY_PARAM_set_flags(rctx.ctx.param,
			      X509_V_FLAG_CRL_CHECK |
			      X509_V_FLAG_POLICY_CHECK |
			      X509_V_FLAG_EXPLICIT_POLICY |
			      X509_V_FLAG_X509_STRICT);

  X509_VERIFY_PARAM_add0_policy(rctx.ctx.param, OBJ_txt2obj(rpki_policy_oid, 1));

  if (X509_verify_cert(&rctx.ctx) <= 0) {
    /*
     * Redundant error message?
     */
    logmsg(rc, log_data_err, "Validation failure for Ghostbuster record %s EE certificate", uri->s);
    mib_increment(rc, uri, ghostbuster_invalid_ee);
    goto error;
  }

  result = 1;

 error:
  if (initialized_store_ctx)
    X509_STORE_CTX_cleanup(&rctx.ctx);
  BIO_free(bio);
  CMS_ContentInfo_free(cms);
  sk_X509_free(signers);
  sk_X509_CRL_pop_free(crls, X509_CRL_free);

  return result;
}

/**
 * Check whether we already have a particular Ghostbuster record,
 * attempt to fetch it and check issuer's signature if we don't.
 */
static void check_ghostbuster(const rcynic_ctx_t *rc,
			      const uri_t *uri,
			      STACK_OF(walk_ctx_t) *wsk,
			      const unsigned char *hash,
			      const size_t hashlen)
{
  STACK_OF(X509) *certs = NULL;
  path_t path;

  assert(rc && uri && wsk);

  if (uri_to_filename(rc, uri, &path, &rc->authenticated) &&
      !access(path.s, F_OK))
    return;

  logmsg(rc, log_telemetry, "Checking Ghostbuster record %s", uri->s);

  assert(rsync_cached_uri(rc, uri));

  if ((certs = walk_ctx_stack_certs(wsk)) == NULL)
    return;

  if (check_ghostbuster_1(rc, uri, &path, &rc->unauthenticated,
			  certs, hash, hashlen)) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, current_ghostbuster_accepted);
    goto done;
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, current_ghostbuster_rejected);
  }

  if (check_ghostbuster_1(rc, uri, &path, &rc->old_authenticated,
			  certs, hash, hashlen)) {
    install_object(rc, uri, &path);
    mib_increment(rc, uri, backup_ghostbuster_accepted);
    goto done;
  } else if (!access(path.s, F_OK)) {
    mib_increment(rc, uri, backup_ghostbuster_rejected);
  }

 done:
  sk_X509_free(certs);
}



/**
 * Recursive walk of certificate hierarchy (core of the program).
 *
 * Walk all products of the current certificate, starting with the
 * ones named in the manifest and continuing with any that we find in
 * the publication directory but which are not named in the manifest.
 *
 * Dispatch to correct checking code for the object named by URI,
 * based on the filename extension in the uri.  CRLs are a special
 * case because we've already checked them by the time we get here, so
 * we just ignore them.  Other objects are either certificates or
 * CMS-signed objects of one kind or another.
 */
static void walk_cert(rcynic_ctx_t *rc, STACK_OF(walk_ctx_t) *wsk)
{
  const unsigned char *hash = NULL;
  size_t hashlen;
  walk_ctx_t *w;
  uri_t uri;

  assert(rc && wsk);

  while ((w = walk_ctx_stack_head(wsk)) != NULL) {

    switch (w->state) {

    case walk_state_initial:

      if (!w->certinfo.sia.s[0] || !w->certinfo.ca) {
	w->state = walk_state_done;
	continue;
      }
      
      if (!w->certinfo.manifest.s[0]) {
	logmsg(rc, log_data_err, "Issuer's certificate does not specify a manifest, skipping collection");
	w->state = walk_state_done;
	continue;
      }

      rc->indent++;

      w->state++;
      continue;

    case walk_state_rsync:

      rsync_tree(rc, &w->certinfo.sia);
      w->state++;
      continue;
      
    case walk_state_ready:

      walk_ctx_loop_init(rc, wsk);      /* sets w->state */
      continue;

    case walk_state_current:
    case walk_state_backup:

      if (!walk_ctx_loop_this(rc, wsk, &uri, &hash, &hashlen)) {
	walk_ctx_loop_next(rc, wsk);
	continue;
      }

      if (endswith(uri.s, ".crl") || endswith(uri.s, ".mft") || endswith(uri.s, ".mnf")) {
	walk_ctx_loop_next(rc, wsk);
	continue;			/* CRLs and manifests checked elsewhere */
      }

      if (hash == NULL) {
	logmsg(rc, log_telemetry, "Object %s present in publication directory but not in manifest", uri.s);
	mib_increment(rc, &uri, object_not_in_manifest);
      }

      if (hash == NULL && !rc->allow_object_not_in_manifest) {
	walk_ctx_loop_next(rc, wsk);
	continue;
      }

      if (endswith(uri.s, ".roa")) {
	check_roa(rc, &uri, wsk, hash, hashlen);
	walk_ctx_loop_next(rc, wsk);
	continue;
      }

      if (endswith(uri.s, ".gbr")) {
	check_ghostbuster(rc, &uri, wsk, hash, hashlen);
	walk_ctx_loop_next(rc, wsk);
	continue;
      }

      if (endswith(uri.s, ".cer")) {
	certinfo_t subject;
	X509 *x = check_cert(rc, &uri, wsk, &subject, hash, hashlen);
	if (!walk_ctx_stack_push(wsk, x, &subject))
	  walk_ctx_loop_next(rc, wsk);
	continue;
      }

      logmsg(rc, log_telemetry, "Don't know how to check object %s, ignoring", uri.s);
      walk_ctx_loop_next(rc, wsk);
      continue;

    case walk_state_done:

      rc->indent--;
      walk_ctx_stack_pop(wsk);	/* Resume our issuer's state */
      continue;

    }
  }
}

/**
 * Check a trust anchor.  Yes, we trust it, by definition, but it
 * still needs to conform to the certificate profile, the
 * self-signature must be correct, etcetera.
 */
static void check_ta(rcynic_ctx_t *rc, STACK_OF(walk_ctx_t) *wsk)
{
  STACK_OF(X509) *certs = walk_ctx_stack_certs(wsk);
  walk_ctx_t *w = walk_ctx_stack_head(wsk);
  int ok = 0;

  if (certs  != NULL && w != NULL)
    ok = check_x509(rc, certs, w->cert, &w->certinfo, &w->certinfo);

  sk_X509_free(certs);

  if (ok)
    walk_cert(rc, wsk);
}



/**
 * Main program.  Parse command line, read config file, iterate over
 * trust anchors found via config file and do a tree walk for each
 * trust anchor.
 */
int main(int argc, char *argv[])
{
  int opt_jitter = 0, use_syslog = 0, use_stderr = 0, syslog_facility = 0;
  int opt_syslog = 0, opt_stderr = 0, opt_level = 0, prune = 1;
  char *cfg_file = "rcynic.conf";
  char *lockfile = NULL, *xmlfile = NULL;
  int c, i, j, ret = 1, jitter = 600, lockfd = -1;
  STACK_OF(CONF_VALUE) *cfg_section = NULL;
  STACK_OF(walk_ctx_t) *wsk = NULL;
  CONF *cfg_handle = NULL;
  walk_ctx_t *w = NULL;
  time_t start = 0, finish;
  unsigned long hash;
  rcynic_ctx_t rc;
  unsigned delay;
  long eline = 0;
  BIO *bio = NULL;

  memset(&rc, 0, sizeof(rc));

  if ((rc.jane = strrchr(argv[0], '/')) == NULL)
    rc.jane = argv[0];
  else
    rc.jane++;

  rc.log_level = log_telemetry;
  rc.allow_stale_crl = 1;
  rc.allow_stale_manifest = 1;

#define QQ(x,y)   rc.priority[x] = y;
  LOG_LEVELS;
#undef QQ

  if (!set_directory(&rc, &rc.authenticated,     "rcynic-data/authenticated/") ||
      !set_directory(&rc, &rc.old_authenticated, "rcynic-data/authenticated.old/") ||
      !set_directory(&rc, &rc.unauthenticated,   "rcynic-data/unauthenticated/"))
    goto done;

  OpenSSL_add_all_algorithms();
  ERR_load_crypto_strings();

  while ((c = getopt(argc, argv, "c:l:sej:V")) > 0) {
    switch (c) {
    case 'c':
      cfg_file = optarg;
      break;
    case 'l':
      opt_level = 1;
      if (!configure_logmsg(&rc, optarg))
	goto done;
      break;
    case 's':
      use_syslog = opt_syslog = 1;
      break;
    case 'e':
      use_stderr = opt_stderr = 1;
      break;
    case 'j':
      if (!configure_integer(&rc, &jitter, optarg))
	goto done;
      opt_jitter = 1;
      break;
    case 'V':
      puts(svn_id);
      ret = 0;
      goto done;
    default:
      logmsg(&rc, log_usage_err,
	     "usage: %s [-c configfile] [-s] [-e] [-l loglevel] [-j jitter] [-V]",
	     rc.jane);
      goto done;
    }
  }

  if ((cfg_handle = NCONF_new(NULL)) == NULL) {
    logmsg(&rc, log_sys_err, "Couldn't create CONF opbject");
    goto done;
  }
  
  if (NCONF_load(cfg_handle, cfg_file, &eline) <= 0) {
    if (eline <= 0)
      logmsg(&rc, log_usage_err, "Couldn't load config file %s", cfg_file);
    else
      logmsg(&rc, log_usage_err, "Error on line %ld of config file %s", eline, cfg_file);
    goto done;
  }

  if (CONF_modules_load(cfg_handle, NULL, 0) <= 0) {
    logmsg(&rc, log_sys_err, "Couldn't configure OpenSSL");
    goto done;
  }

  if ((cfg_section = NCONF_get_section(cfg_handle, "rcynic")) == NULL) {
    logmsg(&rc, log_usage_err, "Couldn't load rcynic section from config file");
    goto done;
  }

  for (i = 0; i < sk_CONF_VALUE_num(cfg_section); i++) {
    CONF_VALUE *val = sk_CONF_VALUE_value(cfg_section, i);

    assert(val && val->name && val->value);

    if (!name_cmp(val->name, "authenticated") &&
    	!set_directory(&rc, &rc.authenticated, val->value))
      goto done;

    else if (!name_cmp(val->name, "old-authenticated") &&
	     !set_directory(&rc, &rc.old_authenticated, val->value))
      goto done;

    else if (!name_cmp(val->name, "unauthenticated") &&
	     !set_directory(&rc, &rc.unauthenticated, val->value))
      goto done;

    else if (!name_cmp(val->name, "rsync-timeout") &&
	     !configure_integer(&rc, &rc.rsync_timeout, val->value))
	goto done;

    else if (!name_cmp(val->name, "rsync-program"))
      rc.rsync_program = strdup(val->value);

    else if (!name_cmp(val->name, "lockfile"))
      lockfile = strdup(val->value);

    else if (!opt_jitter &&
	     !name_cmp(val->name, "jitter") &&
	     !configure_integer(&rc, &jitter, val->value))
      goto done;

    else if (!opt_level &&
	     !name_cmp(val->name, "log-level") &&
	     !configure_logmsg(&rc, val->value))
      goto done;

    else if (!opt_syslog &&
	     !name_cmp(val->name, "use-syslog") &&
	     !configure_boolean(&rc, &use_syslog, val->value))
      goto done;

    else if (!opt_stderr &&
	     !name_cmp(val->name, "use-stderr") &&
	     !configure_boolean(&rc, &use_stderr, val->value))
      goto done;

    else if (!name_cmp(val->name, "syslog-facility") &&
	     !configure_syslog(&rc, &syslog_facility,
			       facilitynames, val->value))
      goto done;

    else if (!name_cmp(val->name, "xml-summary"))
      xmlfile = strdup(val->value);

    else if (!name_cmp(val->name, "allow-stale-crl") &&
	     !configure_boolean(&rc, &rc.allow_stale_crl, val->value))
      goto done;

    else if (!name_cmp(val->name, "allow-stale-manifest") &&
	     !configure_boolean(&rc, &rc.allow_stale_manifest, val->value))
      goto done;

    else if (!name_cmp(val->name, "allow-non-self-signed-trust-anchor") &&
	     !configure_boolean(&rc, &rc.allow_non_self_signed_trust_anchor, val->value))
      goto done;

    else if (!name_cmp(val->name, "require-crl-in-manifest") &&
	     !configure_boolean(&rc, &rc.require_crl_in_manifest, val->value))
      goto done;

    else if (!name_cmp(val->name, "allow-object-not-in-manifest") &&
	     !configure_boolean(&rc, &rc.allow_object_not_in_manifest, val->value))
      goto done;

    else if (!name_cmp(val->name, "use-links") &&
	     !configure_boolean(&rc, &rc.use_links, val->value))
      goto done;

    else if (!name_cmp(val->name, "prune") &&
	     !configure_boolean(&rc, &prune, val->value))
      goto done;

    /*
     * Ugly, but the easiest way to handle all these strings.
     */

#define	QQ(x,y)							\
    else if (!name_cmp(val->name, "syslog-priority-" #x) &&	\
	     !configure_syslog(&rc, &rc.priority[x],		\
			       prioritynames, val->value))	\
      goto done;

    LOG_LEVELS;			/* the semicolon is for emacs */

#undef QQ

  }

  if ((rc.rsync_cache = sk_OPENSSL_STRING_new(uri_cmp)) == NULL) {
    logmsg(&rc, log_sys_err, "Couldn't allocate rsync_cache stack");
    goto done;
  }

  if ((rc.backup_cache = sk_OPENSSL_STRING_new(uri_cmp)) == NULL) {
    logmsg(&rc, log_sys_err, "Couldn't allocate backup_cache stack");
    goto done;
  }

  if ((rc.stale_cache = sk_OPENSSL_STRING_new(uri_cmp)) == NULL) {
    logmsg(&rc, log_sys_err, "Couldn't allocate stale_cache stack");
    goto done;
  }

  if (xmlfile != NULL) {
    if ((rc.host_counters = sk_HOST_MIB_COUNTER_new(host_mib_counter_cmp)) == NULL) {
      logmsg(&rc, log_sys_err, "Couldn't allocate host_counters stack");
      goto done;
    }
    if ((rc.validation_status = sk_VALIDATION_STATUS_new_null()) == NULL) {
      logmsg(&rc, log_sys_err, "Couldn't allocate validation_status stack");
      goto done;
    }
  }

  if ((rc.x509_store = X509_STORE_new()) == NULL) {
    logmsg(&rc, log_sys_err, "Couldn't allocate X509_STORE");
    goto done;
  }

  rc.use_syslog = use_syslog;

  if (use_syslog)
    openlog(rc.jane,
	    LOG_PID | (use_stderr ? LOG_PERROR : 0),
	    (syslog_facility ? syslog_facility : LOG_LOCAL0));

  if (jitter > 0) {
    if (RAND_bytes((unsigned char *) &delay, sizeof(delay)) <= 0) {
      logmsg(&rc, log_sys_err, "Couldn't read random bytes");
      goto done;
    }
    delay %= jitter;
    logmsg(&rc, log_telemetry, "Delaying %u seconds before startup", delay);
    while (delay > 0)
      delay = sleep(delay);      
  }

  if (lockfile &&
      ((lockfd = open(lockfile, O_RDWR|O_CREAT|O_NONBLOCK, 0666)) < 0 ||
       lockf(lockfd, F_TLOCK, 0) < 0)) {
    if (lockfd >= 0 && errno == EAGAIN)
      logmsg(&rc, log_telemetry, "Lock %s held by another process", lockfile);
    else
      logmsg(&rc, log_sys_err, "Problem locking %s: %s", lockfile, strerror(errno));
    lockfd = -1;
    goto done;
  }

  start = time(0);
  logmsg(&rc, log_telemetry, "Starting");

  if (!rm_rf(&rc.old_authenticated)) {
    logmsg(&rc, log_sys_err, "Couldn't remove %s: %s",
	   rc.old_authenticated.s, strerror(errno));
    goto done;
  }

  if (rename(rc.authenticated.s, rc.old_authenticated.s) < 0 &&
      errno != ENOENT) {
    logmsg(&rc, log_sys_err, "Couldn't rename %s to %s: %s",
	   rc.old_authenticated.s, rc.authenticated.s, strerror(errno));
    goto done;
  }

  if (!access(rc.authenticated.s, F_OK) || !mkdir_maybe(&rc, &rc.authenticated)) {
    logmsg(&rc, log_sys_err, "Couldn't prepare directory %s: %s",
	   rc.authenticated.s, strerror(errno));
    goto done;
  }

  for (i = 0; i < sk_CONF_VALUE_num(cfg_section); i++) {
    CONF_VALUE *val = sk_CONF_VALUE_value(cfg_section, i);
    path_t path1, path2;
    certinfo_t ta_certinfo;
    uri_t uri;
    X509 *x = NULL;

    assert(val && val->name && val->value);

    if (!name_cmp(val->name, "trust-anchor")) {
      /*
       * Old local file trust anchor method.
       */
      logmsg(&rc, log_telemetry, "Processing trust anchor from local file %s", val->value);
      if (strlen(val->value) >= sizeof(path1.s)) {
	logmsg(&rc, log_usage_err, "Trust anchor path name too long %s", val->value);
	goto done;
      }
      strcpy(path1.s, val->value);
      if ((x = read_cert(&path1, NULL)) == NULL) {
	logmsg(&rc, log_usage_err, "Couldn't read trust anchor %s", path1.s);
	goto done;
      }
      hash = X509_subject_name_hash(x);
      for (j = 0; j < INT_MAX; j++) {
	if (snprintf(path2.s, sizeof(path2.s), "%s%lx.%d.cer",
		     rc.authenticated.s, hash, j) == sizeof(path2.s)) {
	  logmsg(&rc, log_sys_err,
		 "Couldn't construct path name for trust anchor %s", path1.s);
	  goto done;
	}
	if (access(path2.s, F_OK))
	  break;
      }
      if (j == INT_MAX) {
	logmsg(&rc, log_sys_err, "Couldn't find a free name for trust anchor %s", path1.s);
	goto done;
      }
      uri.s[0] = '\0';
    }

    if (!name_cmp(val->name, "trust-anchor-uri-with-key") ||
	!name_cmp(val->name, "indirect-trust-anchor") ||
	!name_cmp(val->name, "trust-anchor-locator")) {
      /*
       * Newfangled URI + public key method.  Two different versions
       * of essentially the same mechanism.
       *
       * NB: EVP_PKEY_cmp() returns 1 for success, not 0 like every
       *     other xyz_cmp() function in the entire OpenSSL library.
       *     Go figure.
       */
      int unified = (!name_cmp(val->name, "indirect-trust-anchor") ||
		     !name_cmp(val->name, "trust-anchor-locator"));
      EVP_PKEY *pkey = NULL, *xpkey = NULL;
      char *fn;
      if (unified) {
	fn = val->value;
	bio = BIO_new_file(fn, "r");
	if (!bio || BIO_gets(bio, uri.s, sizeof(uri.s)) <= 0) {
	  logmsg(&rc, log_usage_err, "Couldn't read trust anchor URI from %s", fn);
	  goto done;
	}
	uri.s[strcspn(uri.s, " \t\r\n")] = '\0';
	bio = BIO_push(BIO_new(BIO_f_base64()), bio);
      } else {
	j = strcspn(val->value, " \t");
	if (j >= sizeof(uri.s)) {
	  logmsg(&rc, log_usage_err, "Trust anchor URI too long %s", val->value);
	  goto done;
	}
	memcpy(uri.s, val->value, j);
	uri.s[j] = '\0';
	j += strspn(val->value + j, " \t");
	fn = val->value + j;
	bio = BIO_new_file(fn, "rb");
      }
      if (!uri_to_filename(&rc, &uri, &path1, &rc.unauthenticated) ||
	  !uri_to_filename(&rc, &uri, &path2, &rc.authenticated)) {
	logmsg(&rc, log_usage_err, "Couldn't convert trust anchor URI %s to filename", uri.s);
	goto done;
      }
      logmsg(&rc, log_telemetry, "Processing trust anchor from URI %s", uri.s);
      if (!rsync_file(&rc, &uri)) {
	logmsg(&rc, log_data_err, "Could not fetch trust anchor from %s", uri.s);
	continue;
      }
      if (bio)
	pkey = d2i_PUBKEY_bio(bio, NULL);
      BIO_free_all(bio);
      bio = NULL;
      if (!pkey) {
	logmsg(&rc, log_usage_err, "Couldn't read trust anchor public key for %s from %s", uri.s, fn);
	goto done;
      }
      if ((x = read_cert(&path1, NULL)) == NULL)
	logmsg(&rc, log_data_err, "Couldn't read trust anchor %s", path1.s);
      if (x && (xpkey = X509_get_pubkey(x)) == NULL)
	logmsg(&rc, log_data_err, "Rejected %s because couldn't read public key from trust anchor locator", uri.s);
      j = (xpkey && EVP_PKEY_cmp(pkey, xpkey) == 1);
      EVP_PKEY_free(pkey);
      EVP_PKEY_free(xpkey);
      if (!j) {
	logmsg(&rc, log_data_err, "Rejected %s because known public key didn't match trust anchor locator", uri.s);
	X509_free(x);
	continue;
      }
    }

    if (!x)
      continue;

    logmsg(&rc, log_telemetry, "Copying trust anchor %s to %s", path1.s, path2.s);

    if (!mkdir_maybe(&rc, &path2) ||
	!(rc.use_links ? ln(&path1, &path2) : cp(&path1, &path2))) {
      logmsg(&rc, log_sys_err, "Couldn't %s trust anchor %s",
	     (rc.use_links ? "link" : "copy"), path1.s);
      goto done;
    }

    if ((wsk = walk_ctx_stack_new()) == NULL) {
      logmsg(&rc, log_sys_err, "Couldn't allocate walk context stack");
      goto done;
    }

    parse_cert(&rc, x, &ta_certinfo, &uri);
    ta_certinfo.ta = 1;

    if ((w = walk_ctx_stack_push(wsk, x, &ta_certinfo)) == NULL) {
      logmsg(&rc, log_sys_err, "Couldn't push walk context stack");
      goto done;
    }

    check_ta(&rc, wsk);

    /*
     * Once code goes async this will have to be handled elsewhere.
     */
    walk_ctx_stack_free(wsk);
    wsk = NULL;

  }

  if (prune && !prune_unauthenticated(&rc, &rc.unauthenticated,
				      strlen(rc.unauthenticated.s))) {
    logmsg(&rc, log_sys_err, "Trouble pruning old unauthenticated data");
    goto done;
  }

  ret = 0;

 done:
  log_openssl_errors(&rc);

  if (xmlfile != NULL) {

    char tad[sizeof("2006-10-13T11:22:33Z") + 1];
    time_t tad_time = time(0);
    struct tm *tad_tm = gmtime(&tad_time);
    int ok = 1, use_stdout = !strcmp(xmlfile, "-");
    hostname_t hostname;
    FILE *f = NULL;

    strftime(tad, sizeof(tad), "%Y-%m-%dT%H:%M:%SZ", tad_tm);

    ok &= gethostname(hostname.s, sizeof(hostname.s)) == 0;

    if (use_stdout)
      f = stdout;
    else if (ok)
      ok &= (f = fopen(xmlfile, "w")) != NULL;

    if (ok)
      logmsg(&rc, log_telemetry, "Writing XML summary to %s",
	     (use_stdout ? "standard output" : xmlfile));

    if (ok)
      ok &= fprintf(f, "<?xml version=\"1.0\" ?>\n"
		    "<rcynic-summary date=\"%s\" rcynic-version=\"%s\""
		    " summary-version=\"%d\" reporting-hostname=\"%s\">\n"
		    "  <labels>\n"
		    "    <hostname>Publication Repository</hostname>\n",
		    tad, svn_id, XML_SUMMARY_VERSION, hostname.s) != EOF;

    for (j = 0; ok && j < MIB_COUNTER_T_MAX; ++j)
      ok &= fprintf(f, "    <%s kind=\"%s\">%s</%s>\n",
		    mib_counter_label[j], mib_counter_kind[j],
		    (mib_counter_desc[j]
		     ? mib_counter_desc[j]
		     : X509_verify_cert_error_string(mib_counter_openssl[j])),
		    mib_counter_label[j]) != EOF;

    if (ok)
      ok &= fprintf(f, "  </labels>\n") != EOF;

    for (i = 0; ok && i < sk_HOST_MIB_COUNTER_num(rc.host_counters); i++) {
      HOST_MIB_COUNTER *h = sk_HOST_MIB_COUNTER_value(rc.host_counters, i);
      assert(h);

      if (ok)
	ok &= fprintf(f, "  <host>\n    <hostname>%s</hostname>\n",
		      h->hostname.s) != EOF;

      for (j = 0; ok && j < MIB_COUNTER_T_MAX; ++j)
	ok &= fprintf(f, "    <%s>%lu</%s>\n", mib_counter_label[j],
		      h->counters[j], mib_counter_label[j]) != EOF;

      if (ok)
	ok &= fprintf(f, "  </host>\n") != EOF;
    }


    for (i = 0; ok && i < sk_VALIDATION_STATUS_num(rc.validation_status); i++) {
      VALIDATION_STATUS *v = sk_VALIDATION_STATUS_value(rc.validation_status, i);
      assert(v);

      tad_tm = gmtime(&v->timestamp);
      strftime(tad, sizeof(tad), "%Y-%m-%dT%H:%M:%SZ", tad_tm);

      ok &= fprintf(f, "  <validation_status timestamp=\"%s\" status=\"%s\">%s</validation_status>\n",
		    tad, mib_counter_label[v->code], v->uri.s) != EOF;
    }

    if (ok)
      ok &= fprintf(f, "</rcynic-summary>\n") != EOF;

    if (f && !use_stdout)
      ok &= fclose(f) != EOF;

    if (!ok)
      logmsg(&rc, log_sys_err, "Couldn't write XML summary to %s: %s",
	     xmlfile, strerror(errno));

  }

  /*
   * Do NOT free cfg_section, NCONF_free() takes care of that
   */
  sk_OPENSSL_STRING_pop_free(rc.rsync_cache, OPENSSL_STRING_free);
  sk_OPENSSL_STRING_pop_free(rc.backup_cache, OPENSSL_STRING_free);
  sk_OPENSSL_STRING_pop_free(rc.stale_cache, OPENSSL_STRING_free);
  sk_HOST_MIB_COUNTER_pop_free(rc.host_counters, HOST_MIB_COUNTER_free);
  sk_VALIDATION_STATUS_pop_free(rc.validation_status, VALIDATION_STATUS_free);
  X509_STORE_free(rc.x509_store);
  NCONF_free(cfg_handle);
  CONF_modules_free();
  BIO_free(bio);
  EVP_cleanup();
  ERR_free_strings();
  if (rc.rsync_program)
    free(rc.rsync_program);
  if (lockfile && lockfd >= 0)
    unlink(lockfile);
  if (lockfile)
    free(lockfile);
  if (xmlfile)
    free(xmlfile);

  if (start) {
    finish = time(0);
    logmsg(&rc, log_telemetry,
	   "Finished, elapsed time %u:%02u:%02u",
	   (unsigned) ((finish - start) / 3600),
	   (unsigned) ((finish - start) / 60 % 60),
	   (unsigned) ((finish - start) % 60));
  }

  return ret;
}
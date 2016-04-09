/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file tls/validate.c
 * @brief Expose certificate OIDs as attributes, and call validation virtual
 *	server to check cert is valid.
 *
 * @copyright 2001 hereUare Communications, Inc. <raghud@hereuare.com>
 * @copyright 2003  Alan DeKok <aland@freeradius.org>
 * @copyright 2006-2016 The FreeRADIUS server project
 */
#ifdef WITH_TLS
#define LOG_PREFIX "tls - "

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>

/** Validates a certificate using custom logic
 *
 * Before trusting a certificate, you must make sure that the certificate is
 * 'valid'. There are several steps that your application can take in determining if a
 * certificate is valid. Commonly used steps are:
 *
 *   1. Verifying the certificate's signature, and verifying that the certificate has
 *      been issued by a trusted Certificate Authority.
 *
 *   2. Verifying that the certificate is valid for the present date (i.e. it is being
 *      presented within its validity dates).
 *
 *   3. Verifying that the certificate has not been revoked by its issuing Certificate
 *      Authority, by checking with respect to a Certificate Revocation List (CRL).
 *
 *   4. Verifying that the credentials presented by the certificate fulfill additional
 *      requirements specific to the application, such as with respect to access control
 *      lists or with respect to OCSP (Online Certificate Status Processing).
 *
 * NOTE: This callback will be called multiple times based on the depth of the root
 * certificate chain
 *
 * @param ok		preverify ok.  1 if true, 0 if false.
 * @param x509_ctx	containing certs to verify.
 * @return
 *	- 0 if not valid.
 *	- 1 if valid.
 */
int tls_validate_cert_cb(int ok, X509_STORE_CTX *x509_ctx)
{
	X509		*cert;
	SSL		*ssl_session;
	tls_session_t	*tls_session;
	int		err, depth;
	fr_tls_conf_t	*conf;
	int		my_ok = ok;

	VALUE_PAIR	*cert_vps = NULL;
	vp_cursor_t	cursor;

	char		**identity;
#ifdef HAVE_OPENSSL_OCSP_H
	X509		*issuer_cert;
#endif

	char		subject[1024];
	char		common_name[1024];
	char		issuer[1024];

	REQUEST		*request;

	cert = X509_STORE_CTX_get_current_cert(x509_ctx);
	err = X509_STORE_CTX_get_error(x509_ctx);
	depth = X509_STORE_CTX_get_error_depth(x509_ctx);

	/*
	 *	Retrieve the pointer to the SSL of the connection currently treated
	 *	and the application specific data stored into the SSL object.
	 */
	ssl_session = X509_STORE_CTX_get_ex_data(x509_ctx, SSL_get_ex_data_X509_STORE_CTX_idx());
	conf = (fr_tls_conf_t *)SSL_get_ex_data(ssl_session, FR_TLS_EX_INDEX_CONF);
	rad_assert(conf != NULL);

	tls_session = SSL_get_ex_data(ssl_session, FR_TLS_EX_INDEX_TLS_SESSION);
	rad_assert(tls_session != NULL);

	request = (REQUEST *)SSL_get_ex_data(ssl_session, FR_TLS_EX_INDEX_REQUEST);
	rad_assert(request != NULL);

	identity = (char **)SSL_get_ex_data(ssl_session, FR_TLS_EX_INDEX_IDENTITY);

	/*
	 *	For this next bit, we create the attributes *only* if
	 *	we're at the client or issuing certificate, AND we
	 *	have a user identity.  i.e. we don't create the
	 *	attributes for RadSec connections.
	 */
	if (identity && (depth <= 1)) {
		fr_cursor_init(&cursor, &cert_vps);
		tls_session_pairs_from_x509_cert(&cursor, request, tls_session, cert, depth);

		/*
		 *	Add a copy of the cert_vps to session state.
		 */
		if (cert_vps) {
			/*
			 *	Print out all the pairs we have so far
			 */
			rdebug_pair_list(L_DBG_LVL_2, request, cert_vps, "&session-state:");

			/*
			 *	cert_vps have a different talloc parent, so we
			 *	can't just reference them.
			 */
			fr_pair_list_mcopy_by_num(request->state_ctx, &request->state, &cert_vps, 0, 0, TAG_ANY);
			fr_pair_list_free(&cert_vps);
		}
	}

	/*
	 *	Get the Subject & Issuer
	 */
	subject[0] = issuer[0] = '\0';
	X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
	subject[sizeof(subject) - 1] = '\0';

	X509_NAME_oneline(X509_get_issuer_name(x509_ctx->current_cert), issuer, sizeof(issuer));
	issuer[sizeof(issuer) - 1] = '\0';

	/*
	 *	Get the Common Name, if there is a subject.
	 */
	X509_NAME_get_text_by_NID(X509_get_subject_name(cert),
				  NID_commonName, common_name, sizeof(common_name));
	common_name[sizeof(common_name) - 1] = '\0';

	/*
	 *	If the CRL has expired, that might still be OK.
	 */
	if (!my_ok && (conf->allow_expired_crl) && (err == X509_V_ERR_CRL_HAS_EXPIRED)) {
		my_ok = 1;
		X509_STORE_CTX_set_error(x509_ctx, 0);
	}

	if (!my_ok) {
		char const *p = X509_verify_cert_error_string(err);
		RERROR("TLS error: %s (%i)", p, err);
		return my_ok;
	}

	switch (x509_ctx->error) {
	case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
		RERROR("issuer=%s", issuer);
		break;

	case X509_V_ERR_CERT_NOT_YET_VALID:
	case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
		RERROR("notBefore=");
#if 0
		ASN1_TIME_print(bio_err, X509_get_notBefore(x509_ctx->current_cert));
#endif
		break;

	case X509_V_ERR_CERT_HAS_EXPIRED:
	case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
		RERROR("notAfter=");
#if 0
		ASN1_TIME_print(bio_err, X509_get_notAfter(x509_ctx->current_cert));
#endif
		break;
	}

	/*
	 *	Stop checking if this is an intermediary.
	 *
	 *	Client certificates get better OCSP checks.
	 */
	if (depth > 0) {
		RDEBUG2("[verify chain] = %s", my_ok ? "ok" : "invalid");;
		return my_ok;
	}

	/*
	 *	If the conf tells us to, check cert issuer
	 *	against the specified value and fail
	 *	verification if they don't match.
	 */
	if (conf->check_cert_issuer && (strcmp(issuer, conf->check_cert_issuer) != 0)) {
		AUTH("Certificate issuer (%s) does not match specified value (%s)!",
		     issuer, conf->check_cert_issuer);
		my_ok = 0;
	}

	/*
	 *	If the conf tells us to, check the CN in the
	 *	cert against xlat'ed value, but only if the
	 *	previous checks passed.
	 */
	if (my_ok && conf->check_cert_cn) {
		char cn_str[1024];

		if (radius_xlat(cn_str, sizeof(cn_str), request, conf->check_cert_cn, NULL, NULL) < 0) {
			/* if this fails, fail the verification */
			my_ok = 0;
		} else {
			RDEBUG2("checking certificate CN (%s) with xlat'ed value (%s)", common_name, cn_str);
			if (strcmp(cn_str, common_name) != 0) {
				AUTH("Certificate CN (%s) does not match specified value (%s)!",
				     common_name, cn_str);
				my_ok = 0;
			}
		}
	} /* check_cert_cn */

	while (conf->verify_client_cert_cmd) {
		char	filename[256];
		int	fd;
		FILE	*fp;

		snprintf(filename, sizeof(filename), "%s/%s.client.XXXXXXXX",
			 conf->verify_tmp_dir, main_config.name);
		fd = mkstemp(filename);
		if (fd < 0) {
			RDEBUG("Failed creating file in %s: %s",
			       conf->verify_tmp_dir, fr_syserror(errno));
			break;
		}

		fp = fdopen(fd, "w");
		if (!fp) {
			close(fd);
			RDEBUG("Failed opening file %s: %s", filename, fr_syserror(errno));
			break;
		}

		if (!PEM_write_X509(fp, cert)) {
			fclose(fp);
			RDEBUG("Failed writing certificate to file");
			goto do_unlink;
		}
		fclose(fp);

		if (!pair_make_request("TLS-Client-Cert-Filename", filename, T_OP_SET)) {
			RDEBUG("Failed creating TLS-Client-Cert-Filename");

			goto do_unlink;
		}

		RDEBUG("Verifying client certificate: %s", conf->verify_client_cert_cmd);
		if (radius_exec_program(request, NULL, 0, NULL, request, conf->verify_client_cert_cmd,
					request->packet->vps, true, true, EXEC_TIMEOUT) != 0) {
			AUTH("Certificate CN (%s) fails external verification!", common_name);
			my_ok = 0;
		} else {
			RDEBUG("Client certificate CN %s passed external validation", common_name);
		}

	do_unlink:
		unlink(filename);
		break;
	}

#ifdef HAVE_OPENSSL_OCSP_H
	/*
	 *	Do OCSP last, so we have the complete set of attributes
	 *	available for the virtual server.
	 *
	 *	Fixme: Do we want to store the matching TLS-Client-cert-Filename?
	 */
	if (my_ok && conf->ocsp_enable){
		X509_STORE *ocsp_store = NULL;

		ocsp_store = (X509_STORE *)SSL_get_ex_data(ssl_session, FR_TLS_EX_INDEX_STORE);

		RDEBUG2("Starting OCSP Request");
		if (X509_STORE_CTX_get1_issuer(&issuer_cert, x509_ctx, cert) != 1) {
			RERROR("Couldn't get issuer_cert for %s", common_name);
		} else {
			my_ok = tls_ocsp_check(request, ocsp_store, issuer_cert, cert, conf);
		}
	}
#endif

	RDEBUG2("[verify client] = %s", my_ok ? "ok" : "invalid");
	return my_ok;
}

#endif /* WITH_TLS */

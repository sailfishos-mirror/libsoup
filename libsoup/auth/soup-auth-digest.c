/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * soup-auth-digest.c: HTTP Digest Authentication
 *
 * Copyright (C) 2001-2003, Ximian, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "auth/soup-auth-digest-private.h"
#include "soup.h"
#include "soup-message-private.h"
#include "soup-message-headers-private.h"
#include "soup-uri-utils-private.h"

#ifdef G_OS_WIN32
#include <process.h>
#endif

struct _SoupAuthDigest {
	SoupAuth parent;
};

typedef struct {
	char                    *user;
	char                     hex_urp[33];
	char                     hex_a1[33];

	/* These are provided by the server */
	char                    *nonce;
	char                    *opaque;
	SoupAuthDigestQop        qop_options;
	SoupAuthDigestAlgorithm  algorithm;
	char                    *domain;

	/* These are generated by the client */
	char                    *cnonce;
	int                      nc;
	SoupAuthDigestQop        qop;
} SoupAuthDigestPrivate;

static void recompute_hex_a1 (SoupAuthDigestPrivate *priv);

/**
 * SoupAuthDigest:
 *
 * HTTP "Digest" authentication.
 *
 * [class@Session]s support this by default; if you want to disable
 * support for it, call [method@Session.remove_feature_by_type]
 * passing %SOUP_TYPE_AUTH_DIGEST.
 *
 */

G_DEFINE_FINAL_TYPE_WITH_PRIVATE (SoupAuthDigest, soup_auth_digest, SOUP_TYPE_AUTH)

static void
soup_auth_digest_init (SoupAuthDigest *digest)
{
}

static void
soup_auth_digest_finalize (GObject *object)
{
	SoupAuthDigestPrivate *priv = soup_auth_digest_get_instance_private (SOUP_AUTH_DIGEST (object));

	g_free (priv->user);
	g_free (priv->nonce);
	g_free (priv->domain);
	g_free (priv->cnonce);
        g_free (priv->opaque);

	memset (priv->hex_urp, 0, sizeof (priv->hex_urp));
	memset (priv->hex_a1, 0, sizeof (priv->hex_a1));

	G_OBJECT_CLASS (soup_auth_digest_parent_class)->finalize (object);
}

SoupAuthDigestAlgorithm
soup_auth_digest_parse_algorithm (const char *algorithm)
{
	if (!algorithm || !g_ascii_strcasecmp (algorithm, "MD5"))
		return SOUP_AUTH_DIGEST_ALGORITHM_MD5;
	else if (!g_ascii_strcasecmp (algorithm, "MD5-sess"))
		return SOUP_AUTH_DIGEST_ALGORITHM_MD5_SESS;
	else
		return -1;
}

char *
soup_auth_digest_get_algorithm (SoupAuthDigestAlgorithm algorithm)
{
	if (algorithm == SOUP_AUTH_DIGEST_ALGORITHM_MD5)
		return g_strdup ("MD5");
	else if (algorithm == SOUP_AUTH_DIGEST_ALGORITHM_MD5_SESS)
		return g_strdup ("MD5-sess");
	else
		return NULL;
}

SoupAuthDigestQop
soup_auth_digest_parse_qop (const char *qop)
{
	GSList *qop_values, *iter;
	SoupAuthDigestQop out = 0;

	g_return_val_if_fail (qop != NULL, 0);

	qop_values = soup_header_parse_list (qop);
	for (iter = qop_values; iter; iter = iter->next) {
		if (!g_ascii_strcasecmp (iter->data, "auth"))
			out |= SOUP_AUTH_DIGEST_QOP_AUTH;
		else if (!g_ascii_strcasecmp (iter->data, "auth-int"))
			out |= SOUP_AUTH_DIGEST_QOP_AUTH_INT;
	}
	soup_header_free_list (qop_values);

	return out;
}

char *
soup_auth_digest_get_qop (SoupAuthDigestQop qop)
{
	GString *out;

	out = g_string_new (NULL);
	if (qop & SOUP_AUTH_DIGEST_QOP_AUTH)
		g_string_append (out, "auth");
	if (qop & SOUP_AUTH_DIGEST_QOP_AUTH_INT) {
		if (qop & SOUP_AUTH_DIGEST_QOP_AUTH)
			g_string_append (out, ",");
		g_string_append (out, "auth-int");
	}

	return g_string_free (out, FALSE);
}

static gboolean
soup_auth_digest_update (SoupAuth *auth, SoupMessage *msg,
			 GHashTable *auth_params)
{
	SoupAuthDigest *auth_digest = SOUP_AUTH_DIGEST (auth);
	SoupAuthDigestPrivate *priv = soup_auth_digest_get_instance_private (auth_digest);
	const char *stale, *qop;
	guint qop_options;
	gboolean ok = TRUE;

        if (!soup_auth_get_realm (auth) || !g_hash_table_lookup (auth_params, "nonce"))
                return FALSE;

	g_free (priv->domain);
	g_free (priv->nonce);
	g_free (priv->opaque);

	priv->nc = 1;

	priv->domain = g_strdup (g_hash_table_lookup (auth_params, "domain"));
	priv->nonce = g_strdup (g_hash_table_lookup (auth_params, "nonce"));
	priv->opaque = g_strdup (g_hash_table_lookup (auth_params, "opaque"));

	qop = g_hash_table_lookup (auth_params, "qop");
	if (qop) {
		qop_options = soup_auth_digest_parse_qop (qop);
		/* We only support auth */
		if (!(qop_options & SOUP_AUTH_DIGEST_QOP_AUTH))
			ok = FALSE;
		priv->qop = SOUP_AUTH_DIGEST_QOP_AUTH;
	} else
		priv->qop = 0;

	priv->algorithm = soup_auth_digest_parse_algorithm (g_hash_table_lookup (auth_params, "algorithm"));
	if (priv->algorithm == -1)
		ok = FALSE;

        if (ok) {
                stale = g_hash_table_lookup (auth_params, "stale");
                if (stale && !g_ascii_strcasecmp (stale, "TRUE") && *priv->hex_urp)
                        recompute_hex_a1 (priv);
                else {
                        g_free (priv->user);
                        priv->user = NULL;
                        g_free (priv->cnonce);
                        priv->cnonce = NULL;
                        memset (priv->hex_urp, 0, sizeof (priv->hex_urp));
                        memset (priv->hex_a1, 0, sizeof (priv->hex_a1));
                }
        }

	return ok;
}

static GSList *
soup_auth_digest_get_protection_space (SoupAuth *auth, GUri *source_uri)
{
	SoupAuthDigest *auth_digest = SOUP_AUTH_DIGEST (auth);
	SoupAuthDigestPrivate *priv = soup_auth_digest_get_instance_private (auth_digest);
	GSList *space = NULL;
	GUri *uri;
	char **dvec, *d, *dir, *slash;
	int dix;

	if (!priv->domain || !*priv->domain) {
		/* If no domain directive, the protection space is the
		 * whole server.
		 */
		return g_slist_prepend (NULL, g_strdup (""));
	}

	dvec = g_strsplit (priv->domain, " ", 0);
	for (dix = 0; dvec[dix] != NULL; dix++) {
		d = dvec[dix];
		if (*d == '/')
			dir = g_strdup (d);
		else {
			uri = g_uri_parse (d, SOUP_HTTP_URI_FLAGS, NULL);
			if (uri &&
                            g_strcmp0 (g_uri_get_scheme (uri), g_uri_get_scheme (source_uri)) == 0 &&
			    g_uri_get_port (uri) == g_uri_get_port (source_uri) &&
			    !strcmp (g_uri_get_host (uri), g_uri_get_host (source_uri)))
				dir = g_strdup (g_uri_get_path (uri));
			else
				dir = NULL;
			if (uri)
				g_uri_unref (uri);
		}

		if (dir) {
			slash = strrchr (dir, '/');
			if (slash && !slash[1])
				*slash = '\0';

			space = g_slist_prepend (space, dir);
		}
	}
	g_strfreev (dvec);

	return space;
}

void
soup_auth_digest_compute_hex_urp (const char *username,
				  const char *realm,
				  const char *password,
				  char        hex_urp[33])
{
	GChecksum *checksum;

	checksum = g_checksum_new (G_CHECKSUM_MD5);
	g_checksum_update (checksum, (guchar *)username, strlen (username));
	g_checksum_update (checksum, (guchar *)":", 1);
	g_checksum_update (checksum, (guchar *)realm, strlen (realm));
	g_checksum_update (checksum, (guchar *)":", 1);
	g_checksum_update (checksum, (guchar *)password, strlen (password));
        g_strlcpy (hex_urp, g_checksum_get_string (checksum), 33);
	g_checksum_free (checksum);
}

void
soup_auth_digest_compute_hex_a1 (const char              *hex_urp,
				 SoupAuthDigestAlgorithm  algorithm,
				 const char              *nonce,
				 const char              *cnonce,
				 char                     hex_a1[33])
{
	if (algorithm == SOUP_AUTH_DIGEST_ALGORITHM_MD5) {
		/* In MD5, A1 is just user:realm:password, so hex_A1
		 * is just hex_urp.
		 */
		/* You'd think you could say "sizeof (hex_a1)" here,
		 * but you'd be wrong.
		 */
		memcpy (hex_a1, hex_urp, 33);
	} else {
		GChecksum *checksum;

		/* In MD5-sess, A1 is hex_urp:nonce:cnonce */

                g_assert (nonce && cnonce);

		checksum = g_checksum_new (G_CHECKSUM_MD5);
		g_checksum_update (checksum, (guchar *)hex_urp, strlen (hex_urp));
		g_checksum_update (checksum, (guchar *)":", 1);
		g_checksum_update (checksum, (guchar *)nonce, strlen (nonce));
		g_checksum_update (checksum, (guchar *)":", 1);
		g_checksum_update (checksum, (guchar *)cnonce, strlen (cnonce));
                g_strlcpy (hex_a1, g_checksum_get_string (checksum), 33);
		g_checksum_free (checksum);
	}
}

static void
recompute_hex_a1 (SoupAuthDigestPrivate *priv)
{
	soup_auth_digest_compute_hex_a1 (priv->hex_urp,
					 priv->algorithm,
					 priv->nonce,
					 priv->cnonce,
					 priv->hex_a1);
}

static void
soup_auth_digest_authenticate (SoupAuth *auth, const char *username,
			       const char *password)
{
	SoupAuthDigest *auth_digest = SOUP_AUTH_DIGEST (auth);
	SoupAuthDigestPrivate *priv = soup_auth_digest_get_instance_private (auth_digest);
	char *bgen;

	g_clear_pointer (&priv->cnonce, g_free);
	g_clear_pointer (&priv->user, g_free);

	/* Create client nonce */
	bgen = g_strdup_printf ("%p:%lu:%lu",
				auth,
				(unsigned long) getpid (),
				(unsigned long) time (0));
	priv->cnonce = g_base64_encode ((guchar *)bgen, strlen (bgen));
	g_free (bgen);

	priv->user = g_strdup (username);

	/* compute "URP" (user:realm:password) */
	soup_auth_digest_compute_hex_urp (username, soup_auth_get_realm (auth),
					  password ? password : "",
					  priv->hex_urp);

	/* And compute A1 from that */
	recompute_hex_a1 (priv);
}

static gboolean
soup_auth_digest_is_authenticated (SoupAuth *auth)
{
	SoupAuthDigestPrivate *priv = soup_auth_digest_get_instance_private (SOUP_AUTH_DIGEST (auth));

	return priv->cnonce != NULL;
}

void
soup_auth_digest_compute_response (const char        *method,
				   const char        *uri,
				   const char        *hex_a1,
				   SoupAuthDigestQop  qop,
				   const char        *nonce,
				   const char        *cnonce,
				   int                nc,
				   char               response[33])
{
	char hex_a2[33];
	GChecksum *checksum;

	/* compute A2 */
	checksum = g_checksum_new (G_CHECKSUM_MD5);
	g_checksum_update (checksum, (guchar *)method, strlen (method));
	g_checksum_update (checksum, (guchar *)":", 1);
	g_checksum_update (checksum, (guchar *)uri, strlen (uri));
	memcpy (hex_a2, g_checksum_get_string (checksum), sizeof (char) * 33);
	g_checksum_free (checksum);

	/* compute KD */
	checksum = g_checksum_new (G_CHECKSUM_MD5);
	g_checksum_update (checksum, (guchar *)hex_a1, strlen (hex_a1));
	g_checksum_update (checksum, (guchar *)":", 1);
	g_checksum_update (checksum, (guchar *)nonce, strlen (nonce));
	g_checksum_update (checksum, (guchar *)":", 1);

	if (qop) {
		char tmp[9];

                g_assert (cnonce);

		g_snprintf (tmp, 9, "%.8x", nc);
		g_checksum_update (checksum, (guchar *)tmp, strlen (tmp));
		g_checksum_update (checksum, (guchar *)":", 1);
		g_checksum_update (checksum, (guchar *)cnonce, strlen (cnonce));
		g_checksum_update (checksum, (guchar *)":", 1);

		if (!(qop & SOUP_AUTH_DIGEST_QOP_AUTH))
			g_assert_not_reached ();
		g_checksum_update (checksum, (guchar *)"auth", strlen ("auth"));
		g_checksum_update (checksum, (guchar *)":", 1);
	}

	g_checksum_update (checksum, (guchar *)hex_a2, 32);
	memcpy (response, g_checksum_get_string (checksum), sizeof (char) * 33);
	g_checksum_free (checksum);
}

static void
authentication_info_cb (SoupMessage *msg, gpointer data)
{
	SoupAuth *auth = data;
	SoupAuthDigest *auth_digest = SOUP_AUTH_DIGEST (auth);
	SoupAuthDigestPrivate *priv = soup_auth_digest_get_instance_private (auth_digest);
	const char *header;
	GHashTable *auth_params;
	char *nextnonce;

	if (auth != soup_message_get_auth (msg))
		return;

	header = soup_message_headers_get_one_common (soup_message_get_response_headers (msg),
                                                      soup_auth_is_for_proxy (auth) ?
                                                      SOUP_HEADER_PROXY_AUTHENTICATION_INFO :
                                                      SOUP_HEADER_AUTHENTICATION_INFO);
	g_return_if_fail (header != NULL);

	auth_params = soup_header_parse_param_list (header);
	if (!auth_params)
		return;

	nextnonce = g_strdup (g_hash_table_lookup (auth_params, "nextnonce"));
	if (nextnonce) {
		g_free (priv->nonce);
		priv->nonce = nextnonce;
	}

	soup_header_free_param_list (auth_params);
}

static char *
soup_auth_digest_get_authorization (SoupAuth *auth, SoupMessage *msg)
{
	SoupAuthDigest *auth_digest = SOUP_AUTH_DIGEST (auth);
	SoupAuthDigestPrivate *priv = soup_auth_digest_get_instance_private (auth_digest);
	char response[33], *token;
	char *url, *algorithm;
	GString *out;
	GUri *uri;

	uri = soup_message_get_uri (msg);
	g_return_val_if_fail (uri != NULL, NULL);
	url = soup_uri_get_path_and_query (uri);

        g_assert (priv->nonce);
        g_assert (!priv->qop || priv->cnonce);

	soup_auth_digest_compute_response (soup_message_get_method (msg), url, priv->hex_a1,
					   priv->qop, priv->nonce,
					   priv->cnonce, priv->nc,
					   response);

	out = g_string_new ("Digest ");

	soup_header_g_string_append_param_quoted (out, "username", priv->user);
	g_string_append (out, ", ");
	soup_header_g_string_append_param_quoted (out, "realm", soup_auth_get_realm (auth));
	g_string_append (out, ", ");
	soup_header_g_string_append_param_quoted (out, "nonce", priv->nonce);
	g_string_append (out, ", ");
	soup_header_g_string_append_param_quoted (out, "uri", url);
	g_string_append (out, ", ");
	algorithm = soup_auth_digest_get_algorithm (priv->algorithm);
	g_string_append_printf (out, "algorithm=%s", algorithm);
	g_free (algorithm);
	g_string_append (out, ", ");
	soup_header_g_string_append_param_quoted (out, "response", response);

	if (priv->opaque) {
		g_string_append (out, ", ");
		soup_header_g_string_append_param_quoted (out, "opaque", priv->opaque);
	}

	if (priv->qop) {
		char *qop = soup_auth_digest_get_qop (priv->qop);

		g_string_append (out, ", ");
		soup_header_g_string_append_param_quoted (out, "cnonce", priv->cnonce);
		g_string_append_printf (out, ", nc=%.8x, qop=%s",
					priv->nc, qop);
		g_free (qop);
	}

	g_free (url);

	priv->nc++;

	token = g_string_free (out, FALSE);

	soup_message_add_header_handler (msg,
					 "got_headers",
					 soup_auth_is_for_proxy (auth) ?
					 "Proxy-Authentication-Info" :
					 "Authentication-Info",
					 G_CALLBACK (authentication_info_cb),
					 auth);
	return token;
}

static void
soup_auth_digest_class_init (SoupAuthDigestClass *auth_digest_class)
{
	SoupAuthClass *auth_class = SOUP_AUTH_CLASS (auth_digest_class);
	GObjectClass *object_class = G_OBJECT_CLASS (auth_digest_class);

	auth_class->scheme_name = "Digest";
	auth_class->strength = 5;

	auth_class->get_protection_space = soup_auth_digest_get_protection_space;
	auth_class->update = soup_auth_digest_update;
	auth_class->authenticate = soup_auth_digest_authenticate;
	auth_class->is_authenticated = soup_auth_digest_is_authenticated;
	auth_class->get_authorization = soup_auth_digest_get_authorization;

	object_class->finalize = soup_auth_digest_finalize;
}

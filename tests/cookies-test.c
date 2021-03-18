/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2010 Igalia S.L.
 */

#include "test-utils.h"

static SoupServer *server;
static GUri *first_party_uri, *third_party_uri;

static void
server_callback (SoupServer        *server,
		 SoupServerMessage *msg,
		 const char        *path,
		 GHashTable        *query,
		 gpointer           data)
{
	SoupMessageHeaders *response_headers;
	SoupMessageHeaders *request_headers;

	response_headers = soup_server_message_get_response_headers (msg);
	request_headers = soup_server_message_get_request_headers (msg);
	if (g_str_equal (path, "/index.html")) {
		soup_message_headers_replace (response_headers,
					      "Set-Cookie",
					      "foo=bar");
	} else if (g_str_equal (path, "/foo.jpg")) {
		soup_message_headers_replace (response_headers,
					      "Set-Cookie",
					      "baz=qux");
	} else if (soup_message_headers_get_one (request_headers,
						 "Echo-Set-Cookie")) {
		soup_message_headers_replace (response_headers,
					      "Set-Cookie",
					      soup_message_headers_get_one (request_headers,
									    "Echo-Set-Cookie"));
	}

	soup_server_message_set_status (msg, SOUP_STATUS_OK, NULL);
}

typedef struct {
	SoupCookieJarAcceptPolicy policy;
	gboolean try_third_party_again;
	int n_cookies;
} CookiesForPolicy;

static const CookiesForPolicy validResults[] = {
	{ SOUP_COOKIE_JAR_ACCEPT_ALWAYS, FALSE, 2 },
	{ SOUP_COOKIE_JAR_ACCEPT_NEVER, FALSE, 0 },
	{ SOUP_COOKIE_JAR_ACCEPT_GRANDFATHERED_THIRD_PARTY, FALSE, 1 },
	{ SOUP_COOKIE_JAR_ACCEPT_NO_THIRD_PARTY, FALSE, 1 },
	{ SOUP_COOKIE_JAR_ACCEPT_GRANDFATHERED_THIRD_PARTY, TRUE, 2 }
};

static void
do_cookies_accept_policy_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	GUri *uri;
	SoupCookieJar *jar;
	GSList *l, *p;
	int i;

	session = soup_test_session_new (NULL);
	soup_session_add_feature_by_type (session, SOUP_TYPE_COOKIE_JAR);
	jar = SOUP_COOKIE_JAR (soup_session_get_feature (session, SOUP_TYPE_COOKIE_JAR));

	for (i = 0; i < G_N_ELEMENTS (validResults); i++) {
		soup_cookie_jar_set_accept_policy (jar, validResults[i].policy);

		/* We can't use two servers due to limitations in
		 * test_server, so let's swap first and third party here
		 * to simulate a cookie coming from a third party.
		 */
		uri = g_uri_parse_relative (first_party_uri, "/foo.jpg", SOUP_HTTP_URI_FLAGS, NULL);
		msg = soup_message_new_from_uri ("GET", uri);
		soup_message_set_first_party (msg, third_party_uri);
		soup_test_session_send_message (session, msg);
		g_uri_unref (uri);
		g_object_unref (msg);

		uri = g_uri_parse_relative (first_party_uri, "/index.html", SOUP_HTTP_URI_FLAGS, NULL);
		msg = soup_message_new_from_uri ("GET", uri);
		soup_message_set_first_party (msg, first_party_uri);
		soup_test_session_send_message (session, msg);
		g_uri_unref (uri);
		g_object_unref (msg);
        
		if (validResults[i].try_third_party_again) {
                        uri = g_uri_parse_relative (first_party_uri, "/foo.jpg", SOUP_HTTP_URI_FLAGS, NULL);
                        msg = soup_message_new_from_uri ("GET", uri);
			soup_message_set_first_party (msg, third_party_uri);
			soup_test_session_send_message (session, msg);
			g_uri_unref (uri);
			g_object_unref (msg);
		}

		l = soup_cookie_jar_all_cookies (jar);
		g_assert_cmpint (g_slist_length (l), ==, validResults[i].n_cookies);

		for (p = l; p; p = p->next) {
			soup_cookie_jar_delete_cookie (jar, p->data);
			soup_cookie_free (p->data);
		}

		g_slist_free (l);
	}

	soup_test_session_abort_unref (session);
}

static void
do_cookies_subdomain_policy_test (void)
{
	SoupCookieJar *jar;
	GSList *cookies;
	GUri *uri1;
	GUri *uri2;
	GUri *uri3;

	g_test_bug ("792130");

	/* Only the base domain should be considered when deciding
	 * whether a cookie is a third-party cookie.
	 */
	uri1 = g_uri_parse ("https://www.gnome.org", SOUP_HTTP_URI_FLAGS, NULL);
	uri2 = g_uri_parse ("https://foundation.gnome.org", SOUP_HTTP_URI_FLAGS, NULL);
	uri3 = g_uri_parse ("https://www.gnome.org.", SOUP_HTTP_URI_FLAGS, NULL);

	/* We can't check subdomains with a test server running on
	 * localhost, so we'll just check the cookie jar API itself.
	 */

	/* Cookie should be accepted. One cookie in the jar. */
	jar = soup_cookie_jar_new ();
	soup_cookie_jar_set_accept_policy (jar, SOUP_COOKIE_JAR_ACCEPT_NO_THIRD_PARTY);
	soup_cookie_jar_set_cookie_with_first_party (jar, uri1, uri2, "1=foo");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 1);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Cookie should be accepted. Two cookies in the jar. */
	soup_cookie_jar_set_cookie_with_first_party (jar, uri2, uri1, "2=foo");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 2);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Third-party cookie should be rejected, so there are still
	 * only two cookies in the jar.
	 */
	soup_cookie_jar_set_cookie_with_first_party (jar, third_party_uri, uri1, "3=foo");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 2);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Now, we allow the "third-party" to set one cookie as the
	 * first party. Three cookies in the jar.
	 */
	soup_cookie_jar_set_cookie_with_first_party (jar, third_party_uri, third_party_uri, "4=foo");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 3);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Third-party cookie should now be allowed by grandfathering, though
	 * it was blocked before on the same URL. Four cookies in the jar.
	 */
	soup_cookie_jar_set_accept_policy (jar, SOUP_COOKIE_JAR_ACCEPT_GRANDFATHERED_THIRD_PARTY);
	soup_cookie_jar_set_cookie_with_first_party (jar, third_party_uri, uri1, "5=foo");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 4);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Now some Domain attribute tests.*/
	soup_cookie_jar_set_accept_policy (jar, SOUP_COOKIE_JAR_ACCEPT_ALWAYS);

	/* The cookie must be rejected if the Domain is not an appropriate
	 * match for the URI. Still four cookies in the jar.
	 */
	soup_cookie_jar_set_cookie (jar, uri1, "6=foo; Domain=gitlab.gnome.org");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 4);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Now the Domain is an appropriate match. Five cookies in the jar. */
	soup_cookie_jar_set_cookie (jar, uri1, "7=foo; Domain=gnome.org");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 5);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* A leading dot in the domain property should not affect things.
	 * This cookie should be accepted. Six cookies in the jar.
	 */
	soup_cookie_jar_set_cookie (jar, uri1, "8=foo; Domain=.www.gnome.org");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 6);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* The cookie must be rejected if the Domain ends in a trailing dot
	 * but the uri doesn't.
	 */
	soup_cookie_jar_set_cookie (jar, uri1, "9=foo; Domain=www.gnome.org.");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 6);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* The cookie should be accepted if both Domain and URI end with a trailing
	 * dot and they are a match. Seven cookies in the jar.
	 */
	soup_cookie_jar_set_cookie (jar, uri3, "10=foo; Domain=gnome.org.");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 7);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* The cookie should be rejected if URI has trailing dot but Domain doesn't.
	 * Seven cookies in the jar.
	 */
	soup_cookie_jar_set_cookie (jar, uri3, "11=foo; Domain=gnome.org");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 7);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* It should not be possible to set a cookie for a TLD. Still seven
	 * cookies in the jar.
	 */
	soup_cookie_jar_set_cookie (jar, uri1, "12=foo; Domain=.org");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 7);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* It should still not be possible to set a cookie for a TLD, even if
	 * we are tricksy and have a trailing dot. Still only seven cookies.
	 */
	soup_cookie_jar_set_cookie (jar, uri3, "13=foo; Domain=.org.");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 7);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	g_uri_unref (uri1);
	g_uri_unref (uri2);
	g_uri_unref (uri3);
	g_object_unref (jar);
}

static void
do_cookies_strict_secure_test (void)
{
	SoupCookieJar *jar;
	GSList *cookies;
	GUri *insecure_uri;
	GUri *secure_uri;

	insecure_uri = g_uri_parse ("http://gnome.org", SOUP_HTTP_URI_FLAGS, NULL);
	secure_uri = g_uri_parse ("https://gnome.org", SOUP_HTTP_URI_FLAGS, NULL);
	jar = soup_cookie_jar_new ();

	/* Set a cookie from secure origin */
	soup_cookie_jar_set_cookie (jar, secure_uri, "1=foo; secure");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 1);
	g_assert_cmpstr (soup_cookie_get_value(cookies->data), ==, "foo");
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Do not allow an insecure origin to overwrite a secure cookie */
	soup_cookie_jar_set_cookie (jar, insecure_uri, "1=bar");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 1);
	g_assert_cmpstr (soup_cookie_get_value(cookies->data), ==, "foo");
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* Secure can only be set by from secure origin */
	soup_cookie_jar_set_cookie (jar, insecure_uri, "2=foo; secure");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 1);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	/* But we can make one for another path */
	soup_cookie_jar_set_cookie (jar, insecure_uri, "1=foo; path=/foo");
	cookies = soup_cookie_jar_all_cookies (jar);
	g_assert_cmpint (g_slist_length (cookies), ==, 2);
	g_slist_free_full (cookies, (GDestroyNotify)soup_cookie_free);

	g_uri_unref (insecure_uri);
	g_uri_unref (secure_uri);
	g_object_unref (jar);
}

/* FIXME: moar tests! */
static void
do_cookies_parsing_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	SoupCookieJar *jar;
	GSList *cookies, *iter;
	SoupCookie *cookie;
	gboolean got1, got2, got3;

	g_test_bug ("678753");

	session = soup_test_session_new (NULL);
	soup_session_add_feature_by_type (session, SOUP_TYPE_COOKIE_JAR);
	jar = SOUP_COOKIE_JAR (soup_session_get_feature (session, SOUP_TYPE_COOKIE_JAR));

	/* "httponly" is case-insensitive, and its value (if any) is ignored */
	msg = soup_message_new_from_uri ("GET", first_party_uri);
	soup_message_headers_append (soup_message_get_request_headers (msg), "Echo-Set-Cookie",
				     "one=1; httponly; max-age=100");
	soup_test_session_send_message (session, msg);
	g_object_unref (msg);

	msg = soup_message_new_from_uri ("GET", first_party_uri);
	soup_message_headers_append (soup_message_get_request_headers (msg), "Echo-Set-Cookie",
				     "two=2; HttpOnly; max-age=100; SameSite=Invalid");
	soup_test_session_send_message (session, msg);
	g_object_unref (msg);

	msg = soup_message_new_from_uri ("GET", first_party_uri);
	soup_message_headers_append (soup_message_get_request_headers (msg), "Echo-Set-Cookie",
				     "three=3; httpONLY=Wednesday; max-age=100; SameSite=Lax");
	soup_test_session_send_message (session, msg);
	g_object_unref (msg);

	cookies = soup_cookie_jar_get_cookie_list (jar, first_party_uri, TRUE);
	got1 = got2 = got3 = FALSE;

	for (iter = cookies; iter; iter = iter->next) {
		cookie = iter->data;

		if (!strcmp (soup_cookie_get_name (cookie), "one")) {
			got1 = TRUE;
			g_assert_true (soup_cookie_get_http_only (cookie));
			g_assert_true (soup_cookie_get_expires (cookie) != NULL);
		} else if (!strcmp (soup_cookie_get_name (cookie), "two")) {
			got2 = TRUE;
			g_assert_true (soup_cookie_get_http_only (cookie));
			g_assert_true (soup_cookie_get_expires (cookie) != NULL);
			g_assert_cmpint (soup_cookie_get_same_site_policy (cookie), ==, SOUP_SAME_SITE_POLICY_NONE);
		} else if (!strcmp (soup_cookie_get_name (cookie), "three")) {
			got3 = TRUE;
			g_assert_true (soup_cookie_get_http_only (cookie));
			g_assert_true (soup_cookie_get_expires (cookie) != NULL);
			g_assert_cmpint (soup_cookie_get_same_site_policy (cookie), ==, SOUP_SAME_SITE_POLICY_LAX);
		} else {
			soup_test_assert (FALSE, "got unexpected cookie '%s'",
					  soup_cookie_get_name (cookie));
		}

		soup_cookie_free (cookie);
	}
	g_slist_free (cookies);

	g_assert_true (got1);
	g_assert_true (got2);
	g_assert_true (got3);

	soup_test_session_abort_unref (session);
}	

static void
do_cookies_parsing_nopath_nullorigin (void)
{
	SoupCookie *cookie = soup_cookie_parse ("NAME=Value", NULL);
	g_assert_nonnull (cookie);
	g_assert_cmpstr ("/", ==, soup_cookie_get_path (cookie));
	soup_cookie_free (cookie);
}

static void
do_get_cookies_empty_host_test (void)
{
	SoupCookieJar *jar;
	GUri *uri;
	char *cookies;

	jar = soup_cookie_jar_new ();
	uri = g_uri_parse ("file:///whatever.html", SOUP_HTTP_URI_FLAGS, NULL);

	cookies = soup_cookie_jar_get_cookies (jar, uri, FALSE);

	g_assert_null (cookies);

	g_object_unref (jar);
	g_uri_unref (uri);
}

static void
send_callback (GObject *source_object,
	       GAsyncResult *res,
	       GMainLoop *loop)
{
	g_main_loop_quit (loop);
}

static void
do_remove_feature_test (void)
{
	SoupSession *session;
	SoupMessage *msg;
	GUri *uri;
	GMainLoop *loop;

	session = soup_test_session_new (NULL);
	soup_session_add_feature_by_type (session, SOUP_TYPE_COOKIE_JAR);
	uri = g_uri_parse_relative (first_party_uri, "/index.html", SOUP_HTTP_URI_FLAGS, NULL);
	msg = soup_message_new_from_uri ("GET", uri);
	soup_message_set_first_party (msg, first_party_uri);

	loop = g_main_loop_new (NULL, TRUE);
	soup_session_send_async (session, msg, G_PRIORITY_DEFAULT, NULL,
				 (GAsyncReadyCallback)send_callback, loop);
	soup_session_remove_feature_by_type (session, SOUP_TYPE_COOKIE_JAR);

	g_main_loop_run(loop);

	g_main_loop_unref (loop);
	g_object_unref (msg);
	g_uri_unref (uri);
}

int
main (int argc, char **argv)
{
	GUri *server_uri;
	int ret;

	test_init (argc, argv, NULL);

	server = soup_test_server_new (SOUP_TEST_SERVER_IN_THREAD);
	soup_server_add_handler (server, NULL, server_callback, NULL, NULL);
	server_uri = soup_test_server_get_uri (server, "http", NULL);

	first_party_uri = g_uri_build (SOUP_HTTP_URI_FLAGS, "http", NULL, "127.0.0.1",
                                       g_uri_get_port (server_uri), "/", NULL, NULL);
        third_party_uri = g_uri_build (SOUP_HTTP_URI_FLAGS, "http", NULL, "localhost",
                                       g_uri_get_port (server_uri), "/", NULL, NULL);

	g_test_add_func ("/cookies/accept-policy", do_cookies_accept_policy_test);
	g_test_add_func ("/cookies/accept-policy-subdomains", do_cookies_subdomain_policy_test);
	g_test_add_func ("/cookies/parsing", do_cookies_parsing_test);
	g_test_add_func ("/cookies/parsing/no-path-null-origin", do_cookies_parsing_nopath_nullorigin);
	g_test_add_func ("/cookies/get-cookies/empty-host", do_get_cookies_empty_host_test);
	g_test_add_func ("/cookies/remove-feature", do_remove_feature_test);
	g_test_add_func ("/cookies/secure-cookies", do_cookies_strict_secure_test);

	ret = g_test_run ();

	g_uri_unref (first_party_uri);
	g_uri_unref (third_party_uri);
	g_uri_unref (server_uri);
	soup_test_server_quit_unref (server);

	test_cleanup ();
	return ret;
}

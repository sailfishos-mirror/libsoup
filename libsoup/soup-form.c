/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*- */
/* soup-form.c : utility functions for HTML forms */

/*
 * Copyright 2008 Red Hat, Inc.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>

#include "soup-form.h"
#include "soup.h"

/**
 * SECTION:soup-form
 * @section_id: SoupForm
 * @short_description: HTML form handling
 * @see_also: #SoupMultipart
 *
 * libsoup contains several help methods for processing HTML forms as
 * defined by the [the HTML 4.01 specification](http://www.w3.org/TR/html401/interact/forms.html#h-17.13).
 **/

/**
 * SOUP_FORM_MIME_TYPE_URLENCODED:
 *
 * A macro containing the value
 * <literal>"application/x-www-form-urlencoded"</literal>; the default
 * MIME type for POSTing HTML form data.
 *
 **/

/**
 * SOUP_FORM_MIME_TYPE_MULTIPART:
 *
 * A macro containing the value
 * <literal>"multipart/form-data"</literal>; the MIME type used for
 * posting form data that contains files to be uploaded.
 *
 **/

#define XDIGIT(c) ((c) <= '9' ? (c) - '0' : ((c) & 0x4F) - 'A' + 10)
#define HEXCHAR(s) ((XDIGIT (s[1]) << 4) + XDIGIT (s[2]))

static gboolean
form_decode (char *part)
{
	unsigned char *s, *d;

	s = d = (unsigned char *)part;
	do {
		if (*s == '%') {
			if (!g_ascii_isxdigit (s[1]) ||
			    !g_ascii_isxdigit (s[2]))
				return FALSE;
			*d++ = HEXCHAR (s);
			s += 2;
		} else if (*s == '+')
			*d++ = ' ';
		else
			*d++ = *s;
	} while (*s++);

	return TRUE;
}

/**
 * soup_form_decode:
 * @encoded_form: data of type "application/x-www-form-urlencoded"
 *
 * Decodes @form, which is an urlencoded dataset as defined in the
 * HTML 4.01 spec.
 *
 * Returns: (element-type utf8 utf8) (transfer container): a hash
 * table containing the name/value pairs from @encoded_form, which you
 * can free with g_hash_table_destroy().
 **/
GHashTable *
soup_form_decode (const char *encoded_form)
{
	GHashTable *form_data_set;
	char **pairs, *eq, *name, *value;
	int i;

	form_data_set = g_hash_table_new_full (g_str_hash, g_str_equal,
					       g_free, NULL);
	pairs = g_strsplit (encoded_form, "&", -1);
	for (i = 0; pairs[i]; i++) {
		name = pairs[i];
		eq = strchr (name, '=');
		if (eq) {
			*eq = '\0';
			value = eq + 1;
		} else
			value = NULL;
		if (!value || !form_decode (name) || !form_decode (value)) {
			g_free (name);
			continue;
		}

		g_hash_table_replace (form_data_set, name, value);
	}
	g_free (pairs);

	return form_data_set;
}

/**
 * soup_form_decode_multipart:
 * @multipart: a #SoupMultipart
 * @file_control_name: (allow-none): the name of the HTML file upload control, or %NULL
 * @filename: (out) (allow-none): return location for the name of the uploaded file, or %NULL
 * @content_type: (out) (allow-none): return location for the MIME type of the uploaded file, or %NULL
 * @file: (out) (allow-none): return location for the uploaded file data, or %NULL
 *
 * Decodes the "multipart/form-data" request in @multipart; this is a
 * convenience method for the case when you have a single file upload
 * control in a form. (Or when you don't have any file upload
 * controls, but are still using "multipart/form-data" anyway.) Pass
 * the name of the file upload control in @file_control_name, and
 * soup_form_decode_multipart() will extract the uploaded file data
 * into @filename, @content_type, and @file. All of the other form
 * control data will be returned (as strings, as with
 * soup_form_decode()) in the returned #GHashTable.
 *
 * You may pass %NULL for @filename, @content_type and/or @file if you do not
 * care about those fields. soup_form_decode_multipart() may also
 * return %NULL in those fields if the client did not provide that
 * information. You must free the returned filename and content-type
 * with g_free(), and the returned file data with g_bytes_unref().
 *
 * If you have a form with more than one file upload control, you will
 * need to decode it manually, using soup_multipart_new_from_message()
 * and soup_multipart_get_part().
 *
 * Returns: (nullable) (element-type utf8 utf8) (transfer container):
 * a hash table containing the name/value pairs (other than
 * @file_control_name) from @msg, which you can free with
 * g_hash_table_destroy(). On error, it will return %NULL.
 */
GHashTable *
soup_form_decode_multipart (SoupMultipart *multipart,
			    const char    *file_control_name,
			    char         **filename,
			    char         **content_type,
			    GBytes       **file)
{
	GHashTable *form_data_set, *params;
	SoupMessageHeaders *part_headers;
	GBytes *part_body;
	char *disposition, *name;
	int i;

	g_return_val_if_fail (multipart != NULL, NULL);

	if (filename)
		*filename = NULL;
	if (content_type)
		*content_type = NULL;
	if (file)
		*file = NULL;

	form_data_set = g_hash_table_new_full (g_str_hash, g_str_equal,
					       g_free, g_free);
	for (i = 0; i < soup_multipart_get_length (multipart); i++) {
		soup_multipart_get_part (multipart, i, &part_headers, &part_body);
		if (!soup_message_headers_get_content_disposition (
			    part_headers, &disposition, &params))
			continue;
		name = g_hash_table_lookup (params, "name");
		if (g_ascii_strcasecmp (disposition, "form-data") != 0 ||
		    !name) {
			g_free (disposition);
			g_hash_table_destroy (params);
			continue;
		}

		if (file_control_name && !strcmp (name, file_control_name)) {
			if (filename)
				*filename = g_strdup (g_hash_table_lookup (params, "filename"));
			if (content_type)
				*content_type = g_strdup (soup_message_headers_get_content_type (part_headers, NULL));
			if (file)
				*file = g_bytes_ref (part_body);
		} else {
			g_hash_table_insert (form_data_set,
					     g_strdup (name),
					     g_strndup (g_bytes_get_data (part_body, NULL),
							g_bytes_get_size (part_body)));
		}

		g_free (disposition);
		g_hash_table_destroy (params);
	}

	soup_multipart_free (multipart);
	return form_data_set;
}

static void
append_form_encoded (GString *str, const char *in)
{
	const unsigned char *s = (const unsigned char *)in;

	while (*s) {
		if (*s == ' ') {
			g_string_append_c (str, '+');
			s++;
		} else if (!g_ascii_isalnum (*s) && (*s != '-') && (*s != '_')
			   && (*s != '.'))
			g_string_append_printf (str, "%%%02X", (int)*s++);
		else
			g_string_append_c (str, *s++);
	}
}

static void
encode_pair (GString *str, const char *name, const char *value)
{
	g_return_if_fail (name != NULL);
	g_return_if_fail (value != NULL);

	if (str->len)
		g_string_append_c (str, '&');
	append_form_encoded (str, name);
	g_string_append_c (str, '=');
	append_form_encoded (str, value);
}

/**
 * soup_form_encode:
 * @first_field: name of the first form field
 * @...: value of @first_field, followed by additional field names
 * and values, terminated by %NULL.
 *
 * Encodes the given field names and values into a value of type
 * "application/x-www-form-urlencoded", as defined in the HTML 4.01
 * spec.
 *
 * This method requires you to know the names of the form fields (or
 * at the very least, the total number of fields) at compile time; for
 * working with dynamic forms, use soup_form_encode_hash() or
 * soup_form_encode_datalist().
 *
 * Returns: the encoded form
 *
 * See also: soup_message_new_from_encoded_form()
 **/
char *
soup_form_encode (const char *first_field, ...)
{
	va_list args;
	char *encoded;

	va_start (args, first_field);
	encoded = soup_form_encode_valist (first_field, args);
	va_end (args);

	return encoded;
}

/**
 * soup_form_encode_hash:
 * @form_data_set: (element-type utf8 utf8): a hash table containing
 * name/value pairs (as strings)
 *
 * Encodes @form_data_set into a value of type
 * "application/x-www-form-urlencoded", as defined in the HTML 4.01
 * spec.
 *
 * Note that the HTML spec states that "The control names/values are
 * listed in the order they appear in the document." Since this method
 * takes a hash table, it cannot enforce that; if you care about the
 * ordering of the form fields, use soup_form_encode_datalist().
 *
 * Returns: the encoded form
 *
 * See also: soup_message_new_from_encoded_form()
 **/
char *
soup_form_encode_hash (GHashTable *form_data_set)
{
	GString *str = g_string_new (NULL);
	GHashTableIter iter;
	gpointer name, value;

	g_hash_table_iter_init (&iter, form_data_set);
	while (g_hash_table_iter_next (&iter, &name, &value))
		encode_pair (str, name, value);
	return g_string_free (str, FALSE);
}

static void
datalist_encode_foreach (GQuark key_id, gpointer value, gpointer str)
{
	encode_pair (str, g_quark_to_string (key_id), value);
}

/**
 * soup_form_encode_datalist:
 * @form_data_set: a datalist containing name/value pairs
 *
 * Encodes @form_data_set into a value of type
 * "application/x-www-form-urlencoded", as defined in the HTML 4.01
 * spec. Unlike soup_form_encode_hash(), this preserves the ordering
 * of the form elements, which may be required in some situations.
 *
 * Returns: the encoded form
 *
 * See also: soup_message_new_from_encoded_form()
 **/
char *
soup_form_encode_datalist (GData **form_data_set)
{
	GString *str = g_string_new (NULL);

	g_datalist_foreach (form_data_set, datalist_encode_foreach, str);
	return g_string_free (str, FALSE);
}

/**
 * soup_form_encode_valist:
 * @first_field: name of the first form field
 * @args: pointer to additional values, as in soup_form_encode()
 *
 * See soup_form_encode(). This is mostly an internal method, used by
 * various other methods such as soup_form_encode().
 *
 * Returns: the encoded form
 *
 * See also: soup_message_new_from_encoded_form()
 **/
char *
soup_form_encode_valist (const char *first_field, va_list args)
{
	GString *str = g_string_new (NULL);
	const char *name, *value;

	name = first_field;
	value = va_arg (args, const char *);
	while (name && value) {
		encode_pair (str, name, value);

		name = va_arg (args, const char *);
		if (name)
			value = va_arg (args, const char *);
	}

	return g_string_free (str, FALSE);
}

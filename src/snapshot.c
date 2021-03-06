/*
 * Copyright (c) 2020 Nils Fisher <nils_fisher@hotmail.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <unistd.h>
#include <err.h>

#include <expat.h>

#include "snapshot.h"
#include "log.h"
#include "file_util.h"
#include "fetch_util.h"

enum snapshot_scope {
	SNAPSHOT_SCOPE_NONE,
	SNAPSHOT_SCOPE_SNAPSHOT,
	SNAPSHOT_SCOPE_PUBLISH,
	SNAPSHOT_SCOPE_END
};

struct snapshot_xml {
	enum snapshot_scope	scope;
	char			*xmlns;
	int			version;
	char			*session_id;
	int			serial;
	char			*publish_uri;
	char			*publish_data;
	unsigned int		publish_data_length;
	struct notification_xml	*nxml;
};

static void
log_snapshot_xml(struct snapshot_xml *snapshot_xml)
{
	log_info("scope: %d", snapshot_xml->scope);
	log_info("xmlns: %s", snapshot_xml->xmlns ?: "NULL");
	log_info("version: %d", snapshot_xml->version);
	log_info("session_id: %s", snapshot_xml->session_id ?: "NULL");
	log_info("serial: %d", snapshot_xml->serial);
}

static void
zero_snapshot_global_data(struct snapshot_xml *snapshot_xml)
{
	snapshot_xml->scope = SNAPSHOT_SCOPE_NONE;
	snapshot_xml->xmlns = NULL;
	snapshot_xml->version = 0;
	snapshot_xml->session_id = NULL;
	snapshot_xml->serial = 0;
}

static void
zero_snapshot_publish_data(struct snapshot_xml *snapshot_xml)
{
	snapshot_xml->publish_uri = NULL;
	snapshot_xml->publish_data = NULL;
	snapshot_xml->publish_data_length = 0;
}

static void
free_snapshot_publish_data(struct snapshot_xml *snapshot_xml)
{
	free(snapshot_xml->publish_uri);
	free(snapshot_xml->publish_data);
	zero_snapshot_publish_data(snapshot_xml);
}

static void
free_snapshot_xml_data(struct xmldata *xml_data)
{
	struct snapshot_xml *snapshot_xml = xml_data->xml_data;
	XML_ParserFree(xml_data->parser);
	free(snapshot_xml->xmlns);
	free(snapshot_xml->session_id);
	zero_snapshot_global_data(snapshot_xml);
	free_snapshot_publish_data(xml_data->xml_data);
}

static void
write_snapshot_publish(struct xmldata *xml_data)
{
	struct snapshot_xml *snapshot_xml = xml_data->xml_data;
	FILE *f;
	unsigned char *data_decoded;
	int decoded_len;

	f = open_working_uri_write(snapshot_xml->publish_uri, xml_data->opts);
	if (f == NULL)
		fatal("%s - file open fail", __func__);
	/* decode b64 message */
	decoded_len = b64_decode(snapshot_xml->publish_data, &data_decoded);
	if (decoded_len > 0) {
		fwrite(data_decoded, 1, decoded_len, f);
		free(data_decoded);
	}
	fclose(f);
}

static void
start_snapshot_elem(struct xmldata *xml_data, const char **attr)
{
	XML_Parser p = xml_data->parser;
	struct snapshot_xml *snapshot_xml = xml_data->xml_data;
	int i;

	if (snapshot_xml->scope != SNAPSHOT_SCOPE_NONE) {
		PARSE_FAIL(p,
		    "parse failed - entered snapshot elem unexpectedely");
	}
	for (i = 0; attr[i]; i += 2) {
		if (strcmp("xmlns", attr[i]) == 0)
			snapshot_xml->xmlns = xstrdup(attr[i+1]);
		else if (strcmp("version", attr[i]) == 0)
			snapshot_xml->version =
			    (int)strtol(attr[i+1], NULL, BASE10);
		else if (strcmp("session_id", attr[i]) == 0)
			snapshot_xml->session_id = xstrdup(attr[i+1]);
		else if (strcmp("serial", attr[i]) == 0)
			snapshot_xml->serial =
			    (int)strtol(attr[i+1], NULL, BASE10);
		else {
			PARSE_FAIL(p,
			    "parse failed - non conforming "
			    "attribute found in snapshot elem");
		}
	}
	if (!(snapshot_xml->xmlns &&
	      snapshot_xml->version &&
	      snapshot_xml->session_id &&
	      snapshot_xml->serial)) {
		PARSE_FAIL(p,
		    "parse failed - incomplete snapshot attributes");
	}
	if (snapshot_xml->version <= 0 ||
	    snapshot_xml->version > MAX_VERSION)
		PARSE_FAIL(p, "parse failed - invalid version");
	if (strcmp(snapshot_xml->nxml->session_id,
	    snapshot_xml->session_id) != 0)
		PARSE_FAIL(p, "parse failed - session_id mismatch");
	if (snapshot_xml->nxml->serial != snapshot_xml->serial)
		PARSE_FAIL(p, "parse failed - serial mismatch");

	snapshot_xml->scope = SNAPSHOT_SCOPE_SNAPSHOT;
}

static void
end_snapshot_elem(struct xmldata *xml_data)
{
	XML_Parser p = xml_data->parser;
	struct snapshot_xml *snapshot_xml = xml_data->xml_data;

	if (snapshot_xml->scope != SNAPSHOT_SCOPE_SNAPSHOT) {
		PARSE_FAIL(p, "parse failed - exited snapshot "
		    "elem unexpectedely");
	}
	snapshot_xml->scope = SNAPSHOT_SCOPE_END;
}

static void
start_publish_elem(struct xmldata *xml_data, const char **attr)
{
	XML_Parser p = xml_data->parser;
	struct snapshot_xml *snapshot_xml = xml_data->xml_data;
	int i;

	if (snapshot_xml->scope != SNAPSHOT_SCOPE_SNAPSHOT) {
		PARSE_FAIL(p, "parse failed - entered publish "
		    "elem unexpectedely");
	}
	for (i = 0; attr[i]; i += 2) {
		if (strcmp("uri", attr[i]) == 0)
			snapshot_xml->publish_uri = xstrdup(attr[i+1]);
		else if (strcmp("xmlns", attr[i]) == 0);
			/* XXX should we do nothing? */
		else {
			PARSE_FAIL(p, "parse failed - non conforming"
			    " attribute found in publish elem");
		}
	}
	if (!snapshot_xml->publish_uri)
		PARSE_FAIL(p, "parse failed - incomplete publish attributes");
	snapshot_xml->scope = SNAPSHOT_SCOPE_PUBLISH;
}

static void
end_publish_elem(struct xmldata *xml_data)
{
	XML_Parser p = xml_data->parser;
	struct snapshot_xml *snapshot_xml = xml_data->xml_data;

	if (snapshot_xml->scope != SNAPSHOT_SCOPE_PUBLISH) {
		PARSE_FAIL(p, "parse failed - exited publish "
		    "elem unexpectedely");
	}
	if (!snapshot_xml->publish_uri) {
		PARSE_FAIL(p, "parse failed - no data recovered "
		    "from publish elem");
	}
	write_snapshot_publish(xml_data);
	free_snapshot_publish_data(snapshot_xml);
	snapshot_xml->scope = SNAPSHOT_SCOPE_SNAPSHOT;
}

static void
snapshot_xml_elem_start(void *data, const char *el, const char **attr)
{
	struct xmldata *xml_data = data;
	XML_Parser p = xml_data->parser;

	/*
	 * Can only enter here once as we should have no ways to get back to
	 * NONE scope
	 */
	if (strcmp("snapshot", el) == 0)
		start_snapshot_elem(data, attr);
	/*
	 * Will enter here multiple times, BUT never nested. will start
	 * collecting character data in that handler mem is cleared in end
	 * block, (TODO or on parse failure)
	 */
	else if (strcmp("publish", el) == 0)
		start_publish_elem(data, attr);
	else
		PARSE_FAIL(p, "parse failed - unexpected elem exit found");
}

static void
snapshot_xml_elem_end(void *data, const char *el)
{
	struct xmldata *xml_data = data;
	XML_Parser p = xml_data->parser;

	if (strcmp("snapshot", el) == 0)
		end_snapshot_elem(data);
	else if (strcmp("publish", el) == 0)
		end_publish_elem(data);
	else
		PARSE_FAIL(p, "parse failed - unexpected elem exit found");
}

static void
snapshot_content_handler(void *data, const char *content, int length)
{
	int new_length;
	struct xmldata *xml_data = data;
	struct snapshot_xml *snapshot_xml = xml_data->xml_data;

	if (snapshot_xml->scope == SNAPSHOT_SCOPE_PUBLISH) {
		/*
		 * optmisiation, this often gets called with '\n' as the
		 * only data... seems wasteful
		 */
		if (length == 1 && content[0] == '\n')
			return;

		/* append content to publish_data */
		new_length = snapshot_xml->publish_data_length + length;
		snapshot_xml->publish_data = realloc(snapshot_xml->publish_data,
		    new_length + 1);
		if (snapshot_xml->publish_data == NULL)
			fatal("%s - realloc", __func__);

		memcpy(snapshot_xml->publish_data +
		    snapshot_xml->publish_data_length, content, length);
		snapshot_xml->publish_data[new_length] = '\0';
		snapshot_xml->publish_data_length = new_length;
	}
}

static void
setup_xml_data(struct xmldata *xml_data, struct snapshot_xml *snapshot_xml,
    char *uri, char *hash, struct opts *opts, struct notification_xml *nxml)
{
	xml_data->uri = uri;
	xml_data->opts = opts;
	xml_data->hash = hash;
	/* snapshot doesn't use modified since */
	xml_data->modified_since[0] = '\0';

	xml_data->parser = XML_ParserCreate(NULL);
	if (xml_data->parser == NULL)
		fatalx("%s - XML_ParserCreate", __func__);
	XML_SetElementHandler(xml_data->parser, snapshot_xml_elem_start,
	    snapshot_xml_elem_end);
	XML_SetCharacterDataHandler(xml_data->parser, snapshot_content_handler);
	XML_SetUserData(xml_data->parser, xml_data);

	xml_data->xml_data = snapshot_xml;
	zero_snapshot_global_data(snapshot_xml);
	zero_snapshot_publish_data(snapshot_xml);
	snapshot_xml->nxml = nxml;
}

int
fetch_snapshot_xml(char *uri, char *hash, struct opts *opts,
    struct notification_xml* nxml)
{
	struct xmldata xml_data;
	struct snapshot_xml snapshot_xml;
	int ret = 0;
	setup_xml_data(&xml_data, &snapshot_xml, uri, hash, opts, nxml);
	if (fetch_xml_uri(&xml_data) != 200)
		ret = 1;
	free_snapshot_xml_data(&xml_data);
	return ret;
}


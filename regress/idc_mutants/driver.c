/* Mutant probe driver: validates a normal document plus three probe
 * documents against a fixed key/unique/keyref schema, through both
 * xmlSchemaValidateDoc and xmlSchemaValidateStream.  Exit status 0 means:
 * normal data still validates, the probe targeted by this mutant shows
 * the expected deviation, and the other probes still fail as they should.
 */
#include <libxml/parser.h>
#include <libxml/xmlschemas.h>
#include <libxml/xmlIO.h>
#include <stdio.h>
#include <string.h>

static const char kSchema[] =
"<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'"
" xmlns:t='urn:idc:Z1383' targetNamespace='urn:idc:Z1383'"
" elementFormDefault='qualified'>"
"<xs:element name='collection'><xs:complexType><xs:sequence>"
"<xs:element name='batch' maxOccurs='unbounded'><xs:complexType>"
"<xs:choice minOccurs='0' maxOccurs='unbounded'>"
"<xs:element name='rec'><xs:complexType><xs:sequence>"
"<xs:element name='a' type='xs:integer' minOccurs='0'/>"
"<xs:element name='b' type='xs:token' minOccurs='0'/>"
"</xs:sequence></xs:complexType></xs:element>"
"<xs:element name='ref'><xs:complexType><xs:sequence>"
"<xs:element name='a' type='xs:integer' minOccurs='0'/>"
"<xs:element name='b' type='xs:token' minOccurs='0'/>"
"</xs:sequence></xs:complexType></xs:element>"
"</xs:choice></xs:complexType>"
"<xs:key name='dk'><xs:selector xpath='t:rec'/>"
"<xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:key>"
"<xs:unique name='du'><xs:selector xpath='t:rec'/>"
"<xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:unique>"
"<xs:keyref name='dkr' refer='t:dk'><xs:selector xpath='t:ref'/>"
"<xs:field xpath='t:a'/><xs:field xpath='t:b'/></xs:keyref>"
"</xs:element></xs:sequence></xs:complexType></xs:element></xs:schema>";

static const char kNormal[] =
"<collection xmlns='urn:idc:Z1383'><batch>"
"<rec><a>1</a><b>x</b></rec><rec><a>2</a><b>y</b></rec>"
"<ref><a>2</a><b>y</b></ref></batch></collection>";

/* duplicate key: (1,"k") twice via different lexical forms */
static const char kDupKey[] =
"<collection xmlns='urn:idc:Z1383'><batch>"
"<rec><a>1</a><b>k</b></rec><rec><a>+01</a><b>  k </b></rec>"
"</batch></collection>";

/* key with a missing field: unique would let it pass */
static const char kKeyMissing[] =
"<collection xmlns='urn:idc:Z1383'><batch>"
"<rec><a>1</a><b>x</b></rec><rec><a>2</a></rec>"
"<ref><a>1</a><b>x</b></ref></batch></collection>";

/* orphan keyref: (7,"q") is not in this batch's key table */
static const char kKeyrefOrphan[] =
"<collection xmlns='urn:idc:Z1383'><batch>"
"<rec><a>1</a><b>k</b></rec><ref><a>7</a><b>q</b></ref>"
"</batch></collection>";

static void quiet(void* ctx, const xmlError* e) {
    (void)ctx; (void)e;
}

static int validateOne(xmlSchemaPtr schema, const char* xml, int stream) {
    xmlSchemaValidCtxtPtr vc = xmlSchemaNewValidCtxt(schema);
    int ret;
    if (vc == NULL) return -100;
    xmlSchemaSetValidStructuredErrors(vc, quiet, NULL);
    if (!stream) {
        xmlDocPtr doc = xmlReadMemory(xml, (int)strlen(xml), "probe.xml",
                                      NULL, XML_PARSE_NONET);
        if (doc == NULL) { xmlSchemaFreeValidCtxt(vc); return -101; }
        ret = xmlSchemaValidateDoc(vc, doc);
        xmlFreeDoc(doc);
    } else {
        xmlParserInputBufferPtr in = xmlParserInputBufferCreateMem(
            xml, (int)strlen(xml), XML_CHAR_ENCODING_NONE);
        if (in == NULL) { xmlSchemaFreeValidCtxt(vc); return -102; }
        /* the input buffer is adopted by the validator */
        ret = xmlSchemaValidateStream(vc, in, XML_CHAR_ENCODING_NONE,
                                      NULL, NULL);
    }
    xmlSchemaFreeValidCtxt(vc);
    return ret;
}

int main(int argc, char** argv) {
    const char* names[4];
    const char* docs[4];
    const char* mutant;
    xmlSchemaParserCtxtPtr pc;
    xmlSchemaPtr schema;
    int target, ok = 1, i;

    names[0] = "normal";      docs[0] = kNormal;
    names[1] = "dupkey";      docs[1] = kDupKey;
    names[2] = "keymissing";  docs[2] = kKeyMissing;
    names[3] = "keyref";      docs[3] = kKeyrefOrphan;

    if (argc < 2) { fprintf(stderr, "usage: driver <mutant>\n"); return 64; }
    mutant = argv[1];
    if (!strcmp(mutant, "dupkey")) target = 1;
    else if (!strcmp(mutant, "keymissing")) target = 2;
    else if (!strcmp(mutant, "keyref")) target = 3;
    else { fprintf(stderr, "unknown mutant %s\n", mutant); return 64; }

    xmlSetStructuredErrorFunc(NULL, quiet);
    pc = xmlSchemaNewMemParserCtxt(kSchema, (int)sizeof(kSchema) - 1);
    if (pc == NULL) return 65;
    schema = xmlSchemaParse(pc);
    xmlSchemaFreeParserCtxt(pc);
    if (schema == NULL) { fprintf(stderr, "schema parse failed\n"); return 66; }

    for (i = 0; i < 4; i++) {
        int rd = validateOne(schema, docs[i], 0);
        int rs = validateOne(schema, docs[i], 1);
        int valid = (rd == 0) && (rs == 0);
        printf("PROBE %s dom=%d stream=%d\n", names[i], rd, rs);
        if (i == 0) {
            if (valid) printf("NORMAL_VALID\n");
            else { printf("NORMAL_BROKEN\n"); ok = 0; }
        } else if (i == target) {
            if (valid) printf("DEVIATION %s\n", mutant);
            else { printf("NO_DEVIATION %s\n", mutant); ok = 0; }
        } else {
            if (valid) { printf("OVERREACH %s\n", names[i]); ok = 0; }
        }
    }
    xmlSchemaFree(schema);
    return ok ? 0 : 1;
}

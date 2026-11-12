#include <libxml/parser.h>
#include <libxml/xmlschemas.h>
#include <libxml/xmlIO.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "schema_fixture.h"
int main(void) {
 const char *xml="<root><ref k='01'/><row k='1'/><row k='2'/></root>";
 xmlSchemaParserCtxtPtr pc=xmlSchemaNewMemParserCtxt(schema_text,sizeof(schema_text)-1); assert(pc);
 xmlSchemaPtr schema=xmlSchemaParse(pc); assert(schema); xmlSchemaFreeParserCtxt(pc);
 xmlSchemaValidCtxtPtr vc=xmlSchemaNewValidCtxt(schema); assert(vc);
 xmlDocPtr doc=xmlReadMemory(xml,strlen(xml),"local.xml",NULL,XML_PARSE_NONET); assert(doc);
 assert(xmlSchemaValidateDoc(vc,doc)==0); xmlFreeDoc(doc); xmlSchemaFreeValidCtxt(vc);
 vc=xmlSchemaNewValidCtxt(schema); assert(vc);
 xmlParserInputBufferPtr input=xmlParserInputBufferCreateMem(xml,strlen(xml),XML_CHAR_ENCODING_UTF8); assert(input);
 assert(xmlSchemaValidateStream(vc,input,XML_CHAR_ENCODING_UTF8,NULL,NULL)==0);
 xmlSchemaFreeValidCtxt(vc); xmlSchemaFree(schema);
 puts("BASELINE_XSD_TYPED_FORWARD_REF DOM=PASS STREAM=PASS"); return 0;
}

static const char schema_text[]=
"<xs:schema xmlns:xs='http://www.w3.org/2001/XMLSchema'>"
"<xs:element name='root'><xs:complexType><xs:choice minOccurs='0' maxOccurs='unbounded'>"
"<xs:element name='row'><xs:complexType><xs:attribute name='k' type='xs:integer'/></xs:complexType></xs:element>"
"<xs:element name='ref'><xs:complexType><xs:attribute name='k' type='xs:integer'/></xs:complexType></xs:element>"
"</xs:choice></xs:complexType>"
"<xs:key name='k'><xs:selector xpath='row'/><xs:field xpath='@k'/></xs:key>"
"<xs:keyref name='kr' refer='k'><xs:selector xpath='ref'/><xs:field xpath='@k'/></xs:keyref>"
"</xs:element></xs:schema>";

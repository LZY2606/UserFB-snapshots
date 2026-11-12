package pdf
import("bytes";"fmt";"strings";"testing")
func capturePDF(content string) []byte {
 objs:=[]string{"<< /Type /Catalog /Pages 2 0 R >>","<< /Type /Pages /Count 1 /Kids [3 0 R] >>","<< /Type /Page /Parent 2 0 R /MediaBox [0 0 100 100] /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>","<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",fmt.Sprintf("<< /Length %d >>\nstream\n%s\nendstream",len(content),content)}
 var b bytes.Buffer;b.WriteString("%PDF-1.4\n");offset:=[]int{0}
 for i,o:=range objs{offset=append(offset,b.Len());fmt.Fprintf(&b,"%d 0 obj\n%s\nendobj\n",i+1,o)}
 xref:=b.Len();fmt.Fprintf(&b,"xref\n0 %d\n0000000000 65535 f \n",len(objs)+1)
 for _,o:=range offset[1:]{fmt.Fprintf(&b,"%010d 00000 n \n",o)}
 fmt.Fprintf(&b,"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n",len(objs)+1,xref)
 return b.Bytes()
}
func TestCaptureBaseline(t *testing.T){
 data:=capturePDF("BT /F1 10 Tf 10 20 Td /Span << /ActualText (field) >> BDC (AB) Tj EMC (C) Tj ET")
 r,e:=NewReader(bytes.NewReader(data),int64(len(data)));if e!=nil{t.Fatal(e)}
 if r.NumPage()!=1{t.Fatal("page count")};c:=r.Page(1).Content();var text strings.Builder
 for _,v:=range c.Text{text.WriteString(v.S)}
 if text.String()!="ABC"{t.Fatalf("legacy glyph content=%q",text.String())}
 fmt.Println("PDF_BASELINE_OK xref-font-text-operators")
}

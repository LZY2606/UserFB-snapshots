package pagser
import("testing";"strings")
type testRow struct{Name string `pagser:"b"`;Price int `pagser:"->attr(data-price)"`}
type testPage struct{Title string `pagser:"title"`;Rows []testRow `pagser:".row"`}
func TestExtractionBaseline(t *testing.T){
 cfg:=DefaultConfig();cfg.CastError=true;p,err:=NewWithConfig(cfg);if err!=nil{t.Fatal(err)}
 html:=`<title>Catalog</title><div class="row" data-price="10"><b>A</b></div><div class="row" data-price="20"><b>B</b></div>`
 var page testPage;if err:=p.Parse(&page,html);err!=nil{t.Fatal(err)}
 if page.Title!="Catalog" || len(page.Rows)!=2 || page.Rows[1].Price!=20 || page.Rows[0].Name!="A"{t.Fatal(page)}
 var failed testPage;if err:=p.Parse(&failed,strings.Replace(html,`data-price="20"`,`data-price="bad"`,1));err==nil{t.Fatal("expected conversion error")}
 if failed.Title!="Catalog" || len(failed.Rows)!=0{t.Fatal("legacy failure behavior changed")}
 t.Log("PAGSER_EXTRACTION_BASELINE_OK")
}

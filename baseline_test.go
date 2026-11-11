package goupnp
import("context";"fmt";"net/http";"net/http/httptest";"net/url";"testing")
func TestBaseline(t *testing.T){
 srv:=httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter,r *http.Request){
  if r.URL.Path=="/device.xml"{fmt.Fprint(w,`<root xmlns="urn:schemas-upnp-org:device-1-0"><device><UDN>uuid:meter</UDN><serviceList><service><serviceType>urn:test:Meter:1</serviceType><serviceId>meter</serviceId><SCPDURL>/scpd.xml</SCPDURL><controlURL>/control</controlURL><eventSubURL>/events</eventSubURL></service></serviceList></device></root>`);return}
  fmt.Fprint(w,`<scpd xmlns="urn:schemas-upnp-org:service-1-0"><serviceStateTable><stateVariable sendEvents="yes"><name>Reading</name><dataType>ui4</dataType></stateVariable></serviceStateTable></scpd>`)
 }));defer srv.Close()
 u,_:=url.Parse(srv.URL+"/device.xml");root,err:=DeviceByURLCtx(context.Background(),u);if err!=nil{t.Fatal(err)}
 clients,err:=NewServiceClientsFromRootDevice(root,u,"urn:test:Meter:1");if err!=nil||len(clients)!=1{t.Fatalf("clients=%d err=%v",len(clients),err)}
 srvDesc:=clients[0].Service;if srvDesc.EventSubURL.URL.String()!=srv.URL+"/events"{t.Fatal(srvDesc.EventSubURL)}
 scpd,err:=srvDesc.RequestSCPDCtx(context.Background());if err!=nil||len(scpd.StateVariables)!=1||scpd.StateVariables[0].Name!="Reading"{t.Fatalf("scpd=%+v err=%v",scpd,err)}
 fmt.Println("BASELINE_OK device-service-scpd")
}

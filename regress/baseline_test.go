package regress_test
import ("context"; c "github.com/chromedp/chromedp"; "net/http"; "net/http/httptest"; "os"; "testing"; "time")
func TestSourceBindingBaseline(t *testing.T) {
	srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "text/html")
		if r.URL.Path == "/child" { w.Write([]byte(`<p>child</p>`)); return }
		w.Write([]byte(`<button onclick="window.sent=collect('record')">collect</button><iframe src="/child"></iframe>`))
	}))
	defer srv.Close()
	a, ac := c.NewExecAllocator(context.Background(), append(c.DefaultExecAllocatorOptions[:], c.ExecPath(os.Getenv("CHROMEDP_TEST_RUNNER")), c.NoSandbox)...)
	defer ac()
	ctx, cancel := c.NewContext(a)
	defer cancel()
	ctx, deadline := context.WithTimeout(ctx, 20*time.Second)
	defer deadline()
	called := make(chan context.Context, 1)
	release := make(chan struct{})
	defer close(release)
	if err := c.Do(ctx, c.ExposeFunc("collect", func(x context.Context, v string) (string, error) { called <- x; <-release; return v, nil }), c.Navigate(srv.URL), c.Click(c.CSS("button"))); err != nil {
		t.Fatal(err)
	}
	var caller context.Context
	select {
	case caller = <-called:
	case <-ctx.Done(): t.Fatal(ctx.Err())
	}
	if err := c.Do(ctx, c.Navigate(srv.URL+"/new")); err != nil {
		t.Fatal(err)
	}
	select {
	case <-caller.Done():
		t.Fatal("existing ExposeFunc callback unexpectedly ended on document navigation")
	default:
	}
	title, err := c.Run(ctx, c.Text(c.CSS("button")))
	if err != nil || title != "collect" {
		t.Fatalf("%q %v", title, err)
	}
	t.Log("SOURCE_BINDING_BASELINE: collected click callback; navigation kept callback context alive")
}

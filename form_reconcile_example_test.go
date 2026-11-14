package agouti_test

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"

	"github.com/sclevine/agouti"
)

// 录单示例：动态表单协调。
//
// 录单页面在用户选完地区、勾完服务项之后，会把表单控件整体重建一遍，
// 旧的远端元素 ID 全部失效。如果业务脚本自己缓存元素、顺着调用
// Find/Fill/Check/Select，复选组没法一次性调整成指定的集合，Clear 成功
// 之后还会继续往已经被替换掉的旧输入框里发 Value。
//
// Selection.ApplyForm 把这件事收进一个入口：业务脚本只描述每一步的目标
// 状态（字段的 name 和目标值集合），不需要自己维护任何远端元素 ID。
// 每次准备新动作时，ApplyForm 都会从原 Selection 重新解析当前的 form，
// 再在 form 内按 name 找到当前的字段组，因此表单被整体替换后会自然跟随
// 到新实例。Clear/Value/Click 共享一个动作预算，页面脚本反复回摆勾选时
// 不会死循环；返回的 FormResult 报告已确认完成的连续步数和实际发出的
// 动作数。
//
// The example below runs a minimal in-memory WebDriver that serves one
// order form with a text field "city" and a checkbox group "svc", then
// reconciles both in a single ApplyForm call.
func ExampleSelection_ApplyForm() {
	cityValue := ""
	svcSelected := map[string]bool{"express": false, "gift": false}

	respond := func(w http.ResponseWriter, value interface{}) {
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]interface{}{"value": value})
	}
	refs := func(ids ...string) []map[string]string {
		out := []map[string]string{}
		for _, id := range ids {
			out = append(out, map[string]string{"ELEMENT": id})
		}
		return out
	}
	handler := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := strings.TrimPrefix(r.URL.Path, "/session/demo")
		switch {
		case r.Method == "POST" && path == "/elements":
			respond(w, refs("form1"))
		case r.Method == "POST" && path == "/element/form1/elements":
			respond(w, refs("city1", "svc1", "svc2"))
		case r.Method == "GET" && path == "/element/form1/name":
			respond(w, "form")
		case strings.HasSuffix(path, "/attribute/name"):
			if strings.Contains(path, "city1") {
				respond(w, "city")
			} else {
				respond(w, "svc")
			}
		case strings.HasSuffix(path, "/attribute/type"):
			if strings.Contains(path, "city1") {
				respond(w, "text")
			} else {
				respond(w, "checkbox")
			}
		case strings.HasSuffix(path, "/name"):
			respond(w, "input")
		case strings.HasSuffix(path, "/attribute/value"):
			switch {
			case strings.Contains(path, "city1"):
				respond(w, cityValue)
			case strings.Contains(path, "svc1"):
				respond(w, "express")
			default:
				respond(w, "gift")
			}
		case strings.HasSuffix(path, "/enabled"):
			respond(w, true)
		case strings.HasSuffix(path, "/selected"):
			if strings.Contains(path, "svc1") {
				respond(w, svcSelected["express"])
			} else {
				respond(w, svcSelected["gift"])
			}
		case r.Method == "POST" && strings.HasSuffix(path, "/clear"):
			cityValue = ""
			respond(w, nil)
		case r.Method == "POST" && strings.HasSuffix(path, "/value"):
			var req struct{ Value []string }
			json.NewDecoder(r.Body).Decode(&req)
			cityValue = strings.Join(req.Value, "")
			respond(w, nil)
		case r.Method == "POST" && strings.HasSuffix(path, "/click"):
			if strings.Contains(path, "svc1") {
				svcSelected["express"] = !svcSelected["express"]
			} else {
				svcSelected["gift"] = !svcSelected["gift"]
			}
			respond(w, nil)
		default:
			w.WriteHeader(404)
			respond(w, map[string]string{"message": "unexpected " + r.Method + " " + path})
		}
	})
	server := httptest.NewServer(handler)
	defer server.Close()

	page := agouti.JoinPage(server.URL + "/session/demo")
	result, err := page.Find("#order").ApplyForm([]agouti.FormStep{
		{Name: "city", Values: []string{"东京"}},
		{Name: "svc", Values: []string{"express"}},
	}, 10)
	if err != nil {
		fmt.Println("error:", err)
		return
	}
	selected := []string{}
	for _, value := range []string{"express", "gift"} {
		if svcSelected[value] {
			selected = append(selected, value)
		}
	}
	fmt.Printf("completed=%d actions=%d\n", result.Completed, result.Actions)
	fmt.Printf("city=%s svc=%v\n", cityValue, selected)

	// Output:
	// completed=2 actions=3
	// city=东京 svc=[express]
}

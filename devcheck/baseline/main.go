package main
import("fmt";"reflect";"github.com/tobgu/qframe";"github.com/tobgu/qframe/config/groupby")
func main(){
 f:=qframe.New(map[string]interface{}{"g":[]string{"a","b","a","a"},"t":[]int{4,2,1,3},"v":[]int{8,5,2,7}})
 if f.Err!=nil {panic(f.Err)}
 view:=f.Filter(qframe.Filter{Column:"t",Comparator:">",Arg:1}).Sort(qframe.Order{Column:"t"})
 if view.Err!=nil {panic(view.Err)}
 if !reflect.DeepEqual(view.MustIntView("t").Slice(),[]int{2,3,4}){panic("wrong logical view")}
 grouped:=view.GroupBy(groupby.Columns("g")).Aggregate(qframe.Aggregation{Fn:"sum",Column:"v"})
 if grouped.Err!=nil || grouped.Len()!=2 {panic(fmt.Sprint(grouped.Err))}
 total:=0;for _,v:=range grouped.MustIntView("v").Slice(){total+=v};if total!=20 {panic("wrong grouped sum")}
 if !reflect.DeepEqual(f.MustIntView("t").Slice(),[]int{4,2,1,3}) {panic("input changed")}
 fmt.Println("QFRAME_BASELINE_OK visible=3 groups=2 sum=20 original-order=preserved")
}

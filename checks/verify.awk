BEGIN { need["paging"]=1;need["timing"]=1;need["streaming"]=1 }
$1=="CASE" {
  if (!($2 in need) || $4 !~ /^assertions=[1-9][0-9]*$/ || $5!="done=1") bad=1;
  c[$2]++; split($4,a,"="); s[$2]+=a[2]; total++; next
}
$1=="GROUP" {
  if (!($2 in need) || seen[$2]++ || $3!="cases=" c[$2] || $4!="assertions=" s[$2] || $5!="done=1" || c[$2]<1 || s[$2]<1) bad=1;
  groups++; next
}
$1=="COMPLETE" {
  if (finish++ || groups!=3 || $2!="groups=3" || $3!="cases=" total || $4!="assertions=" (s["paging"]+s["timing"]+s["streaming"]) || $5!="done=1") bad=1;
  next
}
END { for (g in need) if (seen[g]!=1) bad=1; if (bad || finish!=1) exit 1 }

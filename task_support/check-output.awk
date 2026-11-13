BEGIN { required["envelope"]=1; required["intervals"]=1; required["traversal"]=1; required["render"]=1 }
/^CASE / {
  if(NF!=4 || !($2 in required) || $4 !~ /^checks=[1-9][0-9]*$/ || done[$2] || final) bad=1;
  key=$2 ":" $3; if(seen[key]++) bad=1;
  n[$2]++; split($4,a,"="); c[$2]+=a[2]; next
}
/^GROUP / {
  if(NF!=4 || !($2 in required) || done[$2]++ || final || $3 !~ /^cases=[1-9][0-9]*$/ || $4 !~ /^checks=[1-9][0-9]*$/) bad=1;
  split($3,a,"="); split($4,b,"="); if(a[2]!=n[$2] || b[2]!=c[$2]) bad=1;
  next
}
/^TARGET_COMPLETE$/ { if(final++) bad=1; for(g in required) if(done[g]!=1) bad=1; next }
END { for(g in required) if(done[g]!=1 || n[g]<1 || c[g]<1) bad=1; if(final!=1) bad=1; if(bad) {print "TARGET_OUTPUT_REJECTED" > "/dev/stderr"; exit 4} }

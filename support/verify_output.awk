BEGIN {
 n=split("camera geometry specular texture pipeline",g," "); pos=1
}
/^PASS / {
 if(done || NF!=3 || pos>n || $2!=g[pos] || $3!~/^[1-9][0-9]*$/) bad=1
 pos++; next
}
/^TARGET_COMPLETE/ {
 if(done || pos!=n+1 || NF!=1 || $1!="TARGET_COMPLETE") bad=1
 done++; next
}
END {
 if(done!=1 || pos!=n+1 || bad){print "target output incomplete or inconsistent" > "/dev/stderr";exit 1}
}

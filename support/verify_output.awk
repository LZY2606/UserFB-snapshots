# Check the fixed driver protocol from its parent, including early exit(0).
BEGIN { n=split("camera density transport compatibility",names," "); for(i=1;i<=n;i++)want[names[i]]=1 }
/^PASS / {
 if(done||NF!=3||!($2 in want)||$3!~/^[1-9][0-9]*$/||seen[$2]++)bad=1;
 next
}
/^TARGET_COMPLETE/ {
 if(done++||NF!=1||$1!="TARGET_COMPLETE")bad=1;
 for(i=1;i<=n;i++)if(seen[names[i]]!=1)bad=1;
 next
}
{ if(done && $0!="")bad=1 }
END {
 if(done!=1)bad=1;
 for(i=1;i<=n;i++)if(seen[names[i]]!=1)bad=1;
 if(bad){print "target output incomplete or inconsistent" > "/dev/stderr";exit 1}
}

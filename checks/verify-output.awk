BEGIN {split("pcm framing errors resources",required," ")}
$1=="CASE_PASS" && NF==4 && $4~/^[1-9][0-9]*$/ {n[$2]++;a[$2]+=$4}
$1=="GROUP_DONE" && NF==4 && $3~/^[1-9][0-9]*$/ && $4~/^[1-9][0-9]*$/ {done[$2]++;gn[$2]=$3;ga[$2]=$4}
$1=="IT_CONTRACT_COMPLETE" && NF==3 && $2~/^[1-9][0-9]*$/ && $3~/^[1-9][0-9]*$/ {complete++;cn=$2;ca=$3}
END {for(i=1;i<=4;i++){g=required[i];if(done[g]!=1||n[g]<1||gn[g]!=n[g]||ga[g]!=a[g])exit 1;tn+=n[g];ta+=a[g]}
if(complete!=1||cn!=tn||ca!=ta)exit 1}

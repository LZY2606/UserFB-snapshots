BEGIN {
    split("blur-reference result-branches units-and-bounds composite-order nested-reuse offscreen-return", a, " ")
    for (i in a) wanted[a[i]]=1
}
{
    if (tolower($0) ~ /skip/) bad=1
    if ($1 == "FILTER_CASE_PASS") {
        n=split($3, kv, "=")
        if (NF != 3 || !($2 in wanted) || seen[$2]++ || n != 2 || kv[1] != "assertions" || kv[2] !~ /^[1-9][0-9]*$/) bad=1
        count++
    }
    if ($1 == "FILTER_TOTAL") {
        if (NF != 2 || $2 != "6" || total++) bad=1
    }
}
END {
    if (bad || count != 6 || total != 1) exit 1
    for (name in wanted) if (seen[name] != 1) exit 1
}

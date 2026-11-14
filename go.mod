module github.com/machinebox/graphql

go 1.20

require (
 github.com/pkg/errors v0.9.1
 github.com/matryer/is v1.4.1
)

replace github.com/pkg/errors => ./third_party/pkgerrors
replace github.com/matryer/is => ./third_party/is

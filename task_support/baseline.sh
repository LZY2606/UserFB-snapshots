source "$(dirname "$0")/env.sh"
go test -count=1 -v ./task_support
go test -count=1 -v ./mkvcore -run '^(TestBlockReader|TestBlockReader_Close)$'

set -euo pipefail
GOPROXY=off GOSUMDB=off go run -mod=vendor ./checks/baseline.go
bash checks/old.sh

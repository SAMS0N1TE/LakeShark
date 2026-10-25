#!/usr/bin/env bash
set -e

RED="\033[1;31m"
GREEN="\033[1;32m"
YELLOW="\033[1;33m"
BLUE="\033[1;34m"
RESET="\033[0m"

export GCL_IGNORE_PREDEFINED_VARS=CI_REGISTRY

BASE_SHA=$(git merge-base HEAD origin/master 2>/dev/null || git rev-parse HEAD~1)

COMMON_ARGS=(
  --variable "CI_MERGE_REQUEST_DIFF_BASE_SHA=$BASE_SHA"
  --variable "CI_REGISTRY=registry.gitlab.com"
  --json-schema-validation=false
)

check_requirements() {
  for cmd in docker git gitlab-ci-local; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
      echo -e "${RED}Missing dependency: $cmd${RESET}"
      exit 1
    fi
    echo -e "${GREEN}Found: $cmd${RESET}"
  done

  if ! docker info >/dev/null 2>&1; then
    echo -e "${RED}Docker daemon is not running or permission denied${RESET}"
    exit 1
  fi
}

list_jobs() {
  gitlab-ci-local --list --json-schema-validation=false | awk 'NR>1 {print $1}'
}

run_job() {
  JOB="$1"
  echo -e "${YELLOW}Running CI job: $JOB${RESET}"
  gitlab-ci-local "$JOB" "${COMMON_ARGS[@]}"
}

cleanup_images() {
  echo -e "${BLUE}Removing libssh CI images only...${RESET}"
  docker images --format "{{.Repository}}:{{.Tag}} {{.ID}}" \
    | grep "$CI_REGISTRY/$BUILD_IMAGES_PROJECT" \
    | awk '{print $2}' \
    | xargs -r docker rmi -f
}

usage() {
  echo
  echo -e "${BLUE}Usage:${RESET}"
  echo "  $0 --list"
  echo "  $0 --run <job-name>"
  echo "  $0 --all"
  echo "  $0 --run <job-name> --clean"
  echo "  $0 --all --clean"
  echo
  exit 1
}

check_requirements

CLEAN=0
MODE=""
JOB=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --list)
      MODE="list"
      shift
      ;;
    --run)
      MODE="run"
      JOB="$2"
      shift 2
      ;;
    --all)
      MODE="all"
      shift
      ;;
    --clean)
      CLEAN=1
      shift
      ;;
    *)
      usage
      ;;
  esac
done

case "$MODE" in
  list)
    list_jobs
    ;;
  run)
    [[ -z "$JOB" ]] && usage
    run_job "$JOB"
    [[ "$CLEAN" -eq 1 ]] && cleanup_images
    ;;
  all)
    for job in $(list_jobs); do
      run_job "$job"
      [[ "$CLEAN" -eq 1 ]] && cleanup_images
    done
    ;;
  *)
    usage
    ;;
esac

echo -e "${GREEN}Done.${RESET}"

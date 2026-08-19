#!/usr/bin/env bash
set -euo pipefail

summary_file="${GITHUB_STEP_SUMMARY:?GITHUB_STEP_SUMMARY must be set}"
test_result="${TEST_RESULT:-skipped}"
build_result="${BUILD_RESULT:-skipped}"
artifact_name="${FAILED_LOG_ARTIFACT:-khttpd-test-diagnostics}"

if [[ "${test_result}" == "failure" ]]; then
  {
    echo
    echo "### Failed test details"
    echo
    if [[ -s ci-test.log ]]; then
      echo "#### Matching failure lines"
      echo
      echo '```text'
      grep -Ei 'FAIL:|FAILED|Failure|error:' ci-test.log | head -n 40 || true
      echo '```'
      echo
      echo "#### Test log tail (last 80 lines)"
      echo
      echo '```text'
      tail -n 80 ci-test.log
      echo '```'
    else
      echo "- No captured test output was found. Check the failed test step log."
    fi
    echo "- Complete logs are available in the \`${artifact_name}\` artifact."
  } >> "${summary_file}"
elif [[ "${build_result}" == "failure" ]]; then
  {
    echo
    echo "### Build failure details"
    echo
    if [[ -s ci-build.log ]]; then
      echo '```text'
      tail -n 80 ci-build.log
      echo '```'
    else
      echo "- No captured build output was found. Check the failed build step log."
    fi
    echo "- Complete logs are available in the \`${artifact_name}\` artifact."
  } >> "${summary_file}"
fi

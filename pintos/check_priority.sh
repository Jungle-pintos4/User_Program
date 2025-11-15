#!/bin/bash
# --- CONFIGURATION ---
# (이 스크립트는 /pintos/ 폴더에 있다고 가정합니다)
PROGRAM_DIR="userprog"
BUILD_DIR="userprog/build"
GREEN='\033[0;32m'
RED='\033[0;31m'
NC='\033[0m' # No Color (색상 초기화)
# (여기에 원하는 테스트만 추가/삭제하세요)
TESTS_TO_RUN=(
    # "tests/threads/alarm-single"
    # "tests/threads/alarm-multiple"
    # "tests/threads/alarm-negative"
    # "tests/threads/alarm-priority"
    # "tests/threads/alarm-simultaneous"
    # "tests/threads/alarm-zero"
    # "tests/threads/priority-change"
    # "tests/threads/priority-preempt"
    # "tests/threads/priority-fifo"
    # "tests/threads/priority-sema"
    # "tests/threads/priority-condvar"
    # "tests/threads/priority-donate-chain"
    # "tests/threads/priority-donate-lower"
    # "tests/threads/priority-donate-multiple"
    # "tests/threads/priority-donate-multiple2"
    # "tests/threads/priority-donate-nest"
    # "tests/threads/priority-donate-one"
    # "tests/threads/priority-donate-sema"
    # "tests/threads/mlfqs/mlfqs-load-1"
    # "tests/threads/mlfqs/mlfqs-block"
    # "tests/threads/mlfqs/mlfqs-fair-2"
    # "tests/threads/mlfqs/mlfqs-fair-20"
    # "tests/threads/mlfqs/mlfqs-load-60"
    # "tests/threads/mlfqs/mlfqs-load-avg"
    # "tests/threads/mlfqs/mlfqs-nice-2"
    # "tests/threads/mlfqs/mlfqs-nice-10"
    # "tests/threads/mlfqs/mlfqs-recent-1"
    "tests/userprog/args-none"
    "tests/userprog/args-single"
    "tests/userprog/args-multiple"
    "tests/userprog/args-many"
    "tests/userprog/args-dbl-space"
    "tests/userprog/halt"
    "tests/userprog/exit"
)
# --- END CONFIGURATION ---
# 0. 스크립트가 올바른 위치(pintos)에서 실행되었는지 확인
if [ ! -d "$PROGRAM_DIR" ]; then
    echo "Error: This script must be run from the 'pintos' root directory."
    echo "Failed to find directory: $PROGRAM_DIR"
    exit 1
fi
# 1. Build
echo "Moving to $PROGRAM_DIR to build..."
cd $PROGRAM_DIR
echo "--- Running make clean ---"
make clean
echo "--- Running make ---"
make
# make가 실패했는지 확인 (exit code가 0이 아닌지)
if [ $? -ne 0 ]; then
    echo "Error: 'make' failed. Exiting."
    cd .. # pintos 루트로 돌아가기
    exit 1
fi
echo "--- Build complete ---"
cd .. # pintos 루트로 돌아오기
# 2. Move to build directory
echo "Moving to $BUILD_DIR..."
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory '$BUILD_DIR' not found."
    echo "Something went wrong with the build process."
    exit 1
fi
# build 디렉토리로 이동합니다.
cd $BUILD_DIR
echo "Now in $(pwd)"
# 3. Run all specified tests
# (make 명령어는 build 디렉토리 내부에서 실행되어야 합니다)
echo "========================================"
echo "Running all specified priority tests..."
echo "(This may take a moment. Raw pintos output is suppressed.)"
echo "========================================"
for TEST_NAME in "${TESTS_TO_RUN[@]}"; do
    echo "--- Running $TEST_NAME ---"
    # stdout과 stderr를 모두 /dev/null로 리디렉션하여 숨깁니다.
    make "${TEST_NAME}.result" &> /dev/null
done
# 4. Check all results (Simplified Summary)
echo "========================================"
echo "Checking results..."
echo "========================================"
ALL_PASSED=true
for TEST_NAME in "${TESTS_TO_RUN[@]}"; do
    TEST_FILE="${TEST_NAME}.result"
    if [ -f "$TEST_FILE" ]; then
        # FAIL이 있는지 확인
        if grep -q "FAIL" "$TEST_FILE"; then
            ALL_PASSED=false
            echo -e "TEST: $TEST_FILE (${RED}FAILED${NC} :x:)"
        else
            echo -e "TEST: $TEST_FILE (${GREEN}PASSED${NC} :O:)"
        fi
    else
        # .result 파일 자체가 생성 안 된 경우 (make 오류)
        ALL_PASSED=false
        echo "TEST: $TEST_FILE (ERROR :느낌표:️ - Result file not found)"
    fi
done
echo "========================================"
if $ALL_PASSED; then
    echo "All specified tests passed! :짠:"
else
    echo "Some tests failed or failed to run."
fi
# 5. Go back to the original directory (pintos root)
cd ../..
echo "Returning to $(pwd)"


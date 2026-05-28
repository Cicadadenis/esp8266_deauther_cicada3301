#!/bin/bash

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

DURATION_FILE="./bin/.compile_duration"
PROGRESS_WIDTH=40
DEFAULT_ESTIMATE=45

draw_progress() {
    local filled=$1
    local width=$2

    echo -ne "\r${CYAN}[${RESET}"
    local i
    for ((i = 0; i < filled; i++)); do
        echo -ne "${GREEN}█${RESET}"
    done
    for ((i = filled; i < width; i++)); do
        echo -ne " "
    done
    echo -ne "${CYAN}]${RESET}"
}

progress_bar_wait() {
    local pid=$1
    local width=$PROGRESS_WIDTH
    local estimate=$DEFAULT_ESTIMATE

    if [ -f "$DURATION_FILE" ]; then
        local saved
        saved=$(cat "$DURATION_FILE" 2>/dev/null)
        if [[ "$saved" =~ ^[0-9]+$ ]] && [ "$saved" -gt 5 ]; then
            estimate=$saved
        fi
    fi

    local start
    start=$(date +%s)

    echo ""
    draw_progress 0 "$width"

    while kill -0 "$pid" 2>/dev/null; do
        local now elapsed filled
        now=$(date +%s)
        elapsed=$((now - start))

        filled=$((elapsed * width / estimate))
        if [ "$filled" -ge "$width" ]; then
            filled=$((width - 1))
        fi

        draw_progress "$filled" "$width"
        sleep 0.1
    done

    local total=$(( $(date +%s) - start ))
    if [ "$total" -lt 1 ]; then
        total=1
    fi
    mkdir -p ./bin
    echo "$total" > "$DURATION_FILE"

    draw_progress "$width" "$width"
    echo -e "\n"
}

echo -e "\n${BOLD}${YELLOW}╔══════════════════════════════════════╗"
echo -e "║        ESP8266 DEAUTHER BUILD        ║"
echo -e "╚══════════════════════════════════════╝${RESET}\n"

echo -e "${CYAN}${BOLD}⚙  Идёт сборка бинарника...${RESET}\n"

COMPILE_LOG=$(mktemp)
arduino-cli compile --fqbn deauther:esp8266:nodemcuv2 --build-path ./bin >"$COMPILE_LOG" 2>&1 &
COMPILE_PID=$!

progress_bar_wait "$COMPILE_PID"

wait "$COMPILE_PID"
EXIT_CODE=$?
ERROR=$(cat "$COMPILE_LOG")
rm -f "$COMPILE_LOG"

if [ $EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}${BOLD}╔══════════════════════════════════════╗"
    echo -e "║   ✔  Бинарник скомпилирован успешно  ║"
    echo -e "╚══════════════════════════════════════╝${RESET}\n"
else
    echo -e "${RED}${BOLD}╔══════════════════════════════════════╗"
    echo -e "║        ✘  Ошибка компиляции          ║"
    echo -e "╚══════════════════════════════════════╝${RESET}\n"
    echo -e "${RED}${BOLD}Детали ошибки:${RESET}"
    echo -e "${RED}────────────────────────────────────────"
    echo -e "$ERROR"
    echo -e "────────────────────────────────────────${RESET}\n"
fi

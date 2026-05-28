#!/bin/bash
# Подготовка окружения для сборки ESP8266 Deauther (Linux / WSL)

set -euo pipefail

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BOARD_INDEX_URL="https://raw.githubusercontent.com/SpacehuhnTech/arduino/main/package_spacehuhn_index.json"
CORE_FQBN="deauther:esp8266"
CORE_VERSION="2.7.5"
TARGET_BOARD="deauther:esp8266:nodemcuv2"
ARDUINO_CLI_INSTALL_DIR="${HOME}/.local/bin"

info()  { echo -e "${CYAN}${BOLD}$*${RESET}"; }
ok()    { echo -e "${GREEN}✔ $*${RESET}"; }
warn()  { echo -e "${YELLOW}⚠ $*${RESET}"; }
fail()  { echo -e "${RED}${BOLD}✘ $*${RESET}" >&2; exit 1; }

run_sudo() {
    if [ "$(id -u)" -eq 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo "$@"
    else
        fail "Нужны права root или sudo для установки системных пакетов."
    fi
}

detect_os() {
    if [ -f /etc/os-release ]; then
        # shellcheck disable=SC1091
        . /etc/os-release
        OS_ID="${ID:-unknown}"
        OS_LIKE="${ID_LIKE:-}"
    else
        fail "Не удалось определить дистрибутив Linux. Скрипт поддерживает Debian/Ubuntu и WSL."
    fi

    if grep -qiE 'microsoft|wsl' /proc/version 2>/dev/null; then
        IS_WSL=1
        ok "Обнаружен WSL"
    else
        IS_WSL=0
        ok "Обнаружен Linux"
    fi
}

install_system_packages() {
    info "Установка системных пакетов..."

    case "$OS_ID" in
        ubuntu|debian|linuxmint|pop)
            run_sudo apt-get update -qq
            run_sudo apt-get install -y \
                curl \
                ca-certificates \
                python3 \
                python3-pip \
                python3-venv \
                unzip \
                build-essential \
                git
            ;;
        fedora|rhel|centos)
            run_sudo dnf install -y \
                curl \
                ca-certificates \
                python3 \
                python3-pip \
                unzip \
                gcc \
                gcc-c++ \
                make \
                git
            ;;
        arch|manjaro)
            run_sudo pacman -Sy --noconfirm \
                curl \
                ca-certificates \
                python \
                python-pip \
                unzip \
                base-devel \
                git
            ;;
        *)
            if echo "$OS_LIKE" | grep -qE 'debian|ubuntu'; then
                run_sudo apt-get update -qq
                run_sudo apt-get install -y \
                    curl ca-certificates python3 python3-pip python3-venv \
                    unzip build-essential git
            else
                warn "Дистрибутив '$OS_ID' не распознан. Убедитесь, что установлены: curl, python3, pip, unzip, build-essential."
            fi
            ;;
    esac

    ok "Системные пакеты установлены"
}

install_arduino_cli() {
    if command -v arduino-cli >/dev/null 2>&1; then
        ok "arduino-cli уже установлен: $(command -v arduino-cli) ($(arduino-cli version 2>/dev/null | head -1))"
        return
    fi

    info "Установка arduino-cli..."

    mkdir -p "$ARDUINO_CLI_INSTALL_DIR"
    curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR="$ARDUINO_CLI_INSTALL_DIR" sh

    export PATH="$ARDUINO_CLI_INSTALL_DIR:$PATH"

    if ! command -v arduino-cli >/dev/null 2>&1; then
        fail "arduino-cli не найден после установки. Добавьте в PATH: export PATH=\"$ARDUINO_CLI_INSTALL_DIR:\$PATH\""
    fi

    ok "arduino-cli установлен: $(arduino-cli version | head -1)"
}

ensure_arduino_cli_in_path() {
    if ! command -v arduino-cli >/dev/null 2>&1 && [ -x "$ARDUINO_CLI_INSTALL_DIR/arduino-cli" ]; then
        export PATH="$ARDUINO_CLI_INSTALL_DIR:$PATH"
    fi

    command -v arduino-cli >/dev/null 2>&1 || fail "arduino-cli не найден. Перезапустите shell или добавьте его в PATH."
}

configure_arduino_cli() {
    info "Настройка arduino-cli..."

    # arduino-cli stores config in different locations depending on install method
    # (classic install, snap, etc.). We should not fail if a config already exists.
    local cfg=""
    if [ -n "${ARDUINO_CLI_CONFIG_FILE:-}" ]; then
        cfg="$ARDUINO_CLI_CONFIG_FILE"
    elif [ -f "${HOME}/.arduino15/arduino-cli.yaml" ]; then
        cfg="${HOME}/.arduino15/arduino-cli.yaml"
    elif [ -f "${HOME}/.config/arduino-cli.yaml" ]; then
        cfg="${HOME}/.config/arduino-cli.yaml"
    fi

    if [ -z "$cfg" ]; then
        # No known config file found; initialize one.
        # If arduino-cli still reports an existing config (race/other default path),
        # don't treat it as fatal.
        arduino-cli config init >/dev/null 2>&1 || true
    else
        ok "Config file already exists: ${cfg}"
    fi

    if ! arduino-cli config dump | grep -Fq "$BOARD_INDEX_URL"; then
        arduino-cli config add board_manager.additional_urls "$BOARD_INDEX_URL"
        ok "Добавлен board manager URL Spacehuhn"
    else
        ok "Board manager URL Spacehuhn уже настроен"
    fi
}

install_deauther_core() {
    info "Обновление индекса плат..."
    arduino-cli core update-index

    if arduino-cli core list | awk '{print $1}' | grep -qx "${CORE_FQBN}@${CORE_VERSION}"; then
        ok "Core ${CORE_FQBN}@${CORE_VERSION} уже установлен"
    elif arduino-cli core list | awk '{print $1}' | grep -qx "${CORE_FQBN}"; then
        warn "Установлена другая версия ${CORE_FQBN}, обновляю до ${CORE_VERSION}..."
        arduino-cli core install "${CORE_FQBN}@${CORE_VERSION}"
        ok "Core ${CORE_FQBN}@${CORE_VERSION} установлен"
    else
        info "Установка ${CORE_FQBN}@${CORE_VERSION} (может занять несколько минут)..."
        arduino-cli core install "${CORE_FQBN}@${CORE_VERSION}"
        ok "Core ${CORE_FQBN}@${CORE_VERSION} установлен"
    fi
}

verify_target_board() {
    info "Проверка платы ${TARGET_BOARD}..."

    if arduino-cli board listall "$CORE_FQBN" | awk '{print $NF}' | grep -qx "$TARGET_BOARD"; then
        ok "Плата ${TARGET_BOARD} доступна"
    else
        warn "Плата ${TARGET_BOARD} не найдена в списке. Доступные платы deauther:"
        arduino-cli board listall "$CORE_FQBN" || true
        fail "Целевая плата ${TARGET_BOARD} недоступна."
    fi
}

install_python_deps() {
    info "Установка Python-зависимостей..."

    if python3 -c "import rcssmin; import minify_html" 2>/dev/null; then
        ok "Пакеты rcssmin и minify-html уже установлены"
        return
    fi

    if python3 -m pip install rcssmin minify-html 2>/dev/null; then
        ok "Установлены rcssmin и minify-html (webConverter.py)"
        return
    fi

    if python3 -m pip install rcssmin minify-html --break-system-packages 2>/dev/null; then
        ok "Установлены rcssmin и minify-html (webConverter.py)"
        return
    fi

    warn "Не удалось установить зависимости автоматически."
    warn "Для регенерации web UI выполните вручную: pip install rcssmin minify-html --break-system-packages"
}

print_summary() {
    echo ""
    echo -e "${BOLD}${GREEN}╔══════════════════════════════════════╗"
    echo -e "║   Окружение готово к компиляции      ║"
    echo -e "╚══════════════════════════════════════╝${RESET}"
    echo ""
    echo -e "  Core:   ${CYAN}${CORE_FQBN}@${CORE_VERSION}${RESET}"
    echo -e "  Плата:  ${CYAN}${TARGET_BOARD}${RESET}"
    echo ""
    echo -e "  Сборка:"
    echo -e "    ${BOLD}cd \"${SCRIPT_DIR}\" && bash install.sh${RESET}"
    echo ""
    echo -e "  Регенерация web UI (опционально):"
    echo -e "    ${BOLD}python3 ../utils/web_converter/webConverter.py --repopath \"$(cd "${SCRIPT_DIR}/.." && pwd)\"${RESET}"
    echo ""
}

main() {
    echo -e "\n${BOLD}${YELLOW}╔══════════════════════════════════════╗"
    echo -e "║   ESP8266 DEAUTHER — ПОДГОТОВКА      ║"
    echo -e "╚══════════════════════════════════════╝${RESET}\n"

    detect_os
    install_system_packages
    install_arduino_cli
    ensure_arduino_cli_in_path
    configure_arduino_cli
    install_deauther_core
    verify_target_board
    install_python_deps
    print_summary
}

main "$@"

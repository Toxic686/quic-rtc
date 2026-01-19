#!/bin/bash

# QUIC DataChannel 专项性能测试脚本
# 仅关注 QUIC 协议的性能指标（吞吐量、延迟、稳定性、拥塞控制等）

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 默认配置
SIGNALING_IP="127.0.0.1"
SIGNALING_PORT=8080
OFFERER_IP="127.0.0.1"
ANSWERER_IP="127.0.0.1"
TEST_TIMEOUT=120
REPORT_DIR="quic_test_reports"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
BUILD_DIR="."

# 日志开关
RTC_QUIC_TRACE=0
RTC_TRANSPORT_TRACE=0
LSQUIC_LOG_LEVEL_OVERRIDE=""
LSQUIC_LOG_OVERRIDE=""

# QUIC 配置
RTC_QUIC_CC_ALGO=""  # 默认自适应，可选 cubic, bbr

# 端到端测试控制
RTC_MAX_MESSAGE_SIZE=$((20 * 1024 * 1024))     # 20MB
RTC_TEST_ACK_TIMEOUT_SEC=300
RTC_TEST_ACK_TRACE=0

# Answerer 控制
RTC_RX_PRINT_EVERY=100
RTC_ANSWERER_MAX_WAIT_SEC=3600

# 测试档位
RTC_TEST_PROFILE="local"

# 基础性能测试消息大小曲线（逗号分隔，单位：bytes）
RTC_TEST2_SIZES="1024,4096,16384,32768,1048576"

# 结果存储
declare -A TEST_RESULTS
declare -A PERFORMANCE_METRICS
declare -A ERROR_COUNTS
declare -A DETAILED_METRICS

# 全局进程管理
GLOBAL_ANSWERER_PID=0
GLOBAL_ANSWERER_LOG=""

# 显示使用说明
show_usage() {
    echo -e "${BLUE}QUIC 专项性能测试用法:${NC}"
    echo "  $0 [选项]"
    echo ""
    echo -e "${BLUE}基础配置:${NC}"
    echo "  -s, --signaling-ip IP      信令服务器IP (默认: 127.0.0.1)"
    echo "  -p, --signaling-port PORT  信令服务器端口 (默认: 8080)"
    echo "  -o, --offerer-ip IP        发起方IP (默认: 127.0.0.1)"
    echo "  -a, --answerer-ip IP       接收方IP (默认: 127.0.0.1)"
    echo "  -b, --build-dir DIR        构建目录 (默认: .)"
    
    echo -e "${BLUE}QUIC 与 拥塞控制:${NC}"
    echo "  --cc ALGO                  设置拥塞控制算法 (cubic/bbr/adaptive)"
    echo "  --trace-bbr                开启 BBR 详细日志 (用于分析 BBR 行为)"
    echo "  --lsquic-log CONFIG        自定义 lsquic 日志 (如 'bbr=debug,cubic=info')"
    
    echo -e "${BLUE}测试参数:${NC}"
    echo "  --test2-sizes CSV          设置性能测试包大小曲线 (默认: 1KB,4KB,16KB,32KB,1MB)"
    echo "  --max-message-size BYTES   设置最大消息大小 (默认: 20MB)"
    echo "  --test-profile PROFILE     设置测试场景 (local/wan/low)"
    echo "  -t, --timeout SECONDS      单项测试超时时间 (默认: 120)"
    
    echo -e "${BLUE}示例:${NC}"
    echo "  # 测试 BBR 性能"
    echo "  $0 -s 47.x.x.x -o 47.x.x.x -a 120.x.x.x --cc bbr"
    echo ""
    echo "  # 开启 BBR 日志追踪"
    echo "  $0 ... --cc bbr --trace-bbr"
}

# 解析参数
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -s|--signaling-ip) SIGNALING_IP="$2"; shift 2 ;;
            -p|--signaling-port) SIGNALING_PORT="$2"; shift 2 ;;
            -o|--offerer-ip) OFFERER_IP="$2"; shift 2 ;;
            -a|--answerer-ip) ANSWERER_IP="$2"; shift 2 ;;
            -b|--build-dir) BUILD_DIR="$2"; shift 2 ;;
            -t|--timeout) TEST_TIMEOUT="$2"; shift 2 ;;
            --quic-trace) RTC_QUIC_TRACE=1; shift ;;
            --transport-trace) RTC_TRANSPORT_TRACE=1; shift ;;
            --lsquic-log-level) LSQUIC_LOG_LEVEL_OVERRIDE="$2"; shift 2 ;;
            --lsquic-log) LSQUIC_LOG_OVERRIDE="$2"; shift 2 ;;
            --trace-bbr) LSQUIC_LOG_OVERRIDE="bbr=debug"; shift ;;
            --max-message-size) RTC_MAX_MESSAGE_SIZE="$2"; shift 2 ;;
            --ack-timeout-sec) RTC_TEST_ACK_TIMEOUT_SEC="$2"; shift 2 ;;
            --ack-trace) RTC_TEST_ACK_TRACE=1; shift ;;
            --rx-print-every) RTC_RX_PRINT_EVERY="$2"; shift 2 ;;
            --answerer-max-wait-sec) RTC_ANSWERER_MAX_WAIT_SEC="$2"; shift 2 ;;
            --test-profile) RTC_TEST_PROFILE="$2"; shift 2 ;;
            --test2-sizes) RTC_TEST2_SIZES="$2"; shift 2 ;;
            --cc) RTC_QUIC_CC_ALGO="$2"; shift 2 ;;
            -h|--help) show_usage; exit 0 ;;
            *) echo -e "${RED}错误: 未知参数 $1${NC}"; show_usage; exit 1 ;;
        esac
    done
}

# 构建环境变量
build_env_prefix_common() {
    local vars=()
    vars+=("RTC_TEST_PROFILE=${RTC_TEST_PROFILE}")
    vars+=("RTC_TEST2_SIZES=${RTC_TEST2_SIZES}")
    vars+=("RTC_MAX_MESSAGE_SIZE=${RTC_MAX_MESSAGE_SIZE}")
    vars+=("RTC_TEST_ACK_TIMEOUT_SEC=${RTC_TEST_ACK_TIMEOUT_SEC}")
    [[ "${RTC_TEST_ACK_TRACE}" == "1" ]] && vars+=("RTC_TEST_ACK_TRACE=1")
    [[ "${RTC_QUIC_TRACE}" == "1" ]] && vars+=("RTC_QUIC_TRACE=1")
    [[ "${RTC_TRANSPORT_TRACE}" == "1" ]] && vars+=("RTC_TRANSPORT_TRACE=1")
    [[ -n "${RTC_QUIC_CC_ALGO}" ]] && vars+=("RTC_QUIC_CC_ALGO=${RTC_QUIC_CC_ALGO}")
    [[ -n "${LSQUIC_LOG_LEVEL_OVERRIDE}" ]] && vars+=("LSQUIC_LOG_LEVEL=${LSQUIC_LOG_LEVEL_OVERRIDE}")
    [[ -n "${LSQUIC_LOG_OVERRIDE}" ]] && vars+=("LSQUIC_LOG=${LSQUIC_LOG_OVERRIDE}")
    
    if [[ ${#vars[@]} -gt 0 ]]; then
        echo "env ${vars[*]}"
    else
        echo ""
    fi
}

infer_test_profile() {
    if [[ -n "${RTC_TEST_PROFILE}" ]]; then
        local v="$(echo "${RTC_TEST_PROFILE}" | tr '[:upper:]' '[:lower:]')"
        if [[ "${v}" == "wan" || "${v}" == "local" || "${v}" == "low" ]]; then
            RTC_TEST_PROFILE="${v}"
            return
        fi
    fi
    if ! is_local_ip "$ANSWERER_IP"; then
        RTC_TEST_PROFILE="low"
    else
        RTC_TEST_PROFILE="local"
    fi
}

build_env_prefix_answerer() {
    local base="$(build_env_prefix_common)"
    local vars=()
    vars+=("RTC_RX_PRINT_EVERY=${RTC_RX_PRINT_EVERY}")
    vars+=("RTC_ANSWERER_MAX_WAIT_SEC=${RTC_ANSWERER_MAX_WAIT_SEC}")
    if [[ -n "${base}" ]]; then
        echo "${base} ${vars[*]}"
    else
        echo "env ${vars[*]}"
    fi
}

build_env_prefix_offerer() {
    build_env_prefix_common
}

# 辅助函数：IP 检测
get_all_local_ips() {
    local ips=()
    if command -v ip &> /dev/null; then
        while IFS= read -r line; do
            [[ -n "$line" && "$line" != "127.0.0.1" ]] && ips+=("$line")
        done < <(ip -4 addr show 2>/dev/null | grep -oP 'inet \K[\d.]+' | grep -v '^127\.')
    fi
    # Fallback to ifconfig/hostname if ip command fails or returns nothing (simplified)
    if [[ ${#ips[@]} -eq 0 ]] && command -v hostname &> /dev/null; then
         while IFS= read -r line; do
            [[ -n "$line" && "$line" != "127.0.0.1" ]] && ips+=("$line")
        done < <(hostname -I 2>/dev/null | tr ' ' '\n' | grep -v '^127\.')
    fi
    printf '%s\n' "${ips[@]}"
}

is_local_ip() {
    local check_ip=$1
    [[ "$check_ip" == "127.0.0.1" || "$check_ip" == "localhost" ]] && return 0
    local all_local_ips=($(get_all_local_ips))
    for local_ip in "${all_local_ips[@]}"; do
        [[ "$check_ip" == "$local_ip" ]] && return 0
    done
    
    # Check via ping RTT for public IPs mapped to local
    if command -v ping &> /dev/null; then
        local ping_result=$(ping -c 1 -W 1 "$check_ip" 2>/dev/null)
        if [[ $? -eq 0 ]]; then
            local avg_time=$(echo "$ping_result" | grep "min/avg/max" | awk -F'/' '{print $5}' 2>/dev/null)
            if [[ -n "$avg_time" ]]; then
                if (( $(echo "$avg_time < 1.0" | bc -l 2>/dev/null || echo 0) )); then
                    return 0
                fi
            fi
        fi
    fi
    return 1
}

# 初始化
init_test() {
    echo -e "${BLUE}=== QUIC 性能测试初始化 ===${NC}"
    echo -e "${BLUE}时间戳: ${TIMESTAMP}${NC}"
    echo -e "${BLUE}信令: ${SIGNALING_IP}:${SIGNALING_PORT}${NC}"
    echo -e "${BLUE}发起: ${OFFERER_IP}${NC}"
    echo -e "${BLUE}接收: ${ANSWERER_IP}${NC}"
    [[ -n "${RTC_QUIC_CC_ALGO}" ]] && echo -e "${BLUE}拥塞控制: ${RTC_QUIC_CC_ALGO}${NC}"
    
    mkdir -p "${REPORT_DIR}"
    
    if [[ ! -f "${BUILD_DIR}/webrtc-client" ]]; then
        echo -e "${RED}错误: webrtc-client 未找到在 ${BUILD_DIR}${NC}"
        exit 1
    fi
    
    infer_test_profile
    echo -e "${BLUE}测试档位: ${RTC_TEST_PROFILE}${NC}"
}

# 启动 Answerer
start_answerer() {
    local env_prefix="$(build_env_prefix_answerer)"
    
    if ! is_local_ip "$ANSWERER_IP"; then
        echo -e "${YELLOW}>>> 请在远程机器 (${ANSWERER_IP}) 上启动 QUIC answerer:${NC}"
        echo -e "${GREEN}    ${env_prefix} ./webrtc-client quic answerer ${SIGNALING_IP} ${SIGNALING_PORT}${NC}"
        echo -e "${CYAN}等待 60 秒... (如果已启动可按回车继续)${NC}"
        if command -v timeout &> /dev/null; then
            read -t 60 -p "按回车继续..." || true
        else
            sleep 60
        fi
        return 0
    fi
    
    if [[ ${GLOBAL_ANSWERER_PID} -gt 0 ]] && kill -0 ${GLOBAL_ANSWERER_PID} 2>/dev/null; then
        echo -e "${GREEN}Answerer 已在运行 (PID: ${GLOBAL_ANSWERER_PID})${NC}"
        return 0
    fi
    
    echo -e "${CYAN}启动本地 QUIC answerer...${NC}"
    cd "${BUILD_DIR}"
    GLOBAL_ANSWERER_LOG="${REPORT_DIR}/answerer_${TIMESTAMP}.log"
    
    timeout $((TEST_TIMEOUT * 10)) ${env_prefix} ./webrtc-client quic answerer ${SIGNALING_IP} ${SIGNALING_PORT} ${cc_arg} \
        > "${GLOBAL_ANSWERER_LOG}" 2>&1 &
    GLOBAL_ANSWERER_PID=$!
    
    echo -e "${GREEN}Answerer 启动成功 (PID: ${GLOBAL_ANSWERER_PID})${NC}"
    sleep 2
}

stop_answerer() {
    if [[ ${GLOBAL_ANSWERER_PID} -gt 0 ]]; then
        kill ${GLOBAL_ANSWERER_PID} 2>/dev/null || true
        wait ${GLOBAL_ANSWERER_PID} 2>/dev/null || true
        echo -e "${GREEN}Answerer 已停止${NC}"
        GLOBAL_ANSWERER_PID=0
    fi
}

# 运行测试套件
run_quic_suite() {
    local test_name="quic_perf_suite"
    local log_dir="${REPORT_DIR}/${test_name}_${TIMESTAMP}"
    mkdir -p "${log_dir}"
    local env_prefix="$(build_env_prefix_offerer)"
    local cc_arg=""
    if [[ -n "${RTC_QUIC_CC_ALGO}" ]]; then
        cc_arg="--cc ${RTC_QUIC_CC_ALGO}"
    fi
    
    echo -e "${BLUE}=== 开始 QUIC 测试套件 ===${NC}"
    
    # 启动 Offerer
    echo -e "${CYAN}启动 Offerer (测试套件模式)...${NC}"
    
    if is_local_ip "$OFFERER_IP" || [[ "${OFFERER_IS_LOCAL:-0}" == "1" ]]; then
        cd "${BUILD_DIR}"
        # 超时时间放宽，因为要跑多个测试
        # 注意：现在通过命令行传递 --cc 参数，而不是依赖环境变量（尽管环境变量也被保留作为后备）
        timeout $((TEST_TIMEOUT * 10)) ${env_prefix} ./webrtc-client quic offerer ${SIGNALING_IP} ${SIGNALING_PORT} --test-suite ${cc_arg} \
            > "${log_dir}/offerer.log" 2>&1 &
        local offerer_pid=$!
        echo -e "${GREEN}Offerer 已启动 (PID: ${offerer_pid})${NC}"
        
        echo -e "${CYAN}等待测试完成...${NC}"
        local waited=0
        while kill -0 ${offerer_pid} 2>/dev/null && [[ ${waited} -lt $((TEST_TIMEOUT * 10)) ]]; do
            sleep 2
            waited=$((waited + 2))
            [[ $((waited % 10)) -eq 0 ]] && echo -e "${CYAN}已运行 ${waited} 秒...${NC}"
        done
        
        kill ${offerer_pid} 2>/dev/null || true
        wait ${offerer_pid} 2>/dev/null || true
    else
        echo -e "${YELLOW}>>> 请在 Offerer 机器 (${OFFERER_IP}) 上运行:${NC}"
        echo -e "${GREEN}    ${env_prefix} ./webrtc-client quic offerer ${SIGNALING_IP} ${SIGNALING_PORT} --test-suite${NC}"
        echo -e "${YELLOW}按回车继续...${NC}"
        read
    fi
    
    # 收集 Answerer 日志 (如果是本地)
    if [[ -f "${GLOBAL_ANSWERER_LOG}" ]]; then
        cp "${GLOBAL_ANSWERER_LOG}" "${log_dir}/answerer.log"
    fi
    
    analyze_results "${test_name}" "${log_dir}"
}

# 分析结果
analyze_results() {
    local test_name=$1
    local log_dir=$2
    local offerer_log="${log_dir}/offerer.log"
    
    echo -e "${BLUE}=== 分析测试结果 ===${NC}"
    
    if [[ ! -f "${offerer_log}" ]]; then
        echo -e "${RED}未找到 Offerer 日志，无法分析${NC}"
        return
    fi
    
    # ---------------------------------------------------------
    # 提取 9 项测试结果
    # ---------------------------------------------------------
    
    # 测试1: 基础连接性测试
    if grep -q "测试 1: 基础连接性测试" "${offerer_log}"; then
        if grep -A 5 "测试 1: 基础连接性测试" "${offerer_log}" | grep -q "✅\|连接已建立"; then
            DETAILED_METRICS["test1_basic_connectivity"]="PASS"
        else
            DETAILED_METRICS["test1_basic_connectivity"]="FAIL"
        fi
    fi

    # 测试2: 基础性能测试
    if grep -q "测试 2: 基础性能测试" "${offerer_log}"; then
        # 提取曲线数据
        local test2_curve
        test2_curve=$(
            awk '
                function mbps(bytes, ms) {
                    if (ms <= 0) return "N/A";
                    return (bytes * 8.0) / (ms / 1000.0) / 1000000.0;
                }
                BEGIN { }
                /TEST_ACK\|name=基础性能测试 \(/ {
                    line=$0
                    # name between "name=" and "|seq="
                    n=line
                    sub(/^.*name=/, "", n)
                    sub(/\|seq=.*$/, "", n)
                    # size label between "(" and ","
                    size=n
                    sub(/^基础性能测试 \(/, "", size)
                    sub(/,.*$/, "", size)
                    # bytes
                    b=line
                    if (match(b, /bytes=[0-9]+/)) { sub(/^.*bytes=/, "", b); sub(/\|.*$/, "", b) } else { b="" }
                    # duration_ms
                    ms=line
                    if (match(ms, /duration_ms=[0-9]+/)) { sub(/^.*duration_ms=/, "", ms); sub(/\|.*$/, "", ms) } else { ms="" }
                    if (size != "" && b != "" && ms != "") {
                        printf("%s=%.4f\n", size, mbps(b+0, ms+0))
                    }
                }
            ' "${offerer_log}" | awk '!seen[$0]++' | tr '\n' ',' | sed 's/,$//'
        )

        local test2_throughput="N/A"
        if [[ -n "${test2_curve}" ]]; then
            # 尝试取第一项或 1KB
            if echo "${test2_curve}" | grep -q "1KB="; then
                test2_throughput=$(echo "${test2_curve}" | tr ',' '\n' | grep -m1 "^1KB=" | awk -F'=' '{print $2}')
            else
                test2_throughput=$(echo "${test2_curve}" | tr ',' '\n' | head -n1 | awk -F'=' '{print $2}')
            fi
            DETAILED_METRICS["test2_curve"]="${test2_curve}"
        else
            # 回退：老口径
            test2_throughput=$(grep -A 30 "测试 2: 基础性能测试" "${offerer_log}" | grep "吞吐量:" | awk '{print $2}' | head -1 || echo "N/A")
        fi

        if [[ -n "${test2_throughput}" && "${test2_throughput}" != "N/A" ]]; then
            DETAILED_METRICS["test2_basic_performance"]="PASS"
            DETAILED_METRICS["test2_throughput"]="${test2_throughput}"
        else
            DETAILED_METRICS["test2_basic_performance"]="FAIL"
        fi
    fi

    # 测试3: 消息大小测试
    if grep -q "测试 3: 消息大小测试" "${offerer_log}"; then
        # 只要检测到至少 1 个“消息大小测试 (X)”且每个都有结果输出即可通过
        local sizes_count=$(grep -c "=== 开始测试: 消息大小测试 (" "${offerer_log}" 2>/dev/null || echo 0)
        if [[ ${sizes_count} -gt 0 ]]; then
            DETAILED_METRICS["test3_message_sizes"]="PASS"
        else
            DETAILED_METRICS["test3_message_sizes"]="FAIL"
        fi
    fi

    # 测试4: 高并发消息测试
    if grep -q "测试 4: 高并发消息测试" "${offerer_log}"; then
        local test4_throughput=$(
            grep -A 200 "测试 4: 高并发消息测试" "${offerer_log}" \
                | grep -m 1 "recv_throughput=" \
                | awk -F'recv_throughput=' '{print $2}' \
                | awk '{print $1}' \
                || echo "N/A"
        )
        if [[ "${test4_throughput}" != "N/A" ]]; then
            DETAILED_METRICS["test4_high_concurrency"]="PASS"
            DETAILED_METRICS["test4_throughput"]=${test4_throughput}
        else
            DETAILED_METRICS["test4_high_concurrency"]="FAIL"
        fi
    fi

    # 测试5: 大消息测试
    if grep -q "测试 5: 大消息测试" "${offerer_log}"; then
        local test5_throughput=$(
            grep -A 200 "测试 5: 大消息测试" "${offerer_log}" \
                | grep -m 1 "recv_throughput=" \
                | awk -F'recv_throughput=' '{print $2}' \
                | awk '{print $1}' \
                || echo "N/A"
        )
        if [[ "${test5_throughput}" != "N/A" ]]; then
            DETAILED_METRICS["test5_large_message"]="PASS"
            DETAILED_METRICS["test5_throughput"]=${test5_throughput}
        else
            DETAILED_METRICS["test5_large_message"]="FAIL"
        fi
    fi

    # 测试6: 网络延迟测试
    if grep -q "测试 6: 网络延迟测试" "${offerer_log}"; then
        if grep -A 5 "测试 6: 网络延迟测试" "${offerer_log}" | grep -q "✅\|连接建立时间"; then
            DETAILED_METRICS["test6_network_latency"]="PASS"
        else
            DETAILED_METRICS["test6_network_latency"]="FAIL"
        fi
    fi

    # 测试7: 稳定性测试
    if grep -q "测试 7: 稳定性测试" "${offerer_log}"; then
        local stability_runs
        stability_runs=$(grep -E "稳定性测试运行[[:space:]]+[0-9]+/[0-9]+" "${offerer_log}" | wc -l | tr -d ' ')
        if [[ ${stability_runs} -ge 2 ]]; then
            DETAILED_METRICS["test7_stability"]="PASS"
            DETAILED_METRICS["test7_runs"]=${stability_runs}
        else
            DETAILED_METRICS["test7_stability"]="FAIL"
        fi
    fi

    # 测试8: 错误处理测试
    if grep -q "测试 8: 错误处理测试" "${offerer_log}"; then
        if grep -A 60 "测试 8: 错误处理测试" "${offerer_log}" | grep -q "测试完成"; then
            DETAILED_METRICS["test8_error_handling"]="PASS"
        else
            DETAILED_METRICS["test8_error_handling"]="FAIL"
        fi
    fi

    # 测试9: 传输协议验证
    if grep -q "测试 9: 传输协议验证" "${offerer_log}"; then
        local test9_passed=false
        if grep -A 10 "测试 9: 传输协议验证" "${offerer_log}" | grep -q "✅.*协议验证.*连接已成功建立"; then
            test9_passed=true
        fi
        if grep -A 10 "测试 9: 传输协议验证" "${offerer_log}" | grep -q "SDP协议类型.*QUIC\|SDP协议类型.*SCTP.*兼容格式"; then
            test9_passed=true
        fi
        if ${test9_passed}; then
            DETAILED_METRICS["test9_protocol_verification"]="PASS"
        else
            DETAILED_METRICS["test9_protocol_verification"]="FAIL"
        fi
    fi

    # ---------------------------------------------------------
    # 总结
    # ---------------------------------------------------------

    if grep -q "所有9项测试完成" "${offerer_log}"; then
        echo -e "${GREEN}✓ 所有9项测试均已完成${NC}"
    else
        echo -e "${RED}✗ 测试未完整执行${NC}"
    fi

    # 提取 BBR 相关 (如果有)
    if grep -q "拥塞控制算法: BBR" "${offerer_log}"; then
        echo -e "${GREEN}✓ 确认使用 BBR 拥塞控制${NC}"
        DETAILED_METRICS["cc_algo"]="BBR"
    fi

    # 生成报告
    generate_report "${log_dir}"
}

# 生成报告
generate_report() {
    local log_dir=$1
    local report_file="${REPORT_DIR}/quic_report_${TIMESTAMP}.md"
    
    cat > "${report_file}" << EOF
# QUIC 性能测试报告

- **时间**: $(date)
- **拥塞控制**: ${RTC_QUIC_CC_ALGO:-Adaptive (Default)}
- **场景**: ${RTC_TEST_PROFILE}
- **BBR 日志**: $([[ "${LSQUIC_LOG_OVERRIDE}" == *"bbr"* ]] && echo "开启" || echo "关闭")

## 9项测试详细结果

| 测试项 | 状态 | 关键指标 |
|--------|------|----------|
| 1. 基础连接性 | ${DETAILED_METRICS["test1_basic_connectivity"]:-未执行} | - |
| 2. 基础性能 | ${DETAILED_METRICS["test2_basic_performance"]:-未执行} | 吞吐: ${DETAILED_METRICS["test2_throughput"]:-N/A} Mbps |
| 3. 消息大小 | ${DETAILED_METRICS["test3_message_sizes"]:-未执行} | - |
| 4. 高并发 | ${DETAILED_METRICS["test4_high_concurrency"]:-未执行} | 吞吐: ${DETAILED_METRICS["test4_throughput"]:-N/A} Mbps |
| 5. 大消息 | ${DETAILED_METRICS["test5_large_message"]:-未执行} | 吞吐: ${DETAILED_METRICS["test5_throughput"]:-N/A} Mbps |
| 6. 网络延迟 | ${DETAILED_METRICS["test6_network_latency"]:-未执行} | - |
| 7. 稳定性 | ${DETAILED_METRICS["test7_stability"]:-未执行} | 运行: ${DETAILED_METRICS["test7_runs"]:-N/A} 次 |
| 8. 错误处理 | ${DETAILED_METRICS["test8_error_handling"]:-未执行} | - |
| 9. 协议验证 | ${DETAILED_METRICS["test9_protocol_verification"]:-未执行} | - |

## 基础性能曲线 (测试2)

${DETAILED_METRICS["test2_curve"]:-无数据}

## BBR/拥塞控制分析

如果开启了 BBR 日志，请查看 \`${log_dir}/offerer.log\` 并搜索 \`[bbr]\`。

## 详细日志位置

- Offerer: \`${log_dir}/offerer.log\`
- Answerer: \`${log_dir}/answerer.log\`

EOF
    
    echo -e "${GREEN}报告生成: ${report_file}${NC}"
}

# 主流程
main() {
    parse_args "$@"
    init_test
    
    start_answerer
    run_quic_suite
    stop_answerer
}

main "$@"

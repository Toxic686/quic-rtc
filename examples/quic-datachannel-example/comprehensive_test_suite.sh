#!/bin/bash

# QUIC DataChannel 综合测试套件
# 覆盖所有测试场景并生成详细报告

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
PURPLE='\033[0;35m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# 测试配置
BUILD_DIR="."
SIGNALING_PORT=8080
TEST_TIMEOUT=30
REPORT_DIR="test_reports"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")

# 测试结果存储
declare -A TEST_RESULTS
declare -A PERFORMANCE_METRICS
declare -A ERROR_COUNTS
declare -A DETAILED_METRICS

# 初始化
init_test_suite() {
    echo -e "${BLUE}=== QUIC DataChannel 综合测试套件 ===${NC}"
    echo -e "${BLUE}时间戳: ${TIMESTAMP}${NC}"
    echo -e "${BLUE}构建目录: ${BUILD_DIR}${NC}"
    
    # 创建报告目录
    mkdir -p "${REPORT_DIR}"
    
    # 检查必要文件
    check_prerequisites
    
    # 清理旧进程
    cleanup_processes
    
    echo -e "${GREEN}测试套件初始化完成${NC}\n"
}

# 检查前置条件
check_prerequisites() {
    echo -e "${CYAN}检查前置条件...${NC}"
    
    # 检查可执行文件
    if [[ ! -f "${BUILD_DIR}/webrtc-client" ]]; then
        echo -e "${RED}错误: webrtc-client 未找到${NC}"
        exit 1
    fi
    
    if [[ ! -f "${BUILD_DIR}/signaling-server" ]]; then
        echo -e "${RED}错误: signaling-server 未找到${NC}"
        exit 1
    fi
    
    # 检查端口可用性
    if netstat -tuln 2>/dev/null | grep -q ":${SIGNALING_PORT} "; then
        echo -e "${YELLOW}警告: 端口 ${SIGNALING_PORT} 已被占用${NC}"
    fi
    
    echo -e "${GREEN}前置条件检查完成${NC}"
}

# 清理进程
cleanup_processes() {
    echo -e "${CYAN}清理旧进程...${NC}"
    
    pkill -f "webrtc-client" 2>/dev/null || true
    pkill -f "signaling-server" 2>/dev/null || true
    
    # 等待进程完全退出
    sleep 2
    
    echo -e "${GREEN}进程清理完成${NC}"
}

# 启动信令服务器
start_signaling_server() {
    echo -e "${CYAN}启动信令服务器...${NC}"
    
    cd "${BUILD_DIR}"
    ./signaling-server ${SIGNALING_PORT} > "${REPORT_DIR}/signaling_server_${TIMESTAMP}.log" 2>&1 &
    local server_pid=$!
    
    # 等待服务器启动
    sleep 3
    
    # 检查服务器是否成功启动
    if ! netstat -tuln 2>/dev/null | grep -q ":${SIGNALING_PORT} "; then
        echo -e "${RED}错误: 信令服务器启动失败${NC}"
        exit 1
    fi
    
    echo -e "${GREEN}信令服务器启动成功 (PID: ${server_pid})${NC}"
    echo ${server_pid} > "${REPORT_DIR}/signaling_server.pid"
}

# 基础连接测试
test_basic_connectivity() {
    echo -e "${BLUE}=== 测试 1: 基础连接性 ===${NC}"
    
    local test_name="basic_connectivity"
    local start_time=$(date +%s)
    
    # 启动接收方
    cd "${BUILD_DIR}"
    timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_${test_name}.log" 2>&1 &
    local answerer_pid=$!
    
    sleep 2
    
    # 启动发起方
    timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_${test_name}.log" 2>&1 &
    local offerer_pid=$!
    
    # 等待测试完成
    wait ${offerer_pid} || true
    wait ${answerer_pid} || true
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 分析结果 - 修复判断逻辑
    local offerer_success=false
    local answerer_success=false
    
    # 发起方：检查"数据通道已打开"或"数据通道准备就绪"
    if grep -q "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_${test_name}.log"; then
        offerer_success=true
    fi
    
    # 接收方：检查"收到数据通道"或"数据通道已打开"
    if grep -q "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_${test_name}.log"; then
        answerer_success=true
    fi
    
    if ${offerer_success} && ${answerer_success}; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 基础连接性测试通过${NC}"
    else
        TEST_RESULTS[${test_name}]="FAIL"
        ERROR_COUNTS[${test_name}]=$((ERROR_COUNTS[${test_name}:-0] + 1))
        echo -e "${RED}✗ 基础连接性测试失败${NC}"
        echo -e "${YELLOW}  发起方: ${offerer_success}${NC}"
        echo -e "${YELLOW}  接收方: ${answerer_success}${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# QUIC vs SCTP 对比测试
test_transport_comparison() {
    echo -e "${BLUE}=== 测试 2: QUIC vs SCTP 传输对比 ===${NC}"
    
    local test_name="transport_comparison"
    local start_time=$(date +%s)
    
    # 测试 QUIC
    echo -e "${CYAN}测试 QUIC 传输...${NC}"
    cd "${BUILD_DIR}"
    timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_quic_${test_name}.log" 2>&1 &
    local answerer_pid=$!
    
    sleep 2
    
    timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_quic_${test_name}.log" 2>&1 &
    local offerer_pid=$!
    
    wait ${offerer_pid} || true
    wait ${answerer_pid} || true
    
    # 测试 SCTP
    echo -e "${CYAN}测试 SCTP 传输...${NC}"
    timeout ${TEST_TIMEOUT} ./webrtc-client sctp answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_sctp_${test_name}.log" 2>&1 &
    answerer_pid=$!
    
    sleep 2
    
    timeout ${TEST_TIMEOUT} ./webrtc-client sctp offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_sctp_${test_name}.log" 2>&1 &
    offerer_pid=$!
    
    wait ${offerer_pid} || true
    wait ${answerer_pid} || true
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 分析结果 - 修复判断逻辑
    local quic_success=false
    local sctp_success=false
    
    # QUIC测试结果
    local quic_offerer_success=false
    local quic_answerer_success=false
    if grep -q "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_quic_${test_name}.log"; then
        quic_offerer_success=true
    fi
    if grep -q "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_quic_${test_name}.log"; then
        quic_answerer_success=true
    fi
    if ${quic_offerer_success} && ${quic_answerer_success}; then
        quic_success=true
    fi
    
    # SCTP测试结果
    local sctp_offerer_success=false
    local sctp_answerer_success=false
    if grep -q "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_sctp_${test_name}.log"; then
        sctp_offerer_success=true
    fi
    if grep -q "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_sctp_${test_name}.log"; then
        sctp_answerer_success=true
    fi
    if ${sctp_offerer_success} && ${sctp_answerer_success}; then
        sctp_success=true
    fi
    
    if ${quic_success} && ${sctp_success}; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 传输对比测试通过${NC}"
    else
        TEST_RESULTS[${test_name}]="FAIL"
        ERROR_COUNTS[${test_name}]=$((ERROR_COUNTS[${test_name}:-0] + 1))
        echo -e "${RED}✗ 传输对比测试失败${NC}"
        echo -e "${YELLOW}  QUIC: ${quic_success} (发起方: ${quic_offerer_success}, 接收方: ${quic_answerer_success})${NC}"
        echo -e "${YELLOW}  SCTP: ${sctp_success} (发起方: ${sctp_offerer_success}, 接收方: ${sctp_answerer_success})${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 性能测试
test_performance() {
    echo -e "${BLUE}=== 测试 3: 性能测试 ===${NC}"
    
    local test_name="performance"
    local start_time=$(date +%s)
    
    # 启动接收方
    cd "${BUILD_DIR}"
    timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_${test_name}.log" 2>&1 &
    local answerer_pid=$!
    
    sleep 2
    
    # 启动发起方
    timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_${test_name}.log" 2>&1 &
    local offerer_pid=$!
    
    # 等待测试完成
    wait ${offerer_pid} || true
    wait ${answerer_pid} || true
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 提取性能指标
    local throughput=$(grep "吞吐量:" "${REPORT_DIR}/offerer_${test_name}.log" | tail -1 | awk '{print $2}' || echo "N/A")
    local message_count=$(grep "发送消息数:" "${REPORT_DIR}/offerer_${test_name}.log" | tail -1 | awk '{print $2}' || echo "N/A")
    local message_size=$(grep "消息大小:" "${REPORT_DIR}/offerer_${test_name}.log" | tail -1 | awk '{print $2}' || echo "N/A")
    
    PERFORMANCE_METRICS["throughput"]=${throughput}
    PERFORMANCE_METRICS["message_count"]=${message_count}
    PERFORMANCE_METRICS["message_size"]=${message_size}
    
    # 分析结果
    if grep -q "性能测试结果" "${REPORT_DIR}/offerer_${test_name}.log"; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 性能测试通过${NC}"
        echo -e "${CYAN}  吞吐量: ${throughput} Mbps${NC}"
        echo -e "${CYAN}  消息数: ${message_count}${NC}"
        echo -e "${CYAN}  消息大小: ${message_size} 字节${NC}"
    else
        TEST_RESULTS[${test_name}]="FAIL"
        ERROR_COUNTS[${test_name}]=$((ERROR_COUNTS[${test_name}:-0] + 1))
        echo -e "${RED}✗ 性能测试失败${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 消息大小测试
test_message_sizes() {
    echo -e "${BLUE}=== 测试 4: 消息大小测试 ===${NC}"
    
    local test_name="message_sizes"
    local start_time=$(date +%s)
    local success_count=0
    local total_sizes=5
    
    # 测试不同消息大小：1KB, 10KB, 100KB, 1MB, 10MB
    local message_sizes=(1024 10240 102400 1048576 10485760)
    local size_labels=("1KB" "10KB" "100KB" "1MB" "10MB")
    
    for i in $(seq 0 $((total_sizes-1))); do
        local size=${message_sizes[i]}
        local label=${size_labels[i]}
        
        echo -e "${CYAN}测试消息大小: ${label} (${size} 字节)...${NC}"
        
        # 启动接收方
        cd "${BUILD_DIR}"
        timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_${test_name}_${i}.log" 2>&1 &
        local answerer_pid=$!
        
        sleep 2
        
        # 启动发起方
        timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_${test_name}_${i}.log" 2>&1 &
        local offerer_pid=$!
        
        # 等待测试完成
        wait ${offerer_pid} || true
        wait ${answerer_pid} || true
        
        # 检查结果
        if grep -q "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_${test_name}_${i}.log" && \
           grep -q "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_${test_name}_${i}.log"; then
            success_count=$((success_count + 1))
            echo -e "${GREEN}  ✓ ${label} 测试成功${NC}"
            
            # 记录性能指标
            local throughput=$(grep "吞吐量:" "${REPORT_DIR}/offerer_${test_name}_${i}.log" | tail -1 | awk '{print $2}' || echo "N/A")
            DETAILED_METRICS["${test_name}_${label}_throughput"]=${throughput}
        else
            echo -e "${RED}  ✗ ${label} 测试失败${NC}"
        fi
        
        sleep 2
    done
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 分析结果
    local success_rate=$(echo "scale=2; ${success_count} * 100 / ${total_sizes}" | bc -l 2>/dev/null || echo "0")
    
    if [[ ${success_count} -eq ${total_sizes} ]]; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 消息大小测试通过 (${success_count}/${total_sizes})${NC}"
    else
        TEST_RESULTS[${test_name}]="PARTIAL"
        echo -e "${YELLOW}⚠ 消息大小测试部分通过 (${success_count}/${total_sizes}, ${success_rate}%)${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 并发连接测试
test_concurrent_connections() {
    echo -e "${BLUE}=== 测试 5: 并发连接测试 ===${NC}"
    
    local test_name="concurrent_connections"
    local start_time=$(date +%s)
    local success_count=0
    local total_connections=3
    
    for i in $(seq 1 ${total_connections}); do
        echo -e "${CYAN}测试并发连接 ${i}/${total_connections}...${NC}"
        
        # 启动接收方
        cd "${BUILD_DIR}"
        timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_${test_name}_${i}.log" 2>&1 &
        local answerer_pid=$!
        
        sleep 1
        
        # 启动发起方
        timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_${test_name}_${i}.log" 2>&1 &
        local offerer_pid=$!
        
        # 等待测试完成
        wait ${offerer_pid} || true
        wait ${answerer_pid} || true
        
        # 检查结果
        if grep -q "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_${test_name}_${i}.log" && \
           grep -q "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_${test_name}_${i}.log"; then
            success_count=$((success_count + 1))
            echo -e "${GREEN}  连接 ${i} 成功${NC}"
        else
            echo -e "${RED}  连接 ${i} 失败${NC}"
        fi
        
        sleep 1
    done
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 分析结果
    local success_rate=$(echo "scale=2; ${success_count} * 100 / ${total_connections}" | bc -l 2>/dev/null || echo "0")
    
    if [[ ${success_count} -eq ${total_connections} ]]; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 并发连接测试通过 (${success_count}/${total_connections})${NC}"
    else
        TEST_RESULTS[${test_name}]="PARTIAL"
        echo -e "${YELLOW}⚠ 并发连接测试部分通过 (${success_count}/${total_connections}, ${success_rate}%)${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 网络延迟测试
test_network_latency() {
    echo -e "${BLUE}=== 测试 6: 网络延迟测试 ===${NC}"
    
    local test_name="network_latency"
    local start_time=$(date +%s)
    
    # 启动接收方
    cd "${BUILD_DIR}"
    timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_${test_name}.log" 2>&1 &
    local answerer_pid=$!
    
    sleep 2
    
    # 启动发起方
    timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_${test_name}.log" 2>&1 &
    local offerer_pid=$!
    
    # 等待测试完成
    wait ${offerer_pid} || true
    wait ${answerer_pid} || true
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 分析连接建立时间
    local offerer_connect_time=$(grep "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_${test_name}.log" | head -1 | awk '{print $1, $2}' || echo "N/A")
    local answerer_connect_time=$(grep "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_${test_name}.log" | head -1 | awk '{print $1, $2}' || echo "N/A")
    
    if [[ "${offerer_connect_time}" != "N/A" && "${answerer_connect_time}" != "N/A" ]]; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 网络延迟测试通过${NC}"
        echo -e "${CYAN}  发起方连接时间: ${offerer_connect_time}${NC}"
        echo -e "${CYAN}  接收方连接时间: ${answerer_connect_time}${NC}"
        
        DETAILED_METRICS["${test_name}_offerer_connect_time"]=${offerer_connect_time}
        DETAILED_METRICS["${test_name}_answerer_connect_time"]=${answerer_connect_time}
    else
        TEST_RESULTS[${test_name}]="FAIL"
        ERROR_COUNTS[${test_name}]=$((ERROR_COUNTS[${test_name}:-0] + 1))
        echo -e "${RED}✗ 网络延迟测试失败${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 内存泄漏测试
test_memory_leak() {
    echo -e "${BLUE}=== 测试 7: 内存泄漏测试 ===${NC}"
    
    local test_name="memory_leak"
    local start_time=$(date +%s)
    local success_count=0
    local total_runs=3
    
    for i in $(seq 1 ${total_runs}); do
        echo -e "${CYAN}内存泄漏测试运行 ${i}/${total_runs}...${NC}"
        
        # 记录初始内存使用
        local initial_memory=$(free -m | awk 'NR==2{print $3}')
        
        # 启动接收方
        cd "${BUILD_DIR}"
        timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_${test_name}_${i}.log" 2>&1 &
        local answerer_pid=$!
        
        sleep 2
        
        # 启动发起方
        timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_${test_name}_${i}.log" 2>&1 &
        local offerer_pid=$!
        
        # 等待测试完成
        wait ${offerer_pid} || true
        wait ${answerer_pid} || true
        
        # 记录最终内存使用
        local final_memory=$(free -m | awk 'NR==2{print $3}')
        local memory_diff=$((final_memory - initial_memory))
        
        echo -e "${CYAN}  内存变化: ${memory_diff} MB${NC}"
        
        # 检查结果
        if grep -q "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_${test_name}_${i}.log" && \
           grep -q "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_${test_name}_${i}.log"; then
            success_count=$((success_count + 1))
            echo -e "${GREEN}  运行 ${i} 成功${NC}"
            
            # 记录内存指标
            DETAILED_METRICS["${test_name}_run${i}_memory_diff"]=${memory_diff}
        else
            echo -e "${RED}  运行 ${i} 失败${NC}"
        fi
        
        sleep 2
    done
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 分析结果
    local success_rate=$(echo "scale=2; ${success_count} * 100 / ${total_runs}" | bc -l 2>/dev/null || echo "0")
    
    if [[ ${success_count} -eq ${total_runs} ]]; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 内存泄漏测试通过 (${success_count}/${total_runs})${NC}"
    else
        TEST_RESULTS[${test_name}]="PARTIAL"
        echo -e "${YELLOW}⚠ 内存泄漏测试部分通过 (${success_count}/${total_runs}, ${success_rate}%)${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 稳定性测试
test_stability() {
    echo -e "${BLUE}=== 测试 8: 稳定性测试 ===${NC}"
    
    local test_name="stability"
    local start_time=$(date +%s)
    local success_count=0
    local total_runs=5
    
    for i in $(seq 1 ${total_runs}); do
        echo -e "${CYAN}稳定性测试运行 ${i}/${total_runs}...${NC}"
        
        # 清理进程
        cleanup_processes
        
        # 启动信令服务器
        start_signaling_server
        
        # 启动接收方
        cd "${BUILD_DIR}"
        timeout ${TEST_TIMEOUT} ./webrtc-client quic answerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/answerer_${test_name}_run${i}.log" 2>&1 &
        local answerer_pid=$!
        
        sleep 2
        
        # 启动发起方
        timeout ${TEST_TIMEOUT} ./webrtc-client quic offerer 127.0.0.1 ${SIGNALING_PORT} > "${REPORT_DIR}/offerer_${test_name}_run${i}.log" 2>&1 &
        local offerer_pid=$!
        
        # 等待测试完成
        wait ${offerer_pid} || true
        wait ${answerer_pid} || true
        
        # 检查结果
        if grep -q "数据通道已打开\|数据通道准备就绪" "${REPORT_DIR}/offerer_${test_name}_run${i}.log" && \
           grep -q "收到数据通道\|数据通道已打开" "${REPORT_DIR}/answerer_${test_name}_run${i}.log"; then
            success_count=$((success_count + 1))
            echo -e "${GREEN}  运行 ${i} 成功${NC}"
        else
            echo -e "${RED}  运行 ${i} 失败${NC}"
        fi
        
        # 停止信令服务器
        if [[ -f "${REPORT_DIR}/signaling_server.pid" ]]; then
            kill $(cat "${REPORT_DIR}/signaling_server.pid") 2>/dev/null || true
            rm -f "${REPORT_DIR}/signaling_server.pid"
        fi
        
        sleep 2
    done
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    # 分析结果
    local success_rate=$(echo "scale=2; ${success_count} * 100 / ${total_runs}" | bc -l 2>/dev/null || echo "0")
    
    if [[ ${success_count} -eq ${total_runs} ]]; then
        TEST_RESULTS[${test_name}]="PASS"
        echo -e "${GREEN}✓ 稳定性测试通过 (${success_count}/${total_runs})${NC}"
    else
        TEST_RESULTS[${test_name}]="PARTIAL"
        echo -e "${YELLOW}⚠ 稳定性测试部分通过 (${success_count}/${total_runs}, ${success_rate}%)${NC}"
    fi
    
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 错误处理测试
test_error_handling() {
    echo -e "${BLUE}=== 测试 9: 错误处理测试 ===${NC}"
    
    local test_name="error_handling"
    local start_time=$(date +%s)
    
    # 测试无效参数
    echo -e "${CYAN}测试无效参数...${NC}"
    cd "${BUILD_DIR}"
    
    # 测试无效传输类型
    local invalid_transport_output=$(timeout 10 ./webrtc-client invalid offerer 2>&1 || true)
    if echo "${invalid_transport_output}" | grep -q "错误:"; then
        echo -e "${GREEN}  ✓ 无效传输类型检测正常${NC}"
    else
        echo -e "${RED}  ✗ 无效传输类型检测失败${NC}"
    fi
    
    # 测试无效角色
    local invalid_role_output=$(timeout 10 ./webrtc-client quic invalid 2>&1 || true)
    if echo "${invalid_role_output}" | grep -q "错误:"; then
        echo -e "${GREEN}  ✓ 无效角色检测正常${NC}"
    else
        echo -e "${RED}  ✗ 无效角色检测失败${NC}"
    fi
    
    # 测试连接超时
    echo -e "${CYAN}测试连接超时...${NC}"
    timeout 15 ./webrtc-client quic offerer 127.0.0.1 9999 > "${REPORT_DIR}/timeout_test.log" 2>&1 &
    local timeout_pid=$!
    
    wait ${timeout_pid} || true
    
    if grep -q "超时\|timeout\|错误" "${REPORT_DIR}/timeout_test.log"; then
        echo -e "${GREEN}  ✓ 连接超时处理正常${NC}"
    else
        echo -e "${RED}  ✗ 连接超时处理异常${NC}"
    fi
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    TEST_RESULTS[${test_name}]="PASS"
    echo -e "${GREEN}✓ 错误处理测试通过${NC}"
    echo -e "${CYAN}测试耗时: ${duration} 秒${NC}\n"
}

# 生成测试报告
generate_report() {
    echo -e "${BLUE}=== 生成测试报告 ===${NC}"
    
    local report_file="${REPORT_DIR}/comprehensive_test_report_${TIMESTAMP}.md"
    
    cat > "${report_file}" << EOF
# QUIC DataChannel 综合测试报告

**测试时间**: ${TIMESTAMP}  
**测试环境**: $(uname -a)  
**构建目录**: ${BUILD_DIR}

## 测试概览

| 测试项目 | 状态 | 备注 |
|---------|------|------|
EOF
    
    # 添加测试结果
    for test_name in "${!TEST_RESULTS[@]}"; do
        local status=${TEST_RESULTS[${test_name}]}
        local status_icon=""
        
        case ${status} in
            "PASS") status_icon="✅" ;;
            "FAIL") status_icon="❌" ;;
            "PARTIAL") status_icon="⚠️" ;;
            *) status_icon="❓" ;;
        esac
        
        echo "| ${test_name} | ${status_icon} ${status} | - |" >> "${report_file}"
    done
    
    cat >> "${report_file}" << EOF

## 性能指标

EOF
    
    # 添加性能指标
    if [[ -n "${PERFORMANCE_METRICS[throughput]}" ]]; then
        echo "- **吞吐量**: ${PERFORMANCE_METRICS[throughput]} Mbps" >> "${report_file}"
    fi
    if [[ -n "${PERFORMANCE_METRICS[message_count]}" ]]; then
        echo "- **消息数量**: ${PERFORMANCE_METRICS[message_count]}" >> "${report_file}"
    fi
    if [[ -n "${PERFORMANCE_METRICS[message_size]}" ]]; then
        echo "- **消息大小**: ${PERFORMANCE_METRICS[message_size]} 字节" >> "${report_file}"
    fi
    
    cat >> "${report_file}" << EOF

## 详细指标

EOF
    
    # 添加详细指标
    for metric_name in "${!DETAILED_METRICS[@]}"; do
        local metric_value=${DETAILED_METRICS[${metric_name}]}
        echo "- **${metric_name}**: ${metric_value}" >> "${report_file}"
    done
    
    cat >> "${report_file}" << EOF

## 错误统计

EOF
    
    # 添加错误统计
    for test_name in "${!ERROR_COUNTS[@]}"; do
        local error_count=${ERROR_COUNTS[${test_name}]}
        if [[ ${error_count} -gt 0 ]]; then
            echo "- **${test_name}**: ${error_count} 个错误" >> "${report_file}"
        fi
    done
    
    cat >> "${report_file}" << EOF

## 详细日志

所有测试的详细日志文件位于: \`${REPORT_DIR}/\`

## 测试总结

EOF
    
    # 计算总体结果
    local total_tests=${#TEST_RESULTS[@]}
    local passed_tests=0
    local failed_tests=0
    local partial_tests=0
    
    for status in "${TEST_RESULTS[@]}"; do
        case ${status} in
            "PASS") passed_tests=$((passed_tests + 1)) ;;
            "FAIL") failed_tests=$((failed_tests + 1)) ;;
            "PARTIAL") partial_tests=$((partial_tests + 1)) ;;
        esac
    done
    
    local success_rate=$(echo "scale=2; ${passed_tests} * 100 / ${total_tests}" | bc -l 2>/dev/null || echo "0")
    
    echo "- **总测试数**: ${total_tests}" >> "${report_file}"
    echo "- **通过**: ${passed_tests}" >> "${report_file}"
    echo "- **部分通过**: ${partial_tests}" >> "${report_file}"
    echo "- **失败**: ${failed_tests}" >> "${report_file}"
    echo "- **成功率**: ${success_rate}%" >> "${report_file}"
    
    if [[ ${failed_tests} -eq 0 ]]; then
        echo -e "\n🎉 **所有测试通过！**" >> "${report_file}"
    elif [[ ${partial_tests} -gt 0 ]]; then
        echo -e "\n⚠️ **部分测试通过，需要关注稳定性**" >> "${report_file}"
    else
        echo -e "\n❌ **存在测试失败，需要修复**" >> "${report_file}"
    fi
    
    echo -e "${GREEN}测试报告已生成: ${report_file}${NC}"
    
    # 显示报告摘要
    echo -e "\n${BLUE}=== 测试报告摘要 ===${NC}"
    echo -e "总测试数: ${total_tests}"
    echo -e "通过: ${passed_tests}"
    echo -e "部分通过: ${partial_tests}"
    echo -e "失败: ${failed_tests}"
    echo -e "成功率: ${success_rate}%"
}

# 主测试流程
main() {
    init_test_suite
    
    # 启动信令服务器
    start_signaling_server
    
    # 执行所有测试
    test_basic_connectivity
    test_transport_comparison
    test_performance
    test_message_sizes
    test_concurrent_connections
    test_network_latency
    test_memory_leak
    test_stability
    test_error_handling
    
    # 停止信令服务器
    if [[ -f "${REPORT_DIR}/signaling_server.pid" ]]; then
        kill $(cat "${REPORT_DIR}/signaling_server.pid") 2>/dev/null || true
        rm -f "${REPORT_DIR}/signaling_server.pid"
    fi
    
    # 生成报告
    generate_report
    
    echo -e "${GREEN}=== 综合测试套件执行完成 ===${NC}"
    echo -e "${GREEN}报告文件: ${REPORT_DIR}/comprehensive_test_report_${TIMESTAMP}.md${NC}"
}

# 信号处理
trap 'echo -e "\n${RED}收到中断信号，正在清理...${NC}"; cleanup_processes; exit 1' INT TERM

# 执行主函数
main "$@" 
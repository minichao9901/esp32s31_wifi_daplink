/*
 * wifi_dap_main.c —— ESP32-S31 上的**无线** CMSIS-DAP 探针（WiFi + TCP）
 *
 *   主机侧：OpenOCD  ──TCP──>  本固件  ──SWD 17~22MHz──>  目标（STM32F103 等）
 *
 * 线上协议 = OpenOCD 的 `cmsis-dap backend tcp`（0.12 起上游主线自带，
 * 源码 src/jtag/drivers/cmsis_dap_tcp.c）。一包 8 字节头 + 载荷，全部小端：
 *
 *      [44 41 50 00]   "DAP"（0x00504144）—— 认包用的魔数
 *      [len_lo len_hi] 载荷长度（**不含** 8 字节头）
 *      [type]          0x01 = 请求（主机→探针）  0x02 = 响应（探针→主机）
 *      [reserved]      0
 *      payload         ← 与 USB 版**逐字节相同**的 CMSIS-DAP 命令 / 响应
 *
 * 所以这一层只干三件事：收命令 → DAP_ExecuteCommand() → 回响应。
 * SWD 位时序完全是 cherrydap_s31_fast 那一套（dedicated GPIO + 无分支展开汇编），
 * 一行没改 —— 换句话说：**探针的"手感"和 USB 版一模一样，只是线变长了**。
 *
 * 实测（2026-09-21，见 README）：握手第一包就是 DAP_Info(0xF0)：
 *      OpenOCD 发  44 41 50 00 02 00 01 00 | 00 F0
 *      探针应回    44 41 50 00 01 00 02 00 | 01      （载荷 = DAP_PACKET_COUNT）
 */

#include <string.h>
#include <errno.h>

#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_chip_info.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"

#include "DAP.h"
#include "DAP_config.h"

/* dedic_selftest.c：高速位时序的开机微基准标定（只测不碰引脚） */
extern void dedic_selftest_run(void);

/* DAP_config.h 里 DAP_GetSerNumString() 读的全局序列号（开机用芯片 MAC 填） */
char g_dap_serial[13] = "000000000000";

static const char *TAG = "cherrydap-wifi";

/* ------------------------------------------------------------------ *
 *  协议常量（对齐上游 cmsis_dap_tcp.c）
 * ------------------------------------------------------------------ */
#define DAP_PKT_HDR_SIGNATURE   0x00504144u   /* "DAP"，小端字节序 44 41 50 00 */
#define DAP_PKT_TYPE_REQUEST    0x01
#define DAP_PKT_TYPE_RESPONSE   0x02
#define DAP_HDR_SIZE            8

#define DAP_TCP_PORT            CONFIG_CHERRYDAP_TCP_PORT

/* 1 = 每收到一条命令就打一行（opcode / 请求长度 / 响应长度）。
 * 排查协议错位（OpenOCD 报 "CMSIS-DAP command mismatch"）时打开 —— 正常情况下置 0，
 * 因为日志走 USB-Serial/JTAG，一行要 ~ms，会明显拖慢吞吐。 */
#define DAP_TCP_TRACE           0
#define DAP_TCP_TRACE_LIMIT     400

/* 收命令 / 发响应各一块；响应要额外留 8 字节放头（一次 send 发完，少一个 TCP 段） */
static uint8_t s_req[DAP_PACKET_SIZE];
static uint8_t s_out[DAP_HDR_SIZE + DAP_PACKET_SIZE];

/* 诊断统计（每次会话结束打一行） */
static struct {
    uint32_t cmds;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    int64_t  svc_us_max;
    int64_t  svc_us_sum;
} s_stat;

/* ------------------------------------------------------------------ *
 *  WiFi
 * ------------------------------------------------------------------ */
static EventGroupHandle_t s_wifi_eg;
#define WIFI_GOT_IP_BIT     BIT0
#define WIFI_AP_UP_BIT      BIT1

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "已连上 AP，等 DHCP ...");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "掉线（reason=%d），2s 后重连", d->reason);
            xEventGroupClearBits(s_wifi_eg, WIFI_GOT_IP_BIT);
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *c = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "客户端接入：" MACSTR, MAC2STR(c->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *c = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "客户端离开：" MACSTR, MAC2STR(c->mac));
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "拿到 IP：" IPSTR "（探针地址 = " IPSTR ":%d）",
                 IP2STR(&e->ip_info.ip), IP2STR(&e->ip_info.ip), DAP_TCP_PORT);
        ESP_LOGI(TAG, "OpenOCD 这样连：-c \"adapter driver cmsis-dap\" -c \"cmsis-dap backend tcp\" "
                      "-c \"cmsis-dap tcp host " IPSTR "\" -c \"cmsis-dap tcp port %d\"",
                 IP2STR(&e->ip_info.ip), DAP_TCP_PORT);
        xEventGroupSetBits(s_wifi_eg, WIFI_GOT_IP_BIT);
    }
}

static void wifi_start_ap(void);   /* 前向声明：STA 连不上时退回它 */

static void wifi_start_sta(void)
{
    esp_netif_create_default_wifi_sta();
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_set_hostname(netif, "cherrydap-s31");

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, CONFIG_CHERRYDAP_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_CHERRYDAP_WIFI_PASSWORD, sizeof(wc.sta.password));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "STA 模式：加入 \"%s\" ...", CONFIG_CHERRYDAP_WIFI_SSID);

    /* ⚠️ 这里**只等一小会儿**；等不到也不是死路 —— 直接退回 SoftAP 兜底。
     *
     * 为什么要有兜底：**STA 模式（接你家路由器）才是日常用法** ——
     * 电脑不用切网、互联网/远程会话都不受影响，探针跟电脑在同一个局域网里说话。
     * 但万一 SSID/密码填错、或者路由器不在，探针就彻底失联了；
     * 退回自开热点（192.168.4.1）至少还留一条路，能连上去改配置。
     *
     * 另外：早先版本在这里死等 30s 才起 TCP 服务，DHCP 一慢就"看着像探针没起来"；
     * 现在监听已经在 app_main 里先起来了（见那里的顺序注释），IP 一到就能连。 */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg, WIFI_GOT_IP_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(20000));
    if (!(bits & WIFI_GOT_IP_BIT)) {
        ESP_LOGW(TAG, "20s 没拿到 IP（SSID=\"%s\"）→ 退回 SoftAP 兜底；"
                      "要接自家 WiFi 就检查 sdkconfig 里的 CHERRYDAP_WIFI_SSID/PASSWORD",
                 CONFIG_CHERRYDAP_WIFI_SSID);
        wifi_start_ap();
    }
}

static void wifi_start_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.ap.ssid, CONFIG_CHERRYDAP_AP_SSID, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(CONFIG_CHERRYDAP_AP_SSID);
    wc.ap.channel = 6;
    wc.ap.max_connection = 4;
    wc.ap.authmode = WIFI_AUTH_OPEN;

    const char *pass = CONFIG_CHERRYDAP_AP_PASSWORD;
    if (strlen(pass) == 0) {
        ESP_LOGW(TAG, "SoftAP 密码为空 → 开放热点（谁都能连，调试可以，长期用请设密码）");
    } else if (strlen(pass) < 8) {
        ESP_LOGE(TAG, "SoftAP 密码 %d 位 < 8，WPA2 不接受 → 退回开放热点", (int)strlen(pass));
    } else {
        strlcpy((char *)wc.ap.password, pass, sizeof(wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_netif_ip_info_t ip = { 0 };
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"), &ip);
    ESP_LOGI(TAG, "SoftAP 模式：热点 \"%s\"（%s）已开，探针地址 = " IPSTR ":%d",
             CONFIG_CHERRYDAP_AP_SSID,
             wc.ap.authmode == WIFI_AUTH_OPEN ? "开放" : "WPA2",
             IP2STR(&ip.ip), DAP_TCP_PORT);
    ESP_LOGI(TAG, "OpenOCD 这样连：-c \"adapter driver cmsis-dap\" -c \"cmsis-dap backend tcp\" "
                  "-c \"cmsis-dap tcp host " IPSTR "\" -c \"cmsis-dap tcp port %d\"",
             IP2STR(&ip.ip), DAP_TCP_PORT);
    xEventGroupSetBits(s_wifi_eg, WIFI_AP_UP_BIT);
}

/* 网络栈 + 事件循环：**必须在建 socket 之前**做（app_main 里最先调）
 * ⚠️ 别叫 netif_init —— lwIP 自己有同名函数，会撞。 */
static void net_stack_init(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
}

/* 真正起 WiFi（netif_init() 之后调；STA 还是 AP 看有没有配 SSID） */
static void wifi_start(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    /* 🚨 关省电：默认的 modem sleep 会把响应拖到几十 ms，OpenOCD 的 tcp min_timeout 一超
     *    就报 "command mismatch"。参考实现也专门做了这一步。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    if (strlen(CONFIG_CHERRYDAP_WIFI_SSID) > 0) {
        wifi_start_sta();
    } else {
        wifi_start_ap();
    }
}

/* ------------------------------------------------------------------ *
 *  TCP：CMSIS-DAP over TCP
 * ------------------------------------------------------------------ */
static int recv_all(int fd, void *buf, size_t n)
{
    uint8_t *p = (uint8_t *)buf;
    while (n) {
        int r = recv(fd, p, n, 0);
        if (r <= 0) {
            return r;
        }
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static int send_all(int fd, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    while (n) {
        int r = send(fd, p, n, 0);
        if (r <= 0) {
            return r;
        }
        p += r;
        n -= (size_t)r;
    }
    return 1;
}

static inline void hdr_put(uint8_t *h, uint16_t len, uint8_t type)
{
    h[0] = (uint8_t)(DAP_PKT_HDR_SIGNATURE >> 0);
    h[1] = (uint8_t)(DAP_PKT_HDR_SIGNATURE >> 8);
    h[2] = (uint8_t)(DAP_PKT_HDR_SIGNATURE >> 16);
    h[3] = (uint8_t)(DAP_PKT_HDR_SIGNATURE >> 24);
    h[4] = (uint8_t)(len >> 0);
    h[5] = (uint8_t)(len >> 8);
    h[6] = type;
    h[7] = 0;                                   /* reserved */
}

/* 一次会话：一路收命令、回响应，直到对端关闭或协议出错 */
static void dap_session(int fd)
{
    uint8_t hdr[DAP_HDR_SIZE];
    int64_t t0 = esp_timer_get_time();

    while (1) {
        int r = recv_all(fd, hdr, sizeof(hdr));
        if (r == 0) {
            ESP_LOGI(TAG, "OpenOCD 断开连接");
            break;
        }
        if (r < 0) {
            ESP_LOGW(TAG, "recv 出错：errno=%d", errno);
            break;
        }

        uint32_t sig = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                       ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        uint16_t len = (uint16_t)(hdr[4] | (hdr[5] << 8));
        if (sig != DAP_PKT_HDR_SIGNATURE) {
            ESP_LOGE(TAG, "包头签名错：0x%08X（应为 0x00504144 \"DAP\"）—— 对端不是 cmsis-dap tcp 后端？", sig);
            break;
        }
        if (hdr[6] != DAP_PKT_TYPE_REQUEST) {
            ESP_LOGE(TAG, "包类型错：0x%02X（应为 0x01 请求）", hdr[6]);
            break;
        }
        if (len > sizeof(s_req)) {
            ESP_LOGE(TAG, "命令太长：%u > %u（DAP_PACKET_SIZE）", len, (unsigned)sizeof(s_req));
            break;
        }
        if (len && recv_all(fd, s_req, len) <= 0) {
            break;
        }

        /* ---- 真正干活的就这一行：CMSIS-DAP 协议层（与 USB 版同一个函数） ---- */
        int64_t t = esp_timer_get_time();
        uint32_t res = DAP_ExecuteCommand(s_req, s_out + DAP_HDR_SIZE);
        int64_t dt = esp_timer_get_time() - t;

        uint16_t rlen = (uint16_t)(res & 0xFFFFu);
#if DAP_TCP_TRACE
        if (s_stat.cmds < DAP_TCP_TRACE_LIMIT) {
            ESP_LOGI(TAG, "#%-3u 收 %-4u B cmd=0x%02X 回 %-4u B", (unsigned)s_stat.cmds,
                     len, s_req[0], rlen);
        }
#endif
        hdr_put(s_out, rlen, DAP_PKT_TYPE_RESPONSE);

        /* 头 + 载荷一次发出去：一问一答的协议里，多一个 TCP 段就是多一份延迟 */
        if (send_all(fd, s_out, DAP_HDR_SIZE + rlen) <= 0) {
            ESP_LOGW(TAG, "send 出错：errno=%d", errno);
            break;
        }

        s_stat.cmds++;
        s_stat.rx_bytes += len;
        s_stat.tx_bytes += rlen;
        s_stat.svc_us_sum += dt;
        if (dt > s_stat.svc_us_max) {
            s_stat.svc_us_max = dt;
        }
        /* 每 500 条报一次：既能看进度，也能看"探针侧服务时间"跟网络延迟的比例 */
        if ((s_stat.cmds % 500) == 0) {
            ESP_LOGI(TAG, "%u 条命令：SWD 侧均值 %.0f us / 最大 %lld us（%.1f MB/s 主机→探针载荷）",
                     (unsigned)s_stat.cmds, (double)s_stat.svc_us_sum / s_stat.cmds,
                     (long long)s_stat.svc_us_max,
                     (double)s_stat.rx_bytes / (double)(esp_timer_get_time() - t0));
        }
    }

    ESP_LOGI(TAG, "会话结束：%u 条命令 / 收 %u B / 发 %u B / SWD 侧均值 %.0f us / 最大 %lld us / 共 %.1f s",
             (unsigned)s_stat.cmds, (unsigned)s_stat.rx_bytes, (unsigned)s_stat.tx_bytes,
             s_stat.cmds ? (double)s_stat.svc_us_sum / s_stat.cmds : 0.0,
             (long long)s_stat.svc_us_max,
             (double)(esp_timer_get_time() - t0) / 1e6);
}

static void dap_tcp_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "DAP 任务跑在 core %d（与 DAP_Setup() 同核 —— dedicated GPIO 的 bundle 按核绑定）",
             xPortGetCoreID());

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "socket() 失败：errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(DAP_TCP_PORT);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "bind(%d) 失败：errno=%d", DAP_TCP_PORT, errno);
        close(listen_fd);
        vTaskDelete(NULL);
        return;
    }
    listen(listen_fd, 4);
    ESP_LOGI(TAG, "CMSIS-DAP over TCP 已监听 :%d，等 OpenOCD 连进来", DAP_TCP_PORT);

    while (1) {
        struct sockaddr_in peer = { 0 };
        socklen_t plen = sizeof(peer);
        int fd = accept(listen_fd, (struct sockaddr *)&peer, &plen);
        if (fd < 0) {
            ESP_LOGW(TAG, "accept 失败：errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 🚨 TCP_NODELAY：一问一答的协议绝不能攒包（Nagle 会平白加几十 ms） */
        int nodelay = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

        ESP_LOGI(TAG, "OpenOCD 已连接：%s:%d", inet_ntoa(peer.sin_addr), ntohs(peer.sin_port));
        dap_session(fd);
        close(fd);

        /* 每次会话重新起算统计（OpenOCD 每次运行会新开一条连接） */
        s_stat.cmds = s_stat.rx_bytes = s_stat.tx_bytes = 0;
        s_stat.svc_us_sum = 0;
        s_stat.svc_us_max = 0;
    }
}

/* ------------------------------------------------------------------ *
 *  入口
 * ------------------------------------------------------------------ */
void app_main(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    /* NVS 给 WiFi 驱动存校准数据用 */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_LOGI(TAG, "ESP32-S31 无线 CMSIS-DAP 探针启动（%d 核 / %d MHz / 固件 %s）",
             chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, DAP_FW_VER);

    /* 高速位时序的开机标定：把这块核的指令/CSR 时序量出来打在日志里（只测不碰引脚） */
    dedic_selftest_run();

    /* DAP 协议层 + SWD 引脚（内部会做 dedicated GPIO 路由 + nRESET 开漏） */
    DAP_Setup();
    ESP_LOGI(TAG, "SWD：SWCLK=GPIO%d  SWDIO=GPIO%d  nRESET=GPIO%d（dedicated GPIO + 展开汇编）",
             DAP_PIN_SWCLK, DAP_PIN_SWDIO, DAP_PIN_nRESET);

    /* 序列号用芯片 MAC，多块板同时在线不会打架 */
    uint8_t mac[6] = { 0 };
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(g_dap_serial, sizeof(g_dap_serial), "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "探针序列号 = %s", g_dap_serial);

    /* 顺序有讲究：
     *   ① netif_init()  —— 网络栈/事件循环必须先起来，否则下面任务里的 socket() 直接失败
     *      （踩过：把 wifi_start() 放最后、任务放前面 → 监听压根没起来，PC 怎么连都连不上）
     *   ② 起 TCP 监听任务（钉 core 0，见下）
     *   ③ wifi_start() —— 关联/DHCP 慢也不影响已经起来的监听 */
    net_stack_init();

    /* 🚨 任务必须钉 core 0：DAP_Connect 会在**这个任务**里重装 dedicated GPIO 路由，
     *    而 bundle 是按核绑定的（app_main/DAP_Setup() 在 core 0 建的）。
     *    优先级 18 < WiFi 任务的 23：WiFi 该抢占就抢占，位时序只在关中断那 ~3µs 里霸道。 */
    xTaskCreatePinnedToCore(&dap_tcp_task, "dap-tcp", 6144, NULL, 18, NULL, 0);

    wifi_start();
}

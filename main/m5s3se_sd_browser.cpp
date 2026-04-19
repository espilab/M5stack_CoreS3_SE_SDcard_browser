// M5s3se_SD_browser/main/m5s3se_sd_browser.cpp
//
// M5Stack CoreS3 SE (ESP32-S3) 用 SDカードファイルブラウザ
// ESP-IDF v5.5.2 対応版

/*
 * 機能:
 *   - USB NCM LANアダプタ + DHCP（rawパケット自前処理）
 *   - http://192.168.7.1/ でSDカードファイル一覧を表示
 *   - DIRボタンでファイル一覧をリスト表示（日付・時刻・サイズ・ファイル名）
 *   - ファイルタップでダウンロード、uploadボタンでアップロード、etc.
 *
 *  NOTE:SDカードは FAT32フォーマットであること。exFATその他は非対応。
 */
#define VERSION "1.0.0"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <utime.h>
#include <time.h>

#include <M5Unified.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "esp_mac.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/inet.h"
#include "netif/etharp.h"

#include "esp_http_server.h"

#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "tinyusb_net.h"
#include "tinyusb_cdc_acm.h"

static const char *TAG = "SD_WEB";

static struct netif s_netif;
static uint8_t  s_server_mac[6];
static uint32_t s_server_ip;
static uint32_t s_client_ip;

#define CLIENT_IP_A 192
#define CLIENT_IP_B 168
#define CLIENT_IP_C 7
#define CLIENT_IP_D 100

typedef enum {
    DHCP_STATE_STOPPED = 0,
    DHCP_STATE_RUNNING,
} dhcp_state_t;
static volatile dhcp_state_t s_dhcp_state = DHCP_STATE_STOPPED;

typedef struct {
    uint8_t  *data;
    uint16_t  len;
} rx_pkt_t;

#define RX_QUEUE_LEN 16
static QueueHandle_t s_rx_queue = NULL;

// SD カード初期化済みフラグ
static bool s_sd_ready  = false;
static sdmmc_card_t *s_sd_card = NULL;  // アンマウント用
static volatile bool s_clock_set = false; // PCから時刻がセットされたか

// LCD容量表示用（DIR完了時に更新）
static volatile uint64_t s_lcd_total = 0;
static volatile uint64_t s_lcd_used  = 0;
static volatile uint64_t s_lcd_free  = 0;
static volatile bool     s_lcd_cap_updated = false;

// -----------------------------------------------------------------------
// DHCP rawパケット処理
// -----------------------------------------------------------------------
#pragma pack(push, 1)
typedef struct {
    uint8_t  op, htype, hlen, hops;
    uint32_t xid;
    uint16_t secs, flags;
    uint32_t ciaddr, yiaddr, siaddr, giaddr;
    uint8_t  chaddr[16];
    uint8_t  sname[64];
    uint8_t  file[128];
    uint8_t  options[312];
} dhcp_pkt_t;

typedef struct {
    uint8_t  dst_mac[6];
    uint8_t  src_mac[6];
    uint16_t ethertype;
    uint8_t  ip_ver_ihl;
    uint8_t  ip_tos;
    uint16_t ip_len;
    uint16_t ip_id;
    uint16_t ip_frag;
    uint8_t  ip_ttl;
    uint8_t  ip_proto;
    uint16_t ip_cksum;
    uint32_t ip_src;
    uint32_t ip_dst;
    uint16_t udp_src;
    uint16_t udp_dst;
    uint16_t udp_len;
    uint16_t udp_cksum;
    dhcp_pkt_t dhcp;
} eth_frame_t;
#pragma pack(pop)

static uint16_t ip_checksum(const void *data, int len)
{
    const uint16_t *p = (const uint16_t *)data;
    uint32_t sum = 0;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const uint8_t *)p;
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return (uint16_t)(~sum);
}

static void send_dhcp_reply(const dhcp_pkt_t *req, uint8_t msg_type,
                             const uint8_t *client_mac)
{
    static eth_frame_t frame;
    memset(&frame, 0, sizeof(frame));

    memcpy(frame.dst_mac, client_mac, 6);
    memcpy(frame.src_mac, s_server_mac, 6);
    frame.ethertype = lwip_htons(0x0800);

    dhcp_pkt_t *rep = &frame.dhcp;
    rep->op = 2; rep->htype = 1; rep->hlen = 6;
    rep->xid = req->xid; rep->flags = 0;
    rep->yiaddr = s_client_ip;
    rep->siaddr = s_server_ip;
    memcpy(rep->chaddr, req->chaddr, 6);

    rep->options[0]=99; rep->options[1]=130;
    rep->options[2]=83; rep->options[3]=99;

    uint8_t *opt = &rep->options[4];
    *opt++=53; *opt++=1; *opt++=msg_type;
    *opt++=54; *opt++=4; memcpy(opt,&s_server_ip,4); opt+=4;
    uint32_t lease=lwip_htonl(3600);
    *opt++=51; *opt++=4; memcpy(opt,&lease,4); opt+=4;
    uint32_t mask=lwip_htonl(0xFFFFFF00);
    *opt++=1;  *opt++=4; memcpy(opt,&mask,4); opt+=4;
    *opt++=3;  *opt++=4; memcpy(opt,&s_server_ip,4); opt+=4;
    *opt++=6;  *opt++=4; memcpy(opt,&s_server_ip,4); opt+=4;
    *opt++=255;

    uint16_t udp_len  = 8 + sizeof(dhcp_pkt_t);
    uint16_t ip_len   = 20 + udp_len;

    frame.udp_src=lwip_htons(67); frame.udp_dst=lwip_htons(68);
    frame.udp_len=lwip_htons(udp_len); frame.udp_cksum=0;

    frame.ip_ver_ihl=0x45; frame.ip_tos=0;
    frame.ip_len=lwip_htons(ip_len); frame.ip_id=0; frame.ip_frag=0;
    frame.ip_ttl=64; frame.ip_proto=17; frame.ip_cksum=0;
    frame.ip_src=s_server_ip; frame.ip_dst=0xFFFFFFFF;
    frame.ip_cksum=ip_checksum(&frame.ip_ver_ihl, 20);

    uint16_t frame_len = 14 + ip_len;
    tinyusb_net_send_sync(&frame, frame_len, NULL, pdMS_TO_TICKS(100));
}

static void handle_dhcp_packet(const uint8_t *frame, uint16_t len)
{
    if (len < 14+20+8+240) return;
    uint16_t ethertype = (frame[12]<<8)|frame[13];
    if (ethertype != 0x0800) return;
    const uint8_t *ip = frame+14;
    if (ip[9] != 17) return;
    uint8_t ihl = (ip[0]&0x0f)*4;
    const uint8_t *udp = ip+ihl;
    uint16_t dst_port = (udp[2]<<8)|udp[3];
    if (dst_port != 67) return;

    const dhcp_pkt_t *dhcp = (const dhcp_pkt_t *)(udp+8);
    if (dhcp->op != 1) return;
    if (dhcp->options[0]!=99||dhcp->options[1]!=130||
        dhcp->options[2]!=83||dhcp->options[3]!=99) return;

    uint8_t msg_type = 0;
    const uint8_t *opt = &dhcp->options[4];
    const uint8_t *end = &dhcp->options[308];
    while (opt<end && *opt!=255) {
        if (*opt==0){opt++;continue;}
        uint8_t code=*opt++; if(opt>=end)break;
        uint8_t olen=*opt++;
        if(code==53&&olen>=1) msg_type=*opt;
        opt+=olen;
    }

    const uint8_t *client_mac = frame+6;
    if      (msg_type==1) send_dhcp_reply(dhcp, 2, client_mac);
    else if (msg_type==3) send_dhcp_reply(dhcp, 5, client_mac);
}

// -----------------------------------------------------------------------
// lwIP — 送受信
// -----------------------------------------------------------------------
static err_t netif_output(struct netif *netif, struct pbuf *p)
{
    esp_err_t ret;
    if (p->next == NULL) {
        ret = tinyusb_net_send_sync(p->payload, (uint16_t)p->len, NULL, pdMS_TO_TICKS(100));
    } else {
        uint8_t buf[1600]; uint16_t total=0;
        for (struct pbuf *q=p; q!=NULL; q=q->next) {
            if (total+q->len>sizeof(buf)) break;
            memcpy(buf+total, q->payload, q->len); total+=q->len;
        }
        ret = tinyusb_net_send_sync(buf, total, NULL, pdMS_TO_TICKS(100));
    }
    return (ret==ESP_OK) ? ERR_OK : ERR_IF;
}

static err_t netif_init_func(struct netif *netif)
{
    netif->linkoutput = netif_output;
    netif->output     = etharp_output;
    netif->mtu        = 1500;
    netif->flags      = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP | NETIF_FLAG_LINK_UP;
    memcpy(netif->hwaddr, s_server_mac, 6);
    netif->hwaddr_len = ETH_HWADDR_LEN;
    return ERR_OK;
}

static esp_err_t usb_recv_callback(void *buffer, uint16_t len, void *ctx)
{
    uint8_t *copy = (uint8_t *)malloc(len);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    rx_pkt_t pkt = { .data=copy, .len=len };
    if (xQueueSendFromISR(s_rx_queue, &pkt, NULL) != pdTRUE) free(copy);
    return ESP_OK;
}

static void net_free_tx_buffer(void *eb, void *ctx) {}

static void lwip_rx_task(void *arg)
{
    rx_pkt_t pkt;
    while (1) {
        if (xQueueReceive(s_rx_queue, &pkt, portMAX_DELAY) == pdTRUE) {
            if (s_dhcp_state == DHCP_STATE_RUNNING)
                handle_dhcp_packet(pkt.data, pkt.len);

            struct pbuf *p = pbuf_alloc(PBUF_RAW, pkt.len, PBUF_POOL);
            if (p) {
                memcpy(p->payload, pkt.data, pkt.len);
                LOCK_TCPIP_CORE();
                if (s_netif.input(p, &s_netif) != ERR_OK) pbuf_free(p);
                UNLOCK_TCPIP_CORE();
            }
            free(pkt.data);
        }
    }
}

// 前方宣言
static void url_decode(char *dst, const char *src, size_t dst_size);

// -----------------------------------------------------------------------
// USB CDC シリアル出力ヘルパー
// -----------------------------------------------------------------------
static void cdc_log(const char *msg)
{
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
        (const uint8_t*)msg, strlen(msg));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
}

// -----------------------------------------------------------------------
// HTTP: POST /api/delete  ファイル/ディレクトリ削除
// Body: {"paths":["/path1","/path2",...]}
// -----------------------------------------------------------------------
static esp_err_t http_post_delete(httpd_req_t *req)
{
    cdc_log("DELETE\r\n");
    // リクエストボディを受信
    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }
    static char body[4096];
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) break;
        received += ret;
    }
    body[received] = '\0';

    // JSONから "paths" 配列を手動パース
    // 形式: {"paths":["/file1","/file2"]}
    int deleted = 0, failed = 0;

    char *arr_start = strstr(body, "\"paths\"");
    if (arr_start) {
        arr_start = strchr(arr_start, '[');
    }
    if (arr_start) {
        arr_start++; // '[' の次
        char *p = arr_start;
        while (*p && *p != ']') {
            // 文字列 "..." を探す
            char *qs = strchr(p, '"');
            if (!qs || qs[1] == '\0') break;
            char *qe = strchr(qs + 1, '"');
            if (!qe) break;

            // パスを取り出してURLデコード
            char enc_path[640] = "";
            int plen = (int)(qe - qs - 1);
            if (plen >= (int)sizeof(enc_path)) plen = sizeof(enc_path) - 1;
            memcpy(enc_path, qs + 1, plen);
            enc_path[plen] = '\0';

            char path[320];
            url_decode(path, enc_path, sizeof(path));

            // /sdcard を付加
            char full[328];
            strlcpy(full, "/sdcard", sizeof(full));
            strlcat(full, path, sizeof(full));

            // ファイルかディレクトリか判定して削除
            struct stat st;
            if (stat(full, &st) == 0) {
                int ret_del;
                if (S_ISDIR(st.st_mode)) {
                    ret_del = rmdir(full);
                } else {
                    ret_del = unlink(full);
                }
                if (ret_del == 0) {
                    deleted++;
                    ESP_LOGI(TAG, "Deleted: %s", full);
                    { char _log[340]; snprintf(_log,sizeof(_log),"DELETED %s\r\n",path); cdc_log(_log); }
                } else {
                    failed++;
                    ESP_LOGW(TAG, "Delete failed: %s (errno=%d)", full, errno);
                }
            } else {
                failed++;
                ESP_LOGW(TAG, "Not found: %s", full);
            }
            p = qe + 1;
        }
    }

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"deleted\":%d,\"failed\":%d}", deleted, failed);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// -----------------------------------------------------------------------
// SD カードマウント関数（初回起動時・再マウント時に使用）
// -----------------------------------------------------------------------
static bool sd_mount(void)
{
    // 既にマウント済みなら先にアンマウント
    if (s_sd_ready) {
        esp_vfs_fat_sdcard_unmount("/sdcard", s_sd_card);
        s_sd_card = NULL;
        //s_sd_ready = false;
        ESP_LOGI(TAG, "SD unmounted.");
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = GPIO_NUM_4;
    slot_cfg.host_id = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {};
    mount_cfg.format_if_mount_failed = false;
    mount_cfg.max_files              = 8;
    mount_cfg.allocation_unit_size  = 16 * 1024;

    sdmmc_card_t *sd_card = NULL;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;

    esp_err_t err = esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot_cfg,
                                             &mount_cfg, &sd_card);
    if (err == ESP_OK) {
        s_sd_ready = true;
        ESP_LOGI(TAG, "SD mounted: %lluMB",
                 (unsigned long long)sd_card->csd.capacity *
                 sd_card->csd.sector_size / (1024*1024));
    } else {
        s_sd_ready = false;
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(err));
    }
    return s_sd_ready;
}

// -----------------------------------------------------------------------
// HTTP: GET /api/mount  SDカード再マウント
// -----------------------------------------------------------------------
static esp_err_t http_get_mount(httpd_req_t *req)
{
    cdc_log("MOUNT\r\n");
    bool ok = sd_mount();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (ok) {
        httpd_resp_send(req, "{\"ok\":true,\"sd\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_send(req, "{\"ok\":false,\"sd\":false}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

// -----------------------------------------------------------------------
// SDカード ファイル一覧をJSON形式で返す
// -----------------------------------------------------------------------
static esp_err_t http_get_dir(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (!s_sd_ready) {
        httpd_resp_send(req, "{\"error\":\"SD card not ready\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // クエリパラメータからパスを取得（デフォルトは "/"）
    char path[320] = "/";
    char query[768];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char param_enc[640];
        if (httpd_query_key_value(query, "path", param_enc, sizeof(param_enc)) == ESP_OK) {
            url_decode(path, param_enc, sizeof(path));
        }
    }

    // SDカードのディレクトリを開く（マウントポイント /sdcard を付加）
    char full_path[328];
    if (strcmp(path, "/") == 0) {
        snprintf(full_path, sizeof(full_path), "/sdcard");
    } else {
        snprintf(full_path, sizeof(full_path), "/sdcard%s", path);
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        ESP_LOGW(TAG, "cannot open: %s (errno=%d)", full_path, errno);
        httpd_resp_send(req, "{\"error\":\"cannot open directory\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    // JSONをチャンク送信で構築
    // CDC: DIR操作をログ出力
    {
        char _log[328];
        snprintf(_log, sizeof(_log), "DIR %s\r\n", path);
        cdc_log(_log);
    }
    httpd_resp_sendstr_chunk(req, "{\"path\":\"");
    httpd_resp_sendstr_chunk(req, path);
    httpd_resp_sendstr_chunk(req, "\",\"files\":[");

    bool first = true;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        // . と .. だけスキップ（.gitignore等の隠しファイルは表示する）
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char entry_path[512];
        strlcpy(entry_path, full_path, sizeof(entry_path));
        strlcat(entry_path, "/", sizeof(entry_path));
        strlcat(entry_path, entry->d_name, sizeof(entry_path));

        struct stat st;
        stat(entry_path, &st);

        // 更新日時
        struct tm *tm_info = localtime(&st.st_mtime);
        char date_str[12] = "----/--/--";
        char time_str[10] = "--:--:--";
        if (tm_info && st.st_mtime > 0) {
            strftime(date_str, sizeof(date_str), "%Y-%m-%d", tm_info);
            strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
        }

        bool is_dir = (entry->d_type == DT_DIR);
        char buf[512];
        snprintf(buf, sizeof(buf),
            "%s{\"name\":\"%s\",\"date\":\"%s\",\"time\":\"%s\","
            "\"size\":%lu,\"dir\":%s}",
            first ? "" : ",",
            entry->d_name,
            date_str,
            time_str,
            (unsigned long)st.st_size,
            is_dir ? "true" : "false");

        httpd_resp_sendstr_chunk(req, buf);
        first = false;
    }
    closedir(dir);

    // SD カード容量情報を取得
    uint64_t total = 0, free_b = 0;
    char cap_buf[128] = ",\"total\":0,\"used\":0,\"free\":0";
    if (esp_vfs_fat_info("/sdcard", &total, &free_b) == ESP_OK) {
        uint64_t used = total - free_b;
        snprintf(cap_buf, sizeof(cap_buf),
            ",\"total\":%llu,\"used\":%llu,\"free\":%llu",
            (unsigned long long)total,
            (unsigned long long)used,
            (unsigned long long)free_b);
        // LCD容量表示用にグローバル変数を更新
        s_lcd_total = total;
        s_lcd_used  = used;
        s_lcd_free  = free_b;
        s_lcd_cap_updated = true;
    }

    httpd_resp_sendstr_chunk(req, "]");
    httpd_resp_sendstr_chunk(req, cap_buf);
    httpd_resp_sendstr_chunk(req, "}");
    httpd_resp_sendstr_chunk(req, NULL);  // 終端
    return ESP_OK;
}

// -----------------------------------------------------------------------
// HTTP: GET /api/download?path=/xxx  ファイルダウンロード
// -----------------------------------------------------------------------
// URLデコード（%XX形式を元の文字に戻す）
static void url_decode(char *dst, const char *src, size_t dst_size)
{
    size_t i = 0;
    while (*src && i < dst_size - 1) {
        if (*src == '%' && src[1] && src[2]) {
            char hex[3] = { src[1], src[2], 0 };
            dst[i++] = (char)strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static esp_err_t http_get_download(httpd_req_t *req)
{
    char query[768];       // 日本語パス対応のため大きめに
    char param_enc[640];   // エンコード済みパス
    char param[320];       // デコード後パス
    char file_path[328];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", param_enc, sizeof(param_enc)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_OK;
    }

    // URLデコード（%E6%97%A5... → 日本語UTF-8）
    url_decode(param, param_enc, sizeof(param));

    // /sdcard を先頭に付加
    snprintf(file_path, sizeof(file_path), "/sdcard%s", param);

    ESP_LOGI(TAG, "Download request: %s", file_path);
    {
        char _log[340];
        snprintf(_log, sizeof(_log), "DOWNLOAD %s\r\n", param);
        cdc_log(_log);
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "File not found: %s", file_path);
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_OK;
    }

    // ファイル名をContent-Dispositionに設定
    const char *fname = strrchr(param, '/');
    fname = fname ? fname + 1 : param;

    char disp[360];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", fname);

    // MIMEタイプ判定（拡張子で簡易判定）
    const char *ext = strrchr(fname, '.');
    const char *mime = "application/octet-stream";
    if (ext) {
        if      (strcasecmp(ext, ".txt")  == 0) mime = "text/plain; charset=utf-8";
        else if (strcasecmp(ext, ".csv")  == 0) mime = "text/csv; charset=utf-8";
        else if (strcasecmp(ext, ".json") == 0) mime = "application/json";
        else if (strcasecmp(ext, ".html") == 0) mime = "text/html";
        else if (strcasecmp(ext, ".jpg")  == 0 ||
                 strcasecmp(ext, ".jpeg") == 0) mime = "image/jpeg";
        else if (strcasecmp(ext, ".png")  == 0) mime = "image/png";
        else if (strcasecmp(ext, ".pdf")  == 0) mime = "application/pdf";
    }

    httpd_resp_set_type(req, mime);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_hdr(req, "Connection", "close");

    // Content-Length を設定（Python側の進捗表示のため）
    struct stat st;
    if (stat(file_path, &st) == 0) {
        char cl_buf[24];
        snprintf(cl_buf, sizeof(cl_buf), "%lu", (unsigned long)st.st_size);
        httpd_resp_set_hdr(req, "Content-Length", cl_buf);
    }

    // 4KBずつ読み込んでチャンク送信
    static uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, (const char *)buf, n) != ESP_OK) {
            ESP_LOGW(TAG, "Download send failed");
            break;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // 終端
    ESP_LOGI(TAG, "Download: %s", file_path);
    return ESP_OK;
}

// -----------------------------------------------------------------------
// HTTP: POST /api/upload  ファイルアップロード
// -----------------------------------------------------------------------
// HTTP: POST /api/upload  ファイルアップロード（ストリーミング版）
// -----------------------------------------------------------------------
static esp_err_t http_post_upload(httpd_req_t *req)
{
    cdc_log("UPLOAD START\r\n");
    // アップロード先パスをクエリから取得
    char query[768];
    char dir_enc[640];
    char dir_path[320] = "/";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        if (httpd_query_key_value(query, "path", dir_enc, sizeof(dir_enc)) == ESP_OK) {
            url_decode(dir_path, dir_enc, sizeof(dir_path));
        }
    }

    // Content-Type から boundary を取得
    char content_type[128];
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));
    char *bp = strstr(content_type, "boundary=");
    if (!bp) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No boundary");
        return ESP_OK;
    }
    // boundary = "--<value>",  end_boundary = "\r\n--<value>"
    char boundary[80];
    snprintf(boundary, sizeof(boundary), "--%s", bp + 9);
    int boundary_len = strlen(boundary);
    char end_boundary[84];
    snprintf(end_boundary, sizeof(end_boundary), "\r\n%s", boundary);
    int end_boundary_len = strlen(end_boundary);

    int total_len = req->content_len;
    int received  = 0;
    ESP_LOGI(TAG, "Upload start: content_len=%d", total_len);

    // ---- フェーズ1: 最初のチャンクでヘッダを解析 ----
    static char hdr_buf[2048];
    int hdr_len = 0;
    {
        int ret = httpd_req_recv(req, hdr_buf, sizeof(hdr_buf) - 1);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Recv failed");
            return ESP_OK;
        }
        hdr_len = ret;
        received += ret;
        hdr_buf[hdr_len] = '\0';
    }

    char *pos = (char*)memmem(hdr_buf, hdr_len, boundary, boundary_len);
    if (!pos) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Boundary not found");
        return ESP_OK;
    }
    char *hdr = pos + boundary_len;
    if (*hdr == '\r') hdr += 2;

    char *fn_key = strstr(hdr, "filename=\"");
    if (!fn_key) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No filename");
        return ESP_OK;
    }
    fn_key += 10;
    char *fn_end = strchr(fn_key, '"');
    if (!fn_end) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Filename parse error");
        return ESP_OK;
    }
    char filename[256] = "";
    int fn_len = (fn_end - fn_key < 255) ? (int)(fn_end - fn_key) : 255;
    memcpy(filename, fn_key, fn_len);
    filename[fn_len] = '\0';
    ESP_LOGI(TAG, "Upload filename: %s", filename);
    {
        char _log[280];
        snprintf(_log, sizeof(_log), "UPLOAD %s\r\n", filename);
        cdc_log(_log);
    }

    char *data_start = strstr(hdr, "\r\n\r\n");
    if (!data_start) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Header end not found");
        return ESP_OK;
    }
    data_start += 4;

    // ファイルパス構築
    char file_path[600];
    strlcpy(file_path, "/sdcard", sizeof(file_path));
    if (strcmp(dir_path, "/") != 0) strlcat(file_path, dir_path, sizeof(file_path));
    strlcat(file_path, "/", sizeof(file_path));
    strlcat(file_path, filename, sizeof(file_path));

    FILE *f = fopen(file_path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot create: %s", file_path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot create file");
        return ESP_OK;
    }

    // ---- フェーズ2: ストリーミング書き込み ----
    // wr_bufにend_boundary分の余裕を持たせ、毎ループ末尾boundaryを検索
    static char wr_buf[8192 + 84];
    int wr_len = 0;
    int total_written = 0;
    bool found_end = false;

    // 最初のチャンクのデータ部分をバッファに入れる
    int first_data_len = hdr_len - (int)(data_start - hdr_buf);
    if (first_data_len > 0 && first_data_len <= (int)sizeof(wr_buf)) {
        memcpy(wr_buf, data_start, first_data_len);
        wr_len = first_data_len;
    }

    while (!found_end) {
        // バッファ内にend_boundaryがあるか検索
        char *tail = (char*)memmem(wr_buf, wr_len, end_boundary, end_boundary_len);
        if (tail) {
            // 末尾boundary発見: そこまでを書き込んで終了
            int write_len = (int)(tail - wr_buf);
            if (write_len > 0) {
                fwrite(wr_buf, 1, write_len, f);
                total_written += write_len;
            }
            ESP_LOGI(TAG, "End boundary found, written=%d", total_written);
            found_end = true;
            break;
        }

        // end_boundary分を残して書き込む
        if (wr_len > end_boundary_len) {
            int write_len = wr_len - end_boundary_len;
            fwrite(wr_buf, 1, write_len, f);
            total_written += write_len;
            memmove(wr_buf, wr_buf + write_len, end_boundary_len);
            wr_len = end_boundary_len;
        }

        // 受信完了チェック
        if (received >= total_len) break;

        // 次のチャンクを受信
        int space = (int)sizeof(wr_buf) - wr_len;
        if (space <= 0) {
            fwrite(wr_buf, 1, wr_len, f);
            total_written += wr_len;
            wr_len = 0;
            space = sizeof(wr_buf);
        }
        int to_read = ((total_len - received) < space) ? (total_len - received) : space;
        int ret = httpd_req_recv(req, wr_buf + wr_len, to_read);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) {
            ESP_LOGE(TAG, "Recv error: %d at received=%d", ret, received);
            break;
        }
        wr_len   += ret;
        received += ret;
    }

    // end_boundaryが見つからなかった場合のフォールバック
    if (!found_end && wr_len > 0) {
        char *tail = (char*)memmem(wr_buf, wr_len, end_boundary, end_boundary_len);
        int final_len = tail ? (int)(tail - wr_buf) : wr_len;
        if (final_len > 0) {
            fwrite(wr_buf, 1, final_len, f);
            total_written += final_len;
        }
        ESP_LOGI(TAG, "Fallback written=%d", total_written);
    }

    fclose(f);
    ESP_LOGI(TAG, "Upload OK: %s (%d bytes written)", file_path, total_written);

    // X-File-Mtime ヘッダがあればタイムスタンプを設定
    char mtime_str[32] = "";
    if (httpd_req_get_hdr_value_str(req, "X-File-Mtime", mtime_str, sizeof(mtime_str)) == ESP_OK) {
        long long mtime_sec = atoll(mtime_str);
        if (mtime_sec > 0) {
            struct utimbuf ut;
            ut.actime  = (time_t)mtime_sec;
            ut.modtime = (time_t)mtime_sec;
            if (utime(file_path, &ut) == 0) {
                ESP_LOGI(TAG, "Timestamp set: %lld", mtime_sec);
            } else {
                ESP_LOGW(TAG, "utime failed: errno=%d", errno);
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


// -----------------------------------------------------------------------
// HTTP: GET /  メインページ
// -----------------------------------------------------------------------
static esp_err_t http_get_root(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head>"
        "<meta charset='utf-8'><title>M5Stack SD Browser</title>"
        "<style>"
        "body{font-family:monospace;background:#1a1a2e;color:#eee;padding:20px;margin:0}"
        "h1{color:#00d4ff;margin-bottom:16px}"
        ".toolbar{margin-bottom:12px}"
        "button{padding:8px 20px;background:#00d4ff;color:#000;border:none;"
               "border-radius:6px;font-size:1em;cursor:pointer;margin-right:8px}"
        "button:hover{background:#00aacc}"
        "#path{color:#ffaa00;margin-bottom:8px;font-size:.95em}"
        "#status{color:#888;font-size:.85em;margin-bottom:12px}"
        "table{width:100%;border-collapse:collapse}"
        "th{text-align:left;color:#aaa;font-weight:normal;"
           "border-bottom:1px solid #333;padding:6px 8px}"
        "td{padding:5px 8px;border-bottom:1px solid #222}"
        "tr:hover td{background:#16213e}"
        ".fname{color:#00ff9f;cursor:pointer}"
        ".fname:hover{text-decoration:underline}"
        ".dir{color:#00d4ff}"
        ".size{text-align:right;color:#aaa}"
        ".date{color:#888}"
        "#sd-status{display:inline-block;padding:3px 10px;border-radius:12px;"
                   "font-size:.8em;font-weight:bold}"
        ".sd-ok{background:#00ff9f;color:#000}"
        ".sd-ng{background:#ff4444;color:#fff}"
        "#prog-wrap{display:none;margin-top:8px;background:#222;border-radius:4px;height:16px;width:100%;max-width:400px}"
        "#prog-bar{height:16px;background:#00d4ff;border-radius:4px;width:0%;transition:width .2s}"
        "#prog-txt{font-size:.8em;color:#aaa;margin-top:2px}"
        "</style></head><body>"
        "<h1>&#128190; M5Stack SD Card Browser</h1>"

        "<div class='toolbar'>"
        "  <button onclick='doDir()'>&#128194; DIR</button>"
        "  <button onclick='doUpload()' style='background:#00ff9f'>&#11014; Upload</button>"
        "  <button onclick='doMount()' style='background:#ffaa00;color:#000'>&#128260; Mount</button>"
        "  <button onclick='doMkdir()' style='background:#aa88ff;color:#000'>&#128193; MkDir</button>"
        "  <button id='btn-del' onclick='doDelete()'"
               " style='background:#555;color:#888;cursor:default'"
               " disabled>&#128465; Delete</button>"
        "  <button id='btn-ren' onclick='doRename()'"
               " style='background:#555;color:#888;cursor:default'"
               " disabled>&#9998; Rename</button>"
        "  <input type='file' id='fileInput' style='display:none' onchange='startUpload(this)'>"
        "  <span id='sd-status' class='sd-ng'>SD: --</span>"
        "  <button onclick='doSetClock()'"
               " style='background:#336699;color:#fff;margin-left:auto;float:right'>"
               "&#128336; Set Clock</button>"
        "</div>"
        "<div id='clock-status' style='color:#00aaff;font-size:.8em;text-align:right;margin-bottom:4px'></div>"
        "<div id='mkdir-row' style='display:none;margin-top:8px'>"
        "  <span style='color:#aa88ff'>New folder:</span>"
        "  <input id='mkdir-name' type='text' placeholder='New folder name'"
               " style='padding:6px;font-size:1em;background:#222;color:#eee;"
                       "border:1px solid #aa88ff;border-radius:4px;width:240px;margin-left:8px'>"
        "  <button onclick='confirmMkdir()'"
               " style='background:#aa88ff;color:#000;margin-left:8px'>OK</button>"
        "  <button onclick='cancelMkdir()'"
               " style='background:#555;color:#eee;margin-left:4px'>Cancel</button>"
        "</div>"
        "<div id='rename-row' style='display:none;margin-top:8px'>"
        "  <span id='rename-label' style='color:#ffcc44'></span>"
        "  <input id='rename-name' type='text' placeholder='New name'"
               " style='padding:6px;font-size:1em;background:#222;color:#eee;"
                       "border:1px solid #ffcc44;border-radius:4px;width:240px;margin-left:8px'>"
        "  <button onclick='confirmRename()'"
               " style='background:#ffcc44;color:#000;margin-left:8px'>OK</button>"
        "  <button onclick='cancelRename()'"
               " style='background:#555;color:#eee;margin-left:4px'>Cancel</button>"
        "</div>"
        "<div id='prog-wrap'><div id='prog-bar'></div></div>"
        "<div id='prog-txt'></div>"

        "<div id='path'></div>"
        "<div id='status'>Ready.</div>"

        "<table id='filetable' style='display:none'>"
        "  <thead><tr>"
        "    <th style='width:24px'>"
               "<input type='checkbox' id='chk-all' onclick='toggleAll(this)'></th>"
        "    <th>Date</th><th>Time</th><th>Size</th><th>Name</th>"
        "  </tr></thead>"
        "  <tbody id='filelist'></tbody>"
        "</table>"
        "<div id='sd-cap'></div>"

        "<script>"
        "var curPath='/';"

        // ディレクトリ一覧取得
        "function doDir(){"
        "  fetchDir(curPath);"
        "}"

        "function fetchDir(path){"
        "  document.getElementById('status').textContent='Loading...';"
        "  fetch('/api/dir?path='+encodeURIComponent(path))"
        "  .then(r=>r.json())"
        "  .then(d=>{"
        "    if(d.error){"
        "      document.getElementById('status').textContent='Error: '+d.error;"
        "      return;"
        "    }"
        "    curPath=d.path;"
        "    document.getElementById('path').textContent='Path: '+curPath;"
        "    document.getElementById('status').textContent=d.files.length+' items';"
        "    document.getElementById('sd-status').className='sd-status sd-ok';"
        "    document.getElementById('sd-status').textContent='SD: OK';"
        "    var tbody=document.getElementById('filelist');"
        "    tbody.innerHTML='';"

        // 親ディレクトリへ戻るエントリ
        "    if(curPath!='/'){"
        "      var tr=document.createElement('tr');"
        "      tr.innerHTML=\"<td></td><td class='date'>--</td><td class='date'>--</td><td class='size'>--</td><td class='dir fname' onclick='goUp()'>[..]</td>\";"
        "      tbody.appendChild(tr);"
        "    }"

        "    d.files.forEach(function(f){"
        "      var tr=document.createElement(\"tr\");"
        "      var sz=f.dir?\"<DIR>\":f.size.toLocaleString()+\" B\";"
        "      var cls=f.dir?\"dir fname\":\"fname\";"
        "      var fp=(curPath===\"/\"?\"\":curPath)+\"/\"+f.name;"
        "      var cb=document.createElement(\"input\");"
        "      cb.type=\"checkbox\"; cb.className=\"row-chk\";"
        "      cb.dataset.path=fp;"
        "      cb.addEventListener(\"change\",updateDelBtn);"
        "      var td0=document.createElement(\"td\"); td0.appendChild(cb);"
        "      var td1=document.createElement(\"td\"); td1.className=\"date\"; td1.textContent=f.date;"
        "      var td2=document.createElement(\"td\"); td2.className=\"date\"; td2.textContent=f.time;"
        "      var td3=document.createElement(\"td\"); td3.className=\"size\"; td3.textContent=sz;"
        "      var td4=document.createElement(\"td\"); td4.className=cls;"
        "      var span=document.createElement(\"span\"); span.textContent=f.name;"
        "      if(f.dir){ span.onclick=function(){enterDir(fp);} }"
        "      else { span.onclick=function(){downloadFile(fp);} }"
        "      td4.appendChild(span);"
        "      tr.appendChild(td0); tr.appendChild(td1); tr.appendChild(td2);"
        "      tr.appendChild(td3); tr.appendChild(td4);"
        "      tbody.appendChild(tr);"
        "    });"

        // hrと容量情報を表示
        "    var capDiv=document.getElementById('sd-cap');"
        "    if(d.total>0){"
        "      var pct=Math.round(d.used/d.total*100);"
        "      capDiv.innerHTML="
        "        '<table style=\"width:100%;border-collapse:collapse\">'"
        "        +'<tr><td colspan=\"5\" style=\"border-top:1px solid #333;padding:0\"></td></tr>'"
        "        +'<tr>'"
        "        +'<td></td>'"
        "        +'<td colspan=\"2\" style=\"padding:5px 8px;color:#aaa\">Total</td>'"
        "        +'<td class=\"size\">'+d.total.toLocaleString()+' B</td>'"
        "        +'<td></td>'"
        "        +'</tr><tr>'"
        "        +'<td></td>'"
        "        +'<td colspan=\"2\" style=\"padding:5px 8px;color:#aaa\">Used</td>'"
        "        +'<td class=\"size\">'+d.used.toLocaleString()+' B</td>'"
        "        +'<td style=\"padding:5px 8px;color:#aaa\">('+pct+'%)</td>'"
        "        +'</tr><tr>'"
        "        +'<td></td>'"
        "        +'<td colspan=\"2\" style=\"padding:5px 8px;color:#aaa\">Free</td>'"
        "        +'<td class=\"size\">'+d.free.toLocaleString()+' B</td>'"
        "        +'<td></td>'"
        "        +'</tr></table>';"
        "    } else {"
        "      capDiv.innerHTML='';"
        "    }"

        "    document.getElementById('filetable').style.display='table';"
        "  })"
        "  .catch(e=>document.getElementById('status').textContent='Fetch error: '+e);"
        "}"

        "function enterDir(path){"
        "  curPath=path;"
        "  fetchDir(path);"
        "}"

        "function goUp(){"
        "  var parts=curPath.split('/').filter(Boolean);"
        "  parts.pop();"
        "  curPath=parts.length?'/'+parts.join('/'):'/';"
        "  fetchDir(curPath);"
        "}"

       
        
        "function downloadFile(path){"
        "    window.open('/api/download?path='+encodeURI(path),'_blank');"
        "}"

        // チェック状態に応じてDeleteボタンの有効/無効を切り替え
        "function updateDelBtn(){"
        "  var checked=document.querySelectorAll('.row-chk:checked').length;"
        "  var btn=document.getElementById('btn-del');"
        "  if(checked>0){"
        "    btn.disabled=false;"
        "    btn.style.background='#ff4444';"
        "    btn.style.color='#fff';"
        "    btn.style.cursor='pointer';"
        "    btn.textContent='Delete ('+checked+')';"
        "  } else {"
        "    btn.disabled=true;"
        "    btn.style.background='#555';"
        "    btn.style.color='#888';"
        "    btn.style.cursor='default';"
        "    btn.textContent='Delete';"
        "  }"
        // Renameボタン: チェックが1つのみ有効
        "  var ren=document.getElementById('btn-ren');"
        "  if(checked===1){"
        "    ren.disabled=false;"
        "    ren.style.background='#ffcc44';"
        "    ren.style.color='#000';"
        "    ren.style.cursor='pointer';"
        "  } else {"
        "    ren.disabled=true;"
        "    ren.style.background='#555';"
        "    ren.style.color='#888';"
        "    ren.style.cursor='default';"
        "    document.getElementById('rename-row').style.display='none';"
        "  }"
        "}"

        // 全選択/全解除
        "function toggleAll(cb){"
        "  document.querySelectorAll('.row-chk').forEach(function(c){c.checked=cb.checked;});"
        "  updateDelBtn();"
        "}"

        // 選択したファイル/ディレクトリを削除
        "function doDelete(){"
        "  var checked=document.querySelectorAll('.row-chk:checked');"
        "  if(!checked.length) return;"
        "  var paths=[];"
        "  checked.forEach(function(c){paths.push(c.dataset.path);});"
        "  document.getElementById('status').textContent='Deleting '+paths.length+' item(s)...';"
        "  fetch('/api/delete',{"
        "    method:'POST',"
        "    headers:{'Content-Type':'application/json'},"
        "    body:JSON.stringify({paths:paths})"
        "  })"
        "  .then(r=>r.json())"
        "  .then(d=>{"
        "    document.getElementById('status').textContent="
        "      'Deleted: '+d.deleted+', Failed: '+d.failed;"
        "    document.getElementById('chk-all').checked=false;"
        "    updateDelBtn();"
        "    fetchDir(curPath);"
        "  })"
        "  .catch(e=>document.getElementById('status').textContent='Delete error: '+e);"
        "}"

        "function doMkdir(){"
        "  document.getElementById('mkdir-row').style.display='block';"
        "  document.getElementById('mkdir-name').value='';"
        "  document.getElementById('mkdir-name').focus();"
        "}"

        "function cancelMkdir(){"
        "  document.getElementById('mkdir-row').style.display='none';"
        "}"

        "function confirmMkdir(){"
        "  var name=document.getElementById('mkdir-name').value.trim();"
        "  if(!name) return;"
        "  if(name.indexOf('/')>=0){ alert('/ は使えません'); return; }"
        "  var fp=(curPath==='/'?'':curPath)+'/'+name;"
        "  fetch('/api/mkdir',{method:'POST',"
        "    headers:{'Content-Type':'application/json'},"
        "    body:JSON.stringify({path:fp})})"
        "  .then(r=>r.json())"
        "  .then(d=>{"
        "    document.getElementById('mkdir-row').style.display='none';"
        "    if(d.ok){ fetchDir(curPath); }"
        "    else { document.getElementById('status').textContent='MkDir failed'; }"
        "  })"
        "  .catch(e=>document.getElementById('status').textContent='MkDir error: '+e);"
        "}"

        "function doRename(){"
        "  var checked=document.querySelectorAll('.row-chk:checked');"
        "  if(checked.length!==1) return;"
        "  var oldPath=checked[0].dataset.path;"
        "  var oldName=oldPath.split('/').pop();"
        "  document.getElementById('rename-label').textContent='Rename: '+oldName+' ->';"
        "  document.getElementById('rename-name').value=oldName;"
        "  document.getElementById('rename-row').style.display='block';"
        "  document.getElementById('rename-name').focus();"
        "  document.getElementById('rename-name').select();"
        "  document.getElementById('mkdir-row').style.display='none';"
        "}"

        "function cancelRename(){"
        "  document.getElementById('rename-row').style.display='none';"
        "}"

        "function confirmRename(){"
        "  var newName=document.getElementById('rename-name').value.trim();"
        "  if(!newName) return;"
        "  if(newName.indexOf('/')>=0){ alert('/ は使えません'); return; }"
        "  var checked=document.querySelectorAll('.row-chk:checked');"
        "  if(checked.length!==1) return;"
        "  var oldPath=checked[0].dataset.path;"
        "  var dir=oldPath.substring(0,oldPath.lastIndexOf('/'));"
        "  var newPath=(dir===''?'':dir)+'/'+newName;"
        "  fetch('/api/rename',{method:'POST',"
        "    headers:{'Content-Type':'application/json'},"
        "    body:JSON.stringify({from:oldPath,to:newPath})})"
        "  .then(r=>r.json())"
        "  .then(d=>{"
        "    document.getElementById('rename-row').style.display='none';"
        "    if(d.ok){ fetchDir(curPath); }"
        "    else { document.getElementById('status').textContent='Rename failed: '+(d.error||''); }"
        "  })"
        "  .catch(e=>document.getElementById('status').textContent='Rename error: '+e);"
        "}"

        "document.addEventListener('keydown',function(e){"
        "  var mkrow=document.getElementById('mkdir-row');"
        "  var rnrow=document.getElementById('rename-row');"
        "  if(mkrow.style.display!=='none'){"
        "    if(e.key==='Enter') confirmMkdir();"
        "    if(e.key==='Escape') cancelMkdir();"
        "  }"
        "  if(rnrow.style.display!=='none'){"
        "    if(e.key==='Enter') confirmRename();"
        "    if(e.key==='Escape') cancelRename();"
        "  }"
        "});"

        "function doSetClock(){"
        "  var now=new Date();"
        // FATはローカル時刻で保存するのでローカル時刻のUnixタイムスタンプを送る
        "  var tzOfs=now.getTimezoneOffset()*-60;"
        "  var t=Math.floor(now.getTime()/1000)+tzOfs;"
        "  var disp=now.toLocaleString();"
        "  fetch('/api/setclock?t='+t)"
        "  .then(r=>r.json())"
        "  .then(d=>{"
        "    if(d.ok){"
        "      document.getElementById('clock-status').textContent='Clock set: '+disp;"
        "    } else {"
        "      document.getElementById('clock-status').textContent='Clock set FAILED';"
        "    }"
        "  })"
        "  .catch(e=>document.getElementById('clock-status').textContent='Clock error: '+e);"
        "}"

        "function doMount(){"
        "  document.getElementById('status').textContent='Mounting SD card...';"
        "  document.getElementById('sd-status').className='sd-ng';"
        "  document.getElementById('sd-status').textContent='SD: ...';"
        "  fetch('/api/mount')"
        "  .then(r=>r.json())"
        "  .then(d=>{"
        "    if(d.sd){"
        "      document.getElementById('sd-status').className='sd-ok';"
        "      document.getElementById('sd-status').textContent='SD: OK';"
        "      document.getElementById('status').textContent='SD mounted.';"
        "      fetchDir(curPath);"
        "    } else {"
        "      document.getElementById('sd-status').className='sd-ng';"
        "      document.getElementById('sd-status').textContent='SD: NG';"
        "      document.getElementById('status').textContent='SD mount failed.';"
        "    }"
        "  })"
        "  .catch(e=>document.getElementById('status').textContent='Mount error: '+e);"
        "}"

        "function doUpload(){"
        "  document.getElementById('fileInput').value='';"
        "  document.getElementById('fileInput').click();"
        "}"

        "function startUpload(input){"
        "  if(!input.files||!input.files.length) return;"
        "  var file=input.files[0];"
        "  var fd=new FormData();"
        "  fd.append('file',file,file.name);"
        "  document.getElementById('prog-wrap').style.display='block';"
        "  document.getElementById('prog-bar').style.width='0%';"
        "  document.getElementById('prog-txt').textContent='Uploading: '+file.name+' ...';"
        "  var xhr=new XMLHttpRequest();"
        "  xhr.open('POST','/api/upload?path='+encodeURI(curPath),true);"
        "  var tzOfs=new Date().getTimezoneOffset()*-60;"
        "  xhr.setRequestHeader('X-File-Mtime',Math.floor(file.lastModified/1000)+tzOfs);"
        "  xhr.upload.onprogress=function(e){"
        "    if(e.lengthComputable){"
        "      var pct=Math.round(e.loaded/e.total*100);"
        "      document.getElementById('prog-bar').style.width=pct+'%';"
        "      document.getElementById('prog-txt').textContent="
        "        file.name+' '+pct+'% ('+formatSize(e.loaded)+'/'+formatSize(e.total)+')';"
        "    }"
        "  };"
        "  xhr.onload=function(){"
        "    document.getElementById('prog-wrap').style.display='none';"
        "    if(xhr.status===200){"
        "      document.getElementById('prog-txt').textContent='Upload complete: '+file.name;"
        "      fetchDir(curPath);"
        "    } else {"
        "      document.getElementById('prog-txt').textContent='Upload failed: '+xhr.status;"
        "    }"
        "  };"
        "  xhr.onerror=function(){"
        "    document.getElementById('prog-txt').textContent='Upload error';"
        "  };"
        "  xhr.send(fd);"
        "}"

        // 容量表示用（GB/MB/KB単位）
        "function fmtBytes(n){"
        "  if(n>=1073741824) return (n/1073741824).toFixed(2)+' GB';"
        "  if(n>=1048576)    return (n/1048576).toFixed(1)+' MB';"
        "  if(n>=1024)       return (n/1024).toFixed(1)+' KB';"
        "  return n+' B';"
        "}"

        "function formatSize(n){"
        "  if(n>=1048576) return (n/1048576).toFixed(1)+'MB';"
        "  if(n>=1024)    return (n/1024).toFixed(1)+'KB';"
        "  return n+'B';"
        "}"

        // 起動時に自動でDIR
        "fetchDir('/');"
        "</script></body></html>";

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

// -----------------------------------------------------------------------
// -----------------------------------------------------------------------
// HTTP: GET /api/setclock?t=<unix_timestamp_local>  時刻設定
// -----------------------------------------------------------------------
static esp_err_t http_get_setclock(httpd_req_t *req)
{
    char query[64];
    char t_str[32] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "t", t_str, sizeof(t_str));
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");

    if (t_str[0] == '\0') {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing t\"}", HTTPD_RESP_USE_STRLEN);
    }

    long long t = atoll(t_str);
    if (t <= 0) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"invalid t\"}", HTTPD_RESP_USE_STRLEN);
    }

    // システム時刻を設定（tはローカル時刻のUnixタイムスタンプ）
    // FATFSがローカル時刻を使うため、UTC変換なしでそのまま設定
    struct timeval tv;
    tv.tv_sec  = (time_t)t;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
    s_clock_set = true;

    time_t now = (time_t)t;
    struct tm *tm_info = localtime(&now);
    char tbuf[32];
    strftime(tbuf, sizeof(tbuf), "%Y-%m-%d %H:%M:%S", tm_info);
    ESP_LOGI("CLOCK", "Time set: %s", tbuf);

    char log_msg[48];
    snprintf(log_msg, sizeof(log_msg), "CLOCK SET: %s\r\n", tbuf);
    cdc_log(log_msg);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"time\":\"%s\"}", tbuf);
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}

// -----------------------------------------------------------------------
// HTTP: POST /api/rename  ファイル/ディレクトリ名変更
// -----------------------------------------------------------------------
static esp_err_t http_post_rename(httpd_req_t *req)
{
    cdc_log("RENAME\r\n");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");

    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"bad request\"}", HTTPD_RESP_USE_STRLEN);
    }

    char *body = (char*)malloc(total + 1);
    if (!body) {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"oom\"}", HTTPD_RESP_USE_STRLEN);
    }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) { free(body); return ESP_FAIL; }
        received += r;
    }
    body[total] = '\0';

    // JSON から "from" と "to" を取得
    char from_enc[640] = "", to_enc[640] = "";
    char *p;

    p = strstr(body, "\"from\"");
    if (p) {
        p = strchr(p + 6, '"');
        if (p) { p++; char *e = strchr(p, '"'); if (e) { strlcpy(from_enc, p, e - p + 1); } }
    }
    p = strstr(body, "\"to\"");
    if (p) {
        p = strchr(p + 4, '"');
        if (p) { p++; char *e = strchr(p, '"'); if (e) { strlcpy(to_enc, p, e - p + 1); } }
    }
    free(body);

    if (from_enc[0] == '\0' || to_enc[0] == '\0') {
        return httpd_resp_send(req, "{\"ok\":false,\"error\":\"missing from/to\"}", HTTPD_RESP_USE_STRLEN);
    }

    char from_path[648], to_path[648];
    snprintf(from_path, sizeof(from_path), "/sdcard%s", from_enc);
    snprintf(to_path,   sizeof(to_path),   "/sdcard%s", to_enc);

    if (rename(from_path, to_path) != 0) {
        ESP_LOGW(TAG, "rename failed: %s -> %s (errno=%d)", from_path, to_path, errno);
        char resp[64];
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"error\":\"errno %d\"}", errno);
        return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    }

    char log_msg[1300];
    snprintf(log_msg, sizeof(log_msg), "RENAMED %s -> %s\r\n", from_enc, to_enc);
    cdc_log(log_msg);

    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

// HTTPサーバ起動
// -----------------------------------------------------------------------

// -----------------------------------------------------------------------
// HTTP: POST /api/mkdir  ディレクトリ作成
// Body: {"path":"/new_dir_full_path"}
// Pythonから: requests.post('http://192.168.7.1/api/mkdir',
//               json={'path': '/new_folder'})
// -----------------------------------------------------------------------
static esp_err_t http_post_mkdir(httpd_req_t *req)
{
    cdc_log("MKDIR\r\n");
    int total = req->content_len;
    if (total <= 0 || total > 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }
    static char body[1024];
    int received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (ret <= 0) break;
        received += ret;
    }
    body[received] = '\0';

    // {"path":"/dir_name"} からパスを取得
    char enc_path[640] = "";
    char *qs = strstr(body, "\"path\"");
    if (qs) qs = strchr(qs, ':');
    if (qs) qs = strchr(qs, '"');
    if (qs) {
        qs++;
        char *qe = strchr(qs, '"');
        if (qe) {
            int plen = (int)(qe - qs);
            if (plen >= (int)sizeof(enc_path)) plen = sizeof(enc_path) - 1;
            memcpy(enc_path, qs, plen);
            enc_path[plen] = '\0';
        }
    }
    if (enc_path[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path");
        return ESP_OK;
    }

    char path[320];
    url_decode(path, enc_path, sizeof(path));

    char full[328];
    strlcpy(full, "/sdcard", sizeof(full));
    strlcat(full, path, sizeof(full));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "close");
    if (mkdir(full, 0777) == 0) {
        ESP_LOGI(TAG, "mkdir OK: %s", full);
        { char _log[340]; snprintf(_log,sizeof(_log),"MKDIR OK %s\r\n",path); cdc_log(_log); }
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    } else {
        ESP_LOGW(TAG, "mkdir failed: %s (errno=%d)", full, errno);
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t cfg    = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers  = 10;
    cfg.lru_purge_enable  = true;
    cfg.server_port       = 80;
    cfg.max_open_sockets  = 5;    // アップロード用に増加
    cfg.backlog_conn      = 3;
    cfg.recv_wait_timeout = 60;   // アップロードのため延長
    cfg.send_wait_timeout = 60;
    cfg.stack_size        = 16384; // アップロードハンドラのため増加

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        return NULL;
    }

    static const httpd_uri_t r1 = { .uri="/",             .method=HTTP_GET,  .handler=http_get_root,     .user_ctx=NULL };
    static const httpd_uri_t r2 = { .uri="/api/dir",      .method=HTTP_GET,  .handler=http_get_dir,      .user_ctx=NULL };
    static const httpd_uri_t r3 = { .uri="/api/download", .method=HTTP_GET,  .handler=http_get_download, .user_ctx=NULL };
    static const httpd_uri_t r4 = { .uri="/api/upload",   .method=HTTP_POST, .handler=http_post_upload,  .user_ctx=NULL };
    static const httpd_uri_t r5 = { .uri="/api/mount",    .method=HTTP_GET,  .handler=http_get_mount,    .user_ctx=NULL };
    static const httpd_uri_t r6 = { .uri="/api/delete",   .method=HTTP_POST, .handler=http_post_delete,  .user_ctx=NULL };
    static const httpd_uri_t r7 = { .uri="/api/mkdir",    .method=HTTP_POST, .handler=http_post_mkdir,   .user_ctx=NULL };
    static const httpd_uri_t r8 = { .uri="/api/setclock", .method=HTTP_GET,  .handler=http_get_setclock, .user_ctx=NULL };
    static const httpd_uri_t r9 = { .uri="/api/rename",   .method=HTTP_POST, .handler=http_post_rename,  .user_ctx=NULL };
    httpd_register_uri_handler(server, &r1);
    httpd_register_uri_handler(server, &r2);
    httpd_register_uri_handler(server, &r3);
    httpd_register_uri_handler(server, &r4);
    httpd_register_uri_handler(server, &r5);
    httpd_register_uri_handler(server, &r6);
    httpd_register_uri_handler(server, &r7);
    httpd_register_uri_handler(server, &r8);
    httpd_register_uri_handler(server, &r9);
    return server;
}

// -----------------------------------------------------------------------
// app_main
// -----------------------------------------------------------------------
extern "C" void app_main(void)
{
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setRotation(1);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
    M5.Display.println("SD Card Browser ver " VERSION);
    M5.Display.println("192.168.7.1");

    ESP_ERROR_CHECK(nvs_flash_init());

    // SD カード初期化（SPI バスは一度だけ初期化）
    // CoreS3 SE: MOSI=37, MISO=35, CLK=36, CS=4
    // M5Unified が SPI3_HOST を使うので SPI2_HOST を使う
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num   = GPIO_NUM_37;
    bus_cfg.miso_io_num   = GPIO_NUM_35;
    bus_cfg.sclk_io_num   = GPIO_NUM_36;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4096;
    esp_err_t sd_err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (sd_err != ESP_OK) {
        ESP_LOGW(TAG, "SPI bus init failed: %s", esp_err_to_name(sd_err));
    }

    if (sd_mount()) {
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Display.println("SD: OK");
    } else {
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.println("SD: NOT FOUND");
    }
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

    // MAC・IP設定
    esp_read_mac(s_server_mac, ESP_MAC_WIFI_STA);
    s_server_mac[5] ^= 0x01;
    IP4_ADDR((ip4_addr_t*)&s_server_ip, 192, 168, 7, 1);
    IP4_ADDR((ip4_addr_t*)&s_client_ip,
             CLIENT_IP_A, CLIENT_IP_B, CLIENT_IP_C, CLIENT_IP_D);

    tcpip_init(NULL, NULL);

    ip4_addr_t ipaddr, netmask, gw;
    IP4_ADDR(&ipaddr,  192, 168, 7, 1);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      192, 168, 7, 1);

    LOCK_TCPIP_CORE();
    netif_add(&s_netif, &ipaddr, &netmask, &gw,
              NULL, netif_init_func, tcpip_input);
    netif_set_default(&s_netif);
    netif_set_up(&s_netif);
    UNLOCK_TCPIP_CORE();

    s_rx_queue = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_pkt_t));
    xTaskCreate(lwip_rx_task, "lwip_rx", 4096, NULL, 5, NULL);

    const tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    // CDC ACM（USBシリアル）初期化
    tinyusb_config_cdcacm_t cdc_cfg = {};
    cdc_cfg.cdc_port = TINYUSB_CDC_ACM_0;
    ESP_ERROR_CHECK(tinyusb_cdcacm_init(&cdc_cfg));

    tinyusb_net_config_t net_config = {};
    net_config.on_recv_callback = usb_recv_callback;
    net_config.free_tx_buffer   = net_free_tx_buffer;
    net_config.user_context     = NULL;
    esp_read_mac(net_config.mac_addr, ESP_MAC_WIFI_STA);
    ESP_ERROR_CHECK(tinyusb_net_init(&net_config));

    s_dhcp_state = DHCP_STATE_RUNNING;

    start_webserver();

    M5.Display.println("Ready!");

    // USB CDC が接続されるまで少し待ってからメッセージ送信
    vTaskDelay(pdMS_TO_TICKS(2000));
    const char *msg = "M5Stack SD Browser Ready!\r\n"
                      "IP: 192.168.7.1\r\n"
                      "Open http://192.168.7.1/ in browser\r\n";
    tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0,
        (const uint8_t*)msg, strlen(msg));
    tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(100));

    while (1) {
        M5.update();

        // 時刻がセットされていれば LCD に表示
        if (s_clock_set) {
            time_t now;
            time(&now);
            struct tm *tm_info = localtime(&now);
            char date_buf[12], time_buf[10];
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", tm_info);
            strftime(time_buf, sizeof(time_buf), "%H:%M:%S", tm_info);

            // 1行上に移動（y=104）
            M5.Display.setCursor(0, 104);
            M5.Display.setTextColor(TFT_CYAN, TFT_BLACK);
            M5.Display.printf("%-12s\n", date_buf);
            M5.Display.printf("%-10s\n", time_buf);
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        }

        // DIR完了後に容量をLCD下3行に表示（KiB単位）
        if (s_lcd_cap_updated) {
            s_lcd_cap_updated = false;
            int dh = M5.Display.height();  // 画面高さ（通常240）
            int ts = 16;                   // テキストサイズ2での行高さ
            M5.Display.setCursor(0, dh - ts * 3);
            M5.Display.setTextColor(TFT_YELLOW, TFT_BLACK);
            M5.Display.printf("Total:%9llu KiB\n", (unsigned long long)(s_lcd_total / 1024));
            M5.Display.printf("Used :%9llu KiB\n", (unsigned long long)(s_lcd_used  / 1024));
            M5.Display.printf("Free :%9llu KiB\n", (unsigned long long)(s_lcd_free  / 1024));
            M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

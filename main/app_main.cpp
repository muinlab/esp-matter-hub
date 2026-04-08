/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_event.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_https_ota.h>
#include <esp_log.h>
#include <esp_netif_ip_addr.h>
#include <esp_sntp.h>
#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include <log_heap_numbers.h>

#include <app_priv.h>
#include <app_reset.h>
#include "bridge_action.h"
#include "ir_engine.h"
#include "ir_mgmt_cluster.h"
#include "local_discovery.h"
#include "status_led.h"
#include "web_server.h"
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <setup_payload/OnboardingCodesUtil.h>
#include <setup_payload/SetupPayload.h>

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif
using namespace chip::DeviceLayer;
#endif

static const char *TAG = "app_main";
uint16_t light_endpoint_id = 0;
uint16_t bridge_slot_endpoint_ids[BRIDGE_SLOT_COUNT] = { 0 };
static uint16_t bridge_aggregator_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;
constexpr uint32_t k_mdns_retry_interval_ms = 500;
constexpr uint32_t k_mdns_retry_max_attempts = 60;

static TaskHandle_t s_mdns_retry_task = nullptr;
static bool s_mdns_retry_running = false;
static portMUX_TYPE s_mdns_retry_lock = portMUX_INITIALIZER_UNLOCKED;

/* OTA -------------------------------------------------------------------- */
#define FIRMWARE_VERSION      "v0.1.8"
#define GITHUB_REPO           "muinlab/esp-matter-hub"
#define GITHUB_API_LATEST_URL "https://api.github.com/repos/" GITHUB_REPO "/releases/latest"
#define OTA_DEFAULT_URL       "https://github.com/" GITHUB_REPO "/releases/latest/download/esp-matter-hub.bin"

static volatile bool s_ota_running  = false;
static volatile bool s_network_ready = false;

static void ota_task(void *arg)
{
    char *url = static_cast<char *>(arg);

    esp_http_client_config_t http_cfg = {};
    http_cfg.url               = url ? url : OTA_DEFAULT_URL;
    http_cfg.timeout_ms        = 60000;
    http_cfg.max_redirection_count = 10;
    http_cfg.buffer_size       = 4096;
    http_cfg.buffer_size_tx    = 2048;
    http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    http_cfg.keep_alive_enable = true;

    esp_https_ota_config_t ota_cfg = {};
    ota_cfg.http_config = &http_cfg;

    ESP_LOGI(TAG, "OTA 시작: %s", http_cfg.url);
    status_led_set_ota(true);
    esp_err_t err = esp_https_ota(&ota_cfg);

    free(url);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA 완료, 재부팅...");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA 실패: %s", esp_err_to_name(err));
        status_led_set_ota(false);
        s_ota_running = false;
    }
    vTaskDelete(nullptr);
}

extern "C" esp_err_t app_trigger_ota(const char *url)
{
    if (s_ota_running) {
        return ESP_ERR_INVALID_STATE;
    }
    char *url_copy = url ? strdup(url) : nullptr;
    s_ota_running = true;
    if (xTaskCreate(ota_task, "ota_task", 8192, url_copy, 5, nullptr) != pdPASS) {
        s_ota_running = false;
        free(url_copy);
        return ESP_FAIL;
    }
    return ESP_OK;
}

extern "C" bool app_ota_is_running()
{
    return s_ota_running;
}

extern "C" const char *app_firmware_version()
{
    return FIRMWARE_VERSION;
}

static bool fetch_latest_version(char *out, size_t max_len)
{
    char buf[1536] = {};
    esp_http_client_config_t cfg = {};
    cfg.url               = GITHUB_API_LATEST_URL;
    cfg.timeout_ms        = 10000;
    cfg.buffer_size       = 4096;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_http_client_set_header(client, "User-Agent", "esp-matter-hub");

    bool ok = false;
    if (esp_http_client_open(client, 0) == ESP_OK) {
        int content_len = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        int len = esp_http_client_read(client, buf, sizeof(buf) - 1);
        ESP_LOGI(TAG, "GitHub API: status=%d content_len=%d read=%d", status, content_len, len);
        if (status == 200 && len > 0) {
            buf[len] = '\0';
            const char *pos = strstr(buf, "\"tag_name\"");
            if (pos) {
                pos = strchr(pos, ':');
                if (pos) pos = strchr(pos, '"');
                if (pos) {
                    pos++;
                    const char *end = strchr(pos, '"');
                    if (end) {
                        size_t tag_len = (size_t)(end - pos);
                        if (tag_len < max_len) {
                            memcpy(out, pos, tag_len);
                            out[tag_len] = '\0';
                            ok = true;
                        }
                    }
                }
            }
            if (!ok) {
                ESP_LOGW(TAG, "GitHub API: tag_name 파싱 실패 (첫 128B: %.128s)", buf);
            }
        } else if (len > 0) {
            buf[len] = '\0';
            ESP_LOGW(TAG, "GitHub API: HTTP %d: %.128s", status, buf);
        }
        esp_http_client_close(client);
    } else {
        ESP_LOGW(TAG, "GitHub API: 연결 실패");
    }
    esp_http_client_cleanup(client);
    return ok;
}

// "vX.Y.Z" → {major, minor, patch}. 파싱 실패 시 false 반환.
static bool parse_semver(const char *s, int *major, int *minor, int *patch)
{
    if (s && s[0] == 'v') s++;
    return s && sscanf(s, "%d.%d.%d", major, minor, patch) == 3;
}

// candidate가 current보다 높은 버전이면 true (다운그레이드 방지).
static bool semver_is_newer(const char *candidate, const char *current)
{
    int ca = 0, cb = 0, cc = 0;
    int ra = 0, rb = 0, rc = 0;
    if (!parse_semver(candidate, &ca, &cb, &cc)) return false;
    if (!parse_semver(current,   &ra, &rb, &rc)) return false;
    if (ca != ra) return ca > ra;
    if (cb != rb) return cb > rb;
    return cc > rc;
}

static void auto_ota_task(void *arg)
{
    // Wi-Fi IP 취득까지 대기
    while (!s_network_ready) {
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    vTaskDelay(pdMS_TO_TICKS(15000)); // IP 취득 후 힙 안정화 대기

    while (true) {

        char latest[32] = {};
        if (fetch_latest_version(latest, sizeof(latest))) {
            ESP_LOGI(TAG, "최신 버전: %s / 현재: %s", latest, FIRMWARE_VERSION);
            if (semver_is_newer(latest, FIRMWARE_VERSION)) {
                ESP_LOGI(TAG, "업그레이드 버전 발견, OTA 시작");
                app_trigger_ota(nullptr);
            } else {
                ESP_LOGI(TAG, "현재 버전이 최신이거나 더 높음, OTA 건너뜀");
            }
        } else {
            ESP_LOGW(TAG, "최신 버전 확인 실패");
        }
        vTaskDelay(pdMS_TO_TICKS(3600000)); // 1시간마다 확인
    }
}
/* OTA end ---------------------------------------------------------------- */

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
extern const uint8_t cd_start[] asm("_binary_certification_declaration_der_start");
extern const uint8_t cd_end[] asm("_binary_certification_declaration_der_end");

const chip::ByteSpan cdSpan(cd_start, static_cast<size_t>(cd_end - cd_start));
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

static void mdns_registration_retry_task(void *arg)
{
    (void)arg;

    for (uint32_t attempt = 1; attempt <= k_mdns_retry_max_attempts; ++attempt) {
        if (app_local_discovery_ready()) {
            break;
        }

        esp_err_t err = app_local_discovery_start(80, nullptr);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "mDNS registration completed after %lu attempt(s)", static_cast<unsigned long>(attempt));
            break;
        }

        if (err != ESP_ERR_INVALID_STATE || attempt == k_mdns_retry_max_attempts) {
            ESP_LOGW(TAG, "mDNS retry attempt %lu/%lu failed: %s", static_cast<unsigned long>(attempt),
                     static_cast<unsigned long>(k_mdns_retry_max_attempts), esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(k_mdns_retry_interval_ms));
    }

    if (!app_local_discovery_ready()) {
        ESP_LOGW(TAG, "mDNS registration timed out after %lu ms",
                 static_cast<unsigned long>(k_mdns_retry_interval_ms * k_mdns_retry_max_attempts));
    }

    taskENTER_CRITICAL(&s_mdns_retry_lock);
    s_mdns_retry_running = false;
    s_mdns_retry_task = nullptr;
    taskEXIT_CRITICAL(&s_mdns_retry_lock);
    vTaskDelete(nullptr);
}

static void ensure_mdns_registration_retry(const char *reason)
{
    if (app_local_discovery_ready()) {
        return;
    }

    bool should_start = false;
    taskENTER_CRITICAL(&s_mdns_retry_lock);
    if (!s_mdns_retry_running) {
        s_mdns_retry_running = true;
        should_start = true;
    }
    taskEXIT_CRITICAL(&s_mdns_retry_lock);

    if (!should_start) {
        return;
    }

    if (xTaskCreate(mdns_registration_retry_task, "mdns_retry", 3072, nullptr, 5, &s_mdns_retry_task) != pdPASS) {
        taskENTER_CRITICAL(&s_mdns_retry_lock);
        s_mdns_retry_running = false;
        s_mdns_retry_task = nullptr;
        taskEXIT_CRITICAL(&s_mdns_retry_lock);
        ESP_LOGW(TAG, "Failed to create mDNS retry task (%s)", reason ? reason : "unknown");
        return;
    }

    ESP_LOGI(TAG, "Scheduled mDNS registration retry (%s)", reason ? reason : "unknown");
}

static void deferred_slot_identity_sync_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(2500));

    esp_err_t err = bridge_action_sync_all_node_labels();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Deferred NodeLabel sync finished with warnings: %s", esp_err_to_name(err));
    }
    bridge_action_log_slot_identity_dump();
    vTaskDelete(nullptr);
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != IP_EVENT || event_id != IP_EVENT_STA_GOT_IP || !event_data) {
        return;
    }

    s_network_ready = true;

    if (app_local_discovery_ready()) {
        return;
    }

    const auto *ip_event = static_cast<const ip_event_got_ip_t *>(event_data);
    ESP_LOGI(TAG, "Web UI ready (IP): http://" IPSTR, IP2STR(&ip_event->ip_info.ip));

    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb([](struct timeval *tv) {
        time_t now = time(nullptr);
        struct tm t;
        localtime_r(&now, &t);
        ESP_LOGI("SNTP", "Time synced: %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec);
    });
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP initialized, syncing time from pool.ntp.org");

    esp_err_t err = app_local_discovery_start(80, &ip_event->ip_info.ip);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS start deferred (IP_EVENT_STA_GOT_IP): %s", esp_err_to_name(err));
        if (err == ESP_ERR_INVALID_STATE) {
            ensure_mdns_registration_retry("ip_event_sta_got_ip");
        }
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    auto refresh_commissioning_led_state = []() {
        chip::Server &server = chip::Server::GetInstance();
        const bool has_fabric = (server.GetFabricTable().FabricCount() > 0);
        const bool window_open = server.GetCommissioningWindowManager().IsCommissioningWindowOpen();
        status_led_set_commissioning((!has_fabric) || window_open);
        ESP_LOGI(TAG, "LED commissioning state refreshed: has_fabric=%u window_open=%u",
                 has_fabric ? 1U : 0U, window_open ? 1U : 0U);
    };

    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        if (!app_local_discovery_ready()) {
            esp_err_t mdns_err = app_local_discovery_start(80, nullptr);
            if (mdns_err != ESP_OK) {
                ESP_LOGW(TAG, "mDNS registration deferred (interface event): %s", esp_err_to_name(mdns_err));
                if (mdns_err == ESP_ERR_INVALID_STATE) {
                    ensure_mdns_registration_retry("interface_ip_changed");
                }
            }
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        refresh_commissioning_led_state();
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        refresh_commissioning_led_state();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        refresh_commissioning_led_state();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        refresh_commissioning_led_state();
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        refresh_commissioning_led_state();
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager  &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                /* After removing last fabric, this example does not remove the Wi-Fi credentials
                 * and still has IP connectivity so, only advertising on DNS-SD.
                 */
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                                            chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        refresh_commissioning_led_state();
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        MEMORY_PROFILER_DUMP_HEAP_STAT("BLE deinitialized");
        break;

    default:
        if (event->Type == chip::DeviceLayer::DeviceEventType::kServerReady) {
            refresh_commissioning_led_state();
        }
        break;
    }
}

static uint16_t s_pending_commission_timeout = 0;

static void open_commissioning_window_work(intptr_t arg)
{
    uint16_t timeout_seconds = static_cast<uint16_t>(arg);
    chip::CommissioningWindowManager &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (commissionMgr.IsCommissioningWindowOpen()) {
        ESP_LOGI(TAG, "Commissioning window already open");
        return;
    }
    if (timeout_seconds == 0) {
        timeout_seconds = k_timeout_seconds;
    }
    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(timeout_seconds),
                                                                 chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    ESP_LOGI(TAG, "Commissioning window opened manually for %u seconds", timeout_seconds);
    status_led_set_commissioning(true);
}

extern "C" esp_err_t app_open_commissioning_window(uint16_t timeout_seconds)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(open_commissioning_window_work,
                                                                    static_cast<intptr_t>(timeout_seconds));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "ScheduleWork failed: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }
    return ESP_OK;
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        /* Driver update */
        app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    err = status_led_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Status LED init failed: %s", esp_err_to_name(err));
    }

    MEMORY_PROFILER_DUMP_HEAP_STAT("Bootup");

    /* Initialize driver */
    app_driver_handle_t button_handle = app_driver_button_init();
    app_reset_button_register(button_handle);

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;

    // node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    MEMORY_PROFILER_DUMP_HEAP_STAT("node created");

    endpoint_t *aggregator_endpoint = nullptr;
    aggregator::config_t aggregator_config;
    aggregator_endpoint = aggregator::create(node, &aggregator_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(aggregator_endpoint != nullptr, ESP_LOGE(TAG, "Failed to create bridge aggregator endpoint"));
    bridge_aggregator_endpoint_id = endpoint::get_id(aggregator_endpoint);
    ESP_LOGI(TAG, "Bridge aggregator created with endpoint_id %d", bridge_aggregator_endpoint_id);

    for (int slot = 0; slot < BRIDGE_SLOT_COUNT; ++slot) {
        endpoint_t *endpoint = nullptr;
        void *slot_priv_data = nullptr;

        dimmable_light::config_t light_config;
        light_config.on_off.on_off = DEFAULT_POWER;
        light_config.on_off_lighting.start_up_on_off = nullptr;
        light_config.level_control.current_level = DEFAULT_BRIGHTNESS;
        light_config.level_control.on_level = DEFAULT_BRIGHTNESS;
        light_config.level_control_lighting.start_up_current_level = DEFAULT_BRIGHTNESS;
        endpoint = dimmable_light::create(node, &light_config, ENDPOINT_FLAG_BRIDGE, slot_priv_data);
        ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create light endpoint for slot %d", slot));

        err = endpoint::set_parent_endpoint(endpoint, aggregator_endpoint);
        ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to set parent aggregator for slot %d", slot));

        char default_node_label[32];
        snprintf(default_node_label, sizeof(default_node_label), "Slot %d", slot);
        char unique_id[32];
        snprintf(unique_id, sizeof(unique_id), "bridge-slot-%d", slot);

        cluster_t *bridged_info_cluster = cluster::get(endpoint, BridgedDeviceBasicInformation::Id);
        if (!bridged_info_cluster) {
            cluster::bridged_device_basic_information::config_t bridged_info_config;
            bridged_info_cluster =
                cluster::bridged_device_basic_information::create(endpoint, &bridged_info_config, CLUSTER_FLAG_SERVER);
            ABORT_APP_ON_FAILURE(bridged_info_cluster != nullptr,
                                 ESP_LOGE(TAG, "Failed to add BridgedDeviceBasicInformation cluster for slot %d", slot));
        }

        attribute_t *node_label_attr =
            cluster::bridged_device_basic_information::attribute::create_node_label(bridged_info_cluster, default_node_label,
                                                                                     strlen(default_node_label));
        ABORT_APP_ON_FAILURE(node_label_attr != nullptr ||
                                 attribute::get(endpoint::get_id(endpoint), BridgedDeviceBasicInformation::Id,
                                                BridgedDeviceBasicInformation::Attributes::NodeLabel::Id) != nullptr,
                             ESP_LOGE(TAG, "Failed to ensure BridgedDeviceBasicInformation.NodeLabel for slot %d", slot));

        esp_matter_attr_val_t unique_id_val = esp_matter_char_str(unique_id, static_cast<uint16_t>(strlen(unique_id)));
        err = attribute::set_val(endpoint::get_id(endpoint), BridgedDeviceBasicInformation::Id,
                                 BridgedDeviceBasicInformation::Attributes::UniqueID::Id, &unique_id_val);
        ABORT_APP_ON_FAILURE(err == ESP_OK,
                             ESP_LOGE(TAG, "Failed to set BridgedDeviceBasicInformation.UniqueID for slot %d", slot));

        bridge_slot_endpoint_ids[slot] = endpoint::get_id(endpoint);
        ESP_LOGI(TAG, "Bridge slot %d created with endpoint_id %d (parent=%d, unique_id=%s)", slot,
                 bridge_slot_endpoint_ids[slot], bridge_aggregator_endpoint_id, unique_id);

        attribute_t *current_level_attribute =
            attribute::get(bridge_slot_endpoint_ids[slot], LevelControl::Id, LevelControl::Attributes::CurrentLevel::Id);
        if (current_level_attribute) {
            attribute::set_deferred_persistence(current_level_attribute);
        }
    }
    light_endpoint_id = bridge_slot_endpoint_ids[0];
    err = bridge_action_init(bridge_slot_endpoint_ids, BRIDGE_SLOT_COUNT);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialize bridge action module, err:%d", err));
    err = ir_engine_init();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialize IR engine, err:%d", err));

    err = ir_mgmt_cluster_init(node);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialize IrManagement cluster, err:%d", err));

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    // Enable secondary network interface
    secondary_network_interface::config_t secondary_network_interface_config;
    endpoint_t *endpoint =
        endpoint::secondary_network_interface::create(node, &secondary_network_interface_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create secondary network interface endpoint"));
#endif

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
    auto * dac_provider = get_dac_provider();
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
    static_cast<ESP32SecureCertDACProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
    static_cast<ESP32FactoryDataProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#endif
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    err = bridge_action_sync_all_node_labels();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Initial NodeLabel sync finished with warnings: %s", esp_err_to_name(err));
    }
    bridge_action_log_slot_identity_dump();
    (void)xTaskCreate(deferred_slot_identity_sync_task, "slot_id_sync", 4096, nullptr, 4, nullptr);

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to register IP_EVENT_STA_GOT_IP handler, mDNS may stay IP-only: %s",
                 esp_err_to_name(err));
    }

    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));

    MEMORY_PROFILER_DUMP_HEAP_STAT("matter started");

    err = app_web_server_start();
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start web server, err:%d", err));

    xTaskCreate(auto_ota_task, "auto_ota", 8192, nullptr, 3, nullptr);

    status_led_set_system_ready(true);

    /* Starting driver with default values */
    for (int slot = 0; slot < BRIDGE_SLOT_COUNT; ++slot) {
        app_driver_light_set_defaults(bridge_slot_endpoint_ids[slot]);
    }

#if CONFIG_ENABLE_ENCRYPTED_OTA
    err = esp_matter_ota_requestor_encrypted_init(s_decryption_key, s_decryption_key_len);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialized the encrypted OTA, err: %d", err));
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif

    while (true) {
        MEMORY_PROFILER_DUMP_HEAP_STAT("Idle");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}

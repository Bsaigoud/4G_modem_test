#if 0
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

#define UART_NUM UART_NUM_1
#define BUF_SIZE 1024
#define TXD_PIN (GPIO_NUM_17)
#define RXD_PIN (GPIO_NUM_16)

static const char *TAG = "SIM700L_TEST";

typedef struct {
    char restart_status[16];
    char imei[32];
    char imsi[32];
    char sim_number[64];
    char sim_pin_status[32];
    char network_status[32];
    int rssi;
    const char *rssi_description;
} sim700l_test_result_t;

static void uart_init()
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE
    };
    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, TXD_PIN, RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
}

static void send_at_command(const char *cmd, char *response, size_t max_len)
{
    uart_flush(UART_NUM);
    uart_write_bytes(UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(UART_NUM, "\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(1000));
    int len = uart_read_bytes(UART_NUM, (uint8_t *)response, max_len - 1, pdMS_TO_TICKS(3000));
    if (len > 0) {
        response[len] = '\0';
        for (int i = 0; i < len; i++) {
            if ((unsigned char)response[i] < 32 && response[i] != '\r' && response[i] != '\n') {
                response[i] = ' ';
            }
        }
    } else {
        strcpy(response, "ERROR");
    }
    ESP_LOGI(TAG, "CMD: %s", cmd);
    ESP_LOGI(TAG, "RESP: %s", response);
}

static void extract_line_value(const char *resp, char *output, const char *prefix)
{
    char buffer[BUF_SIZE];
    strcpy(buffer, resp);
    char *line = strtok(buffer, "\r\n");
    while (line != NULL) {
        if ((prefix && strstr(line, prefix)) || (!prefix && strspn(line, "0123456789") > 10)) {
            while (*line == ' ') line++;
            strcpy(output, line);
            return;
        }
        line = strtok(NULL, "\r\n");
    }
    strcpy(output, "N/A");
}

static void parse_rssi(const char *resp, int *rssi_value, const char **desc)
{
    char buffer[BUF_SIZE];
    strcpy(buffer, resp);
    char *line = strtok(buffer, "\r\n");
    while (line != NULL) {
        if (strstr(line, "+CSQ:")) {
            int rssi;
            if (sscanf(line, "+CSQ: %d", &rssi) == 1) {
                *rssi_value = rssi;
                if (rssi == 99)
                    *desc = "Unknown";
                else if (rssi < 10)
                    *desc = "Poor";
                else if (rssi < 15)
                    *desc = "Fair";
                else if (rssi < 20)
                    *desc = "Moderate";
                else if (rssi < 26)
                    *desc = "Good";
                else
                    *desc = "Excellent";
                return;
            }
        }
        line = strtok(NULL, "\r\n");
    }
    *rssi_value = -1;
    *desc = "Invalid";
}

static void parse_creg_status(const char *resp, char *output)
{
    if (strstr(resp, "+CREG: 0,1"))
        strcpy(output, "Registered (Home)");
    else if (strstr(resp, "+CREG: 0,5"))
        strcpy(output, "Registered (Roaming)");
    else if (strstr(resp, "+CREG: 0,2"))
        strcpy(output, "Searching");
    else if (strstr(resp, "+CREG: 0,3"))
        strcpy(output, "Denied");
    else if (strstr(resp, "+CREG: 0,0"))
        strcpy(output, "Not Registered");
    else
        strcpy(output, "Unknown");
}

static void print_json(const sim700l_test_result_t *result)
{
    printf("{\n");
    printf("  \"test_results\": {\n");
    printf("    \"restart_module\": \"%s\",\n", result->restart_status);
    printf("    \"imei\": \"%s\",\n", result->imei);
    printf("    \"imsi\": \"%s\",\n", result->imsi);
    printf("    \"sim_number\": \"%s\",\n", result->sim_number);
    printf("    \"sim_pin_status\": \"%s\",\n", result->sim_pin_status);
    printf("    \"network_status\": \"%s\",\n", result->network_status);
    printf("    \"signal_quality\": {\n");
    printf("      \"rssi\": %d,\n", result->rssi);
    printf("      \"description\": \"%s\"\n", result->rssi_description);
    printf("    }\n");
    printf("  }\n");
    printf("}\n");
}

static void sim700l_test_task(void *arg)
{
    char resp[BUF_SIZE];
    sim700l_test_result_t result = {
        .restart_status = "Failed",
        .imei = "N/A",
        .imsi = "N/A",
        .sim_number = "N/A",
        .sim_pin_status = "Unknown",
        .network_status = "Unknown",
        .rssi = -1,
        .rssi_description = "N/A"
    };

    send_at_command("AT+CFUN=1,1", resp, sizeof(resp));
    if (strstr(resp, "OK")) strcpy(result.restart_status, "OK");

    vTaskDelay(pdMS_TO_TICKS(7500));

    send_at_command("AT", resp, sizeof(resp));

    send_at_command("AT+GSN", resp, sizeof(resp));
    extract_line_value(resp, result.imei, NULL);

    send_at_command("AT+CIMI", resp, sizeof(resp));
    extract_line_value(resp, result.imsi, NULL);

    send_at_command("AT+CPIN?", resp, sizeof(resp));
    extract_line_value(resp, result.sim_pin_status, NULL);

    send_at_command("AT+CCID", resp, sizeof(resp));
    extract_line_value(resp, result.sim_number, NULL); // Raw number allowed

    // Network registration (retry loop)
    for (int i = 0; i < 12; i++) {
        send_at_command("AT+CREG?", resp, sizeof(resp));
        parse_creg_status(resp, result.network_status);

        // Debug CGREG and COPS
        send_at_command("AT+CGREG?", resp, sizeof(resp));
        ESP_LOGI(TAG, "GPRS Status: %s", resp);

        send_at_command("AT+COPS?", resp, sizeof(resp));
        ESP_LOGI(TAG, "Operator: %s", resp);

        if (strstr(result.network_status, "Registered")) break;

        // Try forcing auto registration
        if (i == 8) {
            ESP_LOGW(TAG, "Trying to force auto-operator selection...");
            send_at_command("AT+COPS=0", resp, sizeof(resp));
        }

        vTaskDelay(pdMS_TO_TICKS(4000));
    }

    send_at_command("AT+CSQ", resp, sizeof(resp));
    parse_rssi(resp, &result.rssi, &result.rssi_description);

    print_json(&result);

    vTaskDelete(NULL);
}

void app_main()
{
    uart_init();
    xTaskCreate(sim700l_test_task, "sim7600_test_task", 4096, NULL, 5, NULL);
}
#endif

#if 1 // start sim check
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "driver/uart.h"
#include <string.h>

#define UART_NUM UART_NUM_1
#define BUF_SIZE 512
#define MAX_RETRIES 5

static const char *TAG = "SIM_DIAG";
static char response[BUF_SIZE];

// Result structure
typedef struct
{
    char sim_status[16];
    char imsi[32];
    char imei[32];
    char sim_number[32];
    char modem_version[64];
    int rssi;
    char rssi_desc[16];
    char network_status[32];
} sim_diag_result_t;

sim_diag_result_t diag_result = {0};

// Helper to send AT command and receive response
bool send_at_command(const char *command, int timeout_ms)
{
    uart_flush(UART_NUM);
    uart_write_bytes(UART_NUM, command, strlen(command));
    uart_write_bytes(UART_NUM, "\r\n", 2);

    memset(response, 0, sizeof(response));
    int len = uart_read_bytes(UART_NUM, (uint8_t *)response, BUF_SIZE - 1, timeout_ms / portTICK_PERIOD_MS);
    if (len > 0)
    {
        response[len] = '\0';
        ESP_LOGI(TAG, "Response for [%s]: %s", command, response);
        return true;
    }
    ESP_LOGW(TAG, "No response for command: %s", command);
    return false;
}

bool check_sim_status()
{
    for (int i = 0; i < MAX_RETRIES; i++)
    {
        if (send_at_command("AT+CPIN?", 2000))
        {
            if (strstr(response, "READY"))
            {
                strcpy(diag_result.sim_status, "READY");
                return true;
            }
            else
            {
                strcpy(diag_result.sim_status, "NOT READY");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool get_imsi()
{
    for (int i = 0; i < MAX_RETRIES; i++)
    {
        if (send_at_command("AT+CIMI", 2000))
        {
            char *line = strtok(response, "\r\n");
            while (line != NULL)
            {
                if (strlen(line) > 0)
                {
                    strncpy(diag_result.modem_version, line, sizeof(diag_result.modem_version) - 1);
                    diag_result.modem_version[sizeof(diag_result.modem_version) - 1] = '\0';
                    return true;
                }
                line = strtok(NULL, "\r\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

bool get_imei()
{
    for (int i = 0; i < MAX_RETRIES; i++)
    {
        if (send_at_command("AT+CGSN", 2000))
        {
            char *line = strtok(response, "\r\n");
            while (line != NULL)
            {
                if (strlen(line) > 10 && strspn(line, "0123456789") == strlen(line))
                {
                    strncpy(diag_result.imei, line, sizeof(diag_result.imei) - 1);
                    return true;
                }
                line = strtok(NULL, "\r\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}
#if 0
bool get_modem_version(void)
{
    for (int i = 0; i < MAX_RETRIES; i++)
    {
        if (send_at_command("AT+CGMM", 2000))
        {
            char *line = strtok(response, "\r\n");
            while (line != NULL)
            {
                // Ignore empty lines or just OK/ERROR
                if (strlen(line) > 0 && strcmp(line, "OK") != 0 && strcmp(line, "ERROR") != 0)
                {
                    strncpy(diag_result.modem_version, line, sizeof(diag_result.modem_version) - 1);
                    diag_result.modem_version[sizeof(diag_result.modem_version) - 1] = '\0';
                    return true;
                }
                line = strtok(NULL, "\r\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    strcpy(diag_result.modem_version, "N/A");
    return false;
}
#else
bool get_modem_version()
{
    for (int i = 0; i < MAX_RETRIES; i++)
    {
        // if (send_at_command("AT+CGMM", 2000))
        if (send_at_command("AT+SIMCOMATI", 2000))
        {
            char *line = strtok(response, "\r\n");
            bool echo_skipped = false;

            while (line != NULL)
            {
                if (!echo_skipped)
                {
                    // Skip the echoed command line
                    echo_skipped = true;
                }
                else if (strlen(line) > 0 && strcmp(line, "OK") != 0 && strcmp(line, "ERROR") != 0)
                {
                    strncpy(diag_result.modem_version, line, sizeof(diag_result.modem_version) - 1);
                    diag_result.modem_version[sizeof(diag_result.modem_version) - 1] = '\0';
                    return true;
                }
                line = strtok(NULL, "\r\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    strcpy(diag_result.modem_version, "N/A");
    return false;
}
#endif
bool get_sim_number()
{
    for (int i = 0; i < MAX_RETRIES; i++)
    {
        if (send_at_command("AT+CNUM", 2000))
        {
            char *line = strtok(response, "\r\n");
            while (line != NULL)
            {
                if (strstr(line, "+CNUM:"))
                {
                    char number[32];
                    if (sscanf(line, "+CNUM: \\\"%*[^\"]\\\",\\\"%31[^\"]\\\"", number) == 1)
                    {
                        strncpy(diag_result.sim_number, number, sizeof(diag_result.sim_number) - 1);
                        return true;
                    }
                }

                line = strtok(NULL, "\r\n");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    strcpy(diag_result.sim_number, "N/A");
    ESP_LOGW(TAG, "SIM phone number not available or not stored on SIM");
    return false;
}

bool check_network_registration()
{
    int retries = 0;
    bool registered = false;

    while (retries < MAX_RETRIES)
    {
        if (send_at_command("AT+CREG?", 2000))
        {
            int n = 0, stat = 0;
            char *creg_line = strstr(response, "+CREG:");
            if (creg_line && sscanf(creg_line, "+CREG: %d,%d", &n, &stat) == 2)
            {
                if (stat == 1)
                {
                    strcpy(diag_result.network_status, "Registered (home)");
                    registered = true;
                    break;
                }
                else if (stat == 5)
                {
                    strcpy(diag_result.network_status, "Registered (roaming)");
                    registered = true;
                    break;
                }
                else
                {
                    strcpy(diag_result.network_status, "Not registered");
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
        retries++;
    }

    if (!registered)
    {
        ESP_LOGW(TAG, "Attempting automatic network registration...");
        send_at_command("AT+COPS=0", 3000);
        vTaskDelay(pdMS_TO_TICKS(5000));

        send_at_command("AT+COPS?", 2000);
        ESP_LOGI(TAG, "Operator Info: %s", response);

        send_at_command("AT+CGREG?", 2000);
        ESP_LOGI(TAG, "Packet domain registration: %s", response);

        send_at_command("AT+CREG?", 2000);
        int n = 0, stat = 0;
        char *creg_line = strstr(response, "+CREG:");
        if (creg_line && sscanf(creg_line, "+CREG: %d,%d", &n, &stat) == 2)
        {
            if (stat == 1)
                strcpy(diag_result.network_status, "Registered (home)");
            else if (stat == 5)
                strcpy(diag_result.network_status, "Registered (roaming)");
            else
                strcpy(diag_result.network_status, "Registration failed");
        }
        else
        {
            strcpy(diag_result.network_status, "Registration failed");
        }
    }

    return registered;
}

void check_signal_quality()
{
    if (send_at_command("AT+CSQ", 1000))
    {
        int rssi = 0, ber = 0;
        char *csq_line = strstr(response, "+CSQ:");
        if (csq_line && sscanf(csq_line, "+CSQ: %d,%d", &rssi, &ber) == 2)
        {
            diag_result.rssi = rssi;
            const char *desc = "Unknown";
            if (rssi <= 9)
                desc = "Poor";
            else if (rssi <= 14)
                desc = "Fair";
            else if (rssi <= 19)
                desc = "Moderate";
            else if (rssi <= 30)
                desc = "Good";
            else if (rssi == 31)
                desc = "Excellent";
            strncpy(diag_result.rssi_desc, desc, sizeof(diag_result.rssi_desc) - 1);
        }
        else
        {
            diag_result.rssi = 99;
            strcpy(diag_result.rssi_desc, "Unknown");
        }
    }
}

void print_json_result()
{
    ESP_LOGI(TAG, "\n========== SIM DIAGNOSTIC RESULT ==========");
    printf("{\n");
    printf("  \"sim_status\": \"%s\",\n", diag_result.sim_status);
    printf("  \"modem_version\": \"%s\",\n", diag_result.modem_version[0] ? diag_result.modem_version : "N/A");
    printf("  \"imsi\": \"%s\",\n", diag_result.imsi[0] ? diag_result.imsi : "N/A");
    printf("  \"imei\": \"%s\",\n", diag_result.imei[0] ? diag_result.imei : "N/A");
    // printf("  \"sim_number\": \"%s\",\n", diag_result.sim_number[0] ? diag_result.sim_number : "N/A");
    // printf("  \"network_registration\": \"%s\",\n", diag_result.network_status);
    // printf("  \"signal_quality\": {\n");
    // printf("    \"rssi\": %d,\n", diag_result.rssi);
    // printf("    \"description\": \"%s\"\n", diag_result.rssi_desc);
    printf("  }\n");
    printf("}\n");
    ESP_LOGI(TAG, "===========================================\n");
}

void run_sim_diagnostics()
{

    memset(&diag_result, 0, sizeof(diag_result));
    send_at_command("AT", 1000);
    send_at_command("AT+CMEE=2", 1000); // AT+CPIN?
    send_at_command("AT+CPIN?", 1000);  // AT+CREG?
    send_at_command("AT+CREG?", 1000);  // AT+CSQ
    send_at_command("AT+CSQ", 1000);    // AT+CIMI

    // check_sim_status();
    get_modem_version();
    get_imsi();
    get_imei();

    // get_sim_number();
    // check_network_registration();
    // check_signal_quality();

    print_json_result();
}

void app_main(void)
{
    const uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

    uart_driver_install(UART_NUM, BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(UART_NUM, &uart_config);
    uart_set_pin(UART_NUM, GPIO_NUM_17, GPIO_NUM_16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    run_sim_diagnostics();
}

#endif // end sim check
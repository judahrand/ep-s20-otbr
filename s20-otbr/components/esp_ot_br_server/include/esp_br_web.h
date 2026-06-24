#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start border router web server, which provides REST APIs and GUI
 *
 * @param[in] base_path is the virtual file path of web server
 */
void esp_br_web_start(char *base_path);

/**
 * @brief Set the safe mode flag for web UI display.
 *
 * When true, the /config endpoint will report "safe_mode": true
 * and the web UI can display a banner.
 *
 * @param[in] safe_mode  true if the device is in safe mode.
 */
void esp_br_web_set_safe_mode(bool safe_mode);

#ifdef __cplusplus
}
#endif

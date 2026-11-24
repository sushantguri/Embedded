#ifndef UART_APP_H
#define UART_APP_H

#include <stddef.h>
#include "app_types.h"

/**
 * @brief Format a measurement record as either CSV or human-readable serial text.
 */
int uart_format_record(char *buf,
                       size_t buf_len,
                       const noise_measurement_t *meas,
                       bool csv_mode);

/* Backward compatibility alias for logger_format_record */
static inline int logger_format_record(char *b, size_t l, const noise_measurement_t *m, bool c) {
    return uart_format_record(b, l, m, c);
}

#ifndef HOST_TEST
#include "esp_err.h"

/**
 * @brief Initialize UART communication (console stdout + optional auxiliary UART1).
 */
esp_err_t uart_init(void);

/**
 * @brief Write a line of text to the active UART interfaces.
 */
void uart_write_line(const char *line, size_t len);

static inline esp_err_t logger_init(void) { return uart_init(); }
static inline void logger_write_line(const char *l, size_t len) { uart_write_line(l, len); }
#endif

#endif /* UART_APP_H */

/**
 * @file auraio_syslog.c
 * @brief Syslog log handler integration for AuraIO
 */

#include "auraio_syslog.h"

#include <syslog.h>

/**
 * Log callback — forwards to syslog(3).
 *
 * AuraIO log levels match syslog priorities 1:1, so we pass the level
 * through directly.  The "%s" format prevents format-string injection
 * if the message happens to contain percent characters.
 */
static void syslog_handler(int level, const char *msg, void *userdata) {
    (void)userdata;
    syslog(level, "%s", msg);
}

void auraio_syslog_install(const auraio_syslog_options_t *options) {
    const char *ident = "auraio";
    int facility = LOG_USER;
    int log_options = LOG_PID | LOG_NDELAY;

    if (options) {
        if (options->ident) ident = options->ident;
        if (options->facility >= 0) facility = options->facility;
        if (options->log_options >= 0) log_options = options->log_options;
    }

    openlog(ident, log_options, facility);
    auraio_set_log_handler(syslog_handler, NULL);
}

void auraio_syslog_remove(void) {
    auraio_set_log_handler(NULL, NULL);
    closelog();
}
